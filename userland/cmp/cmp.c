/* cmp - are these two files the same, and if not, where do they differ.
 *
 *   cmp [-bls] [-i SKIP] [-n LIMIT] FILE1 [FILE2 [SKIP1 [SKIP2]]]
 *
 * `sha256sum` answers "are these the same" for whole files. This answers
 * "where did they start to differ", which is what you want when a
 * download is truncated or a card is going bad: the offset tells you
 * how far the good part goes.
 *
 * Exit status: 0 the same, 1 different, 2 something went wrong. Scripts
 * rely on that split, so a missing file must not look like a difference.
 *
 * Two things about the wording are worth knowing, because they look
 * like mistakes and are not. The first difference is reported as a
 * "char" normally and as a "byte" under -b - that is what diffutils
 * prints, and matching it is the point. And the end-of-file line says
 * "in line N" when the shorter file stopped in the middle of a line and
 * "line N" when it stopped after a complete one; the difference is the
 * whole information in that word.
 *
 * cmp is diffutils, not coreutils, and its option errors carry the
 * program name on both lines. coreutils only puts it on the first.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define BUF_SIZE 65536

static bool opt_bytes  = false;    /* -b */
static bool opt_list   = false;    /* -l */
static bool opt_silent = false;    /* -s */

static void try_help(void)
{
    dprintf(STDERR_FILENO, "cmp: Try 'cmp --help' for more information.\n");
}

/* lp_getopt_err prints coreutils' pair of lines, and diffutils puts the
 * program name on the second one as well. One word apart, so the first
 * line is borrowed from the same place and the second is written here. */
static void bad_option(const lp_getopt_t *g)
{
    if (g->badlong && g->ambig)
        dprintf(STDERR_FILENO, "cmp: option '%s' is ambiguous\n", g->badlong);
    else if (g->badlong)
        dprintf(STDERR_FILENO, "cmp: unrecognized option '%s'\n", g->badlong);
    else
        dprintf(STDERR_FILENO, "cmp: invalid option -- '%c'\n", g->badchar);
    try_help();
}

/* SKIP and LIMIT take the suffixes the manual lists. */
static bool parse_count(const char *s, long long *out)
{
    if (!s || !*s) return false;
    long long v = 0;
    const char *p = s;
    if (*p < '0' || *p > '9') return false;
    for (; *p >= '0' && *p <= '9'; p++)
        v = v * 10 + (*p - '0');

    static const struct { const char *suf; long long mul; } tab[] = {
        { "kB", 1000LL }, { "K", 1024LL }, { "kiB", 1024LL },
        { "MB", 1000000LL }, { "M", 1048576LL }, { "MiB", 1048576LL },
        { "GB", 1000000000LL }, { "G", 1073741824LL }, { "GiB", 1073741824LL },
        { "TB", 1000000000000LL }, { "T", 1099511627776LL },
        { "PB", 1000000000000000LL }, { "P", 1125899906842624LL },
        { NULL, 0 }
    };
    if (*p) {
        for (int i = 0; tab[i].suf; i++)
            if (strcmp(p, tab[i].suf) == 0) { *out = v * tab[i].mul; return true; }
        return false;
    }
    *out = v;
    return true;
}

/* cat -v's rendering, which is what -b uses for the differing byte. */
static void put_visible(unsigned char c, char *out)
{
    int i = 0;
    if (c >= 128) { out[i++] = 'M'; out[i++] = '-'; c -= 128; }
    if (c < 32)        { out[i++] = '^'; out[i++] = (char)(c + 64); }
    else if (c == 127) { out[i++] = '^'; out[i++] = '?'; }
    else                 out[i++] = (char)c;
    out[i] = '\0';
}

static int digits(long long v)
{
    int n = 1;
    while (v >= 10) { v /= 10; n++; }
    return n;
}

/* -1 when the size is not knowable, which is every pipe. */
static long long size_of(const char *name, int fd)
{
    if (fd == STDIN_FILENO) return -1;
    lp_stat_t st;
    if (lp_stat(name, &st, true) < 0) return -1;
    if ((st.mode & LP_S_IFMT) != LP_S_IFREG) return -1;
    return (long long)st.size;
}

