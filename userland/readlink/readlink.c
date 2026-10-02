/* readlink - what a symbolic link points at.
 *
 *   readlink LINK        the target, exactly as it was written
 *   readlink -f PATH     the whole path resolved, following every link
 *   readlink -m PATH     the same, but the last part need not exist
 *
 * Scripts use -f the way they use realpath: to turn "$0" into the file
 * it really is before looking next to it for something else.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static const char *prog = "readlink";
static bool quiet = true;      /* readlink is silent by default */
static char eol = '\n';
static bool no_newline = false;

/* -f/-e/-m. `missing` says how much of the path may be absent:
 *   0  everything must exist   (-e)
 *   1  the last part may not   (-f)
 *   2  none of it need         (-m) */
static bool resolve(const char *path, char *out, size_t cap, int missing)
{
    char cur[4096];
    if (path[0] == '/') {
        strlcpy(cur, path, sizeof cur);
    } else {
        char cwd[2048];
        if (lp_getcwd(cwd, sizeof cwd) < 0) return false;
        snprintf(cur, sizeof cur, "%s/%s", cwd, path);
    }

    char built[4096] = "";
    size_t blen = 0;
    int    links = 0;
    const char *p = cur;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - seg);

        if (n == 1 && seg[0] == '.') continue;
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            char *slash = strrchr(built, '/');
            if (slash) { *slash = '\0'; blen = strlen(built); }
            continue;
        }

        if (blen + n + 2 >= sizeof built) return false;
        built[blen++] = '/';
        memcpy(built + blen, seg, n);
        blen += n;
        built[blen] = '\0';

        char tgt[4096];
        long r = lp_readlink(built, tgt, sizeof tgt - 1);
        if (r > 0) {
            if (++links > 40) return false;
            tgt[r] = '\0';
            char rest[4096];
            strlcpy(rest, p, sizeof rest);
            if (tgt[0] == '/') {
                snprintf(cur, sizeof cur, "%s%s", tgt, rest);
                built[0] = '\0'; blen = 0;
            } else {
                char *slash = strrchr(built, '/');
                if (slash) *slash = '\0';
                snprintf(cur, sizeof cur, "%s/%s%s", built, tgt, rest);
                built[0] = '\0'; blen = 0;
            }
            p = cur;
            continue;
        }
        /* Not a link. Does it have to be there? */
        bool last = (*p == '\0');
        if (missing < 2 && !(missing == 1 && last) && !lp_exists(built))
            return false;
    }

    if (missing == 0 && !lp_exists(blen ? built : "/")) return false;
    strlcpy(out, blen ? built : "/", cap);
    return true;
}

static void usage(int fd)
{
    dprintf(fd, "Usage: readlink [OPTION]... FILE...\n"
                "Print value of a symbolic link or canonical file name.\n\n"
                "  -f, --canonicalize            all but the last component must exist\n"
                "  -e, --canonicalize-existing   all components must exist\n"
                "  -m, --canonicalize-missing    no components need exist\n"
                "  -n, --no-newline              do not output the trailing delimiter\n"
                "  -q, --quiet, -s, --silent     suppress most error messages (default)\n"
                "  -v, --verbose                 report error messages\n"
                "  -z, --zero                    end each output line with NUL\n"
                "      --help     display this help and exit\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "canonicalize", 0, 'f' }, { "canonicalize-existing", 0, 'e' },
        { "canonicalize-missing", 0, 'm' }, { "no-newline", 0, 'n' },
        { "quiet", 0, 'q' }, { "silent", 0, 's' }, { "verbose", 0, 'v' },
        { "zero", 0, 'z' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    int canon = -1;
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "femnqsvz", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'f': canon = 1; break;
        case 'e': canon = 0; break;
        case 'm': canon = 2; break;
        case 'n': no_newline = true; break;
        case 'q': case 's': quiet = true; break;
        case 'v': quiet = false; break;
        case 'z': eol = '\0'; break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default: lp_getopt_err(prog, &g); return 1;
        }
    }

    if (g.ind >= argc) {
        dprintf(STDERR_FILENO, "%s: missing operand\n", prog);
        dprintf(STDERR_FILENO, "Try '%s --help' for more information.\n", prog);
        return 1;
    }

    int rc = 0;
    for (int i = g.ind; i < argc; i++) {
        char out[4096];
        bool ok;
        if (canon >= 0) {
            ok = resolve(argv[i], out, sizeof out, canon);
        } else {
            long n = lp_readlink(argv[i], out, sizeof out - 1);
            ok = (n >= 0);
            if (ok) out[n] = '\0';
        }
        if (!ok) {
            if (!quiet)
                lp_diag(prog, NULL, NULL, "cannot read the link", argv[i], 2);
            rc = 1;
            continue;
        }
        if (no_newline) printf("%s", out);
        else            printf("%s%c", out, eol);
    }
    return rc;
}
