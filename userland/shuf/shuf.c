/* shuf - the same lines, in a random order.
 *
 *   shuf FILE            every line, shuffled
 *   shuf -n 1 FILE       pick one
 *   shuf -i 1-100 -n 5   five numbers out of a hundred
 *   shuf -e a b c        shuffle the arguments themselves
 *
 * The randomness comes from the kernel, not from a seeded generator, so
 * two runs a millisecond apart do not agree.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static char **lines;
static long   n, cap;

static bool add(const char *s)
{
    if (n == cap) {
        long c = cap ? cap * 2 : 1024;
        char **p = realloc(lines, sizeof(char *) * (size_t)c);
        if (!p) return false;
        lines = p; cap = c;
    }
    lines[n] = strdup(s);
    return lines[n++] != NULL;
}

static u64 randbits(void)
{
    u64 v = 0;
    if (lp_getrandom(&v, sizeof v, 0) != (long)sizeof v)
        v = (u64)lp_monotonic_ms() * 6364136223846793005ULL + (u64)lp_getpid();
    return v;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "echo", 0, 'e' }, { "input-range", 1, 'i' },
        { "head-count", 1, 'n' }, { "output", 1, 'o' },
        { "repeat", 0, 'r' }, { "zero-terminated", 0, 'z' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    bool echo_args = false, repeat = false;
    long head = -1, lo_r = 0, hi_r = -1;
    char eol = '\n';
    const char *outfile = NULL;

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "ei:n:o:rz", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'e': echo_args = true; break;
        case 'r': repeat = true; break;
        case 'z': eol = '\0'; break;
        case 'n': head = strtol(g.arg, NULL, 10); break;
        case 'o': outfile = g.arg; break;
        case 'i': {
            char *dash = strchr(g.arg, '-');
            if (!dash) {
                dprintf(STDERR_FILENO, "shuf: invalid input range '%s'\n", g.arg);
                return 1;
            }
            lo_r = strtol(g.arg, NULL, 10);
            hi_r = strtol(dash + 1, NULL, 10);
            break;
        }
        case 'H':
            printf("Usage: shuf [OPTION]... [FILE]\n"
                   "  or:  shuf -e [OPTION]... [ARG]...\n"
                   "  or:  shuf -i LO-HI [OPTION]...\n"
                   "Write a random permutation of the input lines to standard output.\n\n"
                   "  -e, --echo                treat each ARG as an input line\n"
                   "  -i, --input-range=LO-HI   treat each number LO through HI as an input line\n"
                   "  -n, --head-count=COUNT    output at most COUNT lines\n"
                   "  -o, --output=FILE         write result to FILE instead of standard output\n"
                   "  -r, --repeat              output lines can be repeated\n"
                   "  -z, --zero-terminated     line delimiter is NUL, not newline\n"
                   "      --help     display this help and exit\n");
            return 0;
        default: lp_getopt_err("shuf", &g); return 1;
        }
    }

    if (hi_r >= lo_r && hi_r >= 0) {
        for (long v = lo_r; v <= hi_r; v++) {
            char b[32];
            snprintf(b, sizeof b, "%ld", v);
            if (!add(b)) return 1;
        }
    } else if (echo_args) {
        for (int i = g.ind; i < argc; i++)
            if (!add(argv[i])) return 1;
    } else {
        int fd = STDIN_FILENO;
        if (g.ind < argc && strcmp(argv[g.ind], "-") != 0) {
            long f = lp_open(argv[g.ind], O_RDONLY, 0);
            if (f < 0) {
                lp_diag("shuf", NULL, NULL, "cannot open", argv[g.ind], (int)-f);
                return 1;
            }
            fd = (int)f;
        }
        static char line[65536];
        while (readrec(fd, line, sizeof line, eol, NULL) >= 0)
            if (!add(line)) return 1;
        if (fd != STDIN_FILENO) lp_close(fd);
    }

    if (n == 0) return repeat ? 1 : 0;

    int out = STDOUT_FILENO;
    if (outfile) {
        long f = lp_open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f < 0) {
            lp_diag("shuf", NULL, NULL, "cannot open", outfile, (int)-f);
            return 1;
        }
        out = (int)f;
    }

    if (repeat) {
        long want = head < 0 ? n : head;
        for (long i = 0; i < want; i++) {
            const char *s = lines[randbits() % (u64)n];
            lp_write(out, s, strlen(s));
            lp_write(out, &eol, 1);
        }
    } else {
        /* Fisher-Yates, which gives every ordering the same chance. */
        for (long i = n - 1; i > 0; i--) {
            long j = (long)(randbits() % (u64)(i + 1));
            char *t = lines[i]; lines[i] = lines[j]; lines[j] = t;
        }
        long want = (head < 0 || head > n) ? n : head;
        for (long i = 0; i < want; i++) {
            lp_write(out, lines[i], strlen(lines[i]));
            lp_write(out, &eol, 1);
        }
    }
    if (out != STDOUT_FILENO) lp_close(out);
    return 0;
}
