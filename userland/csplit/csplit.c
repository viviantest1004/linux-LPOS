/* csplit - split a file where a pattern matches.
 *
 *   csplit log.txt '/^ERROR/' '{*}'    a file per error
 *   csplit big.txt 100 200             at absolute line numbers
 *   csplit -f part- -b '%02d' x /re/   choose the names
 *
 * split cuts by size; this cuts by content, which is what you want when
 * the pieces are records rather than bytes. Nothing is written until
 * the whole run succeeds - a pattern that never matches removes the
 * files it already made, so a failed run does not leave half a job
 * behind. -k keeps them.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "regex.h"

#define LINE   8192
#define MAXOUT 4096

static const char *prefix = "xx";
static const char *numfmt = NULL;      /* -b */
static int  digits = 2;                /* -n */
static bool keep = false;              /* -k */
static bool quiet = false;             /* -s */
static bool elide_empty = false;       /* -z */

static char made[MAXOUT][256];
static int  nmade = 0;

/* The whole input, and where we are in it. csplit has to be able to
 * look at a line, decide it belongs to the next piece, and put it
 * there - and on a pipe there is no going back. */
static char **lines;
static long   nlines = 0;
static int    piece = 0;

static void name_for(char *out, size_t cap, int n)
{
    if (numfmt) {
        /* GNU takes a printf format here; the only one anybody uses is
         * a zero-padded integer, and that is what is honoured. */
        const char *p = strchr(numfmt, '%');
        int width = digits;
        if (p) {
            p++;
            while (*p == '0') { p++; }
            int w = 0;
            while (*p >= '0' && *p <= '9') w = w * 10 + (*p++ - '0');
            if (w) width = w;
        }
        snprintf(out, cap, "%s%0*d", prefix, width, n);
        return;
    }
    snprintf(out, cap, "%s%0*d", prefix, digits, n);
}

static void cleanup(void)
{
    for (int i = 0; i < nmade; i++)
        lp_unlink(made[i]);
}

static void die(const char *msg, const char *arg)
{
    if (!keep) cleanup();
    if (arg) dprintf(STDERR_FILENO, "csplit: %s: '%s'\n", msg, arg);
    else     dprintf(STDERR_FILENO, "csplit: %s\n", msg);
    lp_exit(1);
}

/* Write lines [from, to) as the next piece. */
static void write_piece(long from, long to)
{
    char name[256];
    name_for(name, sizeof name, piece);
    long out = lp_open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) die("cannot write", name);
    long bytes = 0;
    for (long i = from; i < to; i++) {
        size_t l = strlen(lines[i]);
        lp_write((int)out, lines[i], l);
        bytes += (long)l;
    }
    lp_close((int)out);
    if (bytes == 0 && elide_empty) { lp_unlink(name); return; }
    if (nmade < MAXOUT) strlcpy(made[nmade++], name, 256);
    if (!quiet) printf("%ld\n", bytes);
    piece++;
}

/* Where this pattern says to stop, starting the search after `at`.
 * Returns -1 when a regex does not match again, which ends a {*}
 * rather than failing it. */
static long stop_for(const char *pat, long from, bool *skip, bool *bad)
{
    *skip = false;
    *bad  = false;

    if (pat[0] >= '0' && pat[0] <= '9') {
        long want = strtol(pat, NULL, 10) - 1;      /* 1-based */
        if (want < from || want > nlines) { *bad = true; return -1; }
        return want;
    }
    if (pat[0] != '/' && pat[0] != '%') { *bad = true; return -1; }

    char close = pat[0];
    *skip = (close == '%');
    const char *end = strrchr(pat + 1, close);
    if (!end) { *bad = true; return -1; }

    char re[LINE];
    size_t rl = (size_t)(end - pat - 1);
    if (rl >= sizeof re) rl = sizeof re - 1;
    memcpy(re, pat + 1, rl);
    re[rl] = '\0';
    long offset = end[1] ? strtol(end + 1, NULL, 10) : 0;

    const char *err = NULL;
    lpre *prg = re_compile(re, false, false, &err);
    if (!prg) { *bad = true; return -1; }

    int caps[RE_MAX_CAPS];
    for (long i = from + 1; i < nlines; i++) {
        char tmp[LINE];
        strlcpy(tmp, lines[i], sizeof tmp);
        char *nl = strchr(tmp, '\n');
        if (nl) *nl = '\0';
        if (!re_search(prg, tmp, 0, false, caps)) continue;
        long stop = i + offset;
        if (stop < from)   stop = from;
        if (stop > nlines) stop = nlines;
        return stop;
    }
    return -1;                    /* no further match */
}


