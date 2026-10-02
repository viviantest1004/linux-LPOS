/* unexpand - turn runs of spaces back into tabs.
 *
 *   unexpand file        only the indent, tab stops every 8
 *   unexpand -a file     everywhere on the line
 *   unexpand -t 4 file   stops every 4, and -a with it
 *
 * The rule that makes this harder than expand: a run of blanks may only
 * become a tab when the tab lands in exactly the same column, and a run
 * that ends before a stop has to stay as spaces. So blanks cannot be
 * written as they are read - they are held until something decides what
 * they are worth, which is what all the "pending" bookkeeping below is.
 *
 * One space sitting immediately before a tab stop is the awkward case.
 * It cannot become a tab on its own (a tab from the previous stop would
 * land in the same place, but a tab from *this* column would not), and
 * yet if more blanks follow it, the pair of them can: the space becomes
 * the tab and the rest stay spaces. That is what one_blank_before_stop
 * remembers, and getting it wrong shifts text by one column - the kind
 * of bug that only shows up in somebody else's makefile.
 *
 * By default only the leading blanks are touched. Tabs inside a line are
 * usually column separators in data, and turning "a   b" into "a<tab>b"
 * changes what cut and awk see.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define PROG "unexpand"

static int  status = 0;
static bool convert_entire_line = false;   /* -a and -t turn this on */

/* ── output ── */
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

static void owrite(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        oput((unsigned char)p[i]);
}

/* ── the list of tab stops ──
 * The same three shapes expand understands: explicit columns, "/N" for
 * every multiple of N afterwards, "+N" for every N from the last one. */
static u64   *tab_list;
static size_t n_tabs, tabs_alloc;
static u64    extend_size;
static u64    increment_size;
static u64    tab_size;            /* non-zero when the stops are uniform */
static u64    max_column_width;

static bool is_digit(int c) { return c >= '0' && c <= '9'; }
static bool is_blank(int c) { return c == ' ' || c == '\t'; }

static void oom(void)
{
    dprintf(STDERR_FILENO, PROG ": memory exhausted\n");
    lp_exit(1);
}

