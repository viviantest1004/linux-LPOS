/* awk - scan the input a line at a time, match patterns, run actions.
 *
 *   awk [-F fs] [-v var=value]... 'program' [file...]
 *   awk [-F fs] [-v var=value]... -f progfile... [file...]
 *
 *   -F fs        what separates the fields; one character is literal,
 *                more than one is a regular expression, " " is the
 *                default (runs of blanks, leading ones ignored)
 *   -v var=value set a variable before BEGIN runs
 *   -f file      read the program from a file instead of the argument
 *
 *   awk '{ print $2 }' /etc/hosts
 *   awk -F: '$3 >= 1000 { print $1, $3 }' /etc/passwd
 *   ps | awk 'NR > 1 { n[$4]++ } END { for (c in n) print n[c], c }'
 *   awk '/start/,/end/' log
 *
 * A program is a list of `pattern { action }` rules. Either half can be
 * left out: a pattern on its own prints the lines that match, an action
 * on its own runs on every line. BEGIN runs before the first line and
 * END after the last.
 *
 * Patterns:   BEGIN  END  /regex/  an expression  expr,expr  ! && ||
 * Statements: print printf if else while for for(k in a) break continue
 *             next exit delete { }
 * Operators:  + - * / % ^  ++ --  = += -= *= /= %=  == != < <= > >=
 *             ~ !~  ?:  in  and concatenation by writing two things
 *             next to each other
 * Fields:     $0 $1 $NF $(i); assigning to one rebuilds $0 with OFS
 * Variables:  NR NF FS OFS ORS RS FILENAME RSTART RLENGTH, your own,
 *             and one-dimensional arrays
 * Functions:  length substr index split sub gsub match sprintf tolower
 *             toupper int system
 *
 * ── the decisions ────────────────────────────────────────────────────
 *
 * EVERY NUMBER HERE IS A WHOLE NUMBER. This system has no floating
 * point anywhere - not in the kernel, not in the libc, not in printf -
 * so `7 / 2` is 3 and `x /= 3` truncates. An awk that quietly rounded
 * would be worse than one that says what it does: a decimal in the
 * program text (`x > 3.5`) is refused at parse time rather than read as
 * 3, and sin cos exp log sqrt atan2 rand srand are refused by name.
 * A field like "3.5" read from a file converts to 3 in arithmetic and
 * compares as a string, because pretending otherwise would sort 3.5
 * next to 3 and 10.2 next to 10.
 *
 * The matching is the shared engine in regex.h, in its extended form -
 * awk has always used ERE, so `(a|b)+` means that without backslashes.
 * A regular expression built at run time (`$0 ~ v`) is compiled on
 * first use and kept in a small cache, so a match inside a loop does
 * not recompile the pattern on every line.
 *
 * There are no user-defined functions. They need a call stack, local
 * parameters and recursion, which is a third of this file again, and
 * nothing that gets written on a command line uses them. A program with
 * `function` in it is refused by name rather than half understood.
 * There is no getline either, for the same reason: it brings a second
 * input stream, pipes and its own return protocol with it.
 *
 * `print > "file"` and `>> "file"` do work, because without them the
 * `>` would silently be read as a comparison and the program would
 * print 0 and 1 instead of what it was told to. `print | "command"` is
 * refused with a message rather than mis-parsed.
 *
 * The record is a fixed 8191 bytes and there are at most 256 fields.
 * The whole system unpacks into RAM, so a buffer that grows to fit the
 * worst line anyone might have is memory taken from every board
 * forever. A longer line is cut short, but it says so on stderr and the
 * exit status is 1 - a silently shortened line is how a config file
 * gets destroyed.
 *
 * Values are a tagged union of a long and a string, with awk's usual
 * comparison rule: numeric if both sides are numbers or came from the
 * input looking like numbers, string otherwise. `$1 == "10"` is
 * therefore true for the field ` 10 ` and false for the string "10 ".
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "regex.h"

#define RECMAX    8192       /* one record, and the split copy of it */
#define MAXF       256       /* fields per record */
#define NAMEMAX     48
#define MAXSYM     192
#define MAXRULES    64
#define RECACHE      8       /* compiled run-time regexes kept around */
#define MAXREDIR     8       /* files open for print > "..." */
#define OUTBUF    4096

/* ── values ──────────────────────────────────────────────────────────
 *
 * V_STRNUM is the one that matters: a field or a -v value that looks
 * like a number keeps its text but compares numerically, which is what
 * makes `$1 == 10` and `$1 == "10"` both work on a field of "10". */
enum { V_UNINIT = 0, V_NUM, V_STR, V_STRNUM };

typedef struct {
    int   kind;
    long  num;
    char *str;      /* owned; for V_NUM it is a cache of the text */
} Val;

static void die(const char *msg) __attribute__((noreturn));

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) die("out of memory - the program or the input is too big");
    return p;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* Does this text look like a whole number? Leading and trailing blanks
 * are allowed, a decimal point is not - see the note at the top. */
static bool looks_num(const char *s)
{
    int i = 0, digits = 0;

    while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n') i++;
    if (s[i] == '+' || s[i] == '-') i++;
    while (s[i] >= '0' && s[i] <= '9') { i++; digits++; }
    while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n') i++;
    return digits > 0 && s[i] == '\0';
}

static Val v_num(long n)          { Val v = { V_NUM, n, NULL }; return v; }
static Val v_uninit(void)         { Val v = { V_UNINIT, 0, NULL }; return v; }
static Val v_take(char *s)        { Val v = { V_STR, 0, s }; return v; }
static Val v_str(const char *s)   { return v_take(xstrdup(s)); }

/* A string that came from outside the program: input, a field, -v. */
static Val v_input(const char *s)
{
    if (s[0] == '\0') return v_uninit();
    Val v = v_str(s);
    if (looks_num(s)) v.kind = V_STRNUM;
    return v;
}

static void v_free(Val *v)
{
    if (v->str) free(v->str);
    v->str = NULL;
    v->kind = V_UNINIT;
    v->num = 0;
}

static Val v_copy(const Val *v)
{
    Val r = *v;
    if (v->str) r.str = xstrdup(v->str);
    return r;
}

static long str2num(const char *s)
{
    char *end;
    return strtol(s, &end, 10);
}

static long v_getnum(const Val *v)
{
    if (v->kind == V_NUM) return v->num;
    return v->str ? str2num(v->str) : 0;
}

static const char *v_getstr(Val *v)
{
    if (v->kind == V_NUM) {
        if (!v->str) {
            char b[24];
            snprintf(b, sizeof b, "%ld", v->num);
            v->str = xstrdup(b);
        }
        return v->str;
    }
    return v->str ? v->str : "";
}

/* True when this value takes part in a numeric comparison. */
static bool v_isnum(const Val *v)
{
    return v->kind == V_NUM || v->kind == V_STRNUM || v->kind == V_UNINIT;
}

static bool v_true(Val *v)
{
    if (v->kind == V_NUM)    return v->num != 0;
    if (v->kind == V_UNINIT) return false;
    if (v->kind == V_STRNUM) return str2num(v->str) != 0;
    return v->str[0] != '\0';
}

/* ── a string that grows ─────────────────────────────────────────────
 * Used by concatenation, gsub and printf, which are the only three
 * places where the length of the answer is not known in advance. */
typedef struct { char *p; int len, cap; } SB;

static void sb_init(SB *b) { b->p = NULL; b->len = 0; b->cap = 0; }

static void sb_add(SB *b, const char *s, int n)
{
    if (n <= 0) return;
    if (b->len + n + 1 > b->cap) {
        int want = b->cap ? b->cap * 2 : 64;
        while (want < b->len + n + 1) want *= 2;
        char *np = realloc(b->p, (size_t)want);
        if (!np) die("out of memory building a string");
        b->p = np;
        b->cap = want;
    }
    memcpy(b->p + b->len, s, (size_t)n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_str(SB *b, const char *s) { sb_add(b, s, (int)strlen(s)); }
static void sb_ch(SB *b, char c)         { sb_add(b, &c, 1); }

/* Hand the buffer to the caller, who now owns it. */
static char *sb_take(SB *b)
{
    if (!b->p) return xstrdup("");
    char *p = b->p;
    sb_init(b);
    return p;
}

/* ── output ──────────────────────────────────────────────────────────
 * stdout is buffered: `awk '{print $1}'` over a long file otherwise
 * makes one write syscall per line. */
static char obuf[OUTBUF];
static int  olen;
static int  exit_status;

static void oflush(void)
{
    int off = 0;
    while (off < olen) {
        long w = lp_write(STDOUT_FILENO, obuf + off, (size_t)(olen - off));
        if (w <= 0) { olen = 0; return; }
        off += (int)w;
    }
    olen = 0;
}

static void owrite(const char *s, int n)
{
    while (n > 0) {
        int room = OUTBUF - olen;
        if (room == 0) { oflush(); room = OUTBUF; }
        int take = n < room ? n : room;
        memcpy(obuf + olen, s, (size_t)take);
        olen += take;
        s += take;
        n -= take;
    }
}

static void die(const char *msg)
{
    oflush();
    dprintf(STDERR_FILENO, "awk: %s\n", msg);
    lp_exit(2);
}

/* Files opened by print > "name". Kept open until awk finishes; there
 * are at most MAXREDIR of them, which is more than any one-liner uses. */
static struct { char *name; int fd; } redir[MAXREDIR];
static int nredir;

static int redir_fd(const char *name, bool append)
{
    if (strcmp(name, "/dev/stdout") == 0) return STDOUT_FILENO;
    if (strcmp(name, "/dev/stderr") == 0) return STDERR_FILENO;

    for (int i = 0; i < nredir; i++)
        if (strcmp(redir[i].name, name) == 0) return redir[i].fd;

    if (nredir == MAXREDIR) {
        dprintf(STDERR_FILENO,
                "awk: more than %d files open for output; %s was not "
                "written\n", MAXREDIR, name);
        exit_status = 1;
        return -1;
    }
    long fd = lp_open(name, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC),
                      0644);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "awk: %s: cannot write it - check the directory exists\n",
                name);
        exit_status = 1;
        return -1;
    }
    redir[nredir].name = xstrdup(name);
    redir[nredir].fd = (int)fd;
    nredir++;
    return (int)fd;
}

static void redir_close_all(void)
{
    for (int i = 0; i < nredir; i++) lp_close(redir[i].fd);
    nredir = 0;
}

