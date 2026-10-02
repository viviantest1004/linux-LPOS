/* expand - turn tabs into spaces.
 *
 *   expand file             stops every 8 columns
 *   expand -t 4 file        every 4
 *   expand -t 1,7,25 file   at exactly those columns
 *   expand -i file          only the indent; leave tabs inside the text
 *
 * A tab is not "some spaces". It means "move to the next stop", so how
 * wide it turns out to be depends on where the line already is, and
 * everything awkward here follows from that. The column has to be
 * counted byte by byte from the start of every line; a backspace has to
 * move it back again, or a line that overprints comes out shifted; and
 * once the explicit list of stops is used up a tab is worth exactly one
 * space, which looks arbitrary until you try to write down a better
 * answer for "the next stop after the last one".
 *
 * The file list is one stream, not a loop over files. That is deliberate
 * and it is GNU's behaviour: if the first file ends without a newline,
 * the first line of the second file continues it and keeps its column.
 *
 * Columns are bytes. GNU counts bytes in the C locale too, so a UTF-8
 * line lands in the same place under both - both of them are wrong about
 * Hangul, and being wrong the same way is the point.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define PROG "expand"

static int  status = 0;
static bool convert_entire_line = true;   /* -i turns this off */

/* ── output ───────────────────────────────────────────────────────────
 * Buffered because expand touches every byte of the file and one write
 * syscall per character is the difference between instant and not. */
static char   obuf[65536];
static size_t olen;

static void oflush(void)
{
    size_t off = 0;
    while (off < olen) {
        long w = lp_write(STDOUT_FILENO, obuf + off, olen - off);
        if (w <= 0) {
            dprintf(STDERR_FILENO, PROG ": write error: %s\n",
                    lp_strerror(w < 0 ? (int)-w : 0));
            lp_exit(1);
        }
        off += (size_t)w;
    }
    olen = 0;
}

static void oput(int c)
{
    if (olen == sizeof obuf)
        oflush();
    obuf[olen++] = (char)c;
}

/* ── the list of tab stops ────────────────────────────────────────────
 *
 * Three shapes, and they combine:
 *   3,7,11   stops at those columns
 *   /4       after the explicit stops, every multiple of 4
 *   +4       after the explicit stops, every 4 from the last one
 * With no explicit stops at all, or with exactly one and no /N or +N,
 * the whole thing collapses to a single uniform width. */
static u64   *tab_list;
static size_t n_tabs, tabs_alloc;
static u64    extend_size;      /* the N of "/N" */
static u64    increment_size;   /* the N of "+N" */
static u64    tab_size;         /* non-zero when the stops are uniform */
static u64    max_column_width = 0;

static bool is_digit(int c) { return c >= '0' && c <= '9'; }
static bool is_blank(int c) { return c == ' ' || c == '\t'; }

static void oom(void)
{
    dprintf(STDERR_FILENO, PROG ": memory exhausted\n");
    lp_exit(1);
}

static void add_tab_stop(u64 v)
{
    u64 prev = n_tabs ? tab_list[n_tabs - 1] : 0;
    u64 width = prev <= v ? v - prev : 0;

    if (n_tabs == tabs_alloc) {
        tabs_alloc = tabs_alloc ? tabs_alloc * 2 : 16;
        u64 *nl = realloc(tab_list, tabs_alloc * sizeof *tab_list);
        if (!nl) oom();
        tab_list = nl;
    }
    tab_list[n_tabs++] = v;

    if (max_column_width < width)
        max_column_width = width;
}

static bool set_extend_size(u64 v)
{
    if (extend_size) {
        dprintf(STDERR_FILENO,
                PROG ": '/' specifier only allowed with the last value\n");
        return false;
    }
    extend_size = v;
    return true;
}

static bool set_increment_size(u64 v)
{
    if (increment_size) {
        dprintf(STDERR_FILENO,
                PROG ": '+' specifier only allowed with the last value\n");
        return false;
    }
    increment_size = v;
    return true;
}

/* Accumulate one decimal digit, refusing to wrap around. */
static bool accum(u64 *v, int digit)
{
    if (*v > (~(u64)0 - (u64)digit) / 10)
        return false;
    *v = *v * 10 + (u64)digit;
    return true;
}

