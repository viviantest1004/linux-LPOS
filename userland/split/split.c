/* split - one file into several.
 *
 *   split -l 1000 big.log      1000 lines each: xaa, xab, ...
 *   split -b 10M big.bin part_ 10 megabytes each
 *   split -n 4 big.bin         four pieces
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static const char *prog = "split";
static int  suffix_len = 2;
static bool numeric_suffix = false;
static const char *add_suffix = "";

static void name_for(char *out, size_t cap, const char *prefix, long n)
{
    char sfx[32];
    if (numeric_suffix) {
        snprintf(sfx, sizeof sfx, "%0*ld", suffix_len, n);
    } else {
        for (int i = suffix_len - 1; i >= 0; i--) {
            sfx[i] = (char)('a' + (n % 26));
            n /= 26;
        }
        sfx[suffix_len] = '\0';
    }
    snprintf(out, cap, "%s%s%s", prefix, sfx, add_suffix);
}

static u64 parse_size(const char *s)
{
    char *end;
    u64 v = (u64)strtoll(s, &end, 10);
    switch (*end) {
    case 'K': case 'k': v *= 1024; break;
    case 'M': case 'm': v *= 1048576; break;
    case 'G': case 'g': v *= 1073741824ULL; break;
    case 'b':           v *= 512; break;
    default: break;
    }
    return v;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "lines", 1, 'l' }, { "bytes", 1, 'b' }, { "number", 1, 'n' },
        { "suffix-length", 1, 'a' }, { "numeric-suffixes", 2, 'd' },
        { "additional-suffix", 1, 'S' }, { "verbose", 0, 'v' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    long lines = 1000;
    u64  bytes = 0, chunks = 0;
    bool verbose = false;

    lp_getopt_t g;
    lp_getopt_init_ex(&g, argc, argv, "l:b:n:a:dvC:", lo, LP_GETOPT_NEGNUM);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'l': lines = strtol(g.arg, NULL, 10); bytes = chunks = 0; break;
        case 'b': bytes = parse_size(g.arg); lines = 0; chunks = 0; break;
        case 'C': bytes = parse_size(g.arg); lines = 0; chunks = 0; break;
        case 'n': chunks = (u64)strtoll(g.arg, NULL, 10); lines = 0; bytes = 0; break;
        case 'a': suffix_len = (int)strtol(g.arg, NULL, 10); break;
        case 'd': numeric_suffix = true; break;
        case 'S': add_suffix = g.arg; break;
        case 'v': verbose = true; break;
        case 'H':
            printf("Usage: split [OPTION]... [FILE [PREFIX]]\n"
                   "Output pieces of FILE to PREFIXaa, PREFIXab, ...\n\n"
                   "  -a, --suffix-length=N   generate suffixes of length N (default 2)\n"
                   "  -b, --bytes=SIZE        put SIZE bytes per output file\n"
                   "  -d, --numeric-suffixes  use numeric suffixes starting at 0\n"
                   "  -l, --lines=NUMBER      put NUMBER lines per output file\n"
                   "  -n, --number=CHUNKS     split into CHUNKS files\n"
                   "      --additional-suffix=SUFFIX  append SUFFIX to file names\n"
                   "  -v, --verbose           print a message before each output file\n"
                   "      --help     display this help and exit\n");
            return 0;
        default: lp_getopt_err(prog, &g); return 1;
        }
    }
    if (suffix_len < 1 || suffix_len > 20) suffix_len = 2;

    int fd = STDIN_FILENO;
    const char *prefix = "x";
    if (g.ind < argc && strcmp(argv[g.ind], "-") != 0) {
        long f = lp_open(argv[g.ind], O_RDONLY, 0);
        if (f < 0) {
            lp_diag(prog, "cannot open", "for reading", "cannot open",
                    argv[g.ind], (int)-f);
            return 1;
        }
        fd = (int)f;
    }
    if (g.ind + 1 < argc) prefix = argv[g.ind + 1];

    /* -n needs the size in advance, which only works on a real file. */
    if (chunks) {
        lp_stat_t st;
        if (fd == STDIN_FILENO || lp_stat(argv[g.ind], &st, true) < 0) {
            dprintf(STDERR_FILENO, "%s: -n needs a file it can measure\n", prog);
            return 1;
        }
        bytes = (st.size + chunks - 1) / chunks;
        if (bytes == 0) bytes = 1;
    }

    char  name[1024];
    long  piece = 0;
    int   out = -1;
    u64   in_piece = 0;          /* bytes written to the current piece */
    long  lines_done = 0;
    char  buf[65536];

    /* Open the next piece only when there is something to put in it, so
     * an input that divides exactly does not leave an empty last file. */
    for (;;) {
        long n = lp_read(fd, buf, sizeof buf);
        if (n <= 0) break;

        long i = 0;
        while (i < n) {
            if (out < 0) {
                name_for(name, sizeof name, prefix, piece++);
                long f = lp_open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (f < 0) {
                    lp_diag(prog, "cannot open", "for writing", "cannot open",
                            name, (int)-f);
                    return 1;
                }
                out = (int)f;
                if (verbose) printf("creating file '%s'\n", name);
                in_piece = 0;
                lines_done = 0;
            }

            long take;
            if (bytes) {
                take = n - i;
                if ((u64)take > bytes - in_piece) take = (long)(bytes - in_piece);
            } else {
                /* Up to and including the newline that fills the piece. */
                take = 0;
                while (i + take < n) {
                    take++;
                    if (buf[i + take - 1] == '\n' && ++lines_done >= lines)
                        break;
                }
            }

            lp_write(out, buf + i, (size_t)take);
            i += take;
            in_piece += (u64)take;

            if ((bytes && in_piece >= bytes) || (!bytes && lines_done >= lines)) {
                lp_close(out);
                out = -1;
            }
        }
    }
    if (out >= 0) lp_close(out);
    if (fd != STDIN_FILENO) lp_close(fd);
    return 0;
}
