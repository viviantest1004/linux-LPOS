/* parse.c - input, the lexer and the parser. Part of sh.c; see sh.h.
 *
 * The old shell split each line into statements with a set of string
 * scanners and then worked out block structure by counting keywords.
 * That is why it could not have `eval` (there was no way to run a string
 * that had not come through the line reader), why `case` inside `$(...)`
 * broke, and why every construct had a limit on how long it could be.
 *
 * This is the conventional design instead, the one dash and every other
 * POSIX shell share: a lexer that knows about quoting and produces
 * tokens, and a recursive-descent parser over the grammar in POSIX XCU
 * 2.10 that builds a tree. The tree is then evaluated (exec.c). The
 * shell reads one complete command at a time - up to the end of a line
 * that closes every open construct - runs it, and reads the next, which
 * is what makes `alias` on one line affect the next and lets a syntax
 * error on line 90 of a script not stop lines 1 to 89 from running,
 * exactly as dash behaves.
 *
 * ── Words ──
 *
 * A word is kept as a list of parts rather than as the text it was
 * typed as: literal runs (marked quoted or not), $parameters, command
 * substitutions (already parsed into trees), arithmetic. Quoting is
 * resolved here, once; expansion later only has to look at the marks.
 *
 * ── Memory ──
 *
 * Every node and string of one complete command comes out of one arena,
 * which is thrown away after the command has run. A function definition
 * keeps its arena alive by holding a reference to it, and a running
 * function holds one too, so redefining a function from inside itself
 * does not free the code that is executing. */

/* ── arenas ────────────────────────────────────────────────────────── */

struct ablock {
    struct ablock *next;
    size_t         used, size;
    /* data follows, 16-byte aligned */
};

static arena_t *arena_new(void)
{
    arena_t *a = xcalloc(sizeof *a);
    a->refs = 1;
    return a;
}

static void arena_ref(arena_t *a)
{
    if (a)
        a->refs++;
}

static void arena_unref(arena_t *a)
{
    if (!a || --a->refs > 0)
        return;
    struct ablock *b = a->blocks;
    while (b) {
        struct ablock *n = b->next;
        xfree(b);
        b = n;
    }
    xfree(a);
}

#define AHDR ((sizeof(struct ablock) + 15) & ~(size_t)15)

static void *pa_alloc(size_t n)
{
    arena_t *a = cur_arena;
    n = (n + 15) & ~(size_t)15;
    struct ablock *b = a->blocks;
    if (!b || b->used + n > b->size) {
        size_t sz = b ? b->size * 2 : 1024;
        if (sz > 64 * 1024)
            sz = 64 * 1024;
        if (sz < n)
            sz = n;
        struct ablock *nb = xmalloc(AHDR + sz);
        nb->next = b;
        nb->used = 0;
        nb->size = sz;
        a->blocks = nb;
        b = nb;
    }
    void *p = (char *)b + AHDR + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
}