/* -t may be given more than once and the lists join end to end, which is
 * why this appends rather than replacing. */
static void parse_tab_stops(const char *stops)
{
    bool have = false, want_extend = false, want_incr = false, ok = true;
    u64  val = 0;
    const char *num_start = NULL;

    for (; *stops; stops++) {
        int c = (unsigned char)*stops;

        if (c == ',' || is_blank(c)) {
            if (have) {
                if (want_extend)      ok = set_extend_size(val);
                else if (want_incr)   ok = set_increment_size(val);
                else                  add_tab_stop(val);
                if (!ok) break;
            }
            have = want_extend = want_incr = false;
        } else if (c == '/') {
            if (have) {
                dprintf(STDERR_FILENO,
                        PROG ": '/' specifier not at start of number: '%s'\n",
                        stops);
                ok = false;
            }
            want_extend = true;
        } else if (c == '+') {
            if (have) {
                dprintf(STDERR_FILENO,
                        PROG ": '+' specifier not at start of number: '%s'\n",
                        stops);
                ok = false;
            }
            want_incr = true;
        } else if (is_digit(c)) {
            if (!have) { val = 0; have = true; num_start = stops; }
            if (!accum(&val, c - '0')) {
                size_t len = 0;
                while (is_digit((unsigned char)num_start[len])) len++;
                dprintf(STDERR_FILENO,
                        PROG ": tab stop is too large '%.*s'\n",
                        (int)len, num_start);
                ok = false;
                have = false;                  /* a number that did not fit
                                                * is not a tab stop */
                stops = num_start + len - 1;   /* skip the rest of the number */
            }
        } else {
            dprintf(STDERR_FILENO,
                    PROG ": tab size contains invalid character(s): '%s'\n",
                    stops);
            ok = false;
            break;
        }
    }

    if (ok && have) {
        if (want_extend)    ok = set_extend_size(val);
        else if (want_incr) ok = set_increment_size(val);
        else                add_tab_stop(val);
    }

    if (!ok)
        lp_exit(1);
}

static void finalize_tab_stops(void)
{
    u64 prev = 0;
    for (size_t i = 0; i < n_tabs; i++) {
        if (tab_list[i] == 0) {
            dprintf(STDERR_FILENO, PROG ": tab size cannot be 0\n");
            lp_exit(1);
        }
        if (tab_list[i] <= prev) {
            dprintf(STDERR_FILENO, PROG ": tab sizes must be ascending\n");
            lp_exit(1);
        }
        prev = tab_list[i];
    }
    if (increment_size && extend_size) {
        dprintf(STDERR_FILENO,
                PROG ": '/' specifier is mutually exclusive with '+'\n");
        lp_exit(1);
    }

    if (n_tabs == 0)
        tab_size = max_column_width =
            extend_size ? extend_size : increment_size ? increment_size : 8;
    else if (n_tabs == 1 && !extend_size && !increment_size)
        tab_size = max_column_width = tab_list[0];
    else
        tab_size = 0;
}

/* The first stop strictly past `column`. *last says the stops ran out,
 * which the caller turns into "one space". tab_index only ever moves
 * forward, so a long line does not rescan the list per tab. */
static u64 next_tab_column(u64 column, size_t *tab_index, bool *last)
{
    *last = false;

    if (tab_size)
        return column + (tab_size - column % tab_size);

    for (; *tab_index < n_tabs; (*tab_index)++)
        if (column < tab_list[*tab_index])
            return tab_list[*tab_index];

    if (extend_size)
        return column + (extend_size - column % extend_size);
    if (increment_size) {
        u64 end = n_tabs ? tab_list[n_tabs - 1] : 0;
        return column + (increment_size - ((column - end) % increment_size));
    }

    *last = true;
    return 0;
}

/* ── input ────────────────────────────────────────────────────────────
 * One stream over the whole operand list, as described at the top. */
static char   ibuf[65536];
static size_t ilen, ipos;
static int    ifd = -1;
static const char *iname;
static bool   istdin;
static char **flist;

