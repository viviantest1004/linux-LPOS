/* head - the first part of a file.
 *
 *   head [-n [-]N] [-c [-]N] [-q] [-v] [file]...
 *
 * The old version read with readline(), which strips backspaces and
 * cannot tell a last line with a newline from one without. That is fine
 * for a terminal and wrong for a file, so this one moves bytes.
 *
 * A negative count means "all but the last N", which cannot be answered
 * until the end of the input arrives. Both forms of it keep exactly as
 * much as they have to and no more: N lines, or N bytes, in a ring.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static char  iobuf[65536];
static char  delim = '\n';
static bool  by_bytes = false;
static s64   count = 10;
static bool  from_end = false;      /* the count was written -N */

static void out(const char *p, size_t n)
{
    while (n) {
        long w = lp_write(STDOUT_FILENO, p, n);
        if (w <= 0)
            return;
        p += w;
        n -= (size_t)w;
    }
}

/* head -n N / head -c N: the plain direction. */
static void first(int fd)
{
    s64 left = count;
    if (left <= 0)
        return;
    for (;;) {
        long n = lp_read(fd, iobuf, sizeof iobuf);
        if (n <= 0)
            return;
        if (by_bytes) {
            if (n > left) n = left;
            out(iobuf, (size_t)n);
            left -= n;
            if (left == 0) return;
            continue;
        }
        for (long i = 0; i < n; i++) {
            if (iobuf[i] != delim)
                continue;
            if (--left == 0) {
                out(iobuf, (size_t)(i + 1));
                return;
            }
        }
        out(iobuf, (size_t)n);
    }
}

/* head -c -N: hold back the last N bytes. */
static void all_but_last_bytes(int fd)
{
    size_t keep = (size_t)count;
    if (keep == 0) { first(fd); return; }

    char *ring = malloc(keep);
    if (!ring) return;
    size_t head = 0, filled = 0;

    for (;;) {
        long n = lp_read(fd, iobuf, sizeof iobuf);
        if (n <= 0)
            break;
        for (long i = 0; i < n; i++) {
            if (filled == keep) {
                out(&ring[head], 1);           /* this byte is now safe */
                ring[head] = iobuf[i];
                head = (head + 1) % keep;
            } else {
                ring[(head + filled) % keep] = iobuf[i];
                filled++;
            }
        }
    }
    free(ring);
}

/* head -n -N: hold back the last N lines. */
static void all_but_last_lines(int fd)
{
    size_t  cap = 65536, len = 0;
    char   *buf = malloc(cap);
    long    ringcap = count + 1;
    size_t *nl = malloc(sizeof(size_t) * (size_t)ringcap);
    long    first_nl = 0, pending = 0;   /* newlines held back */
    size_t  emitted = 0;                 /* bytes already dropped from buf */

    if (!buf || !nl) { free(buf); free(nl); return; }

    for (;;) {
        long n = lp_read(fd, iobuf, sizeof iobuf);
        if (n <= 0)
            break;
        if (len + (size_t)n > cap) {
            while (len + (size_t)n > cap) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
        for (long i = 0; i < n; i++) {
            buf[len] = iobuf[i];
            if (iobuf[i] == delim) {
                nl[(first_nl + pending) % ringcap] = emitted + len;
                pending++;
                if (pending > count) {
                    /* The oldest held line is now known not to be in the
                     * last N, so it can go out and leave the buffer. */
                    size_t upto = nl[first_nl % ringcap] - emitted;
                    out(buf, upto + 1);
                    memmove(buf, buf + upto + 1, len - upto);
                    len      -= upto + 1;
                    emitted  += upto + 1;
                    first_nl++;
                    pending--;
                }
            }
            len++;
        }
    }
    free(buf);
    free(nl);
}

static void head_fd(int fd)
{
    if (!from_end)                first(fd);
    else if (by_bytes)            all_but_last_bytes(fd);
    else                          all_but_last_lines(fd);
}

static void usage(int fd)
{
    dprintf(fd, "Usage: head [OPTION]... [FILE]...\n"
                "Print the first 10 lines of each FILE to standard output.\n"
                "With more than one FILE, precede each with a header giving the file name.\n\n"
                "  -c, --bytes=[-]NUM       print the first NUM bytes of each file;\n"
                "                             with the leading '-', print all but the last\n"
                "                             NUM bytes of each file\n"
                "  -n, --lines=[-]NUM       print the first NUM lines instead of the first 10;\n"
                "                             with the leading '-', print all but the last\n"
                "                             NUM lines of each file\n"
                "  -q, --quiet, --silent    never print headers giving file names\n"
                "  -v, --verbose            always print headers giving file names\n"
                "  -z, --zero-terminated    line delimiter is NUL, not newline\n"
                "      --help     display this help and exit\n");
}

static void set_count(const char *s, const char *prog)
{
    from_end = (*s == '-');
    if (from_end) s++;
    char *end;
    s64 v = strtoll(s, &end, 10);
    if (end == s || *end) {
        dprintf(STDERR_FILENO, "%s: invalid number of %s: '%s%s'\n", prog,
                by_bytes ? "bytes" : "lines", from_end ? "-" : "", s);
        lp_exit(1);
    }
    count = v;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "bytes", 1, 'c' }, { "lines", 1, 'n' },
        { "quiet", 0, 'q' }, { "silent", 0, 'q' }, { "verbose", 0, 'v' },
        { "zero-terminated", 0, 'z' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    int show = -1;                     /* -1 auto, 0 never, 1 always */
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "c:n:qvz", lo);

    char shorthand[32];
    int  sh = 0;
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'c': by_bytes = true;  set_count(g.arg, "head"); break;
        case 'n': by_bytes = false; set_count(g.arg, "head"); break;
        case 'q': show = 0; break;
        case 'v': show = 1; break;
        case 'z': delim = '\0'; break;
        case 'H': usage(STDOUT_FILENO); return 0;
        case '?':
            /* head -5 is the old spelling, still in every script. */
            if (g.badchar >= '0' && g.badchar <= '9' && sh < 30) {
                shorthand[sh++] = g.badchar;
                continue;
            }
            lp_getopt_err("head", &g);
            return 1;
        }
    }
    if (sh) { shorthand[sh] = '\0'; by_bytes = false; set_count(shorthand, "head"); }

    int files = argc - g.ind;
    if (files == 0) {
        head_fd(STDIN_FILENO);
        return 0;
    }

    int rc = 0, shown = 0;
    for (int i = g.ind; i < argc; i++) {
        bool banner = (show == 1) || (show == -1 && files > 1);
        if (strcmp(argv[i], "-") == 0) {
            if (banner) printf("%s==> standard input <==\n", shown ? "\n" : "");
            head_fd(STDIN_FILENO);
            shown++;
            continue;
        }
        long fd = lp_open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            lp_diag("head", "cannot open", "for reading",
                    "cannot open", argv[i], (int)-fd);
            rc = 1;
            continue;
        }
        if (banner) printf("%s==> %s <==\n", shown ? "\n" : "", argv[i]);
        head_fd((int)fd);
        lp_close((int)fd);
        shown++;
    }
    return rc;
}