int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "prefix", 1, 'f' }, { "suffix-format", 1, 'b' },
        { "digits", 1, 'n' }, { "keep-files", 0, 'k' },
        { "quiet", 0, 's' }, { "silent", 0, 's' },
        { "elide-empty-files", 0, 'z' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "f:b:n:kszq", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'f': prefix = g.arg; break;
        case 'b': numfmt = g.arg; break;
        case 'n': digits = (int)strtol(g.arg, NULL, 10); break;
        case 'k': keep = true; break;
        case 's': case 'q': quiet = true; break;
        case 'z': elide_empty = true; break;
        case 'H':
            printf("Usage: csplit [OPTION]... FILE PATTERN...\n"
                   "Output pieces of FILE separated by PATTERN(s) to files 'xx00', 'xx01', ...\n"
                   "and print the size in bytes of each piece to standard output.\n\n"
                   "  -b, --suffix-format=FORMAT  use sprintf FORMAT instead of %%02d\n"
                   "  -f, --prefix=PREFIX         use PREFIX instead of 'xx'\n"
                   "  -k, --keep-files            do not remove output files on errors\n"
                   "  -n, --digits=DIGITS         use specified number of digits instead of 2\n"
                   "  -s, --quiet, --silent       do not print counts of output file sizes\n"
                   "  -z, --elide-empty-files     remove empty output files\n"
                   "      --help     display this help and exit\n\n"
                   "Each PATTERN may be:\n"
                   "  INTEGER            copy up to but not including the given line number\n"
                   "  /REGEXP/[OFFSET]   copy up to but not including a matching line\n"
                   "  %%REGEXP%%[OFFSET]   skip to, but not including, a matching line\n"
                   "  {INTEGER}          repeat the previous pattern this many times\n"
                   "  {*}                repeat the previous pattern as many times as possible\n");
            return 0;
        default: lp_getopt_err("csplit", &g); return 1;
        }
    }
    if (digits < 1 || digits > 20) digits = 2;

    if (argc - g.ind < 2) {
        dprintf(STDERR_FILENO, "csplit: missing operand\n");
        dprintf(STDERR_FILENO, "Try 'csplit --help' for more information.\n");
        return 1;
    }

    /* The whole input is read first. csplit has to be able to look at a
     * line, decide it belongs to the next piece, and put it there - and
     * on a pipe there is no going back. */
    const char *fname = argv[g.ind];
    int fd = STDIN_FILENO;
    if (strcmp(fname, "-") != 0) {
        long f = lp_open(fname, O_RDONLY, 0);
        if (f < 0) {
            lp_diag("csplit", NULL, NULL, "cannot open", fname, (int)-f);
            return 1;
        }
        fd = (int)f;
    }

    long cap = 0;
    char buf[LINE];
    bool had_delim;
    for (;;) {
        long n = readrec(fd, buf, sizeof buf, '\n', &had_delim);
        if (n < 0) break;
        if (nlines == cap) {
            cap = cap ? cap * 2 : 1024;
            char **p = realloc(lines, sizeof(char *) * (size_t)cap);
            if (!p) die("out of memory", NULL);
            lines = p;
        }
        size_t len = strlen(buf);
        char *copy = malloc(len + 2);
        if (!copy) die("out of memory", NULL);
        memcpy(copy, buf, len);
        if (had_delim) copy[len++] = '\n';
        copy[len] = '\0';
        lines[nlines++] = copy;
    }
    if (fd != STDIN_FILENO) lp_close(fd);

    long at = 0;                   /* the next line not yet written out */

    for (int i = g.ind + 1; i < argc; i++) {
        const char *pat = argv[i];
        if (pat[0] == '{') {
            dprintf(STDERR_FILENO, "csplit: %s: '{' without a pattern before it\n", pat);
            if (!keep) cleanup();
            return 1;
        }

        /* A {N} or {*} after a pattern repeats it. */
        long repeat = 0;
        bool forever = false;
        if (i + 1 < argc && argv[i + 1][0] == '{') {
            if (argv[i + 1][1] == '*') forever = true;
            else repeat = strtol(argv[i + 1] + 1, NULL, 10);
            i++;
        }

        for (long turn = 0; ; turn++) {
            bool skip, bad;
            long stop = stop_for(pat, at, &skip, &bad);
            if (bad) {
                /* csplit has already written everything up to here as a
                 * piece and printed its size by the time it finds out
                 * the pattern is unusable, so that line comes out too -
                 * and then the files go away unless -k. */
                write_piece(at, nlines);
                dprintf(STDERR_FILENO, "csplit: '%s': %s\n", pat,
                        (pat[0] >= '0' && pat[0] <= '9')
                            ? "line number out of range"
                            : "invalid pattern");
                if (!keep) cleanup();
                return 1;
            }
            if (stop < 0) {
                /* No further match. Under {*} that is simply the end;
                 * anywhere else it is the failure csplit reports. */
                if (forever) break;
                write_piece(at, nlines);
                dprintf(STDERR_FILENO, "csplit: '%s': match not found\n", pat);
                if (!keep) cleanup();
                return 1;
            }
            if (!skip) write_piece(at, stop);
            at = stop;

            if (forever) { if (at >= nlines) break; continue; }
            if (turn >= repeat) break;
        }
    }

    /* Whatever is left is the last piece. csplit always writes it, even
     * when it is empty, unless -z says otherwise. */
    write_piece(at, nlines);
    return 0;
}