/* Write to wherever this print was aimed. */
static void emit(int fd, const char *s, int n)
{
    if (fd == STDOUT_FILENO) { owrite(s, n); return; }
    if (fd < 0) return;
    int off = 0;
    while (off < n) {
        long w = lp_write(fd, s + off, (size_t)(n - off));
        if (w <= 0) return;
        off += (int)w;
    }
}

/* ── symbols and arrays ──────────────────────────────────────────── */

typedef struct AElem {
    struct AElem *next;
    Val  v;
    char key[];
} AElem;

#define ABUCKETS 64
typedef struct { AElem *b[ABUCKETS]; long count; } Arr;

typedef struct {
    char  name[NAMEMAX];
    Val   v;
    Arr  *arr;      /* set the first time it is used with a subscript */
} Sym;

static Sym sym[MAXSYM];
static int nsym;

/* The built-in variables come first so their index is a constant. */
enum { S_NR, S_NF, S_FS, S_OFS, S_ORS, S_RS, S_FILENAME,
       S_RSTART, S_RLENGTH, S_NFIXED };

static int sym_find(const char *name)
{
    for (int i = 0; i < nsym; i++)
        if (strcmp(sym[i].name, name) == 0) return i;
    return -1;
}

static int sym_intern(const char *name)
{
    int i = sym_find(name);
    if (i >= 0) return i;
    if (nsym == MAXSYM)
        die("this program uses more than 192 different names");
    strlcpy(sym[nsym].name, name, NAMEMAX);
    sym[nsym].v = v_uninit();
    sym[nsym].arr = NULL;
    return nsym++;
}

static unsigned hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

static Arr *arr_of(int s)
{
    if (!sym[s].arr) {
        if (sym[s].v.kind != V_UNINIT) {
            dprintf(STDERR_FILENO,
                    "awk: %s is used both as a variable and as an array\n",
                    sym[s].name);
            oflush();
            lp_exit(2);
        }
        sym[s].arr = xmalloc(sizeof(Arr));
        memset(sym[s].arr, 0, sizeof(Arr));
    }
    return sym[s].arr;
}

static AElem *arr_get(Arr *a, const char *key, bool create)
{
    unsigned h = hash(key) % ABUCKETS;
    for (AElem *e = a->b[h]; e; e = e->next)
        if (strcmp(e->key, key) == 0) return e;
    if (!create) return NULL;

    size_t n = strlen(key) + 1;
    AElem *e = xmalloc(sizeof(AElem) + n);
    memcpy(e->key, key, n);
    e->v = v_uninit();
    e->next = a->b[h];
    a->b[h] = e;
    a->count++;
    return e;
}

static void arr_del(Arr *a, const char *key)
{
    unsigned h = hash(key) % ABUCKETS;
    AElem **pp = &a->b[h];
    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            AElem *e = *pp;
            *pp = e->next;
            v_free(&e->v);
            free(e);
            a->count--;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void arr_clear(Arr *a)
{
    for (int i = 0; i < ABUCKETS; i++) {
        AElem *e = a->b[i];
        while (e) { AElem *n = e->next; v_free(&e->v); free(e); e = n; }
        a->b[i] = NULL;
    }
    a->count = 0;
}

static void set_num(int s, long n) { v_free(&sym[s].v); sym[s].v = v_num(n); }
static void set_str(int s, const char *t) { v_free(&sym[s].v); sym[s].v = v_str(t); }
static long get_num(int s) { return v_getnum(&sym[s].v); }
static const char *get_str(int s) { return v_getstr(&sym[s].v); }

/* ── the record and its fields ───────────────────────────────────── */

static char  rec[RECMAX];            /* $0 */
static char  fbuf[RECMAX];           /* the same text, cut into fields */
static char *fld[MAXF + 1];
static bool  fown[MAXF + 1];         /* this field was assigned, not split */
static int   nf;

static void field_release(void)
{
    for (int i = 1; i <= MAXF; i++) {
        if (fown[i]) { free(fld[i]); fown[i] = false; }
        fld[i] = NULL;
    }
}

/* Split s by fs into at most max pieces, filling offs[]/lens[].
 * Returns the count, or -1 when there are too many. */
static lpre *re_for(const char *pat);

static int split_into(const char *s, const char *fs, int *offs, int *lens,
                      int max)
{
    int n = 0, len = (int)strlen(s);

    if (fs[0] == ' ' && fs[1] == '\0') {
        int i = 0;
        for (;;) {
            while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n') i++;
            if (!s[i]) break;
            int start = i;
            while (s[i] && s[i] != ' ' && s[i] != '\t' && s[i] != '\n') i++;
            if (n == max) return -1;
            offs[n] = start; lens[n] = i - start; n++;
        }
        return n;
    }

    if (len == 0) return 0;

    if (fs[1] == '\0') {                  /* one character, taken literally */
        int start = 0;
        for (int i = 0; i <= len; i++) {
            if (i == len || s[i] == fs[0]) {
                if (n == max) return -1;
                offs[n] = start; lens[n] = i - start; n++;
                start = i + 1;
            }
        }
        return n;
    }

    lpre *re = re_for(fs);                /* more than one: a regex */
    int caps[RE_MAX_CAPS], pos = 0, start = 0;
    while (pos <= len && re_search(re, s, pos, pos > 0, caps)) {
        if (caps[1] == caps[0]) {         /* an empty match makes no cut */
            pos = caps[0] + 1;
            continue;
        }
        if (n == max) return -1;
        offs[n] = start; lens[n] = caps[0] - start; n++;
        start = pos = caps[1];
    }
    if (n == max) return -1;
    offs[n] = start; lens[n] = len - start; n++;
    return n;
}

static void split_record(void)
{
    static int offs[MAXF], lens[MAXF];

    field_release();
    strlcpy(fbuf, rec, sizeof fbuf);

    int n = split_into(fbuf, get_str(S_FS), offs, lens, MAXF);
    if (n < 0) {
        oflush();
        dprintf(STDERR_FILENO,
                "awk: line %ld has more than %d fields, which is this "
                "awk's limit\n", get_num(S_NR), MAXF);
        lp_exit(2);
    }
    for (int i = 0; i < n; i++) {
        fld[i + 1] = fbuf + offs[i];
        fbuf[offs[i] + lens[i]] = '\0';
    }
    nf = n;
    set_num(S_NF, nf);
}

static bool rec_truncated;

static void set_record(const char *s)
{
    if (strlcpy(rec, s, sizeof rec) >= sizeof rec) {
        rec_truncated = true;
        exit_status = 1;
    }
    split_record();
}

/* Put $0 back together from the fields, with OFS between them. */
static void rebuild_record(void)
{
    const char *ofs = get_str(S_OFS);
    size_t n = 0;

    rec[0] = '\0';
    for (int i = 1; i <= nf; i++) {
        if (i > 1) n = strlcat(rec, ofs, sizeof rec);
        n = strlcat(rec, fld[i] ? fld[i] : "", sizeof rec);
    }
    if (n >= sizeof rec) {
        oflush();
        dprintf(STDERR_FILENO,
                "awk: line %ld: rebuilding $0 would need more than %d "
                "bytes\n", get_num(S_NR), RECMAX - 1);
        lp_exit(2);
    }
}

static const char *field_str(int i)
{
    if (i == 0) return rec;
    if (i < 1 || i > nf || !fld[i]) return "";
    return fld[i];
}

static void set_nf(long want)
{
    if (want < 0) want = 0;
    if (want > MAXF) {
        dprintf(STDERR_FILENO,
                "awk: NF cannot be set above %d here\n", MAXF);
        exit_status = 1;
        want = MAXF;
    }
    for (int i = (int)want + 1; i <= nf; i++)
        if (fown[i]) { free(fld[i]); fown[i] = false; fld[i] = NULL; }
    for (int i = nf + 1; i <= (int)want; i++) {
        fld[i] = NULL;
        fown[i] = false;
    }
    nf = (int)want;
    set_num(S_NF, nf);
    rebuild_record();
}

static void set_field(int i, const char *s)
{
    if (i == 0) { set_record(s); return; }
    if (i < 0 || i > MAXF) {
        dprintf(STDERR_FILENO,
                "awk: $%d is outside the %d fields this awk keeps\n",
                i, MAXF);
        exit_status = 1;
        return;
    }
    if (i > nf) {
        for (int k = nf + 1; k <= i; k++) { fld[k] = NULL; fown[k] = false; }
        nf = i;
        set_num(S_NF, nf);
    }
    if (fown[i]) free(fld[i]);
    fld[i] = xstrdup(s);
    fown[i] = true;
    rebuild_record();
}

/* ── compiled regexes ────────────────────────────────────────────────
 * Patterns written as /.../ are compiled once when the program is
 * parsed. Ones built at run time go through this cache, so `$0 ~ pat`
 * inside a loop compiles the pattern once and not once per line. */
static struct { char *pat; lpre *re; } recache[RECACHE];
static int recache_next;

static lpre *re_compile_or_die(const char *pat)
{
    const char *err = NULL;
    lpre *re = re_compile(pat, true, false, &err);
    if (!re) {
        oflush();
        dprintf(STDERR_FILENO, "awk: /%s/: %s\n", pat,
                err ? err : "bad regular expression");
        lp_exit(2);
    }
    return re;
}

static lpre *re_for(const char *pat)
{
    for (int i = 0; i < RECACHE; i++)
        if (recache[i].pat && strcmp(recache[i].pat, pat) == 0)
            return recache[i].re;

    int slot = recache_next++ % RECACHE;
    if (recache[slot].pat) { free(recache[slot].pat); re_free(recache[slot].re); }
    recache[slot].re = re_compile_or_die(pat);
    recache[slot].pat = xstrdup(pat);
    return recache[slot].re;
}

/* ── the tree ────────────────────────────────────────────────────── */

enum {
    N_NUM = 1, N_STR, N_RE, N_VAR, N_FIELD, N_SUBSCR,
    N_ASSIGN, N_BIN, N_NEG, N_NOT, N_INCDEC, N_COND, N_AND, N_OR,
    N_MATCH, N_IN, N_CALL, N_CAT,
    N_PRINT, N_PRINTF, N_IF, N_WHILE, N_FOR, N_FORIN, N_BLOCK,
    N_NEXT, N_EXIT, N_BREAK, N_CONT, N_DELETE, N_DELARR, N_EXPR
};

typedef struct Node {
    int  type, op, line, sym, nargs;
    long num;
    char *str;
    lpre *re;
    struct Node *a, *b, *c;
    struct Node *next;            /* the next statement in a list */
    struct Node **args;
} Node;

static Node *nd(int type, Node *a, Node *b, Node *c);

/* ── the lexer ───────────────────────────────────────────────────── */

enum {
    T_EOF = 0, T_NL = 1,
    T_NUMBER = 256, T_STRING, T_ERE, T_NAME, T_FUNC,
    T_BEGIN, T_END, T_IF, T_ELSE, T_WHILE, T_FOR, T_BREAK, T_CONT,
    T_NEXT, T_EXIT, T_PRINT, T_PRINTF, T_DELETE, T_IN, T_DO,
    T_ADDA, T_SUBA, T_MULA, T_DIVA, T_MODA, T_POWA,
    T_EQ, T_NE, T_LE, T_GE, T_AND, T_OR, T_INCR, T_DECR, T_NOMATCH,
    T_APPEND
};

/* builtins */
enum { B_LENGTH = 1, B_SUBSTR, B_INDEX, B_SPLIT, B_SUB, B_GSUB, B_MATCH,
       B_SPRINTF, B_TOLOWER, B_TOUPPER, B_INT, B_SYSTEM };

static const char *src;          /* the whole program text */
static int   tok, tokline, lineno_lex = 1, prevtok;
static long  toknum;
static char  tokstr[1024];

static void perr(const char *what) __attribute__((noreturn));

static void perr(const char *what)
{
    oflush();
    dprintf(STDERR_FILENO, "awk: line %d: %s\n", tokline, what);
    lp_exit(2);
}

/* What the current token looks like, for a message. */
static const char *tok_text(void)
{
    static char b[64];
    switch (tok) {
    case T_EOF:    return "the end of the program";
    case T_NL:     return "the end of the line";
    case T_NUMBER: snprintf(b, sizeof b, "%ld", toknum); return b;
    case T_STRING: snprintf(b, sizeof b, "\"%s\"", tokstr); return b;
    case T_ERE:    snprintf(b, sizeof b, "/%s/", tokstr); return b;
    case T_NAME:
    case T_FUNC:   return tokstr;
    default:
        if (tok < 256) { b[0] = (char)tok; b[1] = '\0'; return b; }
        return "that";
    }
}

static void perr_tok(const char *what) __attribute__((noreturn));

static void perr_tok(const char *what)
{
    static char b[160];
    snprintf(b, sizeof b, "expected %s, but found %s", what, tok_text());
    perr(b);
}

struct kw { const char *name; int tok; int val; };

static const struct kw keywords[] = {
    { "BEGIN", T_BEGIN, 0 }, { "END", T_END, 0 },
    { "if", T_IF, 0 }, { "else", T_ELSE, 0 }, { "while", T_WHILE, 0 },
    { "for", T_FOR, 0 }, { "break", T_BREAK, 0 }, { "continue", T_CONT, 0 },
    { "next", T_NEXT, 0 }, { "exit", T_EXIT, 0 }, { "print", T_PRINT, 0 },
    { "printf", T_PRINTF, 0 }, { "delete", T_DELETE, 0 }, { "in", T_IN, 0 },
    { "do", T_DO, 0 },
    { "length", T_FUNC, B_LENGTH }, { "substr", T_FUNC, B_SUBSTR },
    { "index", T_FUNC, B_INDEX }, { "split", T_FUNC, B_SPLIT },
    { "sub", T_FUNC, B_SUB }, { "gsub", T_FUNC, B_GSUB },
    { "match", T_FUNC, B_MATCH }, { "sprintf", T_FUNC, B_SPRINTF },
    { "tolower", T_FUNC, B_TOLOWER }, { "toupper", T_FUNC, B_TOUPPER },
    { "int", T_FUNC, B_INT }, { "system", T_FUNC, B_SYSTEM },
    { NULL, 0, 0 }
};

static int tokfunc;

/* One escape in a string literal or in a -v/-F value. Returns the
 * bytes consumed from s (which starts after the backslash). */
static int unescape_one(const char *s, char *out)
{
    switch (*s) {
    case 'n': *out = '\n'; return 1;
    case 't': *out = '\t'; return 1;
    case 'r': *out = '\r'; return 1;
    case 'a': *out = '\a'; return 1;
    case 'b': *out = '\b'; return 1;
    case 'f': *out = '\f'; return 1;
    case 'v': *out = '\v'; return 1;
    case '\\': *out = '\\'; return 1;
    case '"': *out = '"';  return 1;
    case '/': *out = '/';  return 1;
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7': {
        int v = 0, k = 0;
        while (k < 3 && s[k] >= '0' && s[k] <= '7') { v = v * 8 + (s[k] - '0'); k++; }
        *out = (char)v;
        return k;
    }
    default: *out = *s; return 1;    /* unknown: the character itself */
    }
}