static void add_tab_stop(u64 v)
{
    u64 prev  = n_tabs ? tab_list[n_tabs - 1] : 0;
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

static bool accum(u64 *v, int digit)
{
    if (*v > (~(u64)0 - (u64)digit) / 10)
        return false;
    *v = *v * 10 + (u64)digit;
    return true;
}

static void parse_tab_stops(const char *stops)
{
    bool have = false, want_extend = false, want_incr = false, ok = true;
    u64  val = 0;
    const char *num_start = NULL;

    for (; *stops; stops++) {
        int c = (unsigned char)*stops;

        if (c == ',' || is_blank(c)) {
            if (have) {
                if (want_extend)    ok = set_extend_size(val);
                else if (want_incr) ok = set_increment_size(val);
                else                add_tab_stop(val);
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
                have = false;
                stops = num_start + len - 1;
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

/* ── input: the operand list read as one stream ── */
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
        if (n < 0) {
            oflush();
            lp_diag(PROG, NULL, NULL, "read error", iname, (int)-n);
            status = 1;
        }
        if (!next_file())
            return -1;
    }
}

/* The held-back blanks. It grows rather than being sized from the widest
 * tab stop, because "/N" and "+N" can put a stop further out than any
 * gap in the explicit list. */
static char   *pending_blank;
static size_t  pending_cap;

static void pending_room(size_t want)
{
    if (want <= pending_cap)
        return;
    size_t cap = pending_cap ? pending_cap : 64;
    while (cap < want) cap *= 2;
    char *p = realloc(pending_blank, cap);
    if (!p) oom();
    pending_blank = p;
    pending_cap = cap;
}

static void usage(void)
{
    printf("Usage: unexpand [OPTION]... [FILE]...\n"
           "Convert blanks in each FILE to tabs, writing to standard output.\n\n"
           "With no FILE, or when FILE is -, read standard input.\n\n"
           "  -a, --all        convert all blanks, instead of just initial blanks\n"
           "      --first-only  convert only leading sequences of blanks (overrides -a)\n"
           "  -t, --tabs=N     have tabs N characters apart instead of 8 (enables -a)\n"
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
        { "all", 0, 'a' }, { "first-only", 0, 'F' }, { "tabs", 1, 't' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    /* The obsolete `unexpand -4`: the digits of one number may be spread
     * over several options, and a comma ends it. This is not the same
     * grammar expand's -4 uses, and the difference is GNU's, not ours. */
    lp_getopt_init(&g, argc, argv, ",0123456789at:", lo);

    bool convert_first_only = false;
    bool have_tabval = false;
    u64  tabval = 0;

    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'a': convert_entire_line = true; break;
        case 'F': convert_first_only = true; break;
        case 't':
            if (!g.arg) {
                dprintf(STDERR_FILENO,
                        PROG ": option requires an argument -- 't'\n"
                        "Try '" PROG " --help' for more information.\n");
                return 1;
            }
            convert_entire_line = true;
            parse_tab_stops(g.arg);
            break;
        case ',':
            if (have_tabval) add_tab_stop(tabval);
            have_tabval = false;
            break;
        case 'H': usage(); return 0;
        default:
            if (is_digit(c)) {
                if (!have_tabval) { tabval = 0; have_tabval = true; }
                if (!accum(&tabval, c - '0')) {
                    dprintf(STDERR_FILENO, PROG ": tab stop value is too large\n");
                    return 1;
                }
                break;
            }
            lp_getopt_err(PROG, &g);
            return 1;
        }
    }
    if (have_tabval)
        add_tab_stop(tabval);

    if (convert_first_only)
        convert_entire_line = false;

    finalize_tab_stops();
    pending_room(max_column_width + 1 > 64 ? max_column_width + 1 : 64);

    static char *just_stdin[2] = { (char *)"-", NULL };
    flist = (g.ind < argc) ? argv + g.ind : just_stdin;
    if (!next_file()) {
        oflush();
        return status;
    }

    for (;;) {
        int    c;
        bool   convert = true;
        u64    column = 0;
        u64    next_stop = 0;
        size_t tab_index = 0;
        /* The start of a line counts as "after a blank", so a stop in
         * column one turns the first space into a tab. */
        bool   prev_blank = true;
        bool   one_blank_before_stop = false;
        size_t pending = 0;

        do {
            c = igetc();

            if (convert) {
                bool blank = c >= 0 && is_blank(c);

                if (blank) {
                    bool last;
                    next_stop = next_tab_column(column, &tab_index, &last);
                    if (last)
                        convert = false;      /* past the stops: leave it alone */

                    if (convert) {
                        if (c == '\t') {
                            column = next_stop;
                            /* A tab already here makes the held blanks
                             * pointless: this tab reaches the same column
                             * whether they are there or not. The one that
                             * survives is a run that had already crossed a
                             * stop - it becomes a tab of its own, and this
                             * one carries on from there. */
                            if (one_blank_before_stop) {
                                pending_blank[0] = '\t';
                                pending = 1;
                            } else {
                                pending = 0;
                            }
                            one_blank_before_stop = false;
                        } else {
                            column++;
                            if (!(prev_blank && column == next_stop)) {
                                if (column == next_stop)
                                    one_blank_before_stop = true;
                                pending_room(pending + 1);
                                pending_blank[pending++] = (char)c;
                                prev_blank = true;
                                continue;
                            }
                            /* The run reaches this stop exactly, so it
                             * becomes a tab. */
                            pending_blank[0] = (char)(c = '\t');
                            pending = one_blank_before_stop ? 1 : 0;
                        }
                    }
                } else if (c == '\b') {
                    if (column) column--;
                    next_stop = column;
                    if (tab_index) tab_index--;
                } else if (c >= 0) {
                    column++;
                }

                if (pending) {
                    if (pending > 1 && one_blank_before_stop)
                        pending_blank[0] = '\t';
                    owrite(pending_blank, pending);
                    pending = 0;
                    one_blank_before_stop = false;
                }

                prev_blank = blank;
                convert &= convert_entire_line || blank;
            }

            if (c < 0) {
                oflush();
                return status;
            }
            oput(c);
        } while (c != '\n');
    }
}
