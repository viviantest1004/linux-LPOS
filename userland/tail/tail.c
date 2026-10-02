/* tail - the last part of a file.
 *
 *   tail [-n [+]N] [-c [+]N] [-f] [-q] [-v] [file]...
 *
 * The last N lines cannot be known until the end of the input, and the
 * input may be a pipe that cannot be rewound. So we keep a buffer of the
 * bytes that might still turn out to be the answer and drop from the
 * front whenever one more line arrives than we need. That bounds memory
 * at N lines however big the file is, and unlike a fixed ring of fixed
 * strings it does not truncate a long line or invent a newline at the
 * end of a file that has none.
 *
 * A leading + counts from the start instead: `tail -n +2` is "everything
 * but the first line", which is how you drop a header.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static char iobuf[65536];
static char delim    = '\n';
static bool by_bytes = false;
static bool from_start = false;      /* the count was written +N */
static s64 count    = 10;

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

/* tail -n +N / tail -c +N */
static void skip_first(int fd)
{
    long left = count > 0 ? count - 1 : 0;   /* +1 means "from the first" */
    for (;;) {
        long n = lp_read(fd, iobuf, sizeof iobuf);
        if (n <= 0)
            return;
        if (left == 0) { out(iobuf, (size_t)n); continue; }
        if (by_bytes) {
            if (n <= left) { left -= n; continue; }
            out(iobuf + left, (size_t)(n - left));
            left = 0;
            continue;
        }
        long i = 0;
        while (i < n && left > 0) {
            if (iobuf[i] == delim) left--;
            i++;
        }
        if (left == 0 && i < n)
            out(iobuf + i, (size_t)(n - i));
    }
}

/* tail -c N */
static void last_bytes(int fd)
{
    size_t keep = (size_t)count;
    if (keep == 0) return;
    char *ring = malloc(keep);
    if (!ring) return;
    size_t head = 0, filled = 0;

    for (;;) {
        long n = lp_read(fd, iobuf, sizeof iobuf);
        if (n <= 0) break;
        for (long i = 0; i < n; i++) {
            if (filled == keep) {
                ring[head] = iobuf[i];
                head = (head + 1) % keep;
            } else {
                ring[(head + filled) % keep] = iobuf[i];
                filled++;
            }
        }
    }
    for (size_t i = 0; i < filled; i++)
        out(&ring[(head + i) % keep], 1);
    free(ring);
}