/* Expand escapes in place, for -v and -F values and string literals. */
static void unescape(const char *in, char *out, size_t size)
{
    size_t o = 0;
    while (*in && o + 1 < size) {
        if (*in == '\\' && in[1]) {
            char c;
            in += 1 + unescape_one(in + 1, &c);
            out[o++] = c;
        } else out[o++] = *in++;
    }
    out[o] = '\0';
}

/* After these tokens a '/' is division; anywhere else it starts a
 * regular expression. Getting this wrong is the classic awk lexer bug:
 * `a / b / c` becomes a regex and `$0 ~ /x/` becomes a division. */
static bool div_follows(int t)
{
    return t == T_NUMBER || t == T_STRING || t == T_NAME || t == T_ERE ||
           t == ')' || t == ']' || t == '$' || t == T_INCR || t == T_DECR;
}

/* A newline after one of these is a line continuation, not the end of
 * a statement. */
static bool nl_ignored(int t)
{
    return t == 0 || t == T_NL || t == '{' || t == '&' || t == '|' ||
           t == ',' || t == ';' || t == T_AND || t == T_OR ||
           t == T_ELSE || t == T_DO || t == '?' || t == ':';
}

static void lex(void)
{
    prevtok = tok;
    for (;;) {
        while (*src == ' ' || *src == '\t' || *src == '\r') src++;

        if (*src == '\\' && src[1] == '\n') { src += 2; lineno_lex++; continue; }
        if (*src == '#') { while (*src && *src != '\n') src++; continue; }

        if (*src == '\n') {
            src++;
            lineno_lex++;
            if (nl_ignored(prevtok)) continue;
            tokline = lineno_lex - 1;
            tok = T_NL;
            return;
        }
        break;
    }

    tokline = lineno_lex;

    if (*src == '\0') { tok = T_EOF; return; }

    /* a number */
    if (*src >= '0' && *src <= '9') {
        long v = 0;
        while (*src >= '0' && *src <= '9') v = v * 10 + (*src++ - '0');
        if (*src == '.' || *src == 'e' || *src == 'E') {
            perr("a number with a decimal point or an exponent, and this "
                 "system has no floating point - use whole numbers");
        }
        toknum = v;
        tok = T_NUMBER;
        return;
    }

    /* a name or a keyword */
    if ((*src >= 'a' && *src <= 'z') || (*src >= 'A' && *src <= 'Z') ||
        *src == '_') {
        int n = 0;
        while ((*src >= 'a' && *src <= 'z') || (*src >= 'A' && *src <= 'Z') ||
               (*src >= '0' && *src <= '9') || *src == '_') {
            if (n < (int)sizeof tokstr - 1) tokstr[n++] = *src;
            src++;
        }
        tokstr[n] = '\0';

        for (const struct kw *k = keywords; k->name; k++)
            if (strcmp(k->name, tokstr) == 0) {
                tok = k->tok;
                tokfunc = k->val;
                return;
            }

        /* The things this awk deliberately does not have, refused by
         * name so the message says why rather than "unknown". */
        static const char *real_math[] = { "sin", "cos", "atan2", "exp",
                                           "log", "sqrt", "rand", "srand",
                                           NULL };
        for (int i = 0; real_math[i]; i++)
            if (strcmp(real_math[i], tokstr) == 0) {
                static char b[160];
                snprintf(b, sizeof b,
                         "%s() needs floating point, which this system does "
                         "not have anywhere - there is no answer it could "
                         "give", tokstr);
                perr(b);
            }
        if (strcmp(tokstr, "function") == 0 || strcmp(tokstr, "func") == 0)
            perr("this awk has no user-defined functions - write the work "
                 "out in the rule, or use a shell script");
        if (strcmp(tokstr, "getline") == 0)
            perr("this awk has no getline - feed the file in as an "
                 "argument, or use a shell loop");
        if (strcmp(tokstr, "close") == 0)
            perr("this awk has no close() - files opened by print > are "
                 "closed when awk finishes");

        tok = T_NAME;
        return;
    }

    /* a string */
    if (*src == '"') {
        src++;
        int n = 0;
        while (*src && *src != '"') {
            if (*src == '\n') perr("a string with no closing quote");
            if (*src == '\\' && src[1]) {
                char c;
                src += 1 + unescape_one(src + 1, &c);
                if (n < (int)sizeof tokstr - 1) tokstr[n++] = c;
            } else {
                if (n < (int)sizeof tokstr - 1) tokstr[n++] = *src;
                src++;
            }
        }
        if (*src != '"') perr("a string with no closing quote");
        src++;
        tokstr[n] = '\0';
        tok = T_STRING;
        return;
    }

    /* a regular expression, if a '/' can start one here */
    if (*src == '/' && !div_follows(prevtok)) {
        src++;
        int n = 0;
        while (*src && *src != '/') {
            if (*src == '\n') perr("a regular expression with no closing /");
            if (*src == '\\' && src[1] == '/') {
                if (n < (int)sizeof tokstr - 1) tokstr[n++] = '/';
                src += 2;
                continue;
            }
            if (*src == '\\' && src[1]) {
                if (n < (int)sizeof tokstr - 2) {
                    tokstr[n++] = '\\';
                    tokstr[n++] = src[1];
                }
                src += 2;
                continue;
            }
            if (n < (int)sizeof tokstr - 1) tokstr[n++] = *src;
            src++;
        }
        if (*src != '/') perr("a regular expression with no closing /");
        src++;
        tokstr[n] = '\0';
        tok = T_ERE;
        return;
    }

    char c = *src++;
    switch (c) {
    case '+': if (*src == '+') { src++; tok = T_INCR; return; }
              if (*src == '=') { src++; tok = T_ADDA; return; }
              break;
    case '-': if (*src == '-') { src++; tok = T_DECR; return; }
              if (*src == '=') { src++; tok = T_SUBA; return; }
              break;
    case '*': if (*src == '=') { src++; tok = T_MULA; return; }
              if (*src == '*') {            /* ** is ^ in some awks */
                  src++;
                  if (*src == '=') { src++; tok = T_POWA; return; }
                  tok = '^';
                  return;
              }
              break;
    case '/': if (*src == '=') { src++; tok = T_DIVA; return; } break;
    case '%': if (*src == '=') { src++; tok = T_MODA; return; } break;
    case '^': if (*src == '=') { src++; tok = T_POWA; return; } break;
    case '=': if (*src == '=') { src++; tok = T_EQ; return; } break;
    case '!': if (*src == '=') { src++; tok = T_NE; return; }
              if (*src == '~') { src++; tok = T_NOMATCH; return; }
              break;
    case '<': if (*src == '=') { src++; tok = T_LE; return; } break;
    case '>': if (*src == '=') { src++; tok = T_GE; return; }
              if (*src == '>') { src++; tok = T_APPEND; return; }
              break;
    case '&': if (*src == '&') { src++; tok = T_AND; return; } break;
    case '|': if (*src == '|') { src++; tok = T_OR; return; } break;
    default:  break;
    }
    tok = (unsigned char)c;
}

