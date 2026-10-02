/* sed - a stream editor.
 *
 *   sed [-n] [-E] [-i] [-e script]... [-f file]... [script] [file]...
 *
 *   -n  print nothing unless a command says to
 *   -E  extended regular expressions: ( ) | + ? { } without backslashes
 *   -i  edit each file in place instead of writing to the screen
 *   -e  one more script, in order
 *   -f  take a script from a file
 *
 * Commands, each of them optionally addressed:
 *
 *   s/from/to/[g][N][i][p]  substitute; g every match, N the Nth one
 *                           onwards, i ignoring case, p print the line
 *   p   print it            d   delete it          q   stop here
 *   y/abc/xyz/              transliterate, byte for byte
 *   a\text  after it        i\text  before it      c\text  instead of it
 *
 * Addresses: N a line number, $ the last line, /re/ a match, N,M and
 * /re/,/re/ a range, and a trailing ! to mean every line but those.
 *
 * In the replacement & is the whole match, \1 to \9 are the groups, \n
 * and \t are what they look like, and \& \\ are those two characters.
 *
 *   sed 's/apple/pear/g' notes.txt
 *   sed -n '/error/p' /var/log/messages
 *   sed -i -e '1,3d' -e 's/tabs/spaces/g' config
 *
 * ── the decisions ────────────────────────────────────────────────────
 *
 * The matching is the shared engine in libc/include/regex.h, not grep's.
 * grep has its own because when it was written nothing else needed one;
 * that one has no groups, and \1 in a replacement is the whole point of
 * sed. Two ideas of what a regular expression means in one system is one
 * too many, so this uses the shared one and grep can move over later.
 *
 * The pattern space is a fixed 4096 bytes rather than a buffer that
 * grows. The whole system unpacks into RAM, so a megabyte spent on a
 * line nobody has is a megabyte gone from every board. A line longer
 * than that is truncated on the way in, and a substitution whose result
 * would not fit is refused with a message and a non-zero exit rather
 * than written out half done - a silently shortened line in a config
 * file is far worse than a command that stops and says so.
 *
 * -i writes a new file next to the old one and renames it over the top.
 * Writing to the file being read would truncate it under the reader, and
 * even done carefully it leaves a window where a power cut - the normal
 * way these boards are turned off - lands in the middle of the file. A
 * rename on one filesystem is atomic, so what survives is either the old
 * file entire or the new one entire and never half of each. The
 * temporary has to be a sibling of the target for that: /tmp is very
 * often another mount, and a rename across mounts fails.
 *
 * There is no hold space, no b/t/: branching and no { } blocks. Those
 * turn sed into a programming language with a jump table, and every use
 * of it here is a one-liner. A script that asks for them is refused by
 * name rather than half understood.
 *
 * Line numbers run across all the input files as one stream, as sed has
 * always done, so $ is the last line of the last file. With -i each file
 * is its own stream instead: editing files in place has to mean that
 * `sed -i '1d' a b` drops the first line of both.
 *
 * A last line with no newline after it gets one. readline() does not
 * report the difference and adding a way to ask was not worth it here.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "regex.h"

#define SPACE    4096      /* the pattern space, and one line of lookahead */
#define MAXCMD     32
#define MAXFILES   64
#define MAXAPPEND   8

enum { A_NONE = 0, A_LINE, A_LAST, A_RE };

typedef struct {
    int    a1type, a2type;
    long   a1line, a2line;
    lpre  *a1re, *a2re;
    bool   negate;
    bool   active;         /* inside a two-address range right now */
    bool   ended;          /* this line closed the range - c prints here */

    char   cmd;            /* s d p q y a i c */

    lpre  *re;             /* s */
    char  *repl;
    bool   global, print;
    int    occur;

    char  *yfrom, *yto;    /* y */
    char  *text;           /* a i c */
} cmd_t;

static cmd_t cmds[MAXCMD];
static int   ncmds;

static bool  opt_n, opt_ere, opt_i;
static int   out_fd = STDOUT_FILENO;
static int   status;
static bool  write_failed;
static long  lineno;

/* ── output ──────────────────────────────────────────────────────── */