static char *pa_strndup(const char *s, size_t n)
{
    char *d = pa_alloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

/* ── input ─────────────────────────────────────────────────────────── */

enum { IS_FILE, IS_STRING, IS_ALIAS, IS_TTY };

typedef struct insrc {
    struct insrc *prev;
    int      kind;
    int      fd;
    char    *buf;           /* what pgetc hands out */
    size_t   pos, len;
    char    *raw;           /* IS_FILE: bytes read but not yet handed out */
    size_t   rpos, rlen, rcap;
    bool     eof;
    bool     byte_mode;     /* a pipe on fd 0: never read past a line */
    int      lineno;
    alias_t *alias;
    bool     alias_blank;   /* the alias value ended in a blank */
} insrc_t;

static insrc_t *in;
static bool     ps1_next = true;        /* the next prompt is PS1 */
static bool     input_interrupted;      /* Ctrl-C at the prompt */
static bool     alias_blank_popped;     /* see readtoken */

static void input_push(insrc_t *s)
{
    s->prev = in;
    s->lineno = s->lineno ? s->lineno : 1;
    in = s;
}

static void input_push_file(int fd, bool tty)
{
    insrc_t *s = xcalloc(sizeof *s);
    s->kind = tty ? IS_TTY : IS_FILE;
    s->fd = fd;
    /* A script coming in on a pipe as stdin shares that pipe with the
     * commands it runs: `sh < script` where a line is `read x` must let
     * `read` see the next line of the script, not have the shell take it
     * in its 4KB read. On a pipe the only way is one byte at a time. A
     * seekable stdin gets its unread bytes given back instead - see
     * input_sync_fd0. */
    if (!tty && fd == 0 && lp_lseek(0, 0, SEEK_CUR) < 0)
        s->byte_mode = true;
    input_push(s);
}

static void input_push_string(const char *str, size_t len, void *alias)
{
    insrc_t *s = xcalloc(sizeof *s);
    s->kind = alias ? IS_ALIAS : IS_STRING;
    s->fd = -1;
    s->buf = xstrndup(str, len);
    s->len = len;
    s->alias = alias;
    if (alias) {
        ((alias_t *)alias)->busy = true;
        s->alias_blank = len > 0 && is_blank((u8)str[len - 1]);
        s->lineno = in ? in->lineno : 1;
    }
    input_push(s);
}

static void input_pop(void)
{
    insrc_t *s = in;
    if (!s)
        return;
    in = s->prev;
    if (s->alias) {
        s->alias->busy = false;
        if (in)
            in->lineno = s->lineno;
    }
    xfree(s->buf);
    xfree(s->raw);
    xfree(s);
}

static int input_depth(void)
{
    int n = 0;
    for (insrc_t *s = in; s; s = s->prev)
        n++;
    return n;
}

static int input_lineno(void)
{
    return in ? in->lineno : 0;
}

/* Hand unread script bytes back to a seekable stdin before a command
 * runs, so that the command reads what follows in the file. */
static void input_sync_fd0(void)
{
    for (insrc_t *s = in; s; s = s->prev) {
        if (s->kind != IS_FILE || s->fd != 0 || s->byte_mode)
            continue;
        size_t unread = (s->rlen - s->rpos);
        if (unread && lp_lseek(0, -(off_t)unread, SEEK_CUR) >= 0)
            s->rpos = s->rlen = 0;
    }
}

/* Move one line from the raw buffer into buf. False at end of input. */
static bool refill_file(insrc_t *s)
{
    size_t start = s->rpos;
    for (;;) {
        for (size_t i = s->rpos; i < s->rlen; i++) {
            if (s->raw[i] == '\n') {
                size_t n = i + 1 - start;
                xfree(s->buf);
                s->buf = xstrndup(s->raw + start, n);
                s->pos = 0;
                s->len = n;
                s->rpos = i + 1;
                if (opt[O_verbose])
                    xwrite_all(2, s->buf, n);
                return true;
            }
        }
        s->rpos = s->rlen;
        if (s->eof)
            break;
        /* Keep what is pending at the front and read more after it. */
        size_t pend = s->rlen - start;
        if (start > 0 && pend)
            memmove(s->raw, s->raw + start, pend);
        s->rlen = pend;
        s->rpos = pend;
        start = 0;
        if (s->rcap < s->rlen + 4096) {
            s->rcap = s->rlen + 4096;
            s->raw = xrealloc(s->raw, s->rcap);
        }
        long n = xread(s->fd, s->raw + s->rlen, s->byte_mode ? 1 : 4096);
        if (n <= 0) {
            s->eof = true;
            continue;
        }
        s->rlen += (size_t)n;
    }
    size_t n = s->rlen - start;
    if (n == 0)
        return false;
    xfree(s->buf);
    s->buf = xstrndup(s->raw + start, n);    /* last line, no newline */
    s->pos = 0;
    s->len = n;
    s->rpos = s->rlen = 0;
    if (opt[O_verbose])
        xwrite_all(2, s->buf, n);
    return true;
}

static bool refill_tty(insrc_t *s)
{
    if (s->eof)
        return false;
    const char *ps;
    if (ps1_next) {
        ps = var_get("PS1");
        if (!ps) ps = sys_geteuid() == 0 ? "# " : "$ ";
    } else {
        ps = var_get("PS2");
        if (!ps) ps = "> ";
    }
    bool first = ps1_next;
    ps1_next = false;
    strbuf_t line = {0};
    long r = edit_read_line(first ? ps : ps, &line);
    if (r == EDIT_INTR) {
        input_interrupted = true;
        sb_free(&line);
        return false;
    }
    if (r == EDIT_EOF) {
        s->eof = true;
        sb_free(&line);
        return false;
    }
    /* History expansion, then history, then the parser - in that order,
     * so the history holds what actually ran. */
    strbuf_t ex = {0};
    bool print = false;
    if (!history_expand(sb_str(&line), &ex, &print)) {
        sb_free(&line);
        sb_free(&ex);
        input_interrupted = true;       /* abandon, like bash */
        exitstatus = 1;
        return false;
    }
    if (print) {
        outs(out2, sb_str(&ex));
        outc(out2, '\n');
        flush_out(out2);
    }
    history_add(sb_str(&ex));
    sb_putc(&ex, '\n');
    xfree(s->buf);
    s->len = ex.len;
    s->buf = sb_take(&ex);
    s->pos = 0;
    sb_free(&line);
    if (opt[O_verbose])
        xwrite_all(2, s->buf, s->len);
    return true;
}

#define PEOF (-1)

static int pgetc(void)
{
    for (;;) {
        insrc_t *s = in;
        if (!s)
            return PEOF;
        if (s->pos < s->len) {
            int c = (u8)s->buf[s->pos++];
            if (c == '\n')
                s->lineno++;
            return c;
        }
        if (s->kind == IS_ALIAS) {
            if (s->alias_blank)
                alias_blank_popped = true;
            input_pop();
            continue;
        }
        bool ok = false;
        if (s->kind == IS_FILE)
            ok = refill_file(s);
        else if (s->kind == IS_TTY)
            ok = refill_tty(s);
        if (!ok)
            return PEOF;
    }
}

static void pungetc(int c)
{
    if (c == PEOF || !in || in->pos == 0)
        return;
    in->pos--;
    if (in->buf[in->pos] == '\n')
        in->lineno--;
}

static int ppeek(void)
{
    int c = pgetc();
    pungetc(c);
    return c;
}

/* Throw away the rest of the line being typed, after a syntax error or
 * Ctrl-C at the prompt. */
static void input_discard_line(void)
{
    while (in && in->kind == IS_ALIAS)
        input_pop();
    if (in && in->kind == IS_TTY) {
        in->pos = in->len;
    }
}

/* ── tokens ────────────────────────────────────────────────────────── */

enum {
    T_EOF, T_NL, T_SEMI, T_AMP, T_PIPE, T_PIPEAMP, T_AND, T_OR,
    T_DSEMI, T_SEMIAND, T_DSEMIAND, T_LPAREN, T_RPAREN, T_REDIR, T_WORD,
    T_DPAREN,           /* (( ... )) - the expression is in tokword */
    /* reserved words */
    T_IF, T_THEN, T_ELSE, T_ELIF, T_FI, T_DO, T_DONE, T_CASE, T_ESAC,
    T_WHILE, T_UNTIL, T_FOR, T_IN, T_LBRACE, T_RBRACE, T_BANG,
    T_DLBRACK, T_FUNCTION, T_TIME
};

static const char *const tokname[] = {
    "end of file", "newline", ";", "&", "|", "|&", "&&", "||",
    ";;", ";&", ";;&", "(", ")", "redirection", "word", "((",
    "if", "then", "else", "elif", "fi", "do", "done", "case", "esac",
    "while", "until", "for", "in", "{", "}", "!", "[[", "function", "time"
};

static const struct { const char *w; int t; } keywords[] = {
    { "if", T_IF }, { "then", T_THEN }, { "else", T_ELSE },
    { "elif", T_ELIF }, { "fi", T_FI }, { "do", T_DO }, { "done", T_DONE },
    { "case", T_CASE }, { "esac", T_ESAC }, { "while", T_WHILE },
    { "until", T_UNTIL }, { "for", T_FOR }, { "in", T_IN },
    { "{", T_LBRACE }, { "}", T_RBRACE }, { "!", T_BANG },
    { "[[", T_DLBRACK }, { "function", T_FUNCTION }, { "time", T_TIME },
};

static bool is_keyword(const char *s)
{
    for (unsigned i = 0; i < sizeof keywords / sizeof *keywords; i++)
        if (strcmp(keywords[i].w, s) == 0)
            return true;
    return strcmp(s, "]]") == 0;
}

#define CHKALIAS 1
#define CHKKWD   2
#define CHKNL    4

static int      tok;            /* the current token */
static word_t  *tokword;        /* its word, for T_WORD and keywords */
static redir_t *tokredir;       /* its redirection, for T_REDIR */
static bool     tokpushback;
static int      checkkwd;
static bool     parse_err;
static int      tok_lineno;

/* Heredocs whose body starts after the next newline. */
typedef struct {
    redir_t *r;
    char    *delim;
} hpending_t;

static hpending_t *hpend;
static int         nhpend, caphpend;

static void syntax_error(const char *fmt, const char *arg)
{
    if (parse_err)
        return;
    parse_err = true;
    strbuf_t b = {0};
    sb_puts(&b, "syntax error");
    if (fmt) {
        sb_puts(&b, ": ");
        sb_printf(&b, fmt, arg);
    }
    int save = cmd_lineno;
    cmd_lineno = in ? in->lineno : 0;
    sh_warn("%s", sb_str(&b));
    cmd_lineno = save;
    sb_free(&b);
}

static void unexpected(int t)
{
    const char *name = t < (int)(sizeof tokname / sizeof *tokname)
                       ? tokname[t] : "token";
    if (t == T_WORD && tokword && tokword->lit)
        name = tokword->lit;
    if (t == T_EOF)
        syntax_error("unexpected end of file", NULL);
    else
        syntax_error("unexpected token `%s'", name);
}

/* ── building words ────────────────────────────────────────────────── */

typedef struct {
    wpart_t  *head, **tail;
    strbuf_t  lit;
    int       litq;         /* quotedness of what is in lit */
    bool      have_lit;
    u8        flags;
} wbuild_t;

static void wb_init(wbuild_t *b)
{
    memset(b, 0, sizeof *b);
    b->tail = &b->head;
}

static void wb_flush(wbuild_t *b)
{
    if (!b->have_lit)
        return;
    wpart_t *p = pa_alloc(sizeof *p);
    p->type = WP_LIT;
    p->quoted = (u8)b->litq;
    p->text = pa_strndup(b->lit.s ? b->lit.s : "", b->lit.len);
    p->len = b->lit.len;
    *b->tail = p;
    b->tail = &p->next;
    b->lit.len = 0;
    b->have_lit = false;
}

static void wb_char(wbuild_t *b, char c, int quoted)
{
    if (b->have_lit && b->litq != quoted)
        wb_flush(b);
    b->have_lit = true;
    b->litq = quoted;
    sb_putc(&b->lit, c);
    if (quoted)
        b->flags |= W_QUOTED;
}

/* "" or '' - a quoted nothing, which still makes a word exist. */
static void wb_empty_quoted(wbuild_t *b)
{
    if (b->have_lit && b->litq)
        return;
    wb_flush(b);
    b->have_lit = true;
    b->litq = 1;
    b->flags |= W_QUOTED;
}

static void wb_part(wbuild_t *b, wpart_t *p)
{
    wb_flush(b);
    *b->tail = p;
    b->tail = &p->next;
    b->flags |= W_HASPARTS;
    if (p->quoted)
        b->flags |= W_QUOTED;
}

static word_t *wb_finish(wbuild_t *b)
{
    wb_flush(b);
    sb_free(&b->lit);
    word_t *w = pa_alloc(sizeof *w);
    w->parts = b->head;
    w->flags = b->flags;
    /* Plain unquoted literal: keep the text handy for keywords, aliases,
     * function names and the like. */
    if (w->parts && !w->parts->next && w->parts->type == WP_LIT &&
        !w->parts->quoted)
        w->lit = w->parts->text;
    else if (!w->parts)
        w->lit = pa_strndup("", 0);
    return w;
}

/* ── the word reader ───────────────────────────────────────────────── */

enum {
    LX_WORD,        /* an ordinary word: ends at a metacharacter */
    LX_DQ,          /* inside "...": ends at the closing quote */
    LX_BRACE,       /* a ${...} operand: ends at } (or stopc) */
    LX_HEREDOC,     /* a heredoc body or prompt: ends at end of input */
    LX_ARITH,       /* inside $((...)): ends at )) */
    LX_SUBSCRIPT,   /* inside [...] of an array element */
    LX_REGEX        /* the right side of =~ in [[ ]] */
};

static bool lex_text(wbuild_t *b, int mode, bool dq, int stopc, int *ended);
static bool lex_dollar(wbuild_t *b, bool dq, int mode);
static bool lex_backquote(wbuild_t *b, bool dq);
static node_t *parse_cmdsub_paren(void);
static node_t *parse_cmdsub_string(const char *s, size_t n);
static int readtoken(void);

static bool is_meta(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' ||
           c == '|' || c == '<' || c == '>' || c == '(' || c == ')';
}

/* Raw capture, for heredoc delimiters: `<<"EOF"` needs the text as
 * typed to work out both the delimiter and whether it was quoted. */
static strbuf_t *lex_raw;

static int rgetc(void)
{
    int c = pgetc();
    if (lex_raw && c != PEOF)
        sb_putc(lex_raw, (char)c);
    return c;
}

static void rungetc(int c)
{
    if (c == PEOF)
        return;
    if (lex_raw && lex_raw->len)
        lex_raw->s[--lex_raw->len] = '\0';
    pungetc(c);
}

static word_t *lex_subword(int mode, bool dq, int stopc, int *ended)
{
    wbuild_t b;
    wb_init(&b);
    if (!lex_text(&b, mode, dq, stopc, ended)) {
        sb_free(&b.lit);
        return NULL;
    }
    return wb_finish(&b);
}

/* $'...' - bash's C-style string. Common enough in scripts written on
 * other systems (IFS=$'\n') to be worth the thirty lines. */
static bool lex_ansi_c(wbuild_t *b)
{
    wb_empty_quoted(b);
    for (;;) {
        int c = rgetc();
        if (c == PEOF) {
            syntax_error("unexpected end of file looking for `%s'", "'");
            return false;
        }
        if (c == '\'')
            return true;
        if (c != '\\') {
            wb_char(b, (char)c, 1);
            continue;
        }
        c = rgetc();
        long v;
        int n;
        switch (c) {
        case 'n': wb_char(b, '\n', 1); break;
        case 't': wb_char(b, '\t', 1); break;
        case 'r': wb_char(b, '\r', 1); break;
        case 'a': wb_char(b, '\a', 1); break;
        case 'b': wb_char(b, '\b', 1); break;
        case 'e': case 'E': wb_char(b, 27, 1); break;
        case 'f': wb_char(b, '\f', 1); break;
        case 'v': wb_char(b, '\v', 1); break;
        case '\\': case '\'': case '"': case '?':
            wb_char(b, (char)c, 1);
            break;
        case 'x':
            v = 0;
            for (n = 0; n < 2; n++) {
                int d = rgetc();
                int x = is_digit(d) ? d - '0'
                      : (d >= 'a' && d <= 'f') ? d - 'a' + 10
                      : (d >= 'A' && d <= 'F') ? d - 'A' + 10 : -1;
                if (x < 0) { rungetc(d); break; }
                v = v * 16 + x;
            }
            if (n) wb_char(b, (char)v, 1);
            else { wb_char(b, '\\', 1); wb_char(b, 'x', 1); }
            break;
        case 'c': {
            int d = rgetc();
            wb_char(b, (char)(d & 0x1f), 1);
            break;
        }
        default:
            if (c >= '0' && c <= '7') {
                v = c - '0';
                for (n = 1; n < 3; n++) {
                    int d = rgetc();
                    if (d < '0' || d > '7') { rungetc(d); break; }
                    v = v * 8 + (d - '0');
                }
                wb_char(b, (char)v, 1);
            } else if (c == PEOF) {
                wb_char(b, '\\', 1);
            } else {
                wb_char(b, '\\', 1);
                wb_char(b, (char)c, 1);
            }
        }
    }
}

/* Read the name of a parameter: an identifier, a run of digits (only
 * inside braces), or one special character. */
static char *lex_param_name(bool braced)
{
    int c = rgetc();
    strbuf_t nm = {0};
    if (is_name_start(c)) {
        while (is_name_char(c)) {
            sb_putc(&nm, (char)c);
            c = rgetc();
        }
        rungetc(c);
    } else if (is_digit(c)) {
        sb_putc(&nm, (char)c);
        if (braced) {
            while (is_digit(c = rgetc()))
                sb_putc(&nm, (char)c);
            rungetc(c);
        }
    } else if (c != PEOF && strchr("@*#?-$!", c)) {
        sb_putc(&nm, (char)c);
    } else {
        rungetc(c);
        sb_free(&nm);
        return NULL;
    }
    char *r = pa_strndup(nm.s, nm.len);
    sb_free(&nm);
    return r;
}

static bool lex_braced(wbuild_t *b, bool dq)
{
    wpart_t *p = pa_alloc(sizeof *p);
    p->type = WP_PARAM;
    p->quoted = dq;
    p->flags = PF_BRACED;

    int c = rgetc();
    if (c == '#') {
        int n = ppeek();
        if (n == '}' || n == PEOF) {
            p->text = pa_strndup("#", 1);
        } else if (is_name_start(n) || is_digit(n) ||
                   (n != PEOF && strchr("@*#?$!", n))) {
            p->flags |= PF_LEN;
            p->text = lex_param_name(true);
        } else if (n == '-') {
            /* ${#-}: the length of $- */
            rgetc();
            if (ppeek() == '}') {
                p->flags |= PF_LEN;
                p->text = pa_strndup("-", 1);
            } else {
                rungetc('-');
                p->text = pa_strndup("#", 1);
            }
        } else {
            p->text = pa_strndup("#", 1);
        }
    } else if (c == '!') {
        int n = ppeek();
        if (n == '}') {
            p->text = pa_strndup("!", 1);
        } else {
            p->flags |= PF_INDIRECT;
            p->text = lex_param_name(true);
        }
    } else {
        rungetc(c);
        p->text = lex_param_name(true);
    }
    if (!p->text) {
        syntax_error("bad substitution", NULL);
        return false;
    }

    c = rgetc();
    if (c == '[' && is_name_start((u8)p->text[0])) {
        int ended = 0;
        p->sub = lex_subword(LX_SUBSCRIPT, false, 0, &ended);
        if (!p->sub || ended != ']') {
            syntax_error("bad substitution", NULL);
            return false;
        }
        if ((p->flags & PF_INDIRECT) && p->sub->lit &&
            (strcmp(p->sub->lit, "@") == 0 || strcmp(p->sub->lit, "*") == 0)) {
            p->flags = (u8)((p->flags & ~PF_INDIRECT) | PF_KEYS);
        }
        c = rgetc();
    }

    int ended = 0;
    bool patop = false;
    switch (c) {
    case '}':
        wb_part(b, p);
        return true;
    case ':': {
        int n = rgetc();
        if (n == '-' || n == '=' || n == '?' || n == '+') {
            p->flags |= PF_COLON;
            c = n;
            break;
        }
        rungetc(n);
        p->op = PO_SUBSTR;
        p->arg = lex_subword(LX_BRACE, false, ':', &ended);
        if (!p->arg)
            return false;
        if (ended == ':') {
            p->arg2 = lex_subword(LX_BRACE, false, 0, &ended);
            if (!p->arg2)
                return false;
        }
        wb_part(b, p);
        return true;
    }
    case '-': case '=': case '?': case '+':
        break;
    case '#': case '%': {
        int n = rgetc();
        if (n == c)
            p->op = c == '#' ? PO_RMPREL : PO_RMSUFL;
        else {
            rungetc(n);
            p->op = c == '#' ? PO_RMPRE : PO_RMSUF;
        }
        patop = true;
        break;
    }
    case '/': {
        int n = rgetc();
        if (n == '/') p->op = PO_REPLALL;
        else if (n == '#') p->op = PO_REPLPRE;
        else if (n == '%') p->op = PO_REPLSUF;
        else { rungetc(n); p->op = PO_REPL; }
        p->arg = lex_subword(LX_BRACE, false, '/', &ended);
        if (!p->arg)
            return false;
        if (ended == '/') {
            p->arg2 = lex_subword(LX_BRACE, dq, 0, &ended);
            if (!p->arg2)
                return false;
        }
        wb_part(b, p);
        return true;
    }
    case '^': case ',': {
        int n = rgetc();
        if (n == c)
            p->op = c == '^' ? PO_UPPER : PO_LOWER;
        else {
            rungetc(n);
            p->op = c == '^' ? PO_UPPER1 : PO_LOWER1;
        }
        patop = true;
        break;
    }
    default:
        syntax_error("bad substitution", NULL);
        return false;
    }
    if (!p->op) {
        p->op = c == '-' ? PO_DEFAULT : c == '=' ? PO_ASSIGN
              : c == '?' ? PO_ERROR : PO_ALT;
    }
    /* The pattern of # % ^ , is not quoted by double quotes around the
     * whole expansion; the word of - = ? + is. */
    p->arg = lex_subword(LX_BRACE, patop ? false : dq, 0, &ended);
    if (!p->arg)
        return false;
    wb_part(b, p);
    return true;
}

static bool lex_dollar(wbuild_t *b, bool dq, int mode)
{
    int c = rgetc();
    if (c == '{')
        return lex_braced(b, dq);
    if (c == '(') {
        int n = rgetc();
        wpart_t *p = pa_alloc(sizeof *p);
        p->quoted = dq;
        if (n == '(') {
            p->type = WP_ARITH;
            int ended = 0;
            p->arg = lex_subword(LX_ARITH, true, 0, &ended);
            if (!p->arg)
                return false;
        } else {
            rungetc(n);
            p->type = WP_CMDSUB;
            p->tree = parse_cmdsub_paren();
            if (parse_err)
                return false;
        }
        wb_part(b, p);
        return true;
    }
    if (c == '\'' && !dq && mode != LX_HEREDOC)
        return lex_ansi_c(b);
    if (c == '"' && !dq && mode != LX_HEREDOC) {
        wb_empty_quoted(b);
        return lex_text(b, LX_DQ, true, 0, NULL);
    }
    rungetc(c);
    char *name = lex_param_name(false);
    if (!name) {
        wb_char(b, '$', dq);
        return true;
    }
    wpart_t *p = pa_alloc(sizeof *p);
    p->type = WP_PARAM;
    p->quoted = dq;
    p->text = name;
    wb_part(b, p);
    return true;
}

/* `...` - the old form of $(...). The text between the backquotes has
 * its own layer of backslash escapes, removed here; what is left is
 * parsed as a script of its own. */
static bool lex_backquote(wbuild_t *b, bool dq)
{
    strbuf_t t = {0};
    for (;;) {
        int c = rgetc();
        if (c == PEOF) {
            sb_free(&t);
            syntax_error("unexpected end of file looking for `%s'", "`");
            return false;
        }
        if (c == '`')
            break;
        if (c == '\\') {
            int n = rgetc();
            if (n == '\\' || n == '`' || n == '$' || (dq && n == '"')) {
                sb_putc(&t, (char)n);
                continue;
            }
            if (n == '\n')
                continue;
            sb_putc(&t, '\\');
            if (n != PEOF)
                sb_putc(&t, (char)n);
            continue;
        }
        sb_putc(&t, (char)c);
    }
    wpart_t *p = pa_alloc(sizeof *p);
    p->type = WP_CMDSUB;
    p->quoted = dq;
    p->tree = parse_cmdsub_string(t.s ? t.s : "", t.len);
    sb_free(&t);
    if (parse_err)
        return false;
    wb_part(b, p);
    return true;
}

/* An array literal after name=: read words up to the closing paren. */
static bool lex_array(wbuild_t *b)
{
    wpart_t *p = pa_alloc(sizeof *p);
    p->type = WP_ARRAY;
    word_t **tail = &p->list;
    strbuf_t *save_raw = lex_raw;
    lex_raw = NULL;
    for (;;) {
        checkkwd = CHKNL;
        int t = readtoken();
        if (t == T_RPAREN)
            break;
        if (t == T_NL)
            continue;
        if (t != T_WORD) {
            lex_raw = save_raw;
            unexpected(t);
            return false;
        }
        *tail = tokword;
        tail = &tokword->next;
    }
    lex_raw = save_raw;
    wb_part(b, p);
    return true;
}

/* The heart of the lexer: read characters into b according to mode.
 * *ended, when given, receives the character that ended the text. */
static bool lex_text(wbuild_t *b, int mode, bool dq, int stopc, int *ended)
{
    int depth = 0;
    for (;;) {
        int c = rgetc();
        if (c == PEOF) {
            if (mode == LX_WORD || mode == LX_HEREDOC || mode == LX_REGEX)
                return true;
            const char *what = mode == LX_DQ ? "\"" : mode == LX_ARITH ? "))"
                             : mode == LX_SUBSCRIPT ? "]" : "}";
            syntax_error("unexpected end of file looking for `%s'", what);
            return false;
        }
        switch (c) {
        case '\\': {
            int n = rgetc();
            if (n == '\n')
                continue;
            if (n == PEOF) {
                wb_char(b, '\\', dq);
                continue;
            }
            bool special;
            if (mode == LX_HEREDOC)
                special = n == '$' || n == '`' || n == '\\';
            else if (dq || mode == LX_ARITH)
                special = n == '$' || n == '`' || n == '"' || n == '\\' ||
                          (mode == LX_BRACE && n == '}');
            else
                special = true;
            if (special) {
                wb_char(b, (char)n, 1);
            } else {
                wb_char(b, '\\', dq);
                wb_char(b, (char)n, dq);
            }
            continue;
        }
        case '\'':
            if (dq || mode == LX_HEREDOC || mode == LX_ARITH)
                break;
            wb_empty_quoted(b);
            for (;;) {
                c = rgetc();
                if (c == PEOF) {
                    syntax_error("unexpected end of file looking for `%s'",
                                 "'");
                    return false;
                }
                if (c == '\'')
                    break;
                wb_char(b, (char)c, 1);
            }
            continue;
        case '"':
            if (mode == LX_DQ) {
                if (ended) *ended = c;
                return true;
            }
            if (mode == LX_HEREDOC)
                break;
            wb_empty_quoted(b);
            if (!lex_text(b, LX_DQ, true, 0, NULL))
                return false;
            continue;
        case '$':
            if (!lex_dollar(b, dq, mode))
                return false;
            continue;
        case '`':
            if (!lex_backquote(b, dq))
                return false;
            continue;
        }

        switch (mode) {
        case LX_WORD:
            if (c == '(' && !b->head && b->have_lit && !b->litq &&
                b->lit.len >= 2 && b->lit.s[b->lit.len - 1] == '=') {
                /* name=( or name+=( starts an array literal */
                size_t n = b->lit.len - 1;
                if (b->lit.s[n - 1] == '+') n--;
                bool ok = n > 0 && is_name_start((u8)b->lit.s[0]);
                for (size_t i = 1; ok && i < n; i++)
                    ok = is_name_char((u8)b->lit.s[i]);
                if (ok) {
                    if (!lex_array(b))
                        return false;
                    continue;
                }
            }
            if (is_meta(c)) {
                rungetc(c);
                return true;
            }
            break;
        case LX_REGEX:
            if (c == '(') depth++;
            else if (c == ')' && depth > 0) depth--;
            else if (depth == 0 && (c == ' ' || c == '\t' || c == '\n' ||
                                    c == ';' || c == '&')) {
                rungetc(c);
                return true;
            }
            break;
        case LX_BRACE:
            if (c == '}' && depth == 0) {
                if (ended) *ended = c;
                return true;
            }
            if (c == stopc && depth == 0) {
                if (ended) *ended = c;
                return true;
            }
            if (c == '{') depth++;
            else if (c == '}') depth--;
            break;
        case LX_ARITH:
            if (c == '(') depth++;
            else if (c == ')') {
                if (depth == 0) {
                    int n = rgetc();
                    if (n == ')') {
                        if (ended) *ended = ')';
                        return true;
                    }
                    rungetc(n);
                } else {
                    depth--;
                }
            }
            break;
        case LX_SUBSCRIPT:
            if (c == '[') depth++;
            else if (c == ']') {
                if (depth == 0) {
                    if (ended) *ended = c;
                    return true;
                }
                depth--;
            } else if (c == '\n') {
                if (ended) *ended = c;
                rungetc(c);
                return true;
            }
            break;
        default:
            break;
        }
        wb_char(b, (char)c, dq);
    }
}

/* Read one word starting at c (already read). Handles the special case
 * of name[subscript]=value, whose subscript may contain spaces. */
static word_t *lex_word(int c)
{
    wbuild_t b;
    wb_init(&b);
    rungetc(c);
    /* name[ at the very start: try for an element assignment */
    if (is_name_start(c)) {
        strbuf_t nm = {0};
        int d;
        while (is_name_char(d = rgetc()))
            sb_putc(&nm, (char)d);
        if (d == '[') {
            int ended = 0;
            word_t *sub = lex_subword(LX_SUBSCRIPT, false, 0, &ended);
            if (!sub) {
                sb_free(&nm);
                return NULL;
            }
            int e1 = ended == ']' ? rgetc() : PEOF;
            int e2 = e1 == '+' ? rgetc() : PEOF;
            if (ended == ']' && (e1 == '=' || (e1 == '+' && e2 == '='))) {
                /* name[sub]=value: keep the subscript aside */
                sb_putn(&b.lit, nm.s, nm.len);
                b.have_lit = true;
                b.litq = 0;
                wb_char(&b, '[', 0);
                wb_flush(&b);
                wpart_t *sp = pa_alloc(sizeof *sp);
                sp->type = WP_LIT;
                sp->quoted = 0;
                sp->text = pa_strndup("", 0);
                sp->sub = sub;          /* marks the element assignment */
                *b.tail = sp;
                b.tail = &sp->next;
                wb_char(&b, ']', 0);
                if (e1 == '+')
                    wb_char(&b, '+', 0);
                wb_char(&b, '=', 0);
            } else {
                /* not an assignment: an ordinary word with brackets in it */
                if (e2 != PEOF) rungetc(e2);
                if (e1 != PEOF) rungetc(e1);
                sb_putn(&b.lit, nm.s, nm.len);
                b.have_lit = true;
                b.litq = 0;
                wb_char(&b, '[', 0);
                wb_flush(&b);
                for (wpart_t *p = sub->parts; p; ) {
                    wpart_t *n = p->next;
                    p->next = NULL;
                    *b.tail = p;
                    b.tail = &p->next;
                    p = n;
                }
                b.flags |= sub->flags;
                if (ended == ']')
                    wb_char(&b, ']', 0);
            }
        } else {
            rungetc(d);
            if (nm.len) {
                sb_putn(&b.lit, nm.s, nm.len);
                b.have_lit = true;
                b.litq = 0;
            }
        }
        sb_free(&nm);
    }
    if (!lex_text(&b, LX_WORD, false, 0, NULL)) {
        sb_free(&b.lit);
        return NULL;
    }
    return wb_finish(&b);
}

/* ── heredocs ──────────────────────────────────────────────────────── */

/* Quote removal on a delimiter as typed. */
static char *heredoc_delim(const char *raw, bool *quoted)
{
    strbuf_t d = {0};
    *quoted = false;
    for (const char *p = raw; *p; p++) {
        if (*p == '\\' && p[1]) {
            *quoted = true;
            sb_putc(&d, *++p);
        } else if (*p == '\'') {
            *quoted = true;
            while (*++p && *p != '\'')
                sb_putc(&d, *p);
            if (!*p) break;
        } else if (*p == '"') {
            *quoted = true;
            while (*++p && *p != '"') {
                if (*p == '\\' && p[1] && strchr("\\\"$`", p[1]))
                    p++;
                sb_putc(&d, *p);
            }
            if (!*p) break;
        } else {
            sb_putc(&d, *p);
        }
    }
    char *r = pa_strndup(d.s ? d.s : "", d.len);
    sb_free(&d);
    return r;
}

static void read_heredocs(void)
{
    for (int i = 0; i < nhpend; i++) {
        redir_t *r = hpend[i].r;
        const char *delim = hpend[i].delim;
        strbuf_t body = {0};
        strbuf_t line = {0};
        for (;;) {
            line.len = 0;
            int c;
            bool got = false;
            while ((c = pgetc()) != PEOF) {
                got = true;
                if (c == '\n') {
                    /* A backslash-newline in an expanding heredoc joins
                     * lines before the delimiter is looked for. */
                    if (r->hexpand && line.len > 0 &&
                        line.s[line.len - 1] == '\\') {
                        size_t bs = 0;
                        while (bs < line.len &&
                               line.s[line.len - 1 - bs] == '\\')
                            bs++;
                        if (bs % 2 == 1) {
                            line.len--;
                            continue;
                        }
                    }
                    break;
                }
                if (r->hstrip && c == '\t' && line.len == 0)
                    continue;
                sb_putc(&line, (char)c);
            }
            sb_str(&line);
            if (strcmp(line.s, delim) == 0)
                break;
            if (c == PEOF) {
                if (got)
                    sb_putn(&body, line.s, line.len), sb_putc(&body, '\n');
                break;
            }
            sb_putn(&body, line.s, line.len);
            sb_putc(&body, '\n');
        }
        r->hbody = pa_strndup(body.s ? body.s : "", body.len);
        sb_free(&body);
        sb_free(&line);
    }
    nhpend = 0;
}

/* ── the token reader ──────────────────────────────────────────────── */

static redir_t *new_redir(int type, int fd)
{
    redir_t *r = pa_alloc(sizeof *r);
    r->type = (u8)type;
    r->fd = fd;
    return r;
}

/* An operator starting with < or > (the io number, if any, in fd). */
static int lex_redir(int c, int fd)
{
    int n = pgetc();
    int type;
    if (c == '<') {
        if (n == '<') {
            int m = pgetc();
            if (m == '-') type = R_HEREDOC;
            else if (m == '<') type = R_HERESTR;
            else { pungetc(m); type = R_HEREDOC; }
            tokredir = new_redir(type, fd < 0 ? 0 : fd);
            if (m == '-')
                tokredir->hstrip = 1;
            return T_REDIR;
        }
        if (n == '&') type = R_DUPIN;
        else if (n == '>') type = R_RDWR;
        else { pungetc(n); type = R_IN; }
        tokredir = new_redir(type, fd < 0 ? 0 : fd);
        return T_REDIR;
    }
    if (n == '>') type = R_APPEND;
    else if (n == '&') type = R_DUPOUT;
    else if (n == '|') type = R_CLOBBER;
    else { pungetc(n); type = R_OUT; }
    tokredir = new_redir(type, fd < 0 ? 1 : fd);
    return T_REDIR;
}

static int readtoken_raw(void)
{
    int c;
    for (;;) {
        c = pgetc();
        if (c == ' ' || c == '\t')
            continue;
        if (c == '#') {
            while ((c = pgetc()) != PEOF && c != '\n')
                ;
            pungetc(c);
            continue;
        }
        if (c == '\\') {
            int n = pgetc();
            if (n == '\n')
                continue;
            pungetc(n);
        }
        break;
    }
    tok_lineno = in ? in->lineno : 0;
    tokword = NULL;
    switch (c) {
    case PEOF:
        return T_EOF;
    case '\n':
        if (nhpend)
            read_heredocs();
        return T_NL;
    case ';': {
        int n = pgetc();
        if (n == ';') {
            int m = pgetc();
            if (m == '&') return T_DSEMIAND;
            pungetc(m);
            return T_DSEMI;
        }
        if (n == '&') return T_SEMIAND;
        pungetc(n);
        return T_SEMI;
    }
    case '&': {
        int n = pgetc();
        if (n == '&') return T_AND;
        if (n == '>') {
            int m = pgetc();
            if (m == '>') {
                tokredir = new_redir(R_APPENDERR, 1);
            } else {
                pungetc(m);
                tokredir = new_redir(R_OUTERR, 1);
            }
            return T_REDIR;
        }
        pungetc(n);
        return T_AMP;
    }
    case '|': {
        int n = pgetc();
        if (n == '|') return T_OR;
        if (n == '&') return T_PIPEAMP;
        pungetc(n);
        return T_PIPE;
    }
    case '(':
        if (checkkwd & CHKKWD) {
            int n = pgetc();
            if (n == '(') {
                /* (( expr )) - bash's arithmetic command */
                int ended = 0;
                tokword = lex_subword(LX_ARITH, true, 0, &ended);
                return tokword ? T_DPAREN : T_EOF;
            }
            pungetc(n);
        }
        return T_LPAREN;
    case ')':
        return T_RPAREN;
    case '<': case '>':
        return lex_redir(c, -1);
    }
    /* A word - or an io number: digits right before < or > */
    if (is_digit(c)) {
        strbuf_t d = {0};
        int x = c;
        while (is_digit(x)) {
            sb_putc(&d, (char)x);
            x = pgetc();
        }
        if ((x == '<' || x == '>') && d.len < 5) {
            int fd = atoi(d.s);
            sb_free(&d);
            return lex_redir(x, fd);
        }
        /* not one: give the characters back and read a word */
        pungetc(x);
        for (size_t i = d.len; i > 0; i--)
            pungetc((u8)d.s[i - 1]);
        sb_free(&d);
        c = pgetc();
    }
    tokword = lex_word(c);
    if (!tokword)
        return T_EOF;
    return T_WORD;
}

static int readtoken(void)
{
    int kwd = checkkwd;
    checkkwd = 0;
    if (tokpushback) {
        tokpushback = false;
        if (tok == T_WORD && tokword && tokword->lit && (kwd & CHKKWD)) {
            for (unsigned i = 0; i < sizeof keywords / sizeof *keywords; i++)
                if (strcmp(keywords[i].w, tokword->lit) == 0)
                    return tok = keywords[i].t;
        }
        return tok;
    }
    for (;;) {
        alias_blank_popped = false;
        int t = readtoken_raw();
        if (parse_err)
            return tok = T_EOF;
        if (t == T_NL && (kwd & CHKNL))
            continue;
        if (t == T_WORD && tokword->lit) {
            if (kwd & CHKKWD) {
                for (unsigned i = 0; i < sizeof keywords / sizeof *keywords; i++)
                    if (strcmp(keywords[i].w, tokword->lit) == 0)
                        return tok = keywords[i].t;
            }
            if ((kwd & CHKALIAS) || alias_blank_popped) {
                alias_t *a = alias_lookup(tokword->lit);
                if (a && !a->busy) {
                    input_push_string(a->val, strlen(a->val), a);
                    kwd |= CHKALIAS;
                    continue;
                }
            }
        }
        return tok = t;
    }
}

static int peektoken(void)
{
    int t = readtoken();
    tokpushback = true;
    return t;
}

/* ── the grammar ───────────────────────────────────────────────────── */

static node_t *compound_list(void);
static node_t *and_or(void);
static node_t *command(void);

static node_t *new_node(int type)
{
    node_t *n = pa_alloc(sizeof *n);
    n->type = (u8)type;
    n->lineno = tok_lineno;
    return n;
}

static node_t *binary(int type, node_t *a, node_t *b)
{
    node_t *n = new_node(type);
    n->a = a;
    n->b = b;
    n->lineno = a ? a->lineno : tok_lineno;
    return n;
}

static void skip_newlines(void)
{
    while (peektoken() == T_NL)
        readtoken();
}

static bool expect(int t)
{
    int got = readtoken();
    if (got != t) {
        unexpected(got);
        return false;
    }
    return true;
}

/* Tokens that end a compound list. */
static bool ends_list(int t)
{
    switch (t) {
    case T_EOF: case T_RPAREN: case T_DSEMI: case T_SEMIAND:
    case T_DSEMIAND: case T_THEN: case T_ELSE: case T_ELIF: case T_FI:
    case T_DO: case T_DONE: case T_ESAC: case T_RBRACE:
        return true;
    }
    return false;
}

/* A list inside a compound command: newlines separate as well as ;. */
static node_t *compound_list(void)
{
    node_t *list = NULL;
    for (;;) {
        checkkwd = CHKNL | CHKKWD | CHKALIAS;
        int t = peektoken();
        if (ends_list(t))
            break;
        node_t *n = and_or();
        if (parse_err)
            return NULL;
        checkkwd = 0;
        t = readtoken();
        if (t == T_AMP) {
            node_t *bg = new_node(N_BG);
            bg->a = n;
            bg->lineno = n->lineno;
            n = bg;
        } else if (t != T_SEMI && t != T_NL) {
            tokpushback = true;
        }
        list = list ? binary(N_SEQ, list, n) : n;
        if (t != T_SEMI && t != T_NL && t != T_AMP)
            break;
    }
    if (!list && !parse_err) {
        checkkwd = CHKKWD;
        unexpected(peektoken());
    }
    return list;
}

static node_t *pipeline(void)
{
    checkkwd = CHKKWD | CHKALIAS;
    int t = readtoken();
    bool timed = false, bang = false;
    if (t == T_TIME) {
        timed = true;
        checkkwd = CHKKWD | CHKALIAS;
        t = readtoken();
    }
    while (t == T_BANG) {
        bang = !bang;
        checkkwd = CHKKWD | CHKALIAS;
        t = readtoken();
    }
    tokpushback = true;
    node_t *first = command();
    if (parse_err)
        return NULL;
    node_t *n = first;
    if (peektoken() == T_PIPE || tok == T_PIPEAMP) {
        node_t *p = new_node(N_PIPE);
        p->lineno = first->lineno;
        p->a = first;
        node_t *last = first;
        while ((t = readtoken()) == T_PIPE || t == T_PIPEAMP) {
            if (t == T_PIPEAMP) {
                /* a |& b is a 2>&1 | b */
                redir_t *r = new_redir(R_DUPOUT, 2);
                wbuild_t wb;
                wb_init(&wb);
                wb_char(&wb, '1', 0);
                r->target = wb_finish(&wb);
                r->next = last->redirs;
                last->redirs = r;
            }
            skip_newlines();
            node_t *c = command();
            if (parse_err)
                return NULL;
            last->next = c;
            last = c;
        }
        tokpushback = true;
        n = p;
    }
    if (bang) {
        node_t *x = new_node(N_NOT);
        x->a = n;
        x->lineno = n->lineno;
        n = x;
    }
    if (timed) {
        node_t *x = new_node(N_TIME);
        x->a = n;
        x->lineno = n->lineno;
        n = x;
    }
    return n;
}

static node_t *and_or(void)
{
    node_t *n = pipeline();
    for (;;) {
        if (parse_err)
            return NULL;
        int t = readtoken();
        if (t != T_AND && t != T_OR) {
            tokpushback = true;
            return n;
        }
        skip_newlines();
        node_t *r = pipeline();
        if (parse_err)
            return NULL;
        n = binary(t == T_AND ? N_AND : N_OR, n, r);
    }
}

/* Read the word after a redirection operator into r. */
static bool redir_target(redir_t *r)
{
    strbuf_t raw = {0};
    bool heredoc = r->type == R_HEREDOC;
    if (heredoc)
        lex_raw = &raw;
    int t = readtoken();
    lex_raw = NULL;
    if (t != T_WORD) {
        sb_free(&raw);
        unexpected(t);
        return false;
    }
    r->target = tokword;
    if (heredoc) {
        /* strip the blanks the token reader skipped */
        char *s = sb_str(&raw);
        while (is_blank(*s))
            s++;
        bool quoted;
        char *delim = heredoc_delim(s, &quoted);
        r->hexpand = !quoted;
        if (nhpend == caphpend) {
            caphpend = caphpend ? caphpend * 2 : 8;
            hpend = xrealloc(hpend, (size_t)caphpend * sizeof *hpend);
        }
        hpend[nhpend].r = r;
        hpend[nhpend].delim = delim;
        nhpend++;
    }
    sb_free(&raw);
    return true;
}

/* Redirections after a compound command. */
static void trailing_redirs(node_t *n)
{
    redir_t **tail = &n->redirs;
    while (*tail)
        tail = &(*tail)->next;
    for (;;) {
        int t = readtoken();
        if (t != T_REDIR) {
            tokpushback = true;
            return;
        }
        redir_t *r = tokredir;
        if (!redir_target(r))
            return;
        *tail = r;
        tail = &r->next;
    }
}

/* Is w an assignment (name=... name+=... name[..]=...)? */
static bool word_is_assign(word_t *w)
{
    wpart_t *p = w->parts;
    if (!p || p->type != WP_LIT || p->quoted)
        return false;
    if (p->next && p->next->sub)
        return true;            /* name[sub]= built by lex_word */
    const char *s = p->text;
    if (!is_name_start((u8)*s))
        return false;
    size_t i = 1;
    while (i < p->len && is_name_char((u8)s[i]))
        i++;
    if (i < p->len && s[i] == '=')
        return true;
    return i + 1 < p->len && s[i] == '+' && s[i + 1] == '=';
}

static node_t *simple_command(void)
{
    node_t *n = new_node(N_CMD);
    word_t **wtail = &n->words, **atail = &n->assigns;
    redir_t **rtail = &n->redirs;
    bool have_cmd = false;
    for (;;) {
        checkkwd = have_cmd ? 0 : CHKALIAS;
        int t = readtoken();
        if (t == T_WORD) {
            word_t *w = tokword;
            if (!have_cmd && word_is_assign(w)) {
                *atail = w;
                atail = &w->next;
                continue;
            }
            if (!have_cmd) {
                have_cmd = true;
                /* name() - a function definition */
                if (!n->assigns && !n->redirs && peektoken() == T_LPAREN &&
                    w->lit) {
                    readtoken();
                    if (!expect(T_RPAREN))
                        return NULL;
                    skip_newlines();
                    node_t *f = new_node(N_FUNC);
                    f->name = w->lit;
                    f->lineno = n->lineno;
                    f->a = command();
                    if (parse_err)
                        return NULL;
                    f->arena = cur_arena;
                    return f;
                }
            }
            *wtail = w;
            wtail = &w->next;
            continue;
        }
        if (t == T_REDIR) {
            redir_t *r = tokredir;
            if (!redir_target(r))
                return NULL;
            *rtail = r;
            rtail = &r->next;
            continue;
        }
        tokpushback = true;
        break;
    }
    if (!n->words && !n->assigns && !n->redirs) {
        unexpected(tok);
        return NULL;
    }
    return n;
}

/* [[ ... ]] */
static dbx_t *db_or(void);

static int db_tok(void)
{
    checkkwd = CHKNL;
    return readtoken();
}

static bool db_is_end(int t)
{
    return t == T_WORD && tokword->lit && strcmp(tokword->lit, "]]") == 0;
}

static const char *const db_unary_ops[] = {
    "-a", "-b", "-c", "-d", "-e", "-f", "-g", "-h", "-k", "-p", "-r",
    "-s", "-t", "-u", "-w", "-x", "-O", "-G", "-L", "-N", "-S", "-z",
    "-n", "-o", "-v", "-R", NULL
};
static const char *const db_binary_ops[] = {
    "==", "=", "!=", "=~", "-eq", "-ne", "-lt", "-le", "-gt", "-ge",
    "-nt", "-ot", "-ef", NULL
};

static bool in_list(const char *const *l, const char *s)
{
    for (; *l; l++)
        if (strcmp(*l, s) == 0)
            return true;
    return false;
}

static dbx_t *db_new(int kind)
{
    dbx_t *x = pa_alloc(sizeof *x);
    x->kind = (u8)kind;
    return x;
}

static dbx_t *db_primary(void)
{
    int t = db_tok();
    if (t == T_LPAREN) {
        dbx_t *x = db_or();
        if (!x) return NULL;
        if (db_tok() != T_RPAREN) {
            unexpected(tok);
            return NULL;
        }
        return x;
    }
    if (t == T_WORD && tokword->lit && strcmp(tokword->lit, "!") == 0) {
        dbx_t *x = db_new(DB_NOT);
        x->l = db_primary();
        return x->l ? x : NULL;
    }
    if (t != T_WORD || db_is_end(t)) {
        unexpected(t);
        return NULL;
    }
    word_t *w1 = tokword;
    if (w1->lit && in_list(db_unary_ops, w1->lit)) {
        int t2 = peektoken();
        if (t2 == T_WORD && !db_is_end(t2)) {
            readtoken();
            dbx_t *x = db_new(DB_UNARY);
            strlcpy(x->op, w1->lit, sizeof x->op);
            x->w1 = tokword;
            return x;
        }
    }
    int t2 = db_tok();
    const char *op = NULL;
    if (t2 == T_WORD && tokword->lit && in_list(db_binary_ops, tokword->lit))
        op = tokword->lit;
    else if (t2 == T_REDIR && tokredir->type == R_IN && tokredir->fd == 0)
        op = "<";
    else if (t2 == T_REDIR && tokredir->type == R_OUT && tokredir->fd == 1)
        op = ">";
    if (!op) {
        tokpushback = true;
        dbx_t *x = db_new(DB_WORD);
        x->w1 = w1;
        return x;
    }
    dbx_t *x = db_new(DB_BINARY);
    strlcpy(x->op, op, sizeof x->op);
    x->w1 = w1;
    if (strcmp(op, "=~") == 0) {
        int c;
        while ((c = pgetc()) == ' ' || c == '\t')
            ;
        pungetc(c);
        x->w2 = lex_subword(LX_REGEX, false, 0, NULL);
        if (!x->w2)
            return NULL;
        return x;
    }
    int t3 = db_tok();
    if (t3 != T_WORD || db_is_end(t3)) {
        unexpected(t3);
        return NULL;
    }
    x->w2 = tokword;
    return x;
}

static dbx_t *db_and(void)
{
    dbx_t *l = db_primary();
    while (l) {
        int t = db_tok();
        if (t != T_AND) {
            tokpushback = true;
            break;
        }
        dbx_t *x = db_new(DB_AND);
        x->l = l;
        x->r = db_primary();
        if (!x->r) return NULL;
        l = x;
    }
    return l;
}

static dbx_t *db_or(void)
{
    dbx_t *l = db_and();
    while (l) {
        int t = db_tok();
        if (t != T_OR) {
            tokpushback = true;
            break;
        }
        dbx_t *x = db_new(DB_OR);
        x->l = l;
        x->r = db_and();
        if (!x->r) return NULL;
        l = x;
    }
    return l;
}

static node_t *command(void)
{
    checkkwd = CHKKWD | CHKALIAS;
    int t = readtoken();
    node_t *n;
    switch (t) {
    case T_IF: {
        n = new_node(N_IF);
        node_t *cur = n;
        cur->a = compound_list();
        if (parse_err || !expect(T_THEN)) return NULL;
        cur->b = compound_list();
        if (parse_err) return NULL;
        for (;;) {
            checkkwd = CHKKWD;
            t = readtoken();
            if (t == T_ELIF) {
                node_t *e = new_node(N_IF);
                cur->c = e;
                cur = e;
                cur->a = compound_list();
                if (parse_err || !expect(T_THEN)) return NULL;
                cur->b = compound_list();
                if (parse_err) return NULL;
                continue;
            }
            if (t == T_ELSE) {
                cur->c = compound_list();
                if (parse_err) return NULL;
                checkkwd = CHKKWD;
                t = readtoken();
            }
            if (t != T_FI) {
                unexpected(t);
                return NULL;
            }
            break;
        }
        break;
    }
    case T_WHILE: case T_UNTIL:
        n = new_node(t == T_WHILE ? N_WHILE : N_UNTIL);
        n->a = compound_list();
        if (parse_err || !expect(T_DO)) return NULL;
        n->b = compound_list();
        if (parse_err) return NULL;
        checkkwd = CHKKWD;
        if (!expect(T_DONE)) return NULL;
        break;
    case T_FOR: {
        checkkwd = CHKKWD;
        t = readtoken();
        if (t == T_DPAREN) {
            /* for ((init; cond; step)) */
            n = new_node(N_FORARITH);
            n->words = tokword;
            checkkwd = CHKNL | CHKKWD;
            t = readtoken();
            if (t == T_SEMI) {
                checkkwd = CHKNL | CHKKWD;
                t = readtoken();
            }
            if (t != T_DO && t != T_LBRACE) {
                unexpected(t);
                return NULL;
            }
            n->b = compound_list();
            if (parse_err) return NULL;
            checkkwd = CHKKWD;
            if (!expect(t == T_DO ? T_DONE : T_RBRACE)) return NULL;
            break;
        }
        if (t != T_WORD || !tokword->lit || !is_valid_name(tokword->lit)) {
            syntax_error("bad for loop variable", NULL);
            return NULL;
        }
        n = new_node(N_FOR);
        n->name = tokword->lit;
        checkkwd = CHKNL | CHKKWD;
        t = readtoken();
        if (t == T_IN) {
            n->flags = 1;       /* has a word list, even if empty */
            word_t **tail = &n->words;
            for (;;) {
                t = readtoken();
                if (t != T_WORD)
                    break;
                *tail = tokword;
                tail = &tokword->next;
            }
            if (t != T_NL && t != T_SEMI) {
                unexpected(t);
                return NULL;
            }
            checkkwd = CHKNL | CHKKWD;
            t = readtoken();
        } else if (t == T_SEMI) {
            checkkwd = CHKNL | CHKKWD;
            t = readtoken();
        }
        if (t != T_DO) {
            unexpected(t);
            return NULL;
        }
        n->b = compound_list();
        if (parse_err) return NULL;
        checkkwd = CHKKWD;
        if (!expect(T_DONE)) return NULL;
        break;
    }
    case T_CASE: {
        n = new_node(N_CASE);
        t = readtoken();
        if (t != T_WORD) {
            unexpected(t);
            return NULL;
        }
        n->words = tokword;
        checkkwd = CHKNL | CHKKWD;
        if (!expect(T_IN))
            return NULL;
        caseitem_t **tail = &n->items;
        for (;;) {
            checkkwd = CHKNL | CHKKWD;
            t = readtoken();
            if (t == T_ESAC)
                break;
            if (t == T_LPAREN)
                t = readtoken();
            caseitem_t *ci = pa_alloc(sizeof *ci);
            word_t **ptail = &ci->pats;
            for (;;) {
                /* keywords are just words in a pattern */
                if (t != T_WORD && !(t >= T_IF && tokword)) {
                    unexpected(t);
                    return NULL;
                }
                *ptail = tokword;
                ptail = &tokword->next;
                t = readtoken();
                if (t != T_PIPE)
                    break;
                t = readtoken();
            }
            if (t != T_RPAREN) {
                unexpected(t);
                return NULL;
            }
            checkkwd = CHKNL | CHKKWD | CHKALIAS;
            t = peektoken();
            if (t != T_DSEMI && t != T_SEMIAND && t != T_DSEMIAND &&
                t != T_ESAC) {
                ci->body = compound_list();
                if (parse_err) return NULL;
            }
            *tail = ci;
            tail = &ci->next;
            checkkwd = CHKNL | CHKKWD;
            t = readtoken();
            if (t == T_ESAC)
                break;
            if (t == T_DSEMI) ci->term = CT_BREAK;
            else if (t == T_SEMIAND) ci->term = CT_FALL;
            else if (t == T_DSEMIAND) ci->term = CT_TEST;
            else {
                unexpected(t);
                return NULL;
            }
        }
        break;
    }
    case T_LBRACE:
        n = new_node(N_GROUP);
        n->a = compound_list();
        if (parse_err) return NULL;
        checkkwd = CHKKWD;
        if (!expect(T_RBRACE)) return NULL;
        break;
    case T_LPAREN:
        n = new_node(N_SUBSHELL);
        n->a = compound_list();
        if (parse_err) return NULL;
        if (!expect(T_RPAREN)) return NULL;
        break;
    case T_DPAREN:
        n = new_node(N_ARITH);
        n->words = tokword;
        break;
    case T_DLBRACK:
        n = new_node(N_DBRACK);
        n->dbx = db_or();
        if (!n->dbx || parse_err) return NULL;
        if (!db_is_end(db_tok())) {
            unexpected(tok);
            return NULL;
        }
        break;
    case T_FUNCTION: {
        t = readtoken();
        if (t != T_WORD || !tokword->lit) {
            unexpected(t);
            return NULL;
        }
        n = new_node(N_FUNC);
        n->name = tokword->lit;
        if (peektoken() == T_LPAREN) {
            readtoken();
            if (!expect(T_RPAREN)) return NULL;
        }
        skip_newlines();
        n->a = command();
        if (parse_err) return NULL;
        n->arena = cur_arena;
        return n;
    }
    case T_WORD: case T_REDIR:
        tokpushback = true;
        return simple_command();
    default:
        unexpected(t);
        return NULL;
    }
    trailing_redirs(n);
    return parse_err ? NULL : n;
}

/* ── entry points ──────────────────────────────────────────────────── */

typedef struct {
    int      tok;
    word_t  *tokword;
    redir_t *tokredir;
    bool     tokpushback;
    int      checkkwd;
    hpending_t *hpend;
    int      nhpend, caphpend;
    strbuf_t *lex_raw;
} pstate_t;

static void pstate_save(pstate_t *s)
{
    s->tok = tok;
    s->tokword = tokword;
    s->tokredir = tokredir;
    s->tokpushback = tokpushback;
    s->checkkwd = checkkwd;
    s->hpend = hpend;
    s->nhpend = nhpend;
    s->caphpend = caphpend;
    s->lex_raw = lex_raw;
    tokpushback = false;
    hpend = NULL;
    nhpend = caphpend = 0;
    lex_raw = NULL;
}

static void pstate_restore(pstate_t *s)
{
    /* Heredocs opened inside $(...) whose bodies have not been read yet
     * (the closing paren came first) belong to the outer line now. */
    hpending_t *inner = hpend;
    int ninner = nhpend;
    tok = s->tok;
    tokword = s->tokword;
    tokredir = s->tokredir;
    tokpushback = s->tokpushback;
    checkkwd = s->checkkwd;
    hpend = s->hpend;
    nhpend = s->nhpend;
    caphpend = s->caphpend;
    lex_raw = s->lex_raw;
    for (int i = 0; i < ninner; i++) {
        if (nhpend == caphpend) {
            caphpend = caphpend ? caphpend * 2 : 8;
            hpend = xrealloc(hpend, (size_t)caphpend * sizeof *hpend);
        }
        hpend[nhpend++] = inner[i];
    }
    xfree(inner);
}

/* $( ... ): the command is parsed right here, from the same input, up to
 * the matching parenthesis. */
static node_t *parse_cmdsub_paren(void)
{
    pstate_t st;
    pstate_save(&st);
    node_t *n = NULL;
    checkkwd = CHKNL | CHKKWD | CHKALIAS;
    if (peektoken() == T_RPAREN) {
        readtoken();            /* $( ) is allowed and empty */
    } else {
        n = compound_list();
        if (!parse_err && readtoken() != T_RPAREN)
            unexpected(tok);
    }
    pstate_restore(&st);
    return n;
}

static node_t *parse_cmdsub_string(const char *s, size_t len)
{
    pstate_t st;
    pstate_save(&st);
    input_push_string(s, len, NULL);
    insrc_t *mine = in;
    node_t *list = NULL;
    for (;;) {
        checkkwd = CHKNL | CHKKWD | CHKALIAS;
        if (peektoken() == T_EOF)
            break;
        node_t *n = compound_list();
        if (parse_err)
            break;
        if (peektoken() != T_EOF) {
            unexpected(readtoken());
            break;
        }
        list = list ? binary(N_SEQ, list, n) : n;
    }
    while (in && in != mine)
        input_pop();
    input_pop();
    pstate_restore(&st);
    return list;
}

/* Parse text as the body of a heredoc (or a prompt): only $, ` and \
 * mean anything. Uses the current arena. */
static word_t *parse_string_word(const char *s, bool heredoc_mode, bool *error)
{
    pstate_t st;
    pstate_save(&st);
    bool save_err = parse_err;
    parse_err = false;
    input_push_string(s, strlen(s), NULL);
    insrc_t *mine = in;
    word_t *w = lex_subword(heredoc_mode ? LX_HEREDOC : LX_WORD, false, 0,
                            NULL);
    while (in && in != mine)
        input_pop();
    input_pop();
    *error = parse_err;
    parse_err = save_err;
    pstate_restore(&st);
    return w;
}

static void parse_reset(void)
{
    tokpushback = false;
    checkkwd = 0;
    nhpend = 0;
    parse_err = false;
    lex_raw = NULL;
}

/* Read and parse one complete command: everything up to a newline that
 * leaves no construct open. NULL for an empty line, PARSE_EOF at the end
 * of input. On a syntax error *error is set and NULL returned. */
static node_t *parse_command(bool *error)
{
    parse_reset();
    input_interrupted = false;
    ps1_next = true;
    *error = false;
    checkkwd = CHKKWD | CHKALIAS;
    int t = peektoken();
    if (input_interrupted) {
        tokpushback = false;
        return NULL;
    }
    if (t == T_EOF && !parse_err) {
        tokpushback = false;
        return PARSE_EOF;
    }
    if (t == T_NL) {
        readtoken();
        return NULL;
    }
    node_t *list = NULL;
    for (;;) {
        node_t *n = and_or();
        if (parse_err || input_interrupted)
            break;
        t = readtoken();
        if (t == T_AMP) {
            node_t *bg = new_node(N_BG);
            bg->a = n;
            bg->lineno = n->lineno;
            n = bg;
        }
        list = list ? binary(N_SEQ, list, n) : n;
        if (t == T_NL || t == T_EOF) {
            if (t == T_EOF)
                tokpushback = false;
            break;
        }
        if (t != T_SEMI && t != T_AMP) {
            unexpected(t);
            break;
        }
        checkkwd = CHKKWD | CHKALIAS;
        t = peektoken();
        if (t == T_NL || t == T_EOF) {
            readtoken();
            break;
        }
    }
    if (input_interrupted) {
        parse_reset();
        input_discard_line();
        return NULL;
    }
    if (parse_err) {
        *error = true;
        parse_reset();
        input_discard_line();
        return NULL;
    }
    return list;
}

/* ── printing a tree back as shell text ────────────────────────────── *
 *
 * For `type f`, `jobs` and `set -x` of a function: the tree turned back
 * into text a person can read, in the same layout bash uses. */

static void fmt_parts(strbuf_t *b, wpart_t *p, bool in_dq);

static void fmt_word(strbuf_t *b, word_t *w)
{
    fmt_parts(b, w->parts, false);
}

static void fmt_lit(strbuf_t *b, const char *s, size_t n, bool quoted,
                    bool in_dq)
{
    if (!quoted || in_dq) {
        for (size_t i = 0; i < n; i++) {
            if (in_dq && strchr("\"\\$`", s[i]))
                sb_putc(b, '\\');
            sb_putc(b, s[i]);
        }
        return;
    }
    sb_putc(b, '\'');
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\'')
            sb_puts(b, "'\\''");
        else
            sb_putc(b, s[i]);
    }
    sb_putc(b, '\'');
}