/* One token of look-ahead, for `for (k in a)`. */
typedef struct { const char *src; int tok, tokline, line, prevtok, func;
                 long num; char str[sizeof tokstr]; } LexState;

static void lex_save(LexState *s)
{
    s->src = src; s->tok = tok; s->tokline = tokline; s->line = lineno_lex;
    s->prevtok = prevtok; s->func = tokfunc; s->num = toknum;
    memcpy(s->str, tokstr, sizeof tokstr);
}

static void lex_restore(const LexState *s)
{
    src = s->src; tok = s->tok; tokline = s->tokline; lineno_lex = s->line;
    prevtok = s->prevtok; tokfunc = s->func; toknum = s->num;
    memcpy(tokstr, s->str, sizeof tokstr);
}

static void expect(int t, const char *what)
{
    if (tok != t) perr_tok(what);
    lex();
}

/* ── the parser ──────────────────────────────────────────────────── */

typedef struct { int kind; Node *pat, *pat2, *act; bool active; } Rule;
enum { R_BEGIN, R_END, R_MAIN };

static Rule rules[MAXRULES];
static int  nrules;
static bool has_main_or_end;

static bool in_print;        /* inside a print list: '>' is a redirect */

static Node *nd(int type, Node *a, Node *b, Node *c)
{
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof *n);
    n->type = type;
    n->line = tokline;
    n->a = a; n->b = b; n->c = c;
    return n;
}

static Node *p_expr(void);
static Node *p_stmt(void);

static bool is_lvalue(const Node *n)
{
    return n->type == N_VAR || n->type == N_FIELD || n->type == N_SUBSCR;
}

/* Can this token start an expression? Used to spot concatenation and
 * to tell `print` with a list from `print` on its own. */
static bool starts_expr(int t)
{
    return t == T_NUMBER || t == T_STRING || t == T_ERE || t == T_NAME ||
           t == T_FUNC || t == '$' || t == '(' || t == '!' || t == '-' ||
           t == '+' || t == T_INCR || t == T_DECR;
}

/* Concatenation cannot start with - or +: those are always binary
 * there, so `a - b` is a subtraction and not a concatenation of a and
 * -b. */
static bool starts_concat(int t)
{
    return t == T_NUMBER || t == T_STRING || t == T_ERE || t == T_NAME ||
           t == T_FUNC || t == '$' || t == '(' || t == '!' ||
           t == T_INCR || t == T_DECR;
}

static Node **p_arglist(int *count)
{
    Node **args = NULL;
    int n = 0, cap = 0;

    if (tok == ')') { *count = 0; return NULL; }
    for (;;) {
        Node *e = p_expr();
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            Node **na = realloc(args, (size_t)cap * sizeof(Node *));
            if (!na) die("out of memory parsing an argument list");
            args = na;
        }
        args[n++] = e;
        if (tok != ',') break;
        lex();
    }
    *count = n;
    return args;
}

static Node *p_call(void)
{
    int b = tokfunc;
    int line = tokline;
    Node *n = nd(N_CALL, NULL, NULL, NULL);
    n->op = b;
    n->line = line;
    lex();

    if (tok != '(') {
        if (b == B_LENGTH) { n->nargs = 0; return n; }   /* length on its own */
        perr_tok("( after this function's name");
    }
    lex();
    bool saved = in_print;
    in_print = false;
    n->args = p_arglist(&n->nargs);
    in_print = saved;
    expect(')', ") to close the arguments");

    static const struct { int b; int lo, hi; const char *use; } sigs[] = {
        { B_LENGTH,   0, 1, "length or length(x)" },
        { B_SUBSTR,   2, 3, "substr(s, from [, count])" },
        { B_INDEX,    2, 2, "index(s, t)" },
        { B_SPLIT,    2, 3, "split(s, array [, fs])" },
        { B_SUB,      2, 3, "sub(/re/, replacement [, target])" },
        { B_GSUB,     2, 3, "gsub(/re/, replacement [, target])" },
        { B_MATCH,    2, 2, "match(s, /re/)" },
        { B_SPRINTF,  1, 32, "sprintf(format, ...)" },
        { B_TOLOWER,  1, 1, "tolower(s)" },
        { B_TOUPPER,  1, 1, "toupper(s)" },
        { B_INT,      1, 1, "int(x)" },
        { B_SYSTEM,   1, 1, "system(command)" },
    };
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        if (sigs[i].b == b && (n->nargs < sigs[i].lo || n->nargs > sigs[i].hi)) {
            static char msg[128];
            snprintf(msg, sizeof msg, "the wrong number of arguments - "
                     "write %s", sigs[i].use);
            perr(msg);
        }

    if (b == B_SPLIT && n->args[1]->type != N_VAR)
        perr("split()'s second argument has to be the name of an array");
    if ((b == B_SUB || b == B_GSUB) && n->nargs == 3 &&
        !is_lvalue(n->args[2]))
        perr("sub() and gsub() can only change a variable, a field or an "
             "array element");
    return n;
}

static Node *p_primary(void)
{
    Node *n;

    switch (tok) {
    case T_NUMBER:
        n = nd(N_NUM, NULL, NULL, NULL);
        n->num = toknum;
        lex();
        return n;

    case T_STRING:
        n = nd(N_STR, NULL, NULL, NULL);
        n->str = xstrdup(tokstr);
        lex();
        return n;

    case T_ERE:
        n = nd(N_RE, NULL, NULL, NULL);
        n->str = xstrdup(tokstr);
        n->re = re_compile_or_die(tokstr);
        lex();
        return n;

    case '$':
        lex();
        n = nd(N_FIELD, p_primary(), NULL, NULL);
        return n;

    case T_INCR:
    case T_DECR: {
        int op = (tok == T_INCR) ? '+' : '-';
        lex();
        Node *t = p_primary();
        if (!is_lvalue(t))
            perr("++ and -- need a variable, a field or an array element");
        n = nd(N_INCDEC, t, NULL, NULL);
        n->op = op;
        n->num = 1;                       /* before, so the new value */
        return n;
    }

    case '(': {
        lex();
        bool saved = in_print;
        in_print = false;
        n = p_expr();
        if (tok == ',')
            perr("a list in brackets - this awk has one-dimensional arrays, "
                 "and print takes its list without brackets");
        in_print = saved;
        expect(')', ") to close the brackets");
        return n;
    }

    case T_FUNC:
        return p_call();

    case T_NAME: {
        int s = sym_intern(tokstr);
        lex();
        if (tok == '[') {
            lex();
            Node *idx = p_expr();
            if (tok == ',')
                perr("a[i,j] needs a two-dimensional array, and this awk "
                     "has one dimension - join the parts yourself, as in "
                     "a[i \"/\" j]");
            expect(']', "] to close the subscript");
            n = nd(N_SUBSCR, idx, NULL, NULL);
            n->sym = s;
            return n;
        }
        n = nd(N_VAR, NULL, NULL, NULL);
        n->sym = s;
        return n;
    }

    default:
        perr_tok("a value here");
    }
}

static Node *p_postfix(void)
{
    Node *n = p_primary();
    while (tok == T_INCR || tok == T_DECR) {
        if (!is_lvalue(n)) break;         /* `a + ++b` and the like */
        Node *p = nd(N_INCDEC, n, NULL, NULL);
        p->op = (tok == T_INCR) ? '+' : '-';
        p->num = 0;                       /* after, so the old value */
        lex();
        n = p;
    }
    return n;
}

static Node *p_pow(void)
{
    Node *n = p_postfix();
    if (tok == '^') {
        lex();
        Node *r = nd(N_BIN, n, NULL, NULL);   /* right associative */
        r->op = '^';
        /* -2 on the right of ^ is a unary minus, so parse a unary here */
        if (tok == '-') { lex(); Node *u = nd(N_NEG, p_pow(), NULL, NULL); r->b = u; }
        else r->b = p_pow();
        return r;
    }
    return n;
}

static Node *p_unary(void)
{
    if (tok == '!') { lex(); return nd(N_NOT, p_unary(), NULL, NULL); }
    if (tok == '-') { lex(); return nd(N_NEG, p_unary(), NULL, NULL); }
    if (tok == '+') { lex(); return p_unary(); }
    return p_pow();
}

static Node *p_mul(void)
{
    Node *n = p_unary();
    while (tok == '*' || tok == '/' || tok == '%') {
        int op = tok;
        lex();
        Node *b = nd(N_BIN, n, p_unary(), NULL);
        b->op = op;
        n = b;
    }
    return n;
}

static Node *p_add(void)
{
    Node *n = p_mul();
    while (tok == '+' || tok == '-') {
        int op = tok;
        lex();
        Node *b = nd(N_BIN, n, p_mul(), NULL);
        b->op = op;
        n = b;
    }
    return n;
}

static Node *p_concat(void)
{
    Node *n = p_add();
    while (starts_concat(tok)) {
        /* Two things written next to each other are joined. A keyword,
         * a comma, a bracket or an operator ends the run, which is why
         * starts_concat leaves out - and +: `a - b` is a subtraction,
         * never a join of a and -b. */
        Node *b = nd(N_CAT, n, p_add(), NULL);
        n = b;
    }
    return n;
}