static void emit(const char *s)
{
    size_t len = strlen(s), off = 0;

    while (off < len) {
        long w = lp_write(out_fd, s + off, len - off);
        if (w <= 0) { write_failed = true; return; }
        off += (size_t)w;
    }
    if (lp_write(out_fd, "\n", 1) != 1)
        write_failed = true;
}

/* ── input: the files as one stream ──────────────────────────────── */

static char **in_files;
static int    in_nfiles, in_next, in_fd = -1, in_fail;

static void in_begin(char **files, int n)
{
    in_files = files; in_nfiles = n; in_next = 0; in_fd = -1;
}

static void in_end(void)
{
    if (in_fd >= 0 && in_fd != STDIN_FILENO)
        lp_close(in_fd);
    in_fd = -1;
}

static bool in_open_next(void)
{
    while (in_next < in_nfiles) {
        const char *p = in_files[in_next++];

        if (strcmp(p, "-") == 0) { in_fd = STDIN_FILENO; return true; }

        long f = lp_open(p, O_RDONLY, 0);
        if (f < 0) {
            dprintf(STDERR_FILENO,
                    "sed: %s: cannot read it - check the name with ls\n", p);
            in_fail++;
            status = 1;
            continue;
        }
        in_fd = (int)f;
        return true;
    }
    return false;
}

/* The length of the line, or -1 when there is no more input. */
static long in_line(char *buf, size_t size)
{
    for (;;) {
        if (in_fd < 0 && !in_open_next())
            return -1;
        long n = readline(in_fd, buf, size);
        if (n >= 0)
            return n;
        if (in_fd != STDIN_FILENO)
            lp_close(in_fd);
        in_fd = -1;
    }
}

/* ── parsing ─────────────────────────────────────────────────────── */

static char perr_msg[160];
static int  perr_pos;

static bool bad(int pos, const char *msg)
{
    perr_pos = pos;
    strlcpy(perr_msg, msg, sizeof perr_msg);
    return false;
}

/* Copy from p up to the next unescaped delim. \<delim> becomes a plain
 * delim; every other backslash pair is passed through untouched, because
 * the regex engine and the replacement both want to see it. Returns the
 * bytes consumed including the delimiter, or -1 if it never arrives. */
static int read_delim(const char *p, char delim, char *out, size_t outsz)
{
    size_t n = 0;
    int i = 0;

    for (;;) {
        char ch = p[i];

        if (ch == '\0' || ch == '\n')
            return -1;
        if (ch == '\\' && p[i + 1] == delim) {
            if (n + 2 > outsz) return -1;
            out[n++] = delim;
            i += 2;
            continue;
        }
        if (ch == '\\' && p[i + 1]) {
            if (n + 3 > outsz) return -1;
            out[n++] = '\\';
            out[n++] = p[i + 1];
            i += 2;
            continue;
        }
        if (ch == delim) { i++; break; }
        if (n + 2 > outsz) return -1;
        out[n++] = ch;
        i++;
    }
    out[n] = '\0';
    return i;
}

/* The two sets of y, with the escapes actually turned into bytes. */
static int y_set(const char *in, char *out, size_t outsz)
{
    size_t n = 0;

    for (int i = 0; in[i]; i++) {
        char ch = in[i];
        if (ch == '\\' && in[i + 1]) {
            char e = in[++i];
            ch = (e == 'n') ? '\n' : (e == 't') ? '\t' :
                 (e == 'r') ? '\r' : e;
        }
        if (n + 2 > outsz) return -1;
        out[n++] = ch;
    }
    out[n] = '\0';
    return (int)n;
}

static bool parse_addr(const char *s, int *pi, int *type, long *line, lpre **re)
{
    int i = *pi;

    *type = A_NONE; *line = 0; *re = NULL;

    if (s[i] >= '0' && s[i] <= '9') {
        long v = 0;
        while (s[i] >= '0' && s[i] <= '9')
            v = v * 10 + (s[i++] - '0');
        *type = A_LINE;
        *line = v;
    } else if (s[i] == '$') {
        i++;
        *type = A_LAST;
    } else if (s[i] == '/') {
        char pat[256];
        int n = read_delim(s + i + 1, '/', pat, sizeof pat);
        if (n < 0)
            return bad(i + 1, "the address is missing its closing /");
        i += 1 + n;

        const char *err = NULL;
        lpre *r = re_compile(pat, opt_ere, false, &err);
        if (!r)
            return bad(i + 1, err ? err : "that regular expression is wrong");
        *type = A_RE;
        *re = r;
    }

    *pi = i;
    return true;
}