static void fmt_parts(strbuf_t *b, wpart_t *p, bool in_dq)
{
    bool open_dq = false;
    for (; p; p = p->next) {
        bool want_dq = p->quoted && p->type != WP_LIT && !in_dq;
        if (p->type == WP_LIT && p->quoted && !in_dq) {
            if (open_dq) { sb_putc(b, '"'); open_dq = false; }
            if (p->sub) {
                fmt_word(b, p->sub);
                continue;
            }
            fmt_lit(b, p->text, p->len, true, false);
            continue;
        }
        if (want_dq && !open_dq) { sb_putc(b, '"'); open_dq = true; }
        if (!want_dq && open_dq) { sb_putc(b, '"'); open_dq = false; }
        bool dq = in_dq || open_dq;
        switch (p->type) {
        case WP_LIT:
            if (p->sub) {
                fmt_word(b, p->sub);
                break;
            }
            fmt_lit(b, p->text, p->len, false, dq);
            break;
        case WP_PARAM: {
            if (!(p->flags & PF_BRACED)) {
                sb_putc(b, '$');
                sb_puts(b, p->text);
                break;
            }
            sb_puts(b, "${");
            if (p->flags & PF_LEN) sb_putc(b, '#');
            if (p->flags & (PF_INDIRECT | PF_KEYS)) sb_putc(b, '!');
            sb_puts(b, p->text);
            if (p->sub) {
                sb_putc(b, '[');
                fmt_word(b, p->sub);
                sb_putc(b, ']');
            }
            static const char *const ops[] = {
                "", "-", "=", "?", "+", "%", "%%", "#", "##", "/", "//",
                "/#", "/%", ":", "^", "^^", ",", ",,"
            };
            if (p->flags & PF_COLON) sb_putc(b, ':');
            sb_puts(b, ops[p->op]);
            if (p->arg) fmt_word(b, p->arg);
            if (p->arg2) {
                sb_putc(b, p->op == PO_SUBSTR ? ':' : '/');
                fmt_word(b, p->arg2);
            }
            sb_putc(b, '}');
            break;
        }
        case WP_CMDSUB:
            sb_puts(b, "$(");
            if (p->tree) fmt_node(b, p->tree, -1);
            sb_putc(b, ')');
            break;
        case WP_ARITH:
            sb_puts(b, "$((");
            fmt_parts(b, p->arg->parts, true);
            sb_puts(b, "))");
            break;
        case WP_ARRAY:
            sb_putc(b, '(');
            for (word_t *w = p->list; w; w = w->next) {
                fmt_word(b, w);
                if (w->next) sb_putc(b, ' ');
            }
            sb_putc(b, ')');
            break;
        }
    }
    if (open_dq)
        sb_putc(b, '"');
}