static long open_one(const char *name)
{
    if (strcmp(name, "-") == 0) return STDIN_FILENO;
    long fd = lp_open(name, O_RDONLY, 0);
    if (fd < 0) {
        lp_diag("cmp", NULL, NULL, "cannot open", name, (int)-fd);
        return -1;
    }
    return fd;
}

/* Throw away SKIP bytes. lseek works on a file and not on a pipe, so
 * the read loop is the fallback rather than the other way round. */
static bool skip_bytes(int fd, long long n)
{
    if (n <= 0) return true;
    if (fd != STDIN_FILENO && lp_lseek(fd, (off_t)n, SEEK_SET) >= 0)
        return true;
    static char junk[BUF_SIZE];
    while (n > 0) {
        size_t want = n > (long long)sizeof junk ? sizeof junk : (size_t)n;
        long got = lp_read(fd, junk, want);
        if (got <= 0) return got == 0;   /* a short file is not an error */
        n -= got;
    }
    return true;
}

static void usage(int fd)
{
    dprintf(fd, "Usage: cmp [OPTION]... FILE1 [FILE2 [SKIP1 [SKIP2]]]\n"
                "Compare two files byte by byte.\n\n"
                "The optional SKIP1 and SKIP2 specify the number of bytes to skip\n"
                "at the beginning of each file (zero by default).\n\n"
                "  -b, --print-bytes          print differing bytes\n"
                "  -i, --ignore-initial=SKIP  skip first SKIP bytes of both inputs\n"
                "  -l, --verbose              output byte numbers and differing byte values\n"
                "  -n, --bytes=LIMIT          compare at most LIMIT bytes\n"
                "  -s, --quiet, --silent      suppress all normal output\n"
                "      --help                 display this help and exit\n\n"
                "If a FILE is '-' or missing, read standard input.\n"
                "Exit status is 0 if inputs are the same, 1 if different, 2 if trouble.\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "print-bytes", 0, 'b' }, { "ignore-initial", 1, 'i' },
        { "verbose", 0, 'l' }, { "bytes", 1, 'n' },
        { "quiet", 0, 's' }, { "silent", 0, 's' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    long long skip1 = 0, skip2 = 0, limit = -1;
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "bi:ln:s", lo);
    for (int c; (c = lp_getopt(&g)) != -1; )
        switch (c) {
        case 'b': opt_bytes = true; break;
        case 'l': opt_list = true; break;
        case 's': opt_silent = true; break;
        case 'i': {
            /* -i takes either SKIP or SKIP1:SKIP2. */
            const char *colon = strchr(g.arg, ':');
            if (colon) {
                char first[32];
                size_t n = (size_t)(colon - g.arg);
                if (n >= sizeof first) n = sizeof first - 1;
                memcpy(first, g.arg, n);
                first[n] = '\0';
                if (!parse_count(first, &skip1) || !parse_count(colon + 1, &skip2)) {
                    dprintf(STDERR_FILENO, "cmp: invalid --ignore-initial value '%s'\n", g.arg);
                    return 2;
                }
            } else if (!parse_count(g.arg, &skip1)) {
                dprintf(STDERR_FILENO, "cmp: invalid --ignore-initial value '%s'\n", g.arg);
                return 2;
            } else {
                skip2 = skip1;
            }
            break;
        }
        case 'n':
            if (!parse_count(g.arg, &limit)) {
                dprintf(STDERR_FILENO, "cmp: invalid --bytes value '%s'\n", g.arg);
                return 2;
            }
            break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default:  bad_option(&g); return 2;
        }

    int nops = argc - g.ind;
    if (nops < 1) {
        dprintf(STDERR_FILENO, "cmp: missing operand after '%s'\n", argv[0]);
        try_help();
        return 2;
    }

    const char *na = argv[g.ind];
    const char *nb = nops > 1 ? argv[g.ind + 1] : "-";
    /* The skips are checked before the operand count, because that is
     * the order the error comes out in when both are wrong. */
    if (nops > 2 && !parse_count(argv[g.ind + 2], &skip1)) {
        dprintf(STDERR_FILENO, "cmp: invalid --ignore-initial value '%s'\n", argv[g.ind + 2]);
        try_help();
        return 2;
    }
    if (nops > 3 && !parse_count(argv[g.ind + 3], &skip2)) {
        dprintf(STDERR_FILENO, "cmp: invalid --ignore-initial value '%s'\n", argv[g.ind + 3]);
        try_help();
        return 2;
    }
    if (nops > 4) {
        dprintf(STDERR_FILENO, "cmp: extra operand '%s'\n", argv[g.ind + 4]);
        try_help();
        return 2;
    }

    long fa = open_one(na);
    if (fa < 0) return 2;
    long fb = open_one(nb);
    if (fb < 0) { if (fa != STDIN_FILENO) lp_close((int)fa); return 2; }

    /* The column the byte offsets line up in comes from the file sizes,
     * and a pipe has no size, so it does not get a vote. */
    long long sa = size_of(na, (int)fa), sb = size_of(nb, (int)fb);
    long long big = sa > sb ? sa : sb;
    if (sa >= 0) big = sa > big ? sa : big;
    int width = big >= 0 ? digits(big) : 19;

    if (!skip_bytes((int)fa, skip1) || !skip_bytes((int)fb, skip2)) {
        dprintf(STDERR_FILENO, "cmp: cannot skip in '%s'\n", na);
        return 2;
    }

    static unsigned char ba[BUF_SIZE], bb[BUF_SIZE];
    long long off = 0, newlines = 0;
    unsigned char last = 0;
    int rc = 0;

    for (;;) {
        size_t want = sizeof ba;
        if (limit >= 0) {
            long long left = limit - off;
            if (left <= 0) break;
            if (left < (long long)want) want = (size_t)left;
        }
        long ra = lp_read((int)fa, ba, want);
        long rb = lp_read((int)fb, bb, want);
        if (ra < 0 || rb < 0) {
            lp_diag("cmp", NULL, NULL, "cannot read", ra < 0 ? na : nb,
                    (int)-(ra < 0 ? ra : rb));
            rc = 2;
            break;
        }

        long n = ra < rb ? ra : rb;
        for (long k = 0; k < n; k++) {
            if (ba[k] != bb[k]) {
                rc = 1;
                if (opt_silent) goto done;
                if (opt_list) {
                    printf("%*lld %3o %3o\n", width, off + k + 1,
                           ba[k], bb[k]);
                } else if (opt_bytes) {
                    char ca[8], cb[8];
                    put_visible(ba[k], ca);
                    put_visible(bb[k], cb);
                    printf("%s %s differ: byte %lld, line %lld is %3o %s %3o %s\n",
                           na, nb, off + k + 1, newlines + 1, ba[k], ca, bb[k], cb);
                    goto done;
                } else {
                    printf("%s %s differ: char %lld, line %lld\n",
                           na, nb, off + k + 1, newlines + 1);
                    goto done;
                }
            }
            if (ba[k] == '\n') newlines++;
            last = ba[k];
        }
        off += n;

        if (ra != rb) {
            /* One ran out first. Which one, and where it stopped, is the
             * information; the word "in" carries whether the last line
             * was finished. */
            rc = 1;
            if (!opt_silent) {
                const char *who = ra < rb ? na : nb;
                if (off == 0)
                    dprintf(STDERR_FILENO, "cmp: EOF on %s which is empty\n", who);
                else if (opt_list)
                    dprintf(STDERR_FILENO, "cmp: EOF on %s after byte %lld\n", who, off);
                else if (last == '\n')
                    dprintf(STDERR_FILENO, "cmp: EOF on %s after byte %lld, line %lld\n",
                            who, off, newlines);
                else
                    dprintf(STDERR_FILENO, "cmp: EOF on %s after byte %lld, in line %lld\n",
                            who, off, newlines + 1);
            }
            break;
        }
        if (ra == 0) break;      /* both ended together */
    }

done:
    if (fa != STDIN_FILENO) lp_close((int)fa);
    if (fb != STDIN_FILENO) lp_close((int)fb);
    return rc;
}