/* The text of a, i and c: to the end of the line. A backslash escapes
 * the next character, and a backslash before the newline puts a real
 * newline into the text and carries on. */
static bool parse_text(const char *s, int *pi, char **out)
{
    int i = *pi;
    char buf[512];
    size_t n = 0;

    while (s[i] == ' ' || s[i] == '\t') i++;
    if (s[i] == '\\') {
        i++;
        if (s[i] == '\n') i++;
        while (s[i] == ' ' || s[i] == '\t') i++;
    }

    while (s[i] && s[i] != '\n') {
        char ch = s[i];
        if (ch == '\\' && s[i + 1]) {
            char e = s[++i];
            ch = (e == 'n') ? '\n' : (e == 't') ? '\t' : e;
        }
        if (n + 1 >= sizeof buf)
            return bad(i + 1, "the text after a, i or c is too long");
        buf[n++] = ch;
        i++;
    }
    buf[n] = '\0';

    if (n == 0)
        return bad(i + 1, "a, i and c need some text after them");

    *out = strdup(buf);
    if (!*out)
        return bad(i + 1, "out of memory");
    *pi = i;
    return true;
}

static bool parse_script(const char *s)
{
    int i = 0;

    for (;;) {
        while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == ';')
            i++;
        if (s[i] == '#') {
            while (s[i] && s[i] != '\n') i++;
            continue;
        }
        if (!s[i])
            return true;

        if (ncmds >= MAXCMD)
            return bad(i + 1, "too many commands - this sed holds 32");

        cmd_t *c = &cmds[ncmds];
        memset(c, 0, sizeof *c);
        c->occur = 1;

        if (!parse_addr(s, &i, &c->a1type, &c->a1line, &c->a1re))
            return false;
        if (c->a1type != A_NONE) {
            while (s[i] == ' ') i++;
            if (s[i] == ',') {
                i++;
                while (s[i] == ' ') i++;
                if (!parse_addr(s, &i, &c->a2type, &c->a2line, &c->a2re))
                    return false;
                if (c->a2type == A_NONE)
                    return bad(i + 1, "the comma needs a second address after it");
            }
        }

        while (s[i] == ' ' || s[i] == '\t') i++;
        while (s[i] == '!') {
            c->negate = !c->negate;
            i++;
            while (s[i] == ' ') i++;
        }

        char m[160];
        char k = s[i];

        switch (k) {
        case 's': {
            i++;
            char d = s[i];
            if (!d || d == '\n' || d == '\\' || d == ' ')
                return bad(i + 1, "s needs a delimiter after it, as in s/from/to/");
            i++;

            char pat[512], rep[512];
            int n = read_delim(s + i, d, pat, sizeof pat);
            if (n < 0)
                return bad(i + 1, "s has no delimiter after the pattern");
            i += n;
            n = read_delim(s + i, d, rep, sizeof rep);
            if (n < 0)
                return bad(i + 1, "s has no closing delimiter");
            i += n;

            bool icase = false;
            for (;;) {
                char f = s[i];
                if (f == 'g') { c->global = true; i++; }
                else if (f == 'p') { c->print = true; i++; }
                else if (f == 'i' || f == 'I') { icase = true; i++; }
                else if (f >= '0' && f <= '9') {
                    long v = 0;
                    while (s[i] >= '0' && s[i] <= '9')
                        v = v * 10 + (s[i++] - '0');
                    if (v < 1)
                        return bad(i, "the number in s counts matches, so it starts at 1");
                    c->occur = (int)v;
                } else break;
            }
            if (s[i] && s[i] != ';' && s[i] != '\n' &&
                s[i] != ' ' && s[i] != '\t') {
                snprintf(m, sizeof m, "unknown flag '%c' on s", s[i]);
                return bad(i + 1, m);
            }

            const char *err = NULL;
            c->re = re_compile(pat, opt_ere, icase, &err);
            if (!c->re)
                return bad(i + 1, err ? err : "that regular expression is wrong");
            c->repl = strdup(rep);
            if (!c->repl)
                return bad(i + 1, "out of memory");
            c->cmd = 's';
            break;
        }

        case 'y': {
            i++;
            char d = s[i];
            if (!d || d == '\n' || d == '\\' || d == ' ')
                return bad(i + 1, "y needs a delimiter after it, as in y/abc/xyz/");
            i++;

            char raw[256], from[256], to[256];
            int n = read_delim(s + i, d, raw, sizeof raw);
            if (n < 0)
                return bad(i + 1, "y has no delimiter after the first set");
            i += n;
            int lf = y_set(raw, from, sizeof from);
            n = read_delim(s + i, d, raw, sizeof raw);
            if (n < 0)
                return bad(i + 1, "y has no closing delimiter");
            i += n;
            int lt = y_set(raw, to, sizeof to);

            if (lf < 0 || lt < 0)
                return bad(i + 1, "the sets in y are too long");
            if (lf != lt)
                return bad(i + 1, "the two sets in y must be the same length");
            if (lf == 0)
                return bad(i + 1, "the sets in y are empty");

            c->yfrom = strdup(from);
            c->yto   = strdup(to);
            if (!c->yfrom || !c->yto)
                return bad(i + 1, "out of memory");
            c->cmd = 'y';
            break;
        }

        case 'a': case 'i': case 'c':
            i++;
            if (!parse_text(s, &i, &c->text))
                return false;
            c->cmd = k;
            break;

        case 'd': case 'p': case 'q':
            i++;
            c->cmd = k;
            while (s[i] == ' ' || s[i] == '\t') i++;
            if (s[i] && s[i] != ';' && s[i] != '\n') {
                snprintf(m, sizeof m, "unexpected '%c' after the %c command",
                         s[i], k);
                return bad(i + 1, m);
            }
            break;

        case '{': case '}':
            return bad(i + 1, "{ } blocks are not supported - "
                              "give each command its own address");

        case 'b': case 't': case ':':
            return bad(i + 1, "branching is not supported - "
                              "run sed twice in a pipeline instead");

        case '\0':
            return bad(i + 1, "the address has no command after it");

        default:
            snprintf(m, sizeof m, "unknown command '%c'", k);
            return bad(i + 1, m);
        }

        ncmds++;
    }
}