/* tail -n N */
static void last_lines(int fd)
{
    if (count <= 0) {
        while (lp_read(fd, iobuf, sizeof iobuf) > 0)
            ;
        return;
    }

    size_t  cap = 65536, len = 0, emitted = 0;
    char   *buf = malloc(cap);
    long    ringcap = count + 1;
    size_t *nl = malloc(sizeof(size_t) * (size_t)ringcap);
    long    first = 0, pending = 0;

    if (!buf || !nl) { free(buf); free(nl); return; }

    for (;;) {
        long n = lp_read(fd, iobuf, sizeof iobuf);
        if (n <= 0) break;
        if (len + (size_t)n > cap) {
            while (len + (size_t)n > cap) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
        for (long i = 0; i < n; i++) {
            buf[len] = iobuf[i];
            if (iobuf[i] == delim) {
                nl[(first + pending) % ringcap] = emitted + len;
                pending++;
                if (pending > count) {
                    size_t upto = nl[first % ringcap] - emitted;
                    memmove(buf, buf + upto + 1, len - upto);
                    len     -= upto + 1;
                    emitted += upto + 1;
                    first++;
                    pending--;
                }
            }
            len++;
        }
    }

    /* A file that does not end in a newline still ends in a line, so
     * what we are holding is one line more than we were counting. */
    if (len && buf[len - 1] != delim && pending >= count && pending > 0) {
        size_t upto = nl[first % ringcap] - emitted;
        memmove(buf, buf + upto + 1, len - upto);
        len -= upto + 1;
    }

    out(buf, len);
    free(buf);
    free(nl);
}

static void tail_fd(int fd)
{
    if (from_start)      skip_first(fd);
    else if (by_bytes)   last_bytes(fd);
    else                 last_lines(fd);
}

/* -f: print what arrives from here on. There is no inotify in this
 * kernel build, so this polls - a second is far below what anyone
 * watching a log can notice, and costs nothing while nothing happens. */
static void follow(int fd)
{
    for (;;) {
        long n = lp_read(fd, iobuf, sizeof iobuf);
        if (n > 0)
            out(iobuf, (size_t)n);
        else
            lp_sleep_ms(1000);
    }
}

static void usage(int fd)
{
    dprintf(fd, "Usage: tail [OPTION]... [FILE]...\n"
                "Print the last 10 lines of each FILE to standard output.\n"
                "With more than one FILE, precede each with a header giving the file name.\n\n"
                "  -c, --bytes=[+]NUM       output the last NUM bytes; or use -c +NUM to\n"
                "                             output starting with byte NUM of each file\n"
                "  -f, --follow             output appended data as the file grows\n"
                "  -n, --lines=[+]NUM       output the last NUM lines, instead of the last 10;\n"
                "                             or use -n +NUM to skip NUM-1 lines at the start\n"
                "  -q, --quiet, --silent    never print headers giving file names\n"
                "  -v, --verbose            always print headers giving file names\n"
                "  -z, --zero-terminated    line delimiter is NUL, not newline\n"
                "      --help     display this help and exit\n");
}

static void set_count(const char *s)
{
    from_start = (*s == '+');
    if (*s == '+' || *s == '-') s++;
    char *end;
    s64 v = strtoll(s, &end, 10);
    if (end == s || *end) {
        dprintf(STDERR_FILENO, "tail: invalid number of %s: '%s'\n",
                by_bytes ? "bytes" : "lines", s);
        lp_exit(1);
    }
    count = v;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "bytes", 1, 'c' }, { "lines", 1, 'n' }, { "follow", 2, 'f' },
        { "quiet", 0, 'q' }, { "silent", 0, 'q' }, { "verbose", 0, 'v' },
        { "zero-terminated", 0, 'z' }, { "retry", 0, 'R' },
        { "sleep-interval", 1, 's' }, { "pid", 1, 'P' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    bool watch = false;
    int  show  = -1;
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "c:n:fFqvzs:", lo);

    char shorthand[32];
    int  sh = 0;
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'c': by_bytes = true;  set_count(g.arg); break;
        case 'n': by_bytes = false; set_count(g.arg); break;
        case 'f': case 'F': watch = true; break;
        case 'q': show = 0; break;
        case 'v': show = 1; break;
        case 'z': delim = '\0'; break;
        case 's': case 'P': case 'R': break;   /* accepted, nothing to do */
        case 'H': usage(STDOUT_FILENO); return 0;
        case '?':
            if (g.badchar >= '0' && g.badchar <= '9' && sh < 30) {
                shorthand[sh++] = g.badchar;    /* tail -5, the old spelling */
                continue;
            }
            lp_getopt_err("tail", &g);
            return 1;
        }
    }
    if (sh) { shorthand[sh] = '\0'; by_bytes = false; set_count(shorthand); }

    int files = argc - g.ind;
    if (files == 0) {
        tail_fd(STDIN_FILENO);
        if (watch) follow(STDIN_FILENO);
        return 0;
    }

    int rc = 0, shown = 0, last_fd = -1;
    for (int i = g.ind; i < argc; i++) {
        bool banner = (show == 1) || (show == -1 && files > 1);
        int  fd;
        if (strcmp(argv[i], "-") == 0) {
            fd = STDIN_FILENO;
            if (banner) printf("%s==> standard input <==\n", shown ? "\n" : "");
        } else {
            long f = lp_open(argv[i], O_RDONLY, 0);
            if (f < 0) {
                lp_diag("tail", "cannot open", "for reading",
                        "cannot open", argv[i], (int)-f);
                rc = 1;
                continue;
            }
            fd = (int)f;
            if (banner) printf("%s==> %s <==\n", shown ? "\n" : "", argv[i]);
        }
        tail_fd(fd);
        shown++;
        if (i + 1 == argc) last_fd = fd;
        else if (fd != STDIN_FILENO) lp_close(fd);
    }

    if (watch && last_fd >= 0)
        follow(last_fd);            /* never returns; Ctrl-C stops it */
    if (last_fd >= 0 && last_fd != STDIN_FILENO)
        lp_close(last_fd);
    return rc;
}
