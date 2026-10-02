/* mkdir - create directories.
 *
 *   mkdir [-pv] [-m MODE] <path>...
 *
 * -p is the interesting one. It walks the path creating each component,
 * and a component that already exists is not an error - which is the
 * whole point, since the caller only wants the path to be there at the
 * end. It does still fail when a component exists and is not a
 * directory, and it names that component rather than the whole path,
 * because "mkdir: cannot create directory 'f': Not a directory" tells
 * you which part of the path is in the way.
 *
 * -m takes an octal mode only. GNU also accepts the symbolic form
 * (u=rwx,go=r); that grammar lives in chmod and is not repeated here, so
 * `mkdir -m u=rwx` is rejected as an invalid mode rather than guessed at.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define EEXIST  17
#define ENOTDIR 20

static mode_t mode = 0777;      /* umask trims it, exactly as GNU does */
static bool   verbose = false;
static bool   mode_given = false;

static void made(const char *path)
{
    if (verbose) printf("mkdir: created directory '%s'\n", path);
}

/* One component. Under -p an existing directory is fine; an existing
 * anything-else is not, and that is what ENOTDIR would say a step later
 * about the wrong path. */
static int make(const char *path, bool parent, mode_t m)
{
    long rc = lp_mkdir(path, m);
    if (rc == 0) {
        made(path);
        /* O_CREAT-style modes are cut by the umask. An explicit -m means
         * exactly that mode, so put it back. */
        if (mode_given && !parent) lp_chmod(path, m);
        return 0;
    }
    if (parent && rc == -EEXIST) {
        if (lp_is_dir(path))
            return 0;
        lp_diag("mkdir", "cannot create directory", NULL, "cannot create",
                path, ENOTDIR);
        return 1;
    }
    lp_diag("mkdir", "cannot create directory", NULL, "cannot create",
            path, (int)-rc);
    return 1;
}

static int mkdir_p(const char *path)
{
    char buf[512];
    if (strlcpy(buf, path, sizeof(buf)) >= sizeof(buf)) {
        lp_diag("mkdir", "cannot create directory", NULL, "path too long",
                path, 36);
        return 1;
    }

    for (char *p = buf + 1; *p; p++) {
        if (*p != '/' || p[1] == '\0')
            continue;
        *p = '\0';
        /* Parents are made 0777 &~umask whatever -m said; GNU applies -m
         * to the directory you asked for, not to the path leading to it. */
        int rc = make(buf, true, 0777);
        *p = '/';
        if (rc) return 1;
    }
    return make(buf, true, mode);
}

static void usage(int fd)
{
    dprintf(fd, "Usage: mkdir [OPTION]... DIRECTORY...\n"
                "Create the DIRECTORY(ies), if they do not already exist.\n\n"
                "  -m, --mode=MODE   set file mode (as in chmod), not a=rwx - umask\n"
                "  -p, --parents     no error if existing, make parent directories as needed\n"
                "  -v, --verbose     print a message for each created directory\n"
                "      --help     display this help and exit\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "mode", 1, 'm' }, { "parents", 0, 'p' }, { "verbose", 0, 'v' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    bool parents = false;
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "m:pv", lo);
    for (int c; (c = lp_getopt(&g)) != -1; )
        switch (c) {
        case 'm': {
            mode_t v = 0;
            const char *s = g.arg;
            if (!s || !*s) s = "";
            for (const char *q = s; ; q++) {
                if (*q == '\0' && q != s) { mode = v; mode_given = true; break; }
                if (*q < '0' || *q > '7') {
                    dprintf(STDERR_FILENO, "mkdir: invalid mode '%s'\n", s);
                    return 1;
                }
                v = (mode_t)(v * 8 + (mode_t)(*q - '0'));
            }
            break;
        }
        case 'p': parents = true; break;
        case 'v': verbose = true; break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default:  lp_getopt_err("mkdir", &g); return 1;
        }

    if (g.ind >= argc) {
        dprintf(STDERR_FILENO, "mkdir: missing operand\n"
                               "Try 'mkdir --help' for more information.\n");
        return 1;
    }

    int rc = 0;
    for (int i = g.ind; i < argc; i++)
        rc |= parents ? mkdir_p(argv[i]) : make(argv[i], false, mode);
    return rc;
}