/* ── addresses at run time ───────────────────────────────────────── */

static bool re_hit(lpre *re, const char *s)
{
    int caps[RE_MAX_CAPS];
    return re_search(re, s, 0, false, caps);
}

static bool one_addr(int type, long line, lpre *re,
                     long ln, bool last, const char *ps)
{
    switch (type) {
    case A_LINE: return ln == line;
    case A_LAST: return last;
    case A_RE:   return re_hit(re, ps);
    default:     return false;
    }
}

static bool addr_match(cmd_t *c, long ln, bool last, const char *ps)
{
    bool m = false;

    c->ended = false;

    if (c->a1type == A_NONE) {
        m = true;
        c->ended = true;
    } else if (c->a2type == A_NONE) {
        m = one_addr(c->a1type, c->a1line, c->a1re, ln, last, ps);
        c->ended = m;
    } else if (!c->active) {
        if (one_addr(c->a1type, c->a1line, c->a1re, ln, last, ps)) {
            m = true;
            c->active = true;
            /* 3,1p is one line: a range that ends before it starts. */
            if (last || (c->a2type == A_LINE && c->a2line <= ln)) {
                c->active = false;
                c->ended  = true;
            }
        }
    } else {
        m = true;
        if (last ||
            (c->a2type == A_LINE && ln >= c->a2line) ||
            (c->a2type == A_LAST && last) ||
            (c->a2type == A_RE   && re_hit(c->a2re, ps))) {
            c->active = false;
            c->ended  = true;
        }
    }

    return c->negate ? !m : m;
}

/* ── s ───────────────────────────────────────────────────────────── */

static char subst_out[SPACE];