static Node *p_rel(void)
{
    Node *n = p_concat();
    int op = 0;

    switch (tok) {
    case '<':    op = '<'; break;
    case T_LE:   op = 'L'; break;
    case T_EQ:   op = 'E'; break;
    case T_NE:   op = 'N'; break;
    case T_GE:   op = 'G'; break;
    case '>':    if (in_print) return n; op = '>'; break;
    default:     return n;
    }
    lex();
    Node *b = nd(N_BIN, n, p_concat(), NULL);
    b->op = op;
    return b;
}

static Node *p_match(void)
{
    Node *n = p_rel();
    while (tok == '~' || tok == T_NOMATCH) {
        int op = (tok == '~') ? '~' : '!';
        lex();
        Node *b = nd(N_MATCH, n, p_rel(), NULL);
        b->op = op;
        n = b;
    }
    return n;
}

static Node *p_in(void)
{
    Node *n = p_match();
    while (tok == T_IN) {
        lex();
        if (tok != T_NAME) perr_tok("the name of an array after `in`");
        Node *b = nd(N_IN, n, NULL, NULL);
        b->sym = sym_intern(tokstr);
        lex();
        n = b;
    }
    return n;
}

static Node *p_and(void)
{
    Node *n = p_in();
    while (tok == T_AND) {
        lex();
        n = nd(N_AND, n, p_in(), NULL);
    }
    return n;
}

static Node *p_or(void)
{
    Node *n = p_and();
    while (tok == T_OR) {
        lex();
        n = nd(N_OR, n, p_and(), NULL);
    }
    return n;
}

static Node *p_ternary(void)
{
    Node *n = p_or();
    if (tok == '?') {
        lex();
        Node *a = p_ternary();
        expect(':', ": to finish the ? : ");
        return nd(N_COND, n, a, p_ternary());
    }
    return n;
}

static Node *p_expr(void)
{
    Node *n = p_ternary();
    int op = 0;

    switch (tok) {
    case '=':    op = '='; break;
    case T_ADDA: op = '+'; break;
    case T_SUBA: op = '-'; break;
    case T_MULA: op = '*'; break;
    case T_DIVA: op = '/'; break;
    case T_MODA: op = '%'; break;
    case T_POWA: op = '^'; break;
    default:     return n;
    }
    if (!is_lvalue(n))
        perr("the left of an = has to be a variable, a field or an array "
             "element");
    lex();
    Node *a = nd(N_ASSIGN, n, p_expr(), NULL);
    a->op = op;
    return a;
}

static void skip_terms(void)
{
    while (tok == T_NL || tok == ';') lex();
}

static Node *p_block(void)
{
    Node *head = NULL, **tail = &head;

    expect('{', "{ to open the action");
    for (;;) {
        skip_terms();
        if (tok == '}' || tok == T_EOF) break;
        Node *s = p_stmt();
        *tail = s;
        tail = &s->next;
    }
    expect('}', "} to close the action");

    Node *b = nd(N_BLOCK, head, NULL, NULL);
    return b;
}

static Node *p_print(int type)
{
    Node *n = nd(type, NULL, NULL, NULL);
    lex();

    in_print = true;
    if (starts_expr(tok)) n->args = p_arglist(&n->nargs);
    in_print = false;

    if (tok == '|')
        perr("print into a command is not supported here - write the "
             "output to a file, or pipe the whole awk command");
    if (tok == '>' || tok == T_APPEND) {
        n->op = (tok == '>') ? '>' : 'A';
        lex();
        n->a = p_concat();
    }
    if (type == N_PRINTF && n->nargs < 1)
        perr("printf needs a format, as in printf \"%s\\n\", $1");
    return n;
}

static Node *p_simple(void)
{
    Node *n;

    switch (tok) {
    case T_PRINT:  return p_print(N_PRINT);
    case T_PRINTF: return p_print(N_PRINTF);

    case T_NEXT:   lex(); return nd(N_NEXT, NULL, NULL, NULL);
    case T_BREAK:  lex(); return nd(N_BREAK, NULL, NULL, NULL);
    case T_CONT:   lex(); return nd(N_CONT, NULL, NULL, NULL);

    case T_EXIT:
        lex();
        n = nd(N_EXIT, NULL, NULL, NULL);
        if (starts_expr(tok)) n->a = p_expr();
        return n;

    case T_DELETE: {
        lex();
        if (tok != T_NAME) perr_tok("the name of an array after delete");
        int s = sym_intern(tokstr);
        lex();
        if (tok == '[') {
            lex();
            n = nd(N_DELETE, p_expr(), NULL, NULL);
            n->sym = s;
            expect(']', "] to close the subscript");
            return n;
        }
        n = nd(N_DELARR, NULL, NULL, NULL);
        n->sym = s;
        return n;
    }

    default:
        return nd(N_EXPR, p_expr(), NULL, NULL);
    }
}

static Node *p_stmt(void)
{
    Node *n;

    switch (tok) {
    case '{':
        return p_block();

    case T_IF: {
        lex();
        expect('(', "( after if");
        Node *cond = p_expr();
        expect(')', ") after the condition");
        skip_terms();
        Node *then = p_stmt();
        n = nd(N_IF, cond, then, NULL);
        /* A newline before `else` is not the end of the statement. */
        LexState save;
        lex_save(&save);
        skip_terms();
        if (tok == T_ELSE) {
            lex();
            skip_terms();
            n->c = p_stmt();
        } else lex_restore(&save);
        return n;
    }

    case T_WHILE: {
        lex();
        expect('(', "( after while");
        Node *cond = p_expr();
        expect(')', ") after the condition");
        skip_terms();
        return nd(N_WHILE, cond, p_stmt(), NULL);
    }

    case T_DO:
        perr("this awk has no do-while - write it as a while loop");

    case T_FOR: {
        lex();
        expect('(', "( after for");

        /* `for (k in a)` or `for (init; cond; step)`. Telling them apart
         * needs one token of look-ahead past the name. */
        if (tok == T_NAME) {
            LexState save;
            lex_save(&save);
            int s = sym_intern(tokstr);
            lex();
            if (tok == T_IN) {
                lex();
                if (tok != T_NAME) perr_tok("the name of an array");
                Node *f = nd(N_FORIN, NULL, NULL, NULL);
                f->sym = s;
                f->num = sym_intern(tokstr);
                lex();
                expect(')', ") after the array name");
                skip_terms();
                f->a = p_stmt();
                return f;
            }
            lex_restore(&save);
        }

        Node *init = (tok == ';') ? NULL : nd(N_EXPR, p_expr(), NULL, NULL);
        expect(';', "; after the first part of the for");
        Node *cond = (tok == ';') ? NULL : p_expr();
        expect(';', "; after the condition of the for");
        Node *step = (tok == ')') ? NULL : nd(N_EXPR, p_expr(), NULL, NULL);
        expect(')', ") to close the for");
        skip_terms();

        /* a: the condition, b: the body, c: the first part, and the
         * step goes in args[0] rather than costing every node a field */
        n = nd(N_FOR, cond, NULL, init);
        n->b = p_stmt();
        if (step) {
            n->args = xmalloc(sizeof(Node *));
            n->args[0] = step;
            n->nargs = 1;
        }
        return n;
    }

    case ';':
        return nd(N_BLOCK, NULL, NULL, NULL);

    default:
        return p_simple();
    }
}

static void add_rule(int kind, Node *pat, Node *pat2, Node *act)
{
    if (nrules == MAXRULES)
        perr("more rules than this awk keeps (64) - put the program in a "
             "file and split it up");
    rules[nrules].kind = kind;
    rules[nrules].pat = pat;
    rules[nrules].pat2 = pat2;
    rules[nrules].act = act;
    rules[nrules].active = false;
    nrules++;
    if (kind != R_BEGIN) has_main_or_end = true;
}

static void parse_program(const char *text)
{
    src = text;
    tok = 0;
    prevtok = 0;
    lineno_lex = 1;
    lex();

    for (;;) {
        skip_terms();
        if (tok == T_EOF) break;

        if (tok == T_BEGIN || tok == T_END) {
            int kind = (tok == T_BEGIN) ? R_BEGIN : R_END;
            lex();
            skip_terms();
            if (tok != '{')
                perr_tok("{ - BEGIN and END always have an action");
            add_rule(kind, NULL, NULL, p_block());
            continue;
        }

        if (tok == '{') { add_rule(R_MAIN, NULL, NULL, p_block()); continue; }

        Node *pat = p_expr(), *pat2 = NULL;
        if (tok == ',') {
            lex();
            skip_terms();
            pat2 = p_expr();
        }
        skip_terms();
        Node *act = (tok == '{') ? p_block() : NULL;
        add_rule(R_MAIN, pat, pat2, act);
    }
}

/* ── the evaluator ───────────────────────────────────────────────── */

enum { F_NORM = 0, F_BREAK, F_CONT, F_NEXT, F_EXIT };

static Val eval(Node *n);
static int exec_list(Node *n);

static int  cur_line;                 /* for run-time messages */
static bool in_end_rule;

static void rt_err(const char *msg) __attribute__((noreturn));

static void rt_err(const char *msg)
{
    oflush();
    dprintf(STDERR_FILENO, "awk: line %d: %s\n", cur_line, msg);
    lp_exit(2);
}

static Val lv_get(Node *lv)
{
    switch (lv->type) {
    case N_VAR:
        if (sym[lv->sym].arr)
            rt_err("this is an array, and it is being used as a plain "
                   "variable");
        return v_copy(&sym[lv->sym].v);
    case N_FIELD: {
        Val i = eval(lv->a);
        long k = v_getnum(&i);
        v_free(&i);
        if (k < 0) rt_err("a field number cannot be negative");
        return v_input(field_str((int)k));
    }
    default: {                        /* N_SUBSCR */
        Val i = eval(lv->a);
        AElem *e = arr_get(arr_of(lv->sym), v_getstr(&i), true);
        v_free(&i);
        return v_copy(&e->v);
    }
    }
}

static void lv_set(Node *lv, Val v)
{
    switch (lv->type) {
    case N_VAR:
        if (sym[lv->sym].arr)
            rt_err("this is an array, and something is assigning to it as "
                   "a plain variable");
        if (lv->sym == S_NF) {
            long want = v_getnum(&v);
            v_free(&v);
            set_nf(want);
            return;
        }
        v_free(&sym[lv->sym].v);
        sym[lv->sym].v = v;
        return;

    case N_FIELD: {
        Val i = eval(lv->a);
        long k = v_getnum(&i);
        v_free(&i);
        if (k < 0) rt_err("a field number cannot be negative");
        set_field((int)k, v_getstr(&v));
        v_free(&v);
        return;
    }

    default: {
        Val i = eval(lv->a);
        AElem *e = arr_get(arr_of(lv->sym), v_getstr(&i), true);
        v_free(&i);
        v_free(&e->v);
        e->v = v;
        return;
    }
    }
}