static void fmt_indent(strbuf_t *b, int indent)
{
    if (indent < 0) {
        sb_putc(b, ' ');
        return;
    }
    sb_putc(b, '\n');
    for (int i = 0; i < indent; i++)
        sb_puts(b, "    ");
}

static void fmt_redirs(strbuf_t *b, redir_t *r)
{
    static const char *const ops[] = {
        "<", ">", ">>", ">|", "<>", "<&", ">&", "<<", "<<<", "&>", "&>>"
    };
    for (; r; r = r->next) {
        sb_putc(b, ' ');
        int deffd = (r->type == R_IN || r->type == R_RDWR ||
                     r->type == R_DUPIN || r->type == R_HEREDOC ||
                     r->type == R_HERESTR) ? 0 : 1;
        if (r->fd != deffd && r->type != R_OUTERR && r->type != R_APPENDERR)
            sb_printf(b, "%d", r->fd);
        sb_puts(b, ops[r->type]);
        if (r->type == R_HEREDOC && r->hstrip)
            sb_putc(b, '-');
        if (r->target)
            fmt_word(b, r->target);
    }
}

static void fmt_list(strbuf_t *b, node_t *n, int indent)
{
    if (!n)
        return;
    if (n->type == N_SEQ) {
        fmt_list(b, n->a, indent);
        if (indent < 0) sb_puts(b, ";");
        fmt_indent(b, indent);
        fmt_list(b, n->b, indent);
        return;
    }
    fmt_node(b, n, indent);
}