static bool add(char *dst, size_t cap, size_t *len, const char *src, size_t n)
{
    if (*len + n + 1 > cap)
        return false;
    memcpy(dst + *len, src, n);
    *len += n;
    return true;
}

static bool expand_repl(const char *rep, const char *ps, const int *caps,
                        char *out, size_t cap, size_t *len)
{
    for (int i = 0; rep[i]; i++) {
        char ch = rep[i];

        if (ch == '&') {
            if (!add(out, cap, len, ps + caps[0],
                     (size_t)(caps[1] - caps[0])))
                return false;
            continue;
        }
        if (ch == '\\' && rep[i + 1]) {
            char e = rep[++i];
            if (e >= '1' && e <= '9') {
                int g = e - '0';
                if (caps[2 * g] < 0)      /* a group that took no part */
                    continue;
                if (!add(out, cap, len, ps + caps[2 * g],
                         (size_t)(caps[2 * g + 1] - caps[2 * g])))
                    return false;
                continue;
            }
            char lit = (e == 'n') ? '\n' : (e == 't') ? '\t' :
                       (e == 'r') ? '\r' : e;
            if (!add(out, cap, len, &lit, 1))
                return false;
            continue;
        }
        if (!add(out, cap, len, &ch, 1))
            return false;
    }
    return true;
}

/* 1 if the line changed, 0 if it did not, -1 if the result would not
 * fit. On -1 the pattern space is left exactly as it was. */
static int subst(cmd_t *c, char *ps)
{
    int caps[RE_MAX_CAPS];
    size_t len = 0;
    int from = 0, hits = 0;
    bool changed = false;

    for (;;) {
        for (int k = 0; k < RE_MAX_CAPS; k++)
            caps[k] = -1;
        if (!re_search(c->re, ps, from, from > 0, caps))
            break;

        int ms = caps[0], me = caps[1];
        if (!add(subst_out, sizeof subst_out, &len,
                 ps + from, (size_t)(ms - from)))
            return -1;

        hits++;
        bool take = c->global ? (hits >= c->occur) : (hits == c->occur);
        if (take) {
            if (!expand_repl(c->repl, ps, caps,
                             subst_out, sizeof subst_out, &len))
                return -1;
            changed = true;
        } else if (!add(subst_out, sizeof subst_out, &len,
                        ps + ms, (size_t)(me - ms))) {
            return -1;
        }

        if (me == ms) {
            /* An empty match would match there for ever. Take the byte
             * after it and carry on past. */
            if (!ps[me]) { from = me; break; }
            if (!add(subst_out, sizeof subst_out, &len, ps + me, 1))
                return -1;
            from = me + 1;
        } else {
            from = me;
        }

        if (!c->global && hits >= c->occur)
            break;
    }

    if (!changed)
        return 0;

    if (!add(subst_out, sizeof subst_out, &len, ps + from, strlen(ps + from)))
        return -1;
    subst_out[len] = '\0';
    memcpy(ps, subst_out, len + 1);
    return 1;
}

static void translit(cmd_t *c, char *ps)
{
    for (size_t i = 0; ps[i]; i++) {
        const char *p = strchr(c->yfrom, ps[i]);
        if (p)
            ps[i] = c->yto[p - c->yfrom];
    }
}

/* ── the loop over the input ─────────────────────────────────────── */

static char cur[SPACE], nxt[SPACE];
static bool warned_long;