static int compare(Val *a, Val *b)
{
    if (v_isnum(a) && v_isnum(b)) {
        long x = v_getnum(a), y = v_getnum(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    int c = strcmp(v_getstr(a), v_getstr(b));
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

static long ipow(long base, long e)
{
    /* A negative exponent is a fraction, and fractions truncate to zero
     * here - except for the bases where they do not. */
    if (e < 0) return (base == 1) ? 1 : ((base == -1) ? ((-e % 2) ? -1 : 1) : 0);
    if (e > 63 && base != 0 && base != 1 && base != -1)
        rt_err("the result of ^ is bigger than a whole number holds");
    long r = 1;
    while (e-- > 0) r *= base;
    return r;
}

static long arith(int op, long x, long y)
{
    switch (op) {
    case '+': return x + y;
    case '-': return x - y;
    case '*': return x * y;
    case '/':
        if (y == 0) rt_err("division by zero");
        return x / y;                 /* whole numbers: 7 / 2 is 3 */
    case '%':
        if (y == 0) rt_err("the remainder of a division by zero");
        return x % y;
    default:  return ipow(x, y);
    }
}

/* The regex for a `~`, match(), sub() or split(): a /literal/ is
 * already compiled, anything else is a string compiled on demand. */
static lpre *re_operand(Node *n)
{
    if (n->type == N_RE) return n->re;
    Val v = eval(n);
    lpre *re = re_for(v_getstr(&v));
    v_free(&v);
    return re;
}

/* ── printf ──────────────────────────────────────────────────────────
 * Written out here rather than handed to the libc's snprintf because
 * awk's printf has a precision (`%.3s`, `%.5d`) and the libc's does
 * not - and because a `%f` has to be refused with a reason rather than
 * printed as something wrong. */
static void num_digits(unsigned long v, int base, bool upper, char *out)
{
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    int n = 0;

    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = dig[v % (unsigned)base]; v /= (unsigned)base; }
    int o = 0;
    while (n > 0) out[o++] = tmp[--n];
    out[o] = '\0';
}

static void pad_into(SB *out, const char *sign, const char *body,
                     int width, bool left, bool zero, int minlen)
{
    int blen = (int)strlen(body), slen = (int)strlen(sign);
    int zeros = (minlen > blen) ? minlen - blen : 0;
    int total = slen + zeros + blen;

    if (!left && zero && width > total) { zeros += width - total; total = width; }

    if (!left)
        for (int i = total; i < width; i++) sb_ch(out, ' ');
    sb_str(out, sign);
    for (int i = 0; i < zeros; i++) sb_ch(out, '0');
    sb_str(out, body);
    if (left)
        for (int i = total; i < width; i++) sb_ch(out, ' ');
}

static void do_format(SB *out, Node **args, int nargs)
{
    Val fv = eval(args[0]);
    const char *f = v_getstr(&fv);
    int argi = 1;

    while (*f) {
        if (*f != '%') { sb_ch(out, *f++); continue; }
        f++;
        if (*f == '%') { sb_ch(out, '%'); f++; continue; }

        bool left = false, zero = false, plus = false, space = false;
        for (;;) {
            if (*f == '-') { left = true; f++; continue; }
            if (*f == '0') { zero = true; f++; continue; }
            if (*f == '+') { plus = true; f++; continue; }
            if (*f == ' ') { space = true; f++; continue; }
            break;
        }

        int width = 0, prec = -1;
        if (*f == '*') {
            f++;
            Val w = (argi < nargs) ? eval(args[argi++]) : v_num(0);
            width = (int)v_getnum(&w);
            v_free(&w);
            if (width < 0) { left = true; width = -width; }
        } else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');

        if (*f == '.') {
            f++;
            prec = 0;
            if (*f == '*') {
                f++;
                Val p = (argi < nargs) ? eval(args[argi++]) : v_num(0);
                prec = (int)v_getnum(&p);
                v_free(&p);
            } else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
            if (prec < 0) prec = 0;
        }

        char conv = *f ? *f++ : 's';
        Val a = (argi < nargs) ? eval(args[argi++]) : v_uninit();
        char digits[40];

        switch (conv) {
        case 'd': case 'i': {
            long v = v_getnum(&a);
            unsigned long m = (v < 0) ? (unsigned long)(-(v + 1)) + 1
                                      : (unsigned long)v;
            num_digits(m, 10, false, digits);
            const char *sign = (v < 0) ? "-" : (plus ? "+" : (space ? " " : ""));
            pad_into(out, sign, digits, width, left, zero && prec < 0,
                     prec < 0 ? 0 : prec);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            long v = v_getnum(&a);
            int base = (conv == 'o') ? 8 : ((conv == 'u') ? 10 : 16);
            num_digits((unsigned long)v, base, conv == 'X', digits);
            pad_into(out, "", digits, width, left, zero && prec < 0,
                     prec < 0 ? 0 : prec);
            break;
        }
        case 'c': {
            char one[2];
            if (a.kind == V_NUM) one[0] = (char)v_getnum(&a);
            else                 one[0] = v_getstr(&a)[0];
            one[1] = '\0';
            pad_into(out, "", one, width, left, false, 0);
            break;
        }
        case 's': {
            const char *s = v_getstr(&a);
            int len = (int)strlen(s);
            if (prec >= 0 && prec < len) {
                char *cut = xmalloc((size_t)prec + 1);
                memcpy(cut, s, (size_t)prec);
                cut[prec] = '\0';
                pad_into(out, "", cut, width, left, false, 0);
                free(cut);
            } else pad_into(out, "", s, width, left, false, 0);
            break;
        }
        case 'f': case 'e': case 'g': case 'F': case 'E': case 'G':
            v_free(&a);
            v_free(&fv);
            rt_err("%f and friends need floating point, which this system "
                   "does not have - print whole numbers with %d");
        default: {
            static char msg[80];
            snprintf(msg, sizeof msg,
                     "%%%c is not a conversion printf knows here", conv);
            v_free(&a);
            v_free(&fv);
            rt_err(msg);
        }
        }
        v_free(&a);
    }
    v_free(&fv);
}

/* ── the built-in functions ──────────────────────────────────────── */

static Val do_substr(Node *n)
{
    Val s = eval(n->args[0]);
    Val m = eval(n->args[1]);
    const char *str = v_getstr(&s);
    long len = (long)strlen(str);
    long from = v_getnum(&m);
    long count = len;

    v_free(&m);
    if (n->nargs == 3) {
        Val c = eval(n->args[2]);
        count = v_getnum(&c);
        v_free(&c);
    }

    /* awk counts from 1, and clamps rather than complaining: substr(s, 0)
     * and substr(s, -3, 5) both have well-defined answers. */
    long start = from, end = (n->nargs == 3) ? from + count : len + 1;
    if (start < 1) start = 1;
    if (end > len + 1) end = len + 1;
    if (end <= start) { v_free(&s); return v_str(""); }

    long take = end - start;
    char *out = xmalloc((size_t)take + 1);
    memcpy(out, str + start - 1, (size_t)take);
    out[take] = '\0';
    v_free(&s);
    return v_take(out);
}

static Val do_split(Node *n)
{
    static int offs[MAXF], lens[MAXF];

    Val s = eval(n->args[0]);
    char fsbuf[64];
    const char *fs;

    if (n->nargs == 3) {
        if (n->args[2]->type == N_RE) {
            /* split(s, a, /re/): the pattern itself, not a match of it */
            strlcpy(fsbuf, n->args[2]->str, sizeof fsbuf);
        } else {
            Val f = eval(n->args[2]);
            strlcpy(fsbuf, v_getstr(&f), sizeof fsbuf);
            v_free(&f);
        }
        fs = fsbuf;
    } else fs = get_str(S_FS);

    if (fs[0] == '\0')
        rt_err("split() was given an empty separator; use \"\" only with a "
               "single character or a regular expression");

    Arr *a = arr_of(n->args[1]->sym);
    arr_clear(a);

    const char *str = v_getstr(&s);
    int cnt = split_into(str, fs, offs, lens, MAXF);
    if (cnt < 0)
        rt_err("split() would make more than 256 pieces, which is this "
               "awk's limit");

    for (int i = 0; i < cnt; i++) {
        char key[24];
        snprintf(key, sizeof key, "%d", i + 1);
        char *piece = xmalloc((size_t)lens[i] + 1);
        memcpy(piece, str + offs[i], (size_t)lens[i]);
        piece[lens[i]] = '\0';
        AElem *e = arr_get(a, key, true);
        v_free(&e->v);
        e->v = v_input(piece);
        free(piece);
    }
    v_free(&s);
    return v_num(cnt);
}

/* & in the replacement is the text that matched; \& is a plain &. */
static void append_repl(SB *out, const char *repl, const char *text,
                        int mstart, int mlen)
{
    for (const char *p = repl; *p; p++) {
        if (*p == '\\' && (p[1] == '&' || p[1] == '\\')) { sb_ch(out, p[1]); p++; }
        else if (*p == '&') sb_add(out, text + mstart, mlen);
        else sb_ch(out, *p);
    }
}

static Val do_sub(Node *n, bool global)
{
    lpre *re = re_operand(n->args[0]);
    Val rv = eval(n->args[1]);
    char rbuf[1024];
    strlcpy(rbuf, v_getstr(&rv), sizeof rbuf);
    v_free(&rv);

    Node *target = (n->nargs == 3) ? n->args[2] : NULL;
    Val tv;
    if (target) tv = lv_get(target);
    else        tv = v_input(rec);

    const char *text = v_getstr(&tv);
    int len = (int)strlen(text), caps[RE_MAX_CAPS], pos = 0;
    long count = 0;
    SB out;
    sb_init(&out);

    while (pos <= len && re_search(re, text, pos, pos > 0, caps)) {
        sb_add(&out, text + pos, caps[0] - pos);
        append_repl(&out, rbuf, text, caps[0], caps[1] - caps[0]);
        count++;
        if (caps[1] == caps[0]) {         /* an empty match: step past one */
            if (caps[0] < len) sb_ch(&out, text[caps[0]]);
            pos = caps[0] + 1;
        } else pos = caps[1];
        if (!global) break;
    }

    if (count == 0) {
        sb_add(&out, NULL, 0);
        free(out.p);
        v_free(&tv);
        return v_num(0);
    }
    if (pos < len) sb_add(&out, text + pos, len - pos);

    char *result = sb_take(&out);
    if (target) lv_set(target, v_take(result));
    else        { set_record(result); free(result); }
    v_free(&tv);
    return v_num(count);
}

static Val do_match(Node *n)
{
    Val s = eval(n->args[0]);
    lpre *re = re_operand(n->args[1]);
    int caps[RE_MAX_CAPS];
    long start = 0, len = -1;

    if (re_search(re, v_getstr(&s), 0, false, caps)) {
        start = caps[0] + 1;
        len = caps[1] - caps[0];
    }
    v_free(&s);
    set_num(S_RSTART, start);
    set_num(S_RLENGTH, len);
    return v_num(start);
}

static Val do_system(Node *n)
{
    Val c = eval(n->args[0]);
    char cmd[1024];
    strlcpy(cmd, v_getstr(&c), sizeof cmd);
    v_free(&c);

    oflush();                          /* the child shares this terminal */

    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "awk: cannot start a process for system()\n");
        return v_num(-1);
    }
    if (pid == 0) {
        char *argv[4];
        argv[0] = (char *)"sh";
        argv[1] = (char *)"-c";
        argv[2] = cmd;
        argv[3] = NULL;
        lp_execve("/bin/sh", argv, environ);
        dprintf(STDERR_FILENO, "awk: /bin/sh: cannot run it\n");
        lp_exit(127);
    }
    int st = 0;
    lp_waitpid(pid, &st, 0);
    return v_num(LP_WIFEXITED(st) ? LP_WEXITSTATUS(st) : 1);
}