static bool next_file(void)
{
    if (ifd >= 0 && !istdin)
        lp_close(ifd);
    ifd = -1;

    while (*flist) {
        const char *f = *flist++;
        ilen = ipos = 0;
        if (strcmp(f, "-") == 0) {
            ifd = STDIN_FILENO; iname = f; istdin = true;
            return true;
        }
        long fd = lp_open(f, O_RDONLY, 0);
        if (fd < 0) {
            oflush();
            lp_diag(PROG, NULL, NULL, "cannot open", f, (int)-fd);
            status = 1;
            continue;
        }
        ifd = (int)fd; iname = f; istdin = false;
        return true;
    }
    return false;
}

static int igetc(void)
{
    for (;;) {
        if (ipos < ilen)
            return (unsigned char)ibuf[ipos++];
        if (ifd < 0)
            return -1;
        long n = lp_read(ifd, ibuf, sizeof ibuf);
        if (n > 0) { ilen = (size_t)n; ipos = 0; continue; }
        /* A directory opens fine and only fails here, which is why the
         * "Is a directory" complaint belongs to the read and not the
         * open. */
        if (n < 0) {
            oflush();
            lp_diag(PROG, NULL, NULL, "read error", iname, (int)-n);
            status = 1;
        }
        if (!next_file())
            return -1;
    }
}

static void usage(void)
{
    printf("Usage: expand [OPTION]... [FILE]...\n"
           "Convert tabs in each FILE to spaces, writing to standard output.\n\n"
           "With no FILE, or when FILE is -, read standard input.\n\n"
           "  -i, --initial    do not convert tabs after non blanks\n"
           "  -t, --tabs=N     have tabs N characters apart, not 8\n"
           "  -t, --tabs=LIST  use comma separated list of tab positions.\n"
           "                     The last specified position can be prefixed with '/'\n"
           "                     to specify a tab size to use after the last\n"
           "                     explicitly specified tab stop.  Also a prefix of '+'\n"
           "                     can be used to align remaining tab stops relative to\n"
           "                     the last specified tab stop instead of the first column\n"
           "      --help        display this help and exit\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "initial", 0, 'i' }, { "tabs", 1, 't' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    /* `expand -3` is the historical spelling of `expand -t 3` and is
     * still in makefiles. Each digit and comma is an option letter whose
     * value is whatever is attached to it, so the whole word arrives in
     * one piece and goes to the same parser -t uses. That is why
     * `expand -12` means one stop at 12 while `expand -1 -2` means two.
     * unexpand spells the same idea differently; both are GNU's. */
    lp_getopt_init(&g, argc, argv,
                   ",::0::1::2::3::4::5::6::7::8::9::it:", lo);

    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'i': convert_entire_line = false; break;
        case 't':
            if (!g.arg) {
                dprintf(STDERR_FILENO,
                        PROG ": option requires an argument -- 't'\n"
                        "Try '" PROG " --help' for more information.\n");
                return 1;
            }
            parse_tab_stops(g.arg);
            break;
        case 'H': usage(); return 0;
        default:
            if (c == ',' || is_digit(c)) {
                size_t n = g.arg ? strlen(g.arg) : 0;
                char *word = malloc(n + 2);
                if (!word) oom();
                word[0] = (char)c;
                if (n) memcpy(word + 1, g.arg, n);
                word[n + 1] = '\0';
                parse_tab_stops(word);
                free(word);
                break;
            }
            lp_getopt_err(PROG, &g);
            return 1;
        }
    }

    finalize_tab_stops();

    static char *just_stdin[2] = { (char *)"-", NULL };
    flist = (g.ind < argc) ? argv + g.ind : just_stdin;
    if (!next_file()) {
        oflush();
        return status;
    }

    for (;;) {
        int  c;
        bool convert = true;
        u64  column = 0;
        size_t tab_index = 0;

        do {
            c = igetc();

            if (convert) {
                if (c == '\t') {
                    bool last;
                    u64 next = next_tab_column(column, &tab_index, &last);
                    if (last)
                        next = column + 1;
                    while (++column < next)
                        oput(' ');
                    c = ' ';
                } else if (c == '\b') {
                    /* Overprinting moves the cursor back, so the next tab
                     * is measured from where the cursor really is. */
                    if (column)    column--;
                    if (tab_index) tab_index--;
                } else if (c >= 0) {
                    column++;
                }
                convert &= convert_entire_line || is_blank(c);
            }

            if (c < 0) {
                oflush();
                return status;
            }
            oput(c);
        } while (c != '\n');
    }
}