/* false when a q command asked to stop. */
static bool run_stream(void)
{
    long curlen = in_line(cur, sizeof cur);

    while (curlen >= 0) {
        long nxtlen = in_line(nxt, sizeof nxt);
        bool last = nxtlen < 0;
        bool deleted = false, quit = false;
        const char *append[MAXAPPEND];
        int nappend = 0;

        lineno++;

        /* readline() drops what will not fit. Losing the end of a line
         * without saying so is how a config file quietly stops working,
         * so say so - once, because a whole file of long lines would
         * otherwise bury the output. */
        if (curlen == (long)sizeof cur - 1 && !warned_long) {
            dprintf(STDERR_FILENO,
                    "sed: line %ld is longer than the %d byte line buffer "
                    "and was cut short\n", lineno, SPACE);
            warned_long = true;
            status = 1;
        }

        for (int ci = 0; ci < ncmds; ci++) {
            cmd_t *c = &cmds[ci];

            if (!addr_match(c, lineno, last, cur))
                continue;

            switch (c->cmd) {
            case 'd':
                deleted = true;
                break;
            case 'p':
                emit(cur);
                break;
            case 'q':
                quit = true;
                break;
            case 'i':
                emit(c->text);
                break;
            case 'a':
                if (nappend < MAXAPPEND)
                    append[nappend++] = c->text;
                break;
            case 'c':
                deleted = true;
                if (c->ended)
                    emit(c->text);
                break;
            case 'y':
                translit(c, cur);
                break;
            case 's': {
                int r = subst(c, cur);
                if (r < 0) {
                    dprintf(STDERR_FILENO,
                            "sed: line %ld: the result is longer than the "
                            "%d byte line buffer - the line is left alone\n",
                            lineno, SPACE);
                    status = 1;
                } else if (r > 0 && c->print) {
                    emit(cur);
                }
                break;
            }
            default:
                break;
            }

            if (deleted || quit)
                break;
        }

        if (!deleted && !opt_n)
            emit(cur);
        for (int a = 0; a < nappend; a++)
            emit(append[a]);

        if (quit || write_failed)
            return false;

        strlcpy(cur, nxt, sizeof cur);
        curlen = nxtlen;
    }
    return true;
}

/* ── driving it ──────────────────────────────────────────────────── */

static void usage(void)
{
    printf("usage: sed [-n] [-E] [-i] [-e script]... [-f file]... "
           "[script] [file]...\n");
    printf("  -n  print only what p asks for   -E  extended expressions\n");
    printf("  -i  edit the files in place      -e  one more script\n");
    printf("  -f  read the script from a file\n");
    printf("commands: s/from/to/[g][N][i][p]  p  d  q  y/abc/xyz/\n");
    printf("          a\\text  i\\text  c\\text\n");
    printf("addresses: N  $  /re/  N,M  /re/,/re/  and ! to invert\n");
    printf("  sed -n '/error/p' log      sed -i 's/old/new/g' file\n");
}

static bool parse_script_file(const char *path)
{
    static char buf[4096];

    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "sed: %s: cannot read the script - check the name with ls\n",
                path);
        return false;
    }
    long got = lp_read((int)fd, buf, sizeof buf - 1);
    lp_close((int)fd);
    if (got < 0) {
        dprintf(STDERR_FILENO, "sed: %s: read failed\n", path);
        return false;
    }
    buf[got] = '\0';

    if (!parse_script(buf)) {
        dprintf(STDERR_FILENO, "sed: %s: %s at position %d\n",
                path, perr_msg, perr_pos);
        return false;
    }
    return true;
}

static void reset_ranges(void)
{
    for (int i = 0; i < ncmds; i++) {
        cmds[i].active = false;
        cmds[i].ended  = false;
    }
}

/* One file, rewritten in place. See the note at the top about why this
 * goes through a sibling temporary and a rename. Returns false when a q
 * command asked for everything to stop. */
static bool edit_in_place(char *path)
{
    char tmp[512];

    if (strcmp(path, "-") == 0) {
        dprintf(STDERR_FILENO,
                "sed: -i cannot edit standard input - name a file\n");
        status = 1;
        return true;
    }
    if (snprintf(tmp, sizeof tmp, "%s.sed-tmp", path) >= (int)sizeof tmp) {
        dprintf(STDERR_FILENO, "sed: %s: the path is too long to edit\n", path);
        status = 1;
        return true;
    }

    lp_stat_t st;
    bool have_mode = lp_stat(path, &st, true) == 0;

    /* The temporary is a sibling so that the rename below stays inside
     * one filesystem, where it is atomic. 0600 until it is in place. */
    long fd = lp_open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "sed: %s: cannot create it - is the directory writable?\n", tmp);
        status = 1;
        return true;
    }

    int before = in_fail;

    out_fd = (int)fd;
    write_failed = false;
    lineno = 0;
    reset_ranges();

    in_begin(&path, 1);
    bool carry_on = run_stream();
    in_end();

    lp_close((int)fd);
    out_fd = STDOUT_FILENO;

    bool lost = write_failed;          /* in_open_next has spoken already */
    write_failed = false;

    if (lost || in_fail != before) {
        lp_unlink(tmp);
        if (lost)
            dprintf(STDERR_FILENO,
                    "sed: %s: the write failed - the file is as it was, "
                    "check the free space with df\n", path);
        status = 1;
        return carry_on;
    }

    if (have_mode)
        lp_chmod(tmp, st.mode & 07777);

    if (lp_rename(tmp, path) < 0) {
        lp_unlink(tmp);
        dprintf(STDERR_FILENO,
                "sed: %s: cannot replace it - check the permissions on the "
                "directory\n", path);
        status = 1;
        return carry_on;
    }
    lp_sync();          /* the power can go at any moment on an SD card */
    return carry_on;
}

