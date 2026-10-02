/* mktemp - a name nothing else has.
 *
 *   mktemp                     /tmp/tmp.XXXXXXXXXX, created
 *   mktemp -d                  a directory instead
 *   mktemp -t myprog.XXXXXX    in $TMPDIR
 *
 * The point is that it creates the file, so nothing can slip in between
 * choosing the name and using it. A script that builds its own name
 * from $$ and then opens it has a race that this does not.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static const char *prog = "mktemp";

static void fill_x(char *tpl)
{
    static const char set[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    size_t len = strlen(tpl);
    size_t end = len;
    while (end > 0 && tpl[end - 1] == 'X') end--;
    size_t nx = len - end;
    unsigned char r[64];
    if (nx > sizeof r) nx = sizeof r;
    if (lp_getrandom(r, nx, 0) != (long)nx) {
        u64 seed = (u64)lp_monotonic_ms() ^ ((u64)lp_getpid() << 20);
        for (size_t i = 0; i < nx; i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            r[i] = (unsigned char)(seed >> 33);
        }
    }
    for (size_t i = 0; i < nx; i++)
        tpl[end + i] = set[r[i] % (sizeof set - 1)];
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "directory", 0, 'd' }, { "dry-run", 0, 'u' },
        { "quiet", 0, 'q' }, { "tmpdir", 2, 'p' }, { "suffix", 1, 'S' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    bool dir = false, dry = false, quiet = false, use_tmpdir = false;
    const char *tmpdir = NULL;

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "duqp:t", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'd': dir = true; break;
        case 'u': dry = true; break;
        case 'q': quiet = true; break;
        case 'p': tmpdir = g.arg ? g.arg : NULL; use_tmpdir = true; break;
        case 't': use_tmpdir = true; break;
        case 'S': break;
        case 'H':
            printf("Usage: mktemp [OPTION]... [TEMPLATE]\n"
                   "Create a temporary file or directory, safely, and print its name.\n"
                   "TEMPLATE must contain at least 3 consecutive 'X's; the default is\n"
                   "tmp.XXXXXXXXXX.\n\n"
                   "  -d, --directory     create a directory, not a file\n"
                   "  -u, --dry-run       do not create anything, merely print a name\n"
                   "  -q, --quiet         suppress diagnostics about failures\n"
                   "  -p, --tmpdir[=DIR]  interpret TEMPLATE relative to DIR\n"
                   "      --help     display this help and exit\n");
            return 0;
        default: lp_getopt_err(prog, &g); return 1;
        }
    }

    char tpl[1024];
    const char *given = (g.ind < argc) ? argv[g.ind] : "tmp.XXXXXXXXXX";
    bool absolute = (given[0] == '/');

    if (!tmpdir) tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir) tmpdir = "/tmp";

    if (absolute || (g.ind < argc && !use_tmpdir && strchr(given, '/')))
        strlcpy(tpl, given, sizeof tpl);
    else if (g.ind < argc && !use_tmpdir)
        strlcpy(tpl, given, sizeof tpl);
    else
        snprintf(tpl, sizeof tpl, "%s/%s", tmpdir, given);

    if (!strstr(tpl, "XXX")) {
        if (!quiet)
            dprintf(STDERR_FILENO, "%s: too few X's in template '%s'\n", prog, given);
        return 1;
    }

    for (int tries = 0; tries < 100; tries++) {
        char name[1024];
        strlcpy(name, tpl, sizeof name);
        fill_x(name);

        if (dry) { printf("%s\n", name); return 0; }

        if (dir) {
            if (lp_mkdir(name, 0700) == 0) { printf("%s\n", name); return 0; }
        } else {
            long fd = lp_open(name, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) { lp_close((int)fd); printf("%s\n", name); return 0; }
        }
    }

    if (!quiet)
        dprintf(STDERR_FILENO, "%s: failed to create %s via template '%s'\n",
                prog, dir ? "directory" : "file", tpl);
    return 1;
}