static Val do_call(Node *n)
{
    switch (n->op) {
    case B_LENGTH: {
        if (n->nargs == 0) return v_num((long)strlen(rec));
        Node *a = n->args[0];
        if (a->type == N_VAR && sym[a->sym].arr)
            return v_num(sym[a->sym].arr->count);
        Val v = eval(a);
        long l = (long)strlen(v_getstr(&v));
        v_free(&v);
        return v_num(l);
    }
    case B_SUBSTR: return do_substr(n);
    case B_INDEX: {
        Val s = eval(n->args[0]), t = eval(n->args[1]);
        const char *hay = v_getstr(&s);
        const char *needle = v_getstr(&t);
        const char *at = strstr(hay, needle);
        long r = at ? (long)(at - hay) + 1 : 0;
        v_free(&s); v_free(&t);
        return v_num(r);
    }
    case B_SPLIT:  return do_split(n);
    case B_SUB:    return do_sub(n, false);
    case B_GSUB:   return do_sub(n, true);
    case B_MATCH:  return do_match(n);
    case B_SPRINTF: {
        SB b;
        sb_init(&b);
        do_format(&b, n->args, n->nargs);
        return v_take(sb_take(&b));
    }
    case B_TOLOWER:
    case B_TOUPPER: {
        Val v = eval(n->args[0]);
        char *s = xstrdup(v_getstr(&v));
        v_free(&v);
        for (char *p = s; *p; p++) {
            if (n->op == B_TOLOWER && *p >= 'A' && *p <= 'Z') *p += 32;
            if (n->op == B_TOUPPER && *p >= 'a' && *p <= 'z') *p -= 32;
        }
        return v_take(s);
    }
    case B_INT: {
        Val v = eval(n->args[0]);
        long l = v_getnum(&v);
        v_free(&v);
        return v_num(l);
    }
    default:       return do_system(n);
    }
}

static Val eval(Node *n)
{
    cur_line = n->line;

    switch (n->type) {
    case N_NUM: return v_num(n->num);
    case N_STR: return v_str(n->str);

    case N_RE: {                       /* a bare /re/ means $0 ~ /re/ */
        int caps[RE_MAX_CAPS];
        return v_num(re_search(n->re, rec, 0, false, caps) ? 1 : 0);
    }

    case N_VAR:
    case N_FIELD:
    case N_SUBSCR:
        return lv_get(n);

    case N_ASSIGN: {
        Val r = eval(n->b);
        if (n->op != '=') {
            Val l = lv_get(n->a);
            long v = arith(n->op, v_getnum(&l), v_getnum(&r));
            v_free(&l);
            v_free(&r);
            r = v_num(v);
        }
        Val keep = v_copy(&r);
        lv_set(n->a, r);
        return keep;
    }

    case N_BIN: {
        Val a = eval(n->a), b = eval(n->b);
        long r;
        switch (n->op) {
        case '<': r = compare(&a, &b) <  0; break;
        case 'L': r = compare(&a, &b) <= 0; break;
        case '>': r = compare(&a, &b) >  0; break;
        case 'G': r = compare(&a, &b) >= 0; break;
        case 'E': r = compare(&a, &b) == 0; break;
        case 'N': r = compare(&a, &b) != 0; break;
        default:  r = arith(n->op, v_getnum(&a), v_getnum(&b)); break;
        }
        v_free(&a); v_free(&b);
        return v_num(r);
    }

    case N_CAT: {
        Val a = eval(n->a), b = eval(n->b);
        SB s;
        sb_init(&s);
        sb_str(&s, v_getstr(&a));
        sb_str(&s, v_getstr(&b));
        v_free(&a); v_free(&b);
        return v_take(sb_take(&s));
    }

    case N_NEG: {
        Val a = eval(n->a);
        long r = -v_getnum(&a);
        v_free(&a);
        return v_num(r);
    }

    case N_NOT: {
        Val a = eval(n->a);
        bool t = v_true(&a);
        v_free(&a);
        return v_num(t ? 0 : 1);
    }

    case N_INCDEC: {
        Val old = lv_get(n->a);
        long before = v_getnum(&old);
        v_free(&old);
        long after = (n->op == '+') ? before + 1 : before - 1;
        lv_set(n->a, v_num(after));
        return v_num(n->num ? after : before);
    }

    case N_COND: {
        Val c = eval(n->a);
        bool t = v_true(&c);
        v_free(&c);
        return eval(t ? n->b : n->c);
    }

    case N_AND: {
        Val a = eval(n->a);
        bool t = v_true(&a);
        v_free(&a);
        if (!t) return v_num(0);
        Val b = eval(n->b);
        t = v_true(&b);
        v_free(&b);
        return v_num(t ? 1 : 0);
    }

    case N_OR: {
        Val a = eval(n->a);
        bool t = v_true(&a);
        v_free(&a);
        if (t) return v_num(1);
        Val b = eval(n->b);
        t = v_true(&b);
        v_free(&b);
        return v_num(t ? 1 : 0);
    }

    case N_MATCH: {
        Val s = eval(n->a);
        lpre *re = re_operand(n->b);
        int caps[RE_MAX_CAPS];
        bool m = re_search(re, v_getstr(&s), 0, false, caps);
        v_free(&s);
        return v_num((n->op == '~') == m ? 1 : 0);
    }

    case N_IN: {
        Val k = eval(n->a);
        AElem *e = arr_get(arr_of(n->sym), v_getstr(&k), false);
        v_free(&k);
        return v_num(e ? 1 : 0);
    }

    default:
        return do_call(n);
    }
}

/* ── running the statements ──────────────────────────────────────── */

static long exit_code_wanted;
static bool exit_code_given;
static bool exiting;

static void do_print(Node *n)
{
    int fd = STDOUT_FILENO;

    if (n->a) {
        Val f = eval(n->a);
        fd = redir_fd(v_getstr(&f), n->op == 'A');
        v_free(&f);
        if (fd < 0) return;
    }

    if (n->type == N_PRINTF) {
        SB b;
        sb_init(&b);
        do_format(&b, n->args, n->nargs);
        emit(fd, b.p ? b.p : "", b.len);
        free(b.p);
        return;
    }

    SB b;
    sb_init(&b);
    if (n->nargs == 0) sb_str(&b, rec);
    for (int i = 0; i < n->nargs; i++) {
        if (i) sb_str(&b, get_str(S_OFS));
        Val v = eval(n->args[i]);
        sb_str(&b, v_getstr(&v));
        v_free(&v);
    }
    sb_str(&b, get_str(S_ORS));
    emit(fd, b.p ? b.p : "", b.len);
    free(b.p);
}

static int exec(Node *n)
{
    if (!n) return F_NORM;
    cur_line = n->line;

    switch (n->type) {
    case N_BLOCK:
        return exec_list(n->a);

    case N_EXPR: {
        Val v = eval(n->a);
        v_free(&v);
        return F_NORM;
    }

    case N_PRINT:
    case N_PRINTF:
        do_print(n);
        return F_NORM;

    case N_IF: {
        Val c = eval(n->a);
        bool t = v_true(&c);
        v_free(&c);
        return t ? exec(n->b) : exec(n->c);
    }

    case N_WHILE:
        for (;;) {
            Val c = eval(n->a);
            bool t = v_true(&c);
            v_free(&c);
            if (!t) break;
            int f = exec(n->b);
            if (f == F_BREAK) break;
            if (f == F_CONT || f == F_NORM) continue;
            return f;
        }
        return F_NORM;

    case N_FOR: {
        if (n->c) exec(n->c);
        for (;;) {
            if (n->a) {
                Val c = eval(n->a);
                bool t = v_true(&c);
                v_free(&c);
                if (!t) break;
            }
            int f = exec(n->b);
            if (f == F_BREAK) break;
            if (f != F_CONT && f != F_NORM) return f;
            if (n->nargs) exec(n->args[0]);
        }
        return F_NORM;
    }

    case N_FORIN: {
        Arr *a = arr_of((int)n->num);
        /* The keys are copied first: the body is allowed to delete
         * elements, and walking the buckets while that happens is how
         * a use-after-free gets written. */
        long cnt = a->count;
        if (cnt == 0) return F_NORM;
        char **keys = xmalloc((size_t)cnt * sizeof(char *));
        long k = 0;
        for (int i = 0; i < ABUCKETS && k < cnt; i++)
            for (AElem *e = a->b[i]; e && k < cnt; e = e->next)
                keys[k++] = xstrdup(e->key);

        int f = F_NORM;
        for (long i = 0; i < k; i++) {
            v_free(&sym[n->sym].v);
            sym[n->sym].v = v_input(keys[i]);
            f = exec(n->a);
            if (f == F_BREAK) { f = F_NORM; break; }
            if (f == F_CONT)  { f = F_NORM; continue; }
            if (f != F_NORM) break;
        }
        for (long i = 0; i < k; i++) free(keys[i]);
        free(keys);
        return f;
    }

    case N_NEXT:
        if (in_end_rule) rt_err("next cannot be used in END - there is no "
                                "next line by then");
        return F_NEXT;

    case N_BREAK:   return F_BREAK;
    case N_CONT:    return F_CONT;

    case N_EXIT:
        if (n->a) {
            Val v = eval(n->a);
            exit_code_wanted = v_getnum(&v);
            exit_code_given = true;
            v_free(&v);
        }
        exiting = true;
        return F_EXIT;

    case N_DELETE: {
        Val k = eval(n->a);
        arr_del(arr_of(n->sym), v_getstr(&k));
        v_free(&k);
        return F_NORM;
    }

    case N_DELARR:
        arr_clear(arr_of(n->sym));
        return F_NORM;

    default:
        return F_NORM;
    }
}

