/* chattr, lsattr - filesystem flags that even root has to undo first.
 *
 *   chattr +i <path>...   immutable: cannot be changed, renamed or deleted
 *   chattr -i <path>...   allow changes again
 *   chattr +a <path>...   append only: can be added to, not overwritten
 *   chattr -a <path>...
 *   lsattr <path>...      show the flags
 *
 * ── What this is for ──
 * Everything on this machine runs as root, so "root can undo it" is
 * true of every protection here. That is not the same as useless.
 *
 * Ransomware and the scripts that carry it do the obvious thing: open
 * the file, write over it, move on. A file marked immutable makes that
 * open fail, and the program does not have code for what to do next. It
 * stops. Somebody sitting at a keyboard would clear the flag and carry
 * on, but the automated case - which is the case that actually happens
 * - does not.
 *
 * The two files worth marking are the ones that decide whether
 * something runs again or somebody gets back in:
 *
 *   chattr +i /root/.ssh/authorized_keys
 *   chattr +i /data/rc.local
 *   chattr +a /data/log/auth
 *
 * The last one is different in kind: append-only means the log can be
 * added to but not rewritten, so an intruder cannot delete the record
 * of their own arrival without first clearing a flag - which is itself
 * something integrity notices.
 *
 * ext4 keeps these flags in the inode, so they survive a reboot. Setting
 * them on a file in the RAM filesystem does nothing useful, because that
 * filesystem is rebuilt from the kernel image every boot anyway.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

/* The generic filesystem flag ioctls.
 *
 * NOT the same number on every architecture, which is what the comment
 * here used to claim: the argument is a long, the size of the argument
 * is part of the ioctl number, and a long is four bytes on the Pi Zero
 * W. Written out as the 64-bit values these came back ENOTTY there, so
 * chattr could neither read nor set a flag on a 32-bit build. */
#define FS_IOC_GETFLAGS  LP_IOR('f', 1, long)
#define FS_IOC_SETFLAGS  LP_IOW('f', 2, long)

#define FS_APPEND_FL     0x00000020
#define FS_IMMUTABLE_FL  0x00000010
#define FS_NODUMP_FL     0x00000040
#define FS_SYNC_FL       0x00000008

static bool get_flags(const char *path, unsigned long *out)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return false;
    long rc = lp_ioctl((int)fd, FS_IOC_GETFLAGS, out);
    lp_close((int)fd);
    return rc == 0;
}

static bool set_flags(const char *path, unsigned long flags)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return false;
    long rc = lp_ioctl((int)fd, FS_IOC_SETFLAGS, &flags);
    lp_close((int)fd);
    return rc == 0;
}

static void show(const char *path)
{
    unsigned long f = 0;
    if (!get_flags(path, &f)) {
        dprintf(STDERR_FILENO, "lsattr: %s: cannot read the flags\n", path);
        return;
    }

    char s[8];
    int  n = 0;
    if (f & FS_IMMUTABLE_FL) s[n++] = 'i';
    if (f & FS_APPEND_FL)    s[n++] = 'a';
    if (f & FS_SYNC_FL)      s[n++] = 'S';
    if (f & FS_NODUMP_FL)    s[n++] = 'd';
    s[n] = '\0';

    printf("%-8s %s\n", n ? s : "-", path);
}

int main(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    bool listing = (strcmp(base, "lsattr") == 0);

    if (argc < 2 || strcmp(argv[1], "-h") == 0) {
        if (listing) {
            printf("usage: lsattr <path>...\n");
        } else {
            printf("usage: chattr +i|-i|+a|-a <path>...\n");
            printf("  +i  immutable - not even root can change it\n");
            printf("      without clearing the flag first\n");
            printf("  +a  append only - can be added to, not overwritten\n\n");
            printf("Worth setting on the two files that decide whether\n");
            printf("something runs again or somebody gets back in:\n");
            printf("  chattr +i /root/.ssh/authorized_keys\n");
            printf("  chattr +i /data/rc.local\n");
        }
        return argc < 2 ? 2 : 0;
    }

    if (listing) {
        for (int i = 1; i < argc; i++)
            show(argv[i]);
        return 0;
    }

    const char *spec = argv[1];
    if ((spec[0] != '+' && spec[0] != '-') || !spec[1]) {
        dprintf(STDERR_FILENO, "chattr: +i, -i, +a or -a\n");
        return 2;
    }

    bool adding = (spec[0] == '+');
    unsigned long bits = 0;

    for (const char *c = spec + 1; *c; c++) {
        switch (*c) {
        case 'i': bits |= FS_IMMUTABLE_FL; break;
        case 'a': bits |= FS_APPEND_FL;    break;
        case 'S': bits |= FS_SYNC_FL;      break;
        case 'd': bits |= FS_NODUMP_FL;    break;
        default:
            dprintf(STDERR_FILENO, "chattr: unknown flag %c\n", *c);
            return 2;
        }
    }

    int rc = 0;
    for (int i = 2; i < argc; i++) {
        unsigned long f = 0;
        if (!get_flags(argv[i], &f)) {
            dprintf(STDERR_FILENO, "chattr: %s: cannot read the flags\n",
                    argv[i]);
            rc = 1;
            continue;
        }

        unsigned long want = adding ? (f | bits) : (f & ~bits);
        if (!set_flags(argv[i], want)) {
            dprintf(STDERR_FILENO,
                    "chattr: %s: cannot set the flags\n"
                    "        (the filesystem may not support them - the\n"
                    "         RAM one does not, only ext4 on /data)\n",
                    argv[i]);
            rc = 1;
            continue;
        }
    }
    return rc;
}
