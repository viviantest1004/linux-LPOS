/* uniq - collapse repeated lines.
 *
 *   uniq [-c] [-d] [-D] [-u] [-i] [-f N] [-s N] [-w N] [INPUT [OUTPUT]]
 *
 * Only lines that are next to each other count as repeats, which is why
 * this is nearly always used after sort:
 *
 *   cat log | sort | uniq -c | sort -rn
 *
 * -f skips leading fields and -s leading characters before comparing,
 * so a log line whose timestamp differs can still be seen as a repeat.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define LINE_MAX 8192

static bool opt_count = false, opt_dups = false, opt_uniq = false;
static bool opt_all_dups = false, opt_ignore_case = false;
static long skip_fields = 0, skip_chars = 0, check_chars = -1;
static char delim = '\n';
static int  outfd = STDOUT_FILENO;

/* The part of a line -f and -s leave to compare. */
static const char *key(const char *s)
{
    for (long f = 0; f < skip_fields; f++) {
        while (*s == ' ' || *s == '\t') s++;
        while (*s && *s != ' ' && *s != '\t') s++;
    }
    while (*s == ' ' || *s == '\t' ) {
        if (skip_fields) s++;              /* a skipped field takes its blanks */
        else break;
    }
    for (long c = 0; c < skip_chars && *s; c++) s++;
    return s;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static bool same(const char *a, const char *b)
{
    const char *x = key(a), *y = key(b);
    long n = check_chars;
    while (*x && *y) {
        if (n == 0) return true;
        int ca = opt_ignore_case ? lower((unsigned char)*x) : (unsigned char)*x;
        int cb = opt_ignore_case ? lower((unsigned char)*y) : (unsigned char)*y;
        if (ca != cb) return false;
        x++; y++;
        if (n > 0) n--;
    }
    if (n == 0) return true;
    return *x == '\0' && *y == '\0';
}

static void emit(const char *line, long run)
{
    char buf[LINE_MAX + 32];
    int  n;
    if (opt_count) n = snprintf(buf, sizeof buf, "%7ld %s%c", run, line, delim);
    else           n = snprintf(buf, sizeof buf, "%s%c", line, delim);
    if (n > 0) lp_write(outfd, buf, (size_t)n);
}

static void usage(int fd)
{
    dprintf(fd, "Usage: uniq [OPTION]... [INPUT [OUTPUT]]\n"
                "Filter adjacent matching lines from INPUT (or standard input),\n"
                "writing to OUTPUT (or standard output).\n\n"
                "  -c, --count           prefix lines by the number of occurrences\n"
                "  -d, --repeated        only print duplicate lines, one for each group\n"
                "  -D                    print all duplicate lines\n"
                "  -f, --skip-fields=N   avoid comparing the first N fields\n"
                "  -i, --ignore-case     ignore differences in case when comparing\n"
                "  -s, --skip-chars=N    avoid comparing the first N characters\n"
                "  -u, --unique          only print unique lines\n"
                "  -z, --zero-terminated line delimiter is NUL, not newline\n"
                "  -w, --check-chars=N   compare no more than N characters in lines\n"
                "      --help     display this help and exit\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "count", 0, 'c' }, { "repeated", 0, 'd' },
        { "all-repeated", 2, 'D' }, { "skip-fields", 1, 'f' },
        { "ignore-case", 0, 'i' }, { "skip-chars", 1, 's' },
        { "unique", 0, 'u' }, { "check-chars", 1, 'w' },
        { "zero-terminated", 0, 'z' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "cdDf:is:uw:z", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'c': opt_count = true; break;
        case 'd': opt_dups = true; break;
        case 'D': opt_all_dups = true; break;
        case 'u': opt_uniq = true; break;
        case 'i': opt_ignore_case = true; break;
        case 'f': skip_fields = strtol(g.arg, NULL, 10); break;
        case 's': skip_chars  = strtol(g.arg, NULL, 10); break;
        case 'w': check_chars = strtol(g.arg, NULL, 10); break;
        case 'z': delim = '\0'; break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default: lp_getopt_err("uniq", &g); return 1;
        }
    }

    if (opt_count && opt_all_dups) {
        dprintf(STDERR_FILENO,
                "uniq: printing all duplicated lines and repeat counts is meaningless\n");
        dprintf(STDERR_FILENO, "Try 'uniq --help' for more information.\n");
        return 1;
    }

    int fd = STDIN_FILENO;
    if (g.ind < argc && strcmp(argv[g.ind], "-") != 0) {
        long f = lp_open(argv[g.ind], O_RDONLY, 0);
        if (f < 0) {
            lp_diag("uniq", NULL, NULL, "cannot open", argv[g.ind], (int)-f);
            return 1;
        }
        fd = (int)f;
    }
    if (g.ind + 1 < argc) {
        long f = lp_open(argv[g.ind + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f < 0) {
            lp_diag("uniq", NULL, NULL, "cannot open", argv[g.ind + 1], (int)-f);
            return 1;
        }
        outfd = (int)f;
    }

    /* -D wants every line of a repeated group, so a group has to be held
     * until it is known to be one. Two buffers do it: the group's first
     * line, and the line being read. */
    char cur[LINE_MAX], prev[LINE_MAX];
    long run = 0;
    bool have_prev = false;

    for (;;) {
        long n = readrec(fd, cur, sizeof cur, delim, NULL);

        if (n >= 0 && have_prev && same(cur, prev)) {
            run++;
            if (opt_all_dups && run == 2) emit(prev, 1);   /* group is real */
            if (opt_all_dups && run >= 2) emit(cur, 1);
            continue;
        }

        if (have_prev && !opt_all_dups) {
            bool show = true;
            if (opt_dups && run < 2) show = false;
            if (opt_uniq && run > 1) show = false;
            if (show) emit(prev, run);
        }

        if (n < 0)
            break;

        strlcpy(prev, cur, sizeof prev);
        have_prev = true;
        run = 1;
    }

    if (fd != STDIN_FILENO)  lp_close(fd);
    if (outfd != STDOUT_FILENO) lp_close(outfd);
    return 0;
}
