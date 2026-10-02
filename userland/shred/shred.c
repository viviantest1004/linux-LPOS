/* shred - overwrite a file before removing it.
 *
 *   shred -u secret.key       three random passes, then delete it
 *   shred -n 1 -z -u f        one pass, then zeros, then delete
 *
 * Read this before trusting it, because the honest answer is not the
 * one the name suggests:
 *
 *   Overwriting a file only erases anything when the filesystem writes
 *   the new bytes over the old ones. On the SD card and the eMMC this
 *   machine runs from, it does not. Flash cannot rewrite a block in
 *   place - the controller writes the new data somewhere else and marks
 *   the old block free, and that old block still holds the old data
 *   until the controller happens to erase it. Nothing a program can do
 *   from up here reaches it. The same is true of any copy-on-write or
 *   log-structured filesystem, and of a journalling one for the parts
 *   of the file that went through the journal.
 *
 *   So this is worth running on a plain ext4 partition on a spinning
 *   disk, and it is close to theatre on flash. If the data must really
 *   be gone from an SD card, encrypt it before it is written, or
 *   destroy the card. A person who trusts shred wrongly is worse off
 *   than one who does not use it, which is why this paragraph is here
 *   and in --help.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static const char *prog = "shred";
static long passes = 3;
static bool zero_last = false, remove_after = false, verbose = false;
static bool force = false;
static s64 want_size = -1;

static u8 buf[65536];

static bool one_pass(int fd, s64 size, long n, bool zeros, const char *name)
{
    if (zeros) memset(buf, 0, sizeof buf);

    if (lp_lseek(fd, 0, 0) < 0) return false;

    s64 left = size;
    while (left > 0) {
        size_t chunk = left > (s64)sizeof buf ? sizeof buf : (size_t)left;
        if (!zeros && lp_getrandom(buf, chunk, 0) != (long)chunk) {
            /* getrandom can come up short before the pool is ready.
             * A weaker pattern still overwrites; saying nothing and
             * writing zeros would be a quieter lie. */
            for (size_t i = 0; i < chunk; i++)
                buf[i] = (u8)(i * 31 + n * 17);
        }
        long w = lp_write(fd, buf, chunk);
        if (w <= 0) {
            dprintf(STDERR_FILENO, "%s: %s: error writing at offset %lld\n",
                    prog, name, (long long)(size - left));
            return false;
        }
        left -= w;
    }
    lp_fsync(fd);
    return true;
}

static void usage(int fd)
{
    dprintf(fd, "Usage: shred [OPTION]... FILE...\n"
                "Overwrite the specified FILE(s) repeatedly, to make it harder\n"
                "for even very expensive hardware probing to recover the data.\n\n"
                "  -f, --force    change permissions to allow writing if necessary\n"
                "  -n, --iterations=N  overwrite N times instead of the default (3)\n"
                "  -s, --size=N   shred this many bytes (suffixes like K, M, G accepted)\n"
                "  -u, --remove   deallocate and remove the file after overwriting\n"
                "  -v, --verbose  show progress\n"
                "  -z, --zero     add a final overwrite with zeros to hide shredding\n"
                "      --help     display this help and exit\n\n"
                "CAUTION: on the SD card or eMMC this machine boots from, overwriting a\n"
                "file does not necessarily overwrite the blocks that held it. Flash\n"
                "cannot rewrite a block in place: the controller puts the new data\n"
                "elsewhere and marks the old block free, and the old data stays there\n"
                "until it happens to be erased. The same goes for copy-on-write and\n"
                "log-structured filesystems. To really destroy data on a card, encrypt\n"
                "it before writing it, or destroy the card.\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "force", 0, 'f' }, { "iterations", 1, 'n' }, { "size", 1, 's' },
        { "remove", 2, 'u' }, { "verbose", 0, 'v' }, { "zero", 0, 'z' },
        { "exact", 0, 'x' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "fn:s:uvzx", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'f': force = true; break;
        case 'n': passes = strtol(g.arg, NULL, 10); break;
        case 'u': remove_after = true; break;
        case 'v': verbose = true; break;
        case 'z': zero_last = true; break;
        case 'x': break;
        case 's': {
            char *end;
            want_size = strtoll(g.arg, &end, 10);
            switch (*end) {
            case 'K': case 'k': want_size *= 1024; break;
            case 'M': case 'm': want_size *= 1048576; break;
            case 'G': case 'g': want_size *= 1073741824LL; break;
            default: break;
            }
            break;
        }
        case 'H': usage(STDOUT_FILENO); return 0;
        default: lp_getopt_err(prog, &g); return 1;
        }
    }

    if (g.ind >= argc) {
        dprintf(STDERR_FILENO, "%s: missing file operand\n", prog);
        dprintf(STDERR_FILENO, "Try '%s --help' for more information.\n", prog);
        return 1;
    }

    int rc = 0;
    for (int i = g.ind; i < argc; i++) {
        const char *name = argv[i];

        lp_stat_t st;
        if (lp_stat(name, &st, true) < 0) {
            dprintf(STDERR_FILENO, "%s: %s: failed to open for writing: "
                                   "No such file or directory\n", prog, name);
            rc = 1;
            continue;
        }
        if ((st.mode & LP_S_IFMT) == LP_S_IFDIR) {
            dprintf(STDERR_FILENO, "%s: %s: invalid file type\n", prog, name);
            rc = 1;
            continue;
        }

        if (force) lp_chmod(name, (st.mode & 07777) | 0600);

        long fd = lp_open(name, O_WRONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "%s: %s: failed to open for writing: %s\n",
                    prog, name, lp_strerror((int)-fd));
            rc = 1;
            continue;
        }

        s64 size = want_size >= 0 ? want_size : (s64)st.size;
        long total = passes + (zero_last ? 1 : 0);
        bool ok = true;

        for (long n = 1; n <= passes && ok; n++) {
            if (verbose)
                dprintf(STDERR_FILENO, "%s: %s: pass %ld/%ld (random)...\n",
                        prog, name, n, total);
            ok = one_pass((int)fd, size, n, false, name);
        }
        if (ok && zero_last) {
            if (verbose)
                dprintf(STDERR_FILENO, "%s: %s: pass %ld/%ld (000000)...\n",
                        prog, name, total, total);
            ok = one_pass((int)fd, size, 0, true, name);
        }
        lp_close((int)fd);

        if (ok && remove_after) {
            if (verbose)
                dprintf(STDERR_FILENO, "%s: %s: removing\n", prog, name);
            /* Shorten it first: a rename-and-unlink dance cannot help
             * on a filesystem that never rewrote the blocks anyway, and
             * truncating at least frees them. */
            long f2 = lp_open(name, O_WRONLY, 0);
            if (f2 >= 0) { lp_ftruncate((int)f2, 0); lp_close((int)f2); }
            if (lp_unlink(name) < 0) {
                dprintf(STDERR_FILENO, "%s: %s: failed to remove\n", prog, name);
                rc = 1;
            } else if (verbose) {
                dprintf(STDERR_FILENO, "%s: %s: removed\n", prog, name);
            }
        }
        if (!ok) rc = 1;
    }
    return rc;
}