static void fmt_node(strbuf_t *b, node_t *n, int indent)
{
    if (!n)
        return;
    int in2 = indent < 0 ? -1 : indent + 1;
    switch (n->type) {
    case N_CMD: {
        bool first = true;
        for (word_t *w = n->assigns; w; w = w->next) {
            if (!first) sb_putc(b, ' ');
            fmt_word(b, w);
            first = false;
        }
        for (word_t *w = n->words; w; w = w->next) {
            if (!first) sb_putc(b, ' ');
            fmt_word(b, w);
            first = false;
        }
        fmt_redirs(b, n->redirs);
        return;
    }
    case N_PIPE:
        for (node_t *c = n->a; c; c = c->next) {
            fmt_node(b, c, indent);
            if (c->next) sb_puts(b, " | ");
        }
        return;
    case N_AND: case N_OR:
        fmt_node(b, n->a, indent);
        sb_puts(b, n->type == N_AND ? " && " : " || ");
        fmt_node(b, n->b, indent);
        return;
    case N_SEQ:
        fmt_list(b, n, indent);
        return;
    case N_BG:
        fmt_node(b, n->a, indent);
        sb_puts(b, " &");
        return;
    case N_NOT:
        sb_puts(b, "! ");
        fmt_node(b, n->a, indent);
        return;
    case N_TIME:
        sb_puts(b, "time ");
        fmt_node(b, n->a, indent);
        return;
    case N_SUBSHELL:
        sb_puts(b, "( ");
        fmt_list(b, n->a, indent < 0 ? -1 : indent);
        sb_puts(b, " )");
        break;
    case N_GROUP:
        sb_puts(b, "{");
        fmt_indent(b, in2);
        fmt_list(b, n->a, in2);
        if (indent < 0) sb_puts(b, ";");
        fmt_indent(b, indent);
        sb_puts(b, "}");
        break;
    case N_IF: {
        sb_puts(b, "if ");
        node_t *c = n;
        for (;;) {
            fmt_list(b, c->a, -1);
            sb_puts(b, "; then");
            fmt_indent(b, in2);
            fmt_list(b, c->b, in2);
            if (indent < 0) sb_puts(b, ";");
            if (c->c && c->c->type == N_IF && !c->c->redirs) {
                fmt_indent(b, indent);
                sb_puts(b, "elif ");
                c = c->c;
                continue;
            }
            if (c->c) {
                fmt_indent(b, indent);
                sb_puts(b, "else");
                fmt_indent(b, in2);
                fmt_list(b, c->c, in2);
                if (indent < 0) sb_puts(b, ";");
            }
            break;
        }
        fmt_indent(b, indent);
        sb_puts(b, "fi");
        break;
    }
    case N_WHILE: case N_UNTIL:
        sb_puts(b, n->type == N_WHILE ? "while " : "until ");
        fmt_list(b, n->a, -1);
        sb_puts(b, "; do");
        fmt_indent(b, in2);
        fmt_list(b, n->b, in2);
        if (indent < 0) sb_puts(b, ";");
        fmt_indent(b, indent);
        sb_puts(b, "done");
        break;
    case N_FOR:
        sb_printf(b, "for %s", n->name);
        if (n->flags) {
            sb_puts(b, " in");
            for (word_t *w = n->words; w; w = w->next) {
                sb_putc(b, ' ');
                fmt_word(b, w);
            }
        }
        sb_puts(b, "; do");
        fmt_indent(b, in2);
        fmt_list(b, n->b, in2);
        if (indent < 0) sb_puts(b, ";");
        fmt_indent(b, indent);
        sb_puts(b, "done");
        break;
    case N_FORARITH:
        sb_puts(b, "for ((");
        fmt_parts(b, n->words->parts, true);
        sb_puts(b, ")); do");
        fmt_indent(b, in2);
        fmt_list(b, n->b, in2);
        if (indent < 0) sb_puts(b, ";");
        fmt_indent(b, indent);
        sb_puts(b, "done");
        break;
    case N_CASE:
        sb_puts(b, "case ");
        fmt_word(b, n->words);
        sb_puts(b, " in");
        for (caseitem_t *ci = n->items; ci; ci = ci->next) {
            fmt_indent(b, in2);
            for (word_t *w = ci->pats; w; w = w->next) {
                fmt_word(b, w);
                if (w->next) sb_puts(b, " | ");
            }
            sb_puts(b, ")");
            if (ci->body) {
                fmt_indent(b, indent < 0 ? -1 : in2 + 1);
                fmt_list(b, ci->body, indent < 0 ? -1 : in2 + 1);
            }
            fmt_indent(b, indent < 0 ? -1 : in2 + 1);
            sb_puts(b, ci->term == CT_FALL ? ";&"
                     : ci->term == CT_TEST ? ";;&" : ";;");
        }
        fmt_indent(b, indent);
        sb_puts(b, "esac");
        break;
    case N_FUNC:
        sb_printf(b, "%s () ", n->name);
        if (indent >= 0) fmt_indent(b, indent);
        fmt_node(b, n->a, indent);
        return;
    case N_ARITH:
        sb_puts(b, "((");
        fmt_parts(b, n->words->parts, true);
        sb_puts(b, "))");
        break;
    case N_DBRACK:
        sb_puts(b, "[[ ... ]]");
        break;
    }
    fmt_redirs(b, n->redirs);
}
