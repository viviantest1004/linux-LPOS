/* mv - move or rename files.
 *
 *   mv [-fnv] <source> <dest>
 *   mv [-fnv] <source>... <directory>
 *
 * Within one filesystem this is a single rename. No data moves - only the
 * directory entry changes - so it is instant whatever the file size, and a
 * power cut cannot leave a half-written file behind.
 *
 * Across filesystems rename fails with EXDEV - moving from /tmp (RAM) to
 * /data (SD card), say. Then we copy and remove the original instead.
 * We do that here rather than calling cp, because the original must not be
 * removed when the copy fails.
 *
 * GNU stats the source before it renames, so a source that is not there
 * comes out as "cannot stat" rather than as a rename failure. That is the
 * line people have seen a thousand times, so this does the same.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define BUF_SIZE 65536
#define ENOENT   2
#define ENOTDIR  20
#define EISDIR   21
#define EXDEV    18

static int  failures = 0;
static bool no_clobber = false;    /* -n */
static bool verbose    = false;    /* -v */

static const char *basename_of(const char *path)
{
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' && p[1] != '\0')
            last = p + 1;
    return last;
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

/* Two paths in one line, which lp_diag does not do because nothing else
 * needs it. The voice switch still applies. */
static void diag2(const char *what, const char *a, const char *b, int err)
{
    const char *msg = lp_strerror(err);
    if (lp_voice() == LP_VOICE_GNU && msg)
        dprintf(STDERR_FILENO, "mv: %s '%s' to '%s': %s\n", what, a, b, msg);
    else
        dprintf(STDERR_FILENO, "mv: %s -> %s: %s (%d)\n", a, b, what, err);
}

/* Crossing filesystems: copy, check, then remove. The order matters. */
static int move_across(const char *src, const char *dst)
{
    lp_stat_t st;
    long r = lp_stat(src, &st, true);
    if (r < 0) {
        lp_diag("mv", "cannot stat", NULL, "cannot read", src, (int)-r);
        return 1;
    }
    if ((st.mode & LP_S_IFMT) == LP_S_IFDIR) {
        /* GNU copies the tree and unlinks it. We do not, because doing
         * it wrong loses a directory, and `cp -r` then `rm -r` is two
         * commands that can be checked one at a time. */
        diag2("cannot move", src, dst, EXDEV);
        return 1;
    }

    long in = lp_open(src, O_RDONLY, 0);
    if (in < 0) {
        lp_diag("mv", "cannot open", "for reading", "cannot open", src, (int)-in);
        return 1;
    }
    long out = lp_open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.mode & 07777);
    if (out < 0) {
        lp_close((int)in);
        lp_diag("mv", "cannot create regular file", NULL,
                "cannot create", dst, (int)-out);
        return 1;
    }

    static char buf[BUF_SIZE];
    int rc = 0;
    for (;;) {
        long n = lp_read((int)in, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            lp_diag("mv", "error reading", NULL, "cannot read", src, (int)-n);
            rc = 1; break;
        }
        long off = 0;
        while (off < n) {
            long w = lp_write((int)out, buf + off, (size_t)(n - off));
            if (w <= 0) {
                lp_diag("mv", "error writing", NULL, "cannot write", dst,
                        w ? (int)-w : 5);
                rc = 1; break;
            }
            off += w;
        }
        if (rc) break;
    }
    lp_close((int)in);
    lp_close((int)out);

    if (rc != 0) {
        /* Do not leave a half-written destination. The original stays. */
        lp_unlink(dst);
        return 1;
    }

    lp_chmod(dst, st.mode & 07777);

    long u = lp_unlink(src);
    if (u < 0) {
        lp_diag("mv", "cannot remove", NULL, "cannot remove", src, (int)-u);
        return 1;
    }
    if (verbose) printf("renamed '%s' -> '%s'\n", src, dst);
    return 0;
}

