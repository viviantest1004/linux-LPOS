/* rm - remove files and directories.
 *
 *   rm [-dfrv] <path>...
 *
 * This cannot be undone, so the two guards GNU has are here too and they
 * work the same way: "/" is refused under -r, and "." or ".." are
 * skipped. Both are cheap and both have saved somebody's afternoon.
 *
 * A directory without -r is not refused with a sentence of our own; it
 * fails with EISDIR the way unlink(2) reports it, so the line reads
 * "rm: cannot remove 'x': Is a directory" - the same words as on Ubuntu.
 *
 * -f means two separate things and they are easy to confuse: a path that
 * is not there is not an error, and nothing is ever asked. We never ask
 * anything, so only the first half does any work here.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

/* linux_dirent64 offsets (same as ls.c) */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

#define ENOENT   2
#define EISDIR   21
#define ENOTEMPTY 39

static bool recursive = false;
static bool dir_ok    = false;   /* -d: rmdir an empty directory */
static bool force     = false;
static bool verbose   = false;
static int  failures  = 0;

/* GNU says `rm: cannot remove 'x': <errno>` for every failure, whatever
 * the failure was. Matching that exactly matters more here than the
 * extra precision our own wording had, because this is one of the first
 * errors anybody sees. */
static void oops(const char *what, const char *path, long rc)
{
    if (force && rc == -ENOENT)
        return;
    lp_diag("rm", "cannot remove", NULL, what, path, (int)-rc);
    failures = 1;
}

static bool join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlcpy(out, dir, cap);
    if (n >= cap) return false;
    if (n > 0 && out[n - 1] != '/') {
        if (n + 1 >= cap) return false;
        out[n++] = '/';
        out[n] = '\0';
    }
    return strlcat(out, name, cap) < cap;
}

static int remove_any(const char *path);

static int remove_dir(const char *path)
{
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) { oops("cannot open", path, fd); return 1; }

    /* Keep the buffer on the stack. As a static, recursion would overwrite
     * the parent's listing and rmdir would fail on a non-empty directory. */
    char dbuf[8192];
    int  rc = 0;

    for (;;) {
        long n = sys_getdents((int)fd, dbuf, sizeof(dbuf));
        if (n == 0) break;
        if (n < 0) { oops("cannot read", path, n); rc = 1; break; }

        for (long off = 0; off < n; ) {
            char       *rec  = dbuf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            char child[512];
            if (!join(child, sizeof(child), path, name)) {
                oops("path too long", name, -36);
                rc = 1;
                continue;
            }
            rc |= remove_any(child);
        }
    }

    lp_close((int)fd);

    /* rmdir only works on an empty directory, so anything that failed
     * above makes this fail too. */
    long r = lp_rmdir(path);
    if (r < 0) { oops("cannot remove", path, r); return 1; }
    if (verbose) printf("removed directory '%s'\n", path);
    return rc;
}

static int remove_any(const char *path)
{
    lp_stat_t st;
    long r = lp_stat(path, &st, false);      /* remove the link, not its target */
    if (r < 0) { oops("cannot remove", path, r); return force ? 0 : 1; }

    if ((st.mode & LP_S_IFMT) == LP_S_IFDIR) {
        if (recursive)
            return remove_dir(path);
        if (dir_ok) {
            long d = lp_rmdir(path);
            if (d < 0) { oops("cannot remove", path, d); return 1; }
            if (verbose) printf("removed directory '%s'\n", path);
            return 0;
        }
        /* unlink(2) on a directory is EISDIR, and that is the word GNU
         * prints. Saying "use -r" instead would be friendlier and would
         * also be a sentence nobody meets anywhere else. */
        oops("cannot remove", path, -EISDIR);
        return 1;
    }

    r = lp_unlink(path);
    if (r < 0) { oops("cannot remove", path, r); return force ? 0 : 1; }
    if (verbose) printf("removed '%s'\n", path);
    return 0;
}

/* "." and ".." as the last element: removing them means removing the
 * directory you are standing in by another name. */
static bool is_dot_dir(const char *p)
{
    const char *last = p;
    for (const char *c = p; *c; c++)
        if (*c == '/' && c[1] != '\0')
            last = c + 1;
    size_t n = strlen(last);
    while (n > 0 && last[n - 1] == '/') n--;
    return (n == 1 && last[0] == '.') || (n == 2 && last[0] == '.' && last[1] == '.');
}

static void usage(int fd)
{
    dprintf(fd, "Usage: rm [OPTION]... [FILE]...\n"
                "Remove (unlink) the FILE(s).\n\n"
                "  -f, --force           ignore nonexistent files and arguments, never prompt\n"
                "  -r, -R, --recursive   remove directories and their contents recursively\n"
                "  -d, --dir             remove empty directories\n"
                "  -v, --verbose         explain what is being done\n"
                "      --help     display this help and exit\n\n"
                "By default, rm does not remove directories.  Use the --recursive (-r or -R)\n"
                "option to remove each listed directory, too, along with all of its contents.\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "force", 0, 'f' }, { "recursive", 0, 'r' }, { "dir", 0, 'd' },
        { "verbose", 0, 'v' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "dfrRv", lo);
    for (int c; (c = lp_getopt(&g)) != -1; )
        switch (c) {
        case 'd': dir_ok = true; break;
        case 'f': force = true; break;
        case 'r': case 'R': recursive = true; break;
        case 'v': verbose = true; break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default:  lp_getopt_err("rm", &g); return 1;
        }

    if (g.ind >= argc) {
        /* `rm -f` with nothing to remove has removed everything it was
         * asked to, so it succeeds and says nothing. */
        if (force) return 0;
        dprintf(STDERR_FILENO, "rm: missing operand\n"
                               "Try 'rm --help' for more information.\n");
        return 1;
    }

    for (int i = g.ind; i < argc; i++) {
        const char *p = argv[i];

        if (is_dot_dir(p)) {
            dprintf(STDERR_FILENO,
                    "rm: refusing to remove '.' or '..' directory: skipping '%s'\n", p);
            failures = 1;
            continue;
        }
        /* Only -r can reach the whole tree, so only -r needs the guard;
         * a plain `rm /` fails with EISDIR on its own. */
        if (recursive) {
            bool only_slash = p[0] == '/';
            for (const char *c = p; *c && only_slash; c++)
                if (*c != '/') only_slash = false;
            if (only_slash) {
                dprintf(STDERR_FILENO,
                        "rm: it is dangerous to operate recursively on '%s'\n"
                        "rm: use --no-preserve-root to override this failsafe\n", p);
                failures = 1;
                continue;
            }
        }
        remove_any(p);
    }
    return failures;
}
