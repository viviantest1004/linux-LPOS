/* ln - make another name for a file.
 *
 *   ln -s <target> <name>     a symbolic link
 *   ln <file> <name>          a hard link
 *
 * A symbolic link is a small file holding a path; a hard link is a
 * second directory entry for the same data. The symbolic one is what
 * people mean, and it is the one that works across filesystems - which
 * matters here, where /data and / are different filesystems.
 *
 * When the last argument is a directory the link goes inside it under
 * the target's own name, and with a single argument it goes into the
 * current directory the same way. /etc/rc needs the first form to link
 * a directory's worth of libraries into /lib without a basename command
 * to take the paths apart.
 *
 * A hard link across filesystems fails with EXDEV, and the errno alone
 * does not say what to do about it. Under `voice lp` a second line does;
 * under GNU's voice it would be a line Ubuntu never prints, so it is not
 * there.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "syscall.h"

#define AT_FDCWD (-100)
#define ENOENT   2
#define ENOTDIR  20
#define EXDEV    18

static bool symbolic = false;   /* -s */
static bool force    = false;   /* -f */
static bool verbose  = false;   /* -v */

static bool is_directory(const char *path)
{
    lp_stat_t st;
    if (lp_stat(path, &st, true) < 0)
        return false;
    return (st.mode & LP_S_IFMT) == LP_S_IFDIR;
}

static bool join(char *out, size_t cap, const char *dir, const char *name)
{
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    size_t n = strlcpy(out, dir, cap);
    if (n >= cap) return false;
    if (n > 0 && out[n - 1] != '/') {
        if (n + 1 >= cap) return false;
        out[n++] = '/';
        out[n] = '\0';
    }
    return strlcat(out, base, cap) < cap;
}

static int link_one(const char *target, const char *name)
{
    if (force)
        lp_unlink(name);       /* a destination in the way is removed */

    long r;
    if (symbolic) {
        r = lp_symlink(target, name);
    } else {
        /* A hard link to something that is not there is a different
         * failure from a link that cannot be made, and GNU says so. */
        if (!lp_exists(target)) {
            lp_diag("ln", "failed to access", NULL, "not there", target, ENOENT);
            return 1;
        }
        /* linkat(AT_FDCWD, old, AT_FDCWD, new, 0) */
        r = sys_call5(SYS_linkat, AT_FDCWD, (long)target,
                      AT_FDCWD, (long)name, 0);
    }

    if (r < 0) {
        lp_diag("ln", symbolic ? "failed to create symbolic link"
                               : "failed to create hard link",
                NULL, "cannot create the link", name, (int)-r);
        if (!symbolic && -r == EXDEV && lp_voice() == LP_VOICE_LP)
            dprintf(STDERR_FILENO,
                    "ln: a hard link cannot cross filesystems; -s makes a "
                    "symbolic one, which can.\n");
        return 1;
    }
    if (verbose) printf("'%s' -> '%s'\n", name, target);
    return 0;
}

static void usage(int fd)
{
    dprintf(fd, "Usage: ln [OPTION]... TARGET LINK_NAME\n"
                "  or:  ln [OPTION]... TARGET\n"
                "  or:  ln [OPTION]... TARGET... DIRECTORY\n"
                "Create hard links by default, symbolic links with --symbolic.\n\n"
                "  -f, --force                 remove existing destination files\n"
                "  -s, --symbolic              make symbolic links instead of hard links\n"
                "  -v, --verbose               print name of each linked file\n"
                "      --help     display this help and exit\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "force", 0, 'f' }, { "symbolic", 0, 's' },
        { "verbose", 0, 'v' }, { "no-dereference", 0, 'n' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "fnsv", lo);
    for (int c; (c = lp_getopt(&g)) != -1; )
        switch (c) {
        case 'f': force = true; break;
        case 'n': break;      /* we never follow a symlink destination */
        case 's': symbolic = true; break;
        case 'v': verbose = true; break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default:  lp_getopt_err("ln", &g); return 1;
        }

    int nargs = argc - g.ind;
    if (nargs == 0) {
        dprintf(STDERR_FILENO, "ln: missing file operand\n"
                               "Try 'ln --help' for more information.\n");
        return 1;
    }

    char joined[512];
    if (nargs == 1) {
        /* One argument means "here, under its own name". */
        if (!join(joined, sizeof joined, ".", argv[g.ind])) {
            lp_diag("ln", "failed to create hard link", NULL, "path too long",
                    argv[g.ind], 36);
            return 1;
        }
        return link_one(argv[g.ind], joined);
    }

    const char *dest = argv[argc - 1];
    bool dest_is_dir = is_directory(dest);

    if (nargs > 2 && !dest_is_dir) {
        lp_diag("ln", "target", NULL, "not a directory", dest,
                lp_exists(dest) ? ENOTDIR : ENOENT);
        return 1;
    }

    int rc = 0;
    for (int i = g.ind; i < argc - 1; i++) {
        if (dest_is_dir) {
            if (!join(joined, sizeof joined, dest, argv[i])) {
                lp_diag("ln", "failed to create hard link", NULL,
                        "path too long", argv[i], 36);
                rc = 1;
                continue;
            }
            rc |= link_one(argv[i], joined);
        } else {
            rc |= link_one(argv[i], dest);
        }
    }
    return rc;
}