int main(int argc, char **argv)
{
    char *files[MAXFILES];
    int   nfiles = 0;
    bool  script_from_option = false, have_script = false;

    /* Two passes over argv. -E has to be known before any script is
     * compiled, so the flags are read first; both loops step over the
     * value of -e and -f so it can never be mistaken for a file name. */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-h") == 0) { usage(); return 0; }
        if (strcmp(a, "--") == 0) break;
        if (strcmp(a, "-n") == 0) { opt_n = true; continue; }
        if (strcmp(a, "-E") == 0) { opt_ere = true; continue; }
        if (strcmp(a, "-i") == 0) { opt_i = true; continue; }
        if (strcmp(a, "-e") == 0 || strcmp(a, "-f") == 0) {
            if (i + 1 >= argc) {
                dprintf(STDERR_FILENO, "sed: %s needs a %s after it\n", a,
                        a[1] == 'e' ? "script" : "file name");
                return 2;
            }
            i++;
            script_from_option = true;
            continue;
        }
        if (a[0] == '-' && a[1]) {
            dprintf(STDERR_FILENO,
                    "sed: unknown option %s - try sed -h\n", a);
            return 2;
        }
    }

    bool only_files = false;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];

        if (!only_files) {
            if (strcmp(a, "--") == 0) { only_files = true; continue; }
            if (strcmp(a, "-n") == 0 || strcmp(a, "-E") == 0 ||
                strcmp(a, "-i") == 0)
                continue;
            if (strcmp(a, "-e") == 0) {
                if (!parse_script(argv[++i])) {
                    dprintf(STDERR_FILENO, "sed: %s at position %d\n",
                            perr_msg, perr_pos);
                    return 2;
                }
                have_script = true;
                continue;
            }
            if (strcmp(a, "-f") == 0) {
                if (!parse_script_file(argv[++i]))
                    return 2;
                have_script = true;
                continue;
            }
            if (!script_from_option && !have_script) {
                if (!parse_script(a)) {
                    dprintf(STDERR_FILENO, "sed: %s at position %d\n",
                            perr_msg, perr_pos);
                    return 2;
                }
                have_script = true;
                continue;
            }
        }

        if (nfiles >= MAXFILES) {
            dprintf(STDERR_FILENO,
                    "sed: too many files - %d at a time, use xargs\n",
                    MAXFILES);
            return 2;
        }
        files[nfiles++] = a;
    }

    if (!have_script) {
        dprintf(STDERR_FILENO,
                "sed: no script given - try sed -h\n");
        return 2;
    }
    if (ncmds == 0) {
        dprintf(STDERR_FILENO,
                "sed: the script has no commands in it\n");
        return 2;
    }

    if (opt_i) {
        if (nfiles == 0) {
            dprintf(STDERR_FILENO,
                    "sed: -i needs the files to edit named on the line\n");
            return 2;
        }
        for (int f = 0; f < nfiles; f++)
            if (!edit_in_place(files[f]))
                break;          /* q stops the whole run, as it does in a pipe */
    } else {
        char *dash = "-";
        in_begin(nfiles ? files : &dash, nfiles ? nfiles : 1);
        run_stream();
        in_end();
        if (write_failed) {
            dprintf(STDERR_FILENO, "sed: write failed\n");
            status = 1;
        }
    }

    return status;
}