static int exec_list(Node *n)
{
    for (; n; n = n->next) {
        int f = exec(n);
        if (f != F_NORM) return f;
    }
    return F_NORM;
}

/* ── input ───────────────────────────────────────────────────────── */

static int   in_fd = -1;
static char  inbuf[8192];
static int   ibeg, iend;
static bool  ieof;

static void input_start(int fd)
{
    in_fd = fd;
    ibeg = iend = 0;
    ieof = false;
}

static bool next_record(void)
{
    const char *rs = get_str(S_RS);
    if (rs[0] == '\0')
        die("RS is empty; paragraph mode is not supported here - one "
            "record is one line, or set RS to a single character");
    if (rs[1] != '\0')
        die("RS has to be a single character here");

    char sep = rs[0];
    int rlen = 0;
    bool any = false;

    for (;;) {
        int i = ibeg;
        while (i < iend && inbuf[i] != sep) i++;

        int take = i - ibeg;
        if (take > 0) {
            any = true;
            int room = RECMAX - 1 - rlen;
            int copy = take < room ? take : room;
            if (copy < take) { rec_truncated = true; exit_status = 1; }
            memcpy(rec + rlen, inbuf + ibeg, (size_t)copy);
            rlen += copy;
            ibeg = i;
        }

        if (i < iend) {                     /* found the separator */
            ibeg = i + 1;
            rec[rlen] = '\0';
            return true;
        }

        if (ieof) {
            if (!any) return false;
            rec[rlen] = '\0';
            return true;
        }

        long n = lp_read(in_fd, inbuf, sizeof inbuf);
        if (n <= 0) { ieof = true; continue; }
        ibeg = 0;
        iend = (int)n;
    }
}

/* ── the main loop ───────────────────────────────────────────────── */

static bool run_main_rules(void)      /* false: exit was reached */
{
    for (int i = 0; i < nrules; i++) {
        Rule *r = &rules[i];
        if (r->kind != R_MAIN) continue;

        bool matched;
        if (!r->pat) matched = true;
        else if (r->pat2) {                   /* a range: from, to */
            if (!r->active) {
                Val a = eval(r->pat);
                matched = v_true(&a);
                v_free(&a);
                if (matched) {
                    Val b = eval(r->pat2);
                    bool ends = v_true(&b);
                    v_free(&b);
                    r->active = !ends;        /* one line long if both match */
                }
            } else {
                matched = true;
                Val b = eval(r->pat2);
                if (v_true(&b)) r->active = false;
                v_free(&b);
            }
        } else {
            Val a = eval(r->pat);
            matched = v_true(&a);
            v_free(&a);
        }
        if (!matched) continue;

        if (!r->act) {
            owrite(rec, (int)strlen(rec));
            owrite(get_str(S_ORS), (int)strlen(get_str(S_ORS)));
            continue;
        }
        int f = exec_list(r->act->a);
        if (f == F_NEXT) return true;
        if (f == F_EXIT) return false;
    }
    return true;
}

static void run_end_rules(void)
{
    in_end_rule = true;
    for (int i = 0; i < nrules; i++)
        if (rules[i].kind == R_END) {
            if (exec_list(rules[i].act->a) == F_EXIT) break;
        }
}

/* var=value on the command line, and -v. */
static bool set_assignment(const char *s)
{
    const char *eq = strchr(s, '=');
    if (!eq || eq == s) return false;

    char name[NAMEMAX];
    size_t n = (size_t)(eq - s);
    if (n >= sizeof name) return false;
    memcpy(name, s, n);
    name[n] = '\0';
    for (size_t i = 0; i < n; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '_'))
            return false;

    char value[1024];
    unescape(eq + 1, value, sizeof value);
    int idx = sym_intern(name);
    v_free(&sym[idx].v);
    sym[idx].v = v_input(value);
    return true;
}

static void usage(int fd)
{
    dprintf(fd, "usage: awk [-F fs] [-v var=value]... 'program' [file...]\n");
    dprintf(fd, "       awk [-F fs] [-v var=value]... -f progfile [file...]\n");
    dprintf(fd, "  -F fs  field separator: one character is literal, more "
                "is a regex\n");
    dprintf(fd, "  -v x=1 set a variable before BEGIN\n");
    dprintf(fd, "  -f f   read the program from a file\n");
    dprintf(fd, "  rules are `pattern { action }`; BEGIN and END run "
                "before and after\n");
    dprintf(fd, "  print printf if while for for(k in a) next exit "
                "delete break continue\n");
    dprintf(fd, "  length substr index split sub gsub match sprintf "
                "tolower toupper int system\n");
    dprintf(fd, "  whole numbers only - there is no floating point here, "
                "so 7 / 2 is 3\n");
    dprintf(fd, "  awk -F: '$3 >= 1000 { print $1 }' /etc/passwd\n");
}

/* The program text, from the argument or from -f. A 32KB awk program
 * is already far past what belongs on this system. */
static char progbuf[32768];

static void read_prog_file(const char *path)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "awk: %s: cannot read the program - check the name\n", path);
        lp_exit(2);
    }
    size_t len = strlen(progbuf);
    for (;;) {
        long n = lp_read((int)fd, progbuf + len, sizeof progbuf - len - 2);
        if (n <= 0) break;
        len += (size_t)n;
        if (len + 2 >= sizeof progbuf) {
            dprintf(STDERR_FILENO,
                    "awk: %s: the program is larger than 32KB\n", path);
            lp_exit(2);
        }
    }
    progbuf[len] = '\n';
    progbuf[len + 1] = '\0';
    lp_close((int)fd);
}

int main(int argc, char **argv)
{
    /* The built-in variables, in the order the S_ constants say. */
    sym_intern("NR"); sym_intern("NF"); sym_intern("FS"); sym_intern("OFS");
    sym_intern("ORS"); sym_intern("RS"); sym_intern("FILENAME");
    sym_intern("RSTART"); sym_intern("RLENGTH");
    set_num(S_NR, 0);
    set_num(S_NF, 0);
    set_str(S_FS, " ");
    set_str(S_OFS, " ");
    set_str(S_ORS, "\n");
    set_str(S_RS, "\n");
    set_str(S_FILENAME, "");
    set_num(S_RSTART, 0);
    set_num(S_RLENGTH, -1);

    bool have_prog = false;
    int i = 1;

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }

        if (strcmp(a, "-h") == 0) { usage(STDOUT_FILENO); return 0; }

        if (a[1] == 'F') {
            const char *v = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!v) { dprintf(STDERR_FILENO, "awk: -F needs a separator, as in -F:\n"); return 2; }
            char fs[64];
            unescape(v, fs, sizeof fs);
            if (fs[0] == '\0') {
                dprintf(STDERR_FILENO,
                        "awk: -F was given nothing to split on\n");
                return 2;
            }
            set_str(S_FS, fs);
            continue;
        }
        if (a[1] == 'v') {
            const char *v = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!v || !set_assignment(v)) {
                dprintf(STDERR_FILENO,
                        "awk: -v needs var=value, as in -v n=1\n");
                return 2;
            }
            continue;
        }
        if (a[1] == 'f') {
            const char *v = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!v) {
                dprintf(STDERR_FILENO, "awk: -f needs the name of a file\n");
                return 2;
            }
            read_prog_file(v);
            have_prog = true;
            continue;
        }
        dprintf(STDERR_FILENO,
                "awk: %s is not an option i know; awk -h lists them\n", a);
        return 2;
    }

    if (!have_prog) {
        if (i >= argc) { usage(STDERR_FILENO); return 2; }
        if (strlcpy(progbuf, argv[i++], sizeof progbuf) >= sizeof progbuf) {
            dprintf(STDERR_FILENO,
                    "awk: the program is larger than 32KB - put it in a "
                    "file and use awk -f\n");
            return 2;
        }
    }

    parse_program(progbuf);

    /* BEGIN */
    for (int r = 0; r < nrules && !exiting; r++)
        if (rules[r].kind == R_BEGIN)
            if (exec_list(rules[r].act->a) == F_EXIT) break;

    /* The main loop only reads anything if some rule wants a line. A
     * program that is only BEGIN must not sit waiting on the keyboard. */
    if (!exiting && has_main_or_end) {
        char **files = &argv[i];
        int nfiles = argc - i;

        for (int fi = 0; fi < nfiles || (fi == 0 && nfiles == 0); fi++) {
            const char *path = (nfiles == 0) ? "-" : files[fi];
            int fd;

            if (strcmp(path, "-") == 0) {
                fd = STDIN_FILENO;
                set_str(S_FILENAME, "-");
            } else {
                long f = lp_open(path, O_RDONLY, 0);
                if (f < 0) {
                    if (strchr(path, '=')) {
                        dprintf(STDERR_FILENO,
                                "awk: %s: cannot open it - if you meant to "
                                "set a variable, use -v %s\n", path, path);
                    } else {
                        dprintf(STDERR_FILENO,
                                "awk: %s: cannot open it - check the name "
                                "with ls\n", path);
                    }
                    exit_status = 2;
                    continue;
                }
                fd = (int)f;
                set_str(S_FILENAME, path);
            }

            input_start(fd);
            while (next_record()) {
                set_num(S_NR, get_num(S_NR) + 1);
                split_record();
                if (!run_main_rules()) break;
            }
            if (fd != STDIN_FILENO) lp_close(fd);
            if (exiting) break;
        }
    }

    run_end_rules();

    if (rec_truncated)
        dprintf(STDERR_FILENO,
                "awk: at least one line was longer than %d bytes and was "
                "cut short\n", RECMAX - 1);

    oflush();
    redir_close_all();
    /* `exit 3` means 3, even when a file could not be opened earlier. */
    return exit_code_given ? (int)exit_code_wanted : exit_status;
}
