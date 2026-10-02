/* cp - copy files.
 *
 *   cp [-fnvRLP] <source> <dest>
 *   cp [-fnvRLP] <source>... <directory>
 *
 * Permissions follow the source. Owner and timestamps do not - there is
 * one user on this system, so they would mean nothing, and GNU only
 * copies them when asked with -p, which is not here either.
 *
 * Copying a file onto itself is refused before anything is opened,
 * because O_TRUNC on the destination would empty the source first and
 * the contents would be gone. The test is device plus inode, not the
 * path text, so `cp a link-to-a` is caught as well.
 *
 * -q is this system's own, and the only option here GNU does not have:
 * /etc/rc uses `cp -q` to mean "copy it if it is there, say nothing if
 * it is not". Our shell has no `if` and no `test`, so without it the
 * boot script has no way to express that. The exit status still says it
 * failed; only the message goes away.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define BUF_SIZE  65536

/* linux_dirent64 offsets. We read by offset rather than declaring a
 * struct, so nothing depends on the compiler's padding. (Same as ls.c) */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

#define ENOENT  2
#define ENOTDIR 20
#define EEXIST  17

static bool recursive  = false;
static bool no_clobber = false;
static bool force      = false;
static bool verbose    = false;
static bool quiet      = false;      /* -q, see the header */
static int  deref      = 0;          /* -L: 1, follow every link; -P: -1, none */
static int  failures   = 0;

/* A missing source under -q is the case the boot script asks us to keep
 * quiet about. Everything else still gets a line. */
static void diag(const char *before, const char *after, const char *lp,
                 const char *path, long rc)
{
    failures = 1;
    if (quiet && -rc == ENOENT)
        return;
    lp_diag("cp", before, after, lp, path, (int)-rc);
}

/* Last element of a path. "/a/b/c" -> "c", "/a/b/" -> "b" */
static const char *basename_of(const char *path)
{
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' && p[1] != '\0')
            last = p + 1;
    return last;
}

/* Join dir and name. false if it would not fit. */
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

static void announce(const char *src, const char *dst)
{
    if (verbose) printf("'%s' -> '%s'\n", src, dst);
}

static int copy_file(const char *src, const char *dst, const lp_stat_t *st)
{
    if (no_clobber && lp_exists(dst))
        return 0;               /* already there - leaving it alone is success */

    long in = lp_open(src, O_RDONLY, 0);
    if (in < 0) { diag("cannot open", "for reading", "cannot open", src, in); return 1; }

    /* Create it with the source's permissions, so an executable stays one. */
    long out = lp_open(dst, O_WRONLY | O_CREAT | O_TRUNC, st->mode & 07777);
    if (out < 0 && force) {
        /* -f: a destination that cannot be opened is removed and remade,
         * which is the only way past a file whose mode forbids writing. */
        lp_unlink(dst);
        out = lp_open(dst, O_WRONLY | O_CREAT | O_TRUNC, st->mode & 07777);
    }
    if (out < 0) {
        lp_close((int)in);
        diag("cannot create regular file", NULL, "cannot create", dst, out);
        return 1;
    }
    announce(src, dst);

    static char buf[BUF_SIZE];
    int rc = 0;
    for (;;) {
        long n = lp_read((int)in, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) { diag("error reading", NULL, "cannot read", src, n); rc = 1; break; }

        long off = 0;
        while (off < n) {
            long w = lp_write((int)out, buf + off, (size_t)(n - off));
            if (w <= 0) {
                diag("error writing", NULL, "cannot write", dst, w ? w : -5);
                rc = 1;
                break;
            }
            off += w;
        }
        if (rc) break;
    }

    lp_close((int)in);
    lp_close((int)out);

    /* umask trims the mode O_CREAT asked for. Set it exactly. */
    if (rc == 0)
        lp_chmod(dst, st->mode & 07777);
    return rc;
}

static int copy_any(const char *src, const char *dst, bool cmdline);

static int copy_dir(const char *src, const char *dst, const lp_stat_t *st)
{
    long r = lp_mkdir(dst, st->mode & 07777);
    if (r < 0 && r != -EEXIST) {
        diag("cannot create directory", NULL, "cannot create", dst, r);
        return 1;
    }
    if (r == 0) announce(src, dst);

    long fd = lp_open(src, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) { diag("cannot open", "for reading", "cannot open", src, fd); return 1; }

    /* Keep the buffer on the stack. As a static, a recursive call would
     * overwrite the parent's and entries would vanish silently. 8KB a level. */
    char dbuf[8192];
    int  rc = 0;

    /* getdents does not hand over everything at once. Loop until it returns 0. */
    for (;;) {
        long n = sys_getdents((int)fd, dbuf, sizeof(dbuf));
        if (n == 0) break;
        if (n < 0) { diag("error reading", NULL, "cannot read", src, n); rc = 1; break; }

        for (long off = 0; off < n; ) {
            char       *rec  = dbuf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            char s[512], t[512];
            if (!join(s, sizeof(s), src, name) ||
                !join(t, sizeof(t), dst, name)) {
                diag("cannot create regular file", NULL, "path too long", name, -36);
                rc = 1;
                continue;
            }
            rc |= copy_any(s, t, false);
        }
    }

    lp_close((int)fd);
    return rc;
}