static int move_one(const char *src, const char *dst)
{
    lp_stat_t ss;
    long sr = lp_stat(src, &ss, false);
    if (sr < 0) {
        lp_diag("mv", "cannot stat", NULL, "cannot read", src, (int)-sr);
        return 1;
    }

    lp_stat_t ds;
    bool dst_there = lp_stat(dst, &ds, false) == 0;
    if (dst_there && ds.dev == ss.dev && ds.ino == ss.ino) {
        dprintf(STDERR_FILENO, "mv: '%s' and '%s' are the same file\n", src, dst);
        return 1;
    }
    if (no_clobber && dst_there) {
        /* Not an error - -n asked for this - but silence would leave you
         * wondering which of the two files you are now looking at. */
        dprintf(STDERR_FILENO, "mv: not replacing '%s'\n", dst);
        return 0;
    }

    /* rename() reports these as ENOTDIR and EISDIR, which on their own
     * say nothing about which side was which. GNU spells it out. */
    if (dst_there) {
        bool s_dir = (ss.mode & LP_S_IFMT) == LP_S_IFDIR;
        bool d_dir = (ds.mode & LP_S_IFMT) == LP_S_IFDIR;
        if (s_dir && !d_dir) {
            dprintf(STDERR_FILENO,
                    "mv: cannot overwrite non-directory '%s' with directory '%s'\n",
                    dst, src);
            return 1;
        }
        if (!s_dir && d_dir) {
            dprintf(STDERR_FILENO,
                    "mv: cannot overwrite directory '%s' with non-directory\n", dst);
            return 1;
        }
    }

    long r = lp_rename(src, dst);
    if (r == 0) {
        if (verbose) printf("renamed '%s' -> '%s'\n", src, dst);
        return 0;
    }
    if (r == -EXDEV)
        return move_across(src, dst);

    diag2("cannot move", src, dst, (int)-r);
    return 1;
}

static void usage(int fd)
{
    dprintf(fd, "Usage: mv [OPTION]... SOURCE DEST\n"
                "  or:  mv [OPTION]... SOURCE... DIRECTORY\n"
                "Rename SOURCE to DEST, or move SOURCE(s) to DIRECTORY.\n\n"
                "  -f, --force                  do not prompt before overwriting\n"
                "  -n, --no-clobber             do not overwrite an existing file\n"
                "  -v, --verbose                explain what is being done\n"
                "      --help     display this help and exit\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "force", 0, 'f' }, { "no-clobber", 0, 'n' },
        { "verbose", 0, 'v' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "fnv", lo);
    for (int c; (c = lp_getopt(&g)) != -1; )
        switch (c) {
        case 'f': no_clobber = false; break;  /* we never prompt anyway */
        case 'n': no_clobber = true;  break;
        case 'v': verbose = true; break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default:  lp_getopt_err("mv", &g); return 1;
        }

    int nargs = argc - g.ind;
    if (nargs == 0) {
        dprintf(STDERR_FILENO, "mv: missing file operand\n"
                               "Try 'mv --help' for more information.\n");
        return 1;
    }
    if (nargs == 1) {
        dprintf(STDERR_FILENO, "mv: missing destination file operand after '%s'\n"
                               "Try 'mv --help' for more information.\n", argv[g.ind]);
        return 1;
    }

    const char *dst = argv[argc - 1];
    bool dst_is_dir = lp_is_dir(dst);

    if (nargs > 2 && !dst_is_dir) {
        lp_diag("mv", "target", NULL, "not a directory", dst,
                lp_exists(dst) ? ENOTDIR : ENOENT);
        return 1;
    }

    for (int i = g.ind; i < argc - 1; i++) {
        if (dst_is_dir) {
            char full[512];
            if (!join(full, sizeof(full), dst, basename_of(argv[i]))) {
                lp_diag("mv", "cannot move", NULL, "path too long", argv[i], 36);
                failures = 1;
                continue;
            }
            failures |= move_one(argv[i], full);
        } else {
            failures |= move_one(argv[i], dst);
        }
    }
    return failures;
}
