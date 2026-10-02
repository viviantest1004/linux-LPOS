/* realpath - the absolute path, with every symlink followed.
 *
 * The same resolution readlink -f does; this is the name people reach
 * for, and it exists on every distribution.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"


static const char *prog = "realpath";

/* `missing`: 0 every component must exist (-e), 1 all but the last (the
 * default), 2 none of them (-m). */
static bool resolve(const char *path, char *out, size_t cap, int missing)
{
    char cur[4096];
    if (path[0] == '/') strlcpy(cur, path, sizeof cur);
    else {
        char cwd[2048];
        if (lp_getcwd(cwd, sizeof cwd) < 0) return false;
        snprintf(cur, sizeof cur, "%s/%s", cwd, path);
    }

    char built[4096] = "";
    size_t blen = 0;
    int links = 0;
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
            if (tgt[0] == '/') snprintf(cur, sizeof cur, "%s%s", tgt, rest);
            else {
                char *slash = strrchr(built, '/');
                if (slash) *slash = '\0';
                snprintf(cur, sizeof cur, "%s/%s%s", built, tgt, rest);
            }
            built[0] = '\0'; blen = 0;
            p = cur;
            continue;
        }
        bool last = (*p == '\0');
        if (missing == 0 && !lp_exists(built)) return false;
        if (missing == 1 && !last && !lp_exists(built)) return false;
    }
    strlcpy(out, blen ? built : "/", cap);
    return missing != 0 || lp_exists(out);
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "canonicalize-existing", 0, 'e' },
        { "canonicalize-missing", 0, 'm' },
        { "quiet", 0, 'q' }, { "relative-to", 1, 'R' },
        { "logical", 0, 'L' }, { "physical", 0, 'P' },
        { "zero", 0, 'z' }, { "no-symlinks", 0, 's' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    int  missing = 1;              /* the default: only the last may be absent */
    bool quiet = false;
    char eol = '\n';

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "emqzsLP", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'e': missing = 0; break;
        case 'm': missing = 2; break;
        case 'q': quiet = true; break;
        case 'z': eol = '\0'; break;
        case 's': case 'L': case 'P': case 'R': break;
        case 'H':
            dprintf(STDOUT_FILENO,
                    "Usage: realpath [OPTION]... FILE...\n"
                    "Print the resolved absolute file name.\n\n"
                    "  -e, --canonicalize-existing  all components must exist\n"
                    "  -m, --canonicalize-missing   no components need exist\n"
                    "  -q, --quiet                  suppress most error messages\n"
                    "  -z, --zero                   end each output line with NUL\n"
                    "      --help     display this help and exit\n");
            return 0;
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
        if (!resolve(argv[i], out, sizeof out, missing)) {
            if (!quiet)
                lp_diag(prog, NULL, NULL, "cannot resolve", argv[i], 2);
            rc = 1;
            continue;
        }
        printf("%s%c", out, eol);
    }
    return rc;
}