/* A symlink named on the command line is followed - `cp link out` gives
 * a copy of the file, which is what GNU does and what people expect. A
 * symlink found inside a directory being copied is recreated as a link,
 * which is also what GNU does: -R without -P still preserves them.
 * -L follows every link and -P none, as in GNU; scripts written for
 * GNU cp use `cp -L` (lp-bugreport did, and copied nothing). */
static int copy_any(const char *src, const char *dst, bool cmdline)
{
    lp_stat_t st;
    long r = lp_stat(src, &st, deref > 0 || (deref == 0 && cmdline));
    if (r < 0) { diag("cannot stat", NULL, "cannot stat", src, r); return 1; }

    /* Same file: opening the destination would truncate the source. */
    lp_stat_t ds;
    if (lp_stat(dst, &ds, false) == 0 && ds.dev == st.dev && ds.ino == st.ino) {
        dprintf(STDERR_FILENO, "cp: '%s' and '%s' are the same file\n", src, dst);
        failures = 1;
        return 1;
    }

    if ((st.mode & LP_S_IFMT) == LP_S_IFLNK) {
        char target[512];
        long n = lp_readlink(src, target, sizeof(target) - 1);
        if (n < 0) { diag("cannot read symbolic link", NULL, "cannot read the link", src, n); return 1; }
        target[n] = '\0';
        lp_unlink(dst);                        /* replace any existing entry */
        long lr = lp_symlink(target, dst);
        if (lr < 0) { diag("cannot create symbolic link", NULL, "cannot create the link", dst, lr); return 1; }
        announce(src, dst);
        return 0;
    }

    if ((st.mode & LP_S_IFMT) == LP_S_IFDIR) {
        if (!recursive) {
            dprintf(STDERR_FILENO, "cp: -r not specified; omitting directory '%s'\n", src);
            failures = 1;
            return 1;
        }
        return copy_dir(src, dst, &st);
    }

    return copy_file(src, dst, &st);
}

static void usage(int fd)
{
    dprintf(fd, "Usage: cp [OPTION]... SOURCE DEST\n"
                "  or:  cp [OPTION]... SOURCE... DIRECTORY\n"
                "Copy SOURCE to DEST, or multiple SOURCE(s) to DIRECTORY.\n\n"
                "  -L, --dereference            always follow symbolic links in SOURCE\n"
                "  -P, --no-dereference         never follow symbolic links in SOURCE\n"
                "  -f, --force                  if an existing destination file cannot be\n"
                "                                 opened, remove it and try again\n"
                "  -n, --no-clobber             do not overwrite an existing file and do not fail\n"
                "  -R, -r, --recursive          copy directories recursively\n"
                "  -v, --verbose                explain what is being done\n"
                "  -q                           say nothing when the source is missing\n"
                "                                 (this system's own; the exit status still fails)\n"
                "      --help     display this help and exit\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "force", 0, 'f' }, { "no-clobber", 0, 'n' },
        { "recursive", 0, 'R' }, { "verbose", 0, 'v' },
        { "dereference", 0, 'L' }, { "no-dereference", 0, 'P' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "fnqrRvLP", lo);
    for (int c; (c = lp_getopt(&g)) != -1; )
        switch (c) {
        case 'f': force = true; break;
        case 'n': no_clobber = true; break;
        case 'q': quiet = true; break;
        case 'r': case 'R': recursive = true; break;
        case 'v': verbose = true; break;
        case 'L': deref = 1; break;
        case 'P': deref = -1; break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default:  lp_getopt_err("cp", &g); return 1;
        }

    int nargs = argc - g.ind;
    if (nargs == 0) {
        dprintf(STDERR_FILENO, "cp: missing file operand\n"
                               "Try 'cp --help' for more information.\n");
        return 1;
    }
    if (nargs == 1) {
        dprintf(STDERR_FILENO, "cp: missing destination file operand after '%s'\n"
                               "Try 'cp --help' for more information.\n", argv[g.ind]);
        return 1;
    }

    const char *dst = argv[argc - 1];
    bool dst_is_dir = lp_is_dir(dst);

    /* With several sources the destination has to be a directory, or
     * each would overwrite the last and only one would survive. GNU
     * names the reason it is not one, so the errno decides the line. */
    if (nargs > 2 && !dst_is_dir) {
        lp_diag("cp", "target", NULL, "not a directory", dst,
                lp_exists(dst) ? ENOTDIR : ENOENT);
        return 1;
    }

    for (int i = g.ind; i < argc - 1; i++) {
        if (dst_is_dir) {
            char full[512];
            if (!join(full, sizeof(full), dst, basename_of(argv[i]))) {
                diag("cannot create regular file", NULL, "path too long", argv[i], -36);
                continue;
            }
            copy_any(argv[i], full, true);
        } else {
            copy_any(argv[i], dst, true);
        }
    }
    return failures;
}
