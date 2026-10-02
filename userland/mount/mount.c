/* mount - mount and unmount filesystems.
 *
 *   mount                        list what is mounted (/proc/mounts)
 *   mount <device> <dir>         guess the filesystem type
 *   mount -t <type> <device> <dir>
 *   mount -o ro,nosuid ...       flags, and filesystem options such as
 *                                size=64M or errors=remount-ro
 *   mount -L <label> ...         only if the filesystem carries it
 *   mount -w ...                 wait for the device (USB takes a second)
 *   umount <dir>                 (when argv[0] is umount)
 *
 * With no type given we try the list below in order. The kernel returns
 * EINVAL for a type that does not fit, so trying them one by one works.
 * Our kernel only carries a handful of filesystems, so this is quick. */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "syscall.h"

static const char *AUTO_TYPES[] = { "ext4", "vfat", "ext2", NULL };

/* ── Is this the filesystem we meant? ─────────────────────────────────
 *
 * /etc/rc tries four device names for the data partition, because it is
 * called something different depending on whether the machine booted
 * from an SD card, a USB stick, NVMe or a virtual disk. Only one of
 * them exists - normally.
 *
 * Boot from USB with an unrelated SD card still in the slot and there
 * are two, and the SD card is tried first. It would be mounted as
 * /data, and the log, the SSH host key and the boot counter would all
 * be written onto somebody else's card.
 *
 * So the partition can be required to carry a label. mksdcard.sh writes
 * LPZERODATA on the data partition and LPZERO on the boot partition,
 * and -L makes mount check for it.
 *
 * This matters at least as much for the boot partition as for the data
 * one, and for a while it was only done on the data one. /boot is where
 * authorized_keys, firewall.conf, wpa_supplicant.conf and e2fsck come
 * from - an SSH key, a firewall policy, the WiFi credentials, and a
 * binary this system runs as root. Mounting a stranger's FAT partition
 * there is not a filesystem mix-up; it is handing them the machine.
 *
 * Where the label lives depends on the filesystem:
 *
 *   ext2/3/4   in the superblock, 1024 bytes into the partition and
 *              120 bytes into that, NUL padded to 16
 *   FAT32      in the boot sector at offset 71, space padded to 11
 *   FAT12/16   the same, at offset 43
 *
 * The FAT label is also kept in a root directory entry, and a tool that
 * renames the volume may write only that one and leave the boot sector
 * as it was. That suits us: this asks "did we make this", and renaming
 * the drive in Windows should not stop the board booting from it.
 *
 * An unlabelled filesystem is allowed through: images made before the
 * label existed are still in use, and refusing them would break an
 * upgrade for a check that is about not touching what is definitely
 * somebody else's.
 */
#define EXT_SB_OFFSET  1024
#define EXT_MAGIC_OFF    56
#define EXT_LABEL_OFF   120
#define EXT_LABEL_LEN    16
#define EXT_MAGIC    0xEF53

#define FAT32_LABEL_OFF  71
#define FAT16_LABEL_OFF  43
#define FAT_LABEL_LEN    11

/* Copy a fixed-width label out and trim the padding. FAT pads with
 * spaces, ext with NULs, so both are trimmed from the right. */
static void read_fixed_label(const u8 *src, size_t len, char *out)
{
    memcpy(out, src, len);
    out[len] = '\0';
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\0'))
        out[--len] = '\0';
}

static bool label_matches(const char *dev, const char *want)
{
    long fd = lp_open(dev, O_RDONLY, 0);
    if (fd < 0)
        return false;

    /* One read covers both: the FAT boot sector at 0 and the ext
     * superblock at 1024. */
    u8 head[2048];
    memset(head, 0, sizeof(head));
    long n = lp_read((int)fd, head, sizeof(head));
    lp_close((int)fd);

    if (n < (long)sizeof(head))
        return false;

    char label[FAT_LABEL_LEN > EXT_LABEL_LEN
               ? FAT_LABEL_LEN + 1 : EXT_LABEL_LEN + 1];

    u16 magic = (u16)(head[EXT_SB_OFFSET + EXT_MAGIC_OFF] |
                      (head[EXT_SB_OFFSET + EXT_MAGIC_OFF + 1] << 8));
    if (magic == EXT_MAGIC) {
        read_fixed_label(head + EXT_SB_OFFSET + EXT_LABEL_OFF,
                         EXT_LABEL_LEN, label);
    } else if (head[510] == 0x55 && head[511] == 0xAA &&
               (memcmp(head + 0x52, "FAT", 3) == 0 ||
                memcmp(head + 0x36, "FAT", 3) == 0)) {
        int off = (memcmp(head + 0x52, "FAT", 3) == 0)
                  ? FAT32_LABEL_OFF : FAT16_LABEL_OFF;
        read_fixed_label(head + off, FAT_LABEL_LEN, label);
    } else {
        return false;               /* not a filesystem we can identify */
    }

    if (label[0] == '\0')
        return true;                 /* unlabelled: from an older image */

    return strcmp(label, want) == 0;
}

/* Work out what is on a device by looking at it.
 *
 * Trying each filesystem in turn does work - the kernel says EINVAL for
 * one that does not fit - but every failed attempt is a line in the
 * kernel log, and those lines land on the screen during boot where they
 * read as something having gone wrong. Nothing has: we asked a question
 * and got an answer. Reading the superblock ourselves asks it quietly.
 *
 *   ext2/3/4   magic 0xEF53, 56 bytes into the superblock, which itself
 *              starts 1024 bytes in
 *   FAT        the boot sector ends in 0x55 0xAA and names itself, at
 *              0x36 for FAT12/16 and 0x52 for FAT32
 *
 * NULL when it is none of those, and then we fall back to trying. */
static const char *sniff_type(const char *dev)
{
    long fd = lp_open(dev, O_RDONLY, 0);
    if (fd < 0)
        return NULL;

    static u8 buf[2048];
    long n = lp_read((int)fd, buf, sizeof(buf));
    lp_close((int)fd);

    if (n < 2048)
        return NULL;

    /* ext: superblock at 1024, s_magic at +56 */
    u16 magic = (u16)(buf[1024 + 56] | (buf[1024 + 57] << 8));
    if (magic == 0xEF53)
        return "ext4";              /* ext2 and ext3 mount as ext4 too */

    if (buf[510] == 0x55 && buf[511] == 0xAA) {
        if (memcmp(buf + 0x52, "FAT", 3) == 0 ||
            memcmp(buf + 0x36, "FAT", 3) == 0)
            return "vfat";
    }

    return NULL;
}

static int show_mounts(void)
{
    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "mount: cannot read /proc/mounts"
                " (is /proc mounted?)\n");
        return 1;
    }

    char buf[4096];
    for (;;) {
        long n = lp_read((int)fd, buf, sizeof(buf));
        if (n <= 0) break;
        lp_write(STDOUT_FILENO, buf, (size_t)n);
    }
    lp_close((int)fd);
    return 0;
}

static int do_umount(const char *target)
{
    long rc = sys_call2(SYS_umount2, (long)target, 0);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "umount: %s: failed (%ld)\n", target, -rc);
        return 1;
    }
    return 0;
}

/* Split an -o string into the two things it actually holds.
 *
 * Some option words describe this mount and the kernel takes them as
 * bits - nosuid, ro. The rest describe the filesystem and are handed to
 * it as a string: size=64M for tmpfs, errors=remount-ro for ext4. They
 * have to be separated, because a filesystem's own parser rejects the
 * words it has never heard of, and "nosuid" is one of those.
 *
 * Returns the flags; `rest` gets everything not consumed. */
static unsigned long opt_flag(const char *word)
{
    if (strcmp(word, "bind")    == 0) return MS_BIND;
    if (strcmp(word, "ro")      == 0) return MS_RDONLY;
    if (strcmp(word, "noexec")  == 0) return MS_NOEXEC;
    if (strcmp(word, "nosuid")  == 0) return MS_NOSUID;
    if (strcmp(word, "nodev")   == 0) return MS_NODEV;
    if (strcmp(word, "remount") == 0) return MS_REMOUNT;
    return 0;
}

/* "rw" is the absence of MS_RDONLY rather than a flag of its own, so it
 * cannot come back from opt_flag - but it still has to be recognised,
 * or it would be passed through to the filesystem, which has never
 * heard of it and refuses the whole mount. */
static bool opt_is_noop(const char *word)
{
    return strcmp(word, "rw") == 0 || strcmp(word, "defaults") == 0 ||
           strcmp(word, "async") == 0 || strcmp(word, "atime") == 0 ||
           strcmp(word, "dev") == 0 || strcmp(word, "exec") == 0 ||
           strcmp(word, "suid") == 0;
}

static unsigned long parse_options(const char *opts, char *rest, size_t size)
{
    unsigned long flags = 0;

    for (const char *p = opts; *p; ) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);

        char word[64];
        if (len >= sizeof(word))
            len = sizeof(word) - 1;
        memcpy(word, p, len);
        word[len] = '\0';

        unsigned long f = opt_flag(word);
        if (f) {
            flags |= f;
        } else if (opt_is_noop(word)) {
            /* nothing to do, and nothing to pass on */
        } else if (word[0]) {
            if (rest[0])
                strlcat(rest, ",", size);
            strlcat(rest, word, size);
        }

        if (!comma)
            break;
        p = comma + 1;
    }
    return flags;
}

int main(int argc, char **argv)
{
    /* argv[0] decides: the same binary is installed under both names. */
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];

    if (strcmp(base, "umount") == 0) {
        if (argc < 2) {
            dprintf(STDERR_FILENO, "usage: umount <dir>\n");
            return 2;
        }
        return do_umount(argv[1]);
    }

    if (argc == 1)
        return show_mounts();

    const char *type   = NULL;
    const char *want_label = NULL;
    bool  wait_for_it  = false;
    unsigned long flags = 0;
    char  data_buf[256];
    const char *data   = NULL;
    const char *args[2] = { NULL, NULL };
    int nargs = 0;

    data_buf[0] = '\0';

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            type = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            flags |= parse_options(argv[++i], data_buf, sizeof(data_buf));
            data = data_buf[0] ? data_buf : NULL;
        } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            want_label = argv[++i];
        } else if (strcmp(argv[i], "-w") == 0) {
            wait_for_it = true;
        } else if (nargs < 2) {
            args[nargs++] = argv[i];
        }
    }

    /* A remount names one thing, not two: the place it is mounted. The
     * kernel ignores the source and the type for MS_REMOUNT - what it
     * changes is the flags of a mount that already exists. This is how
     * /etc/rc turns the disk-rooted amd64 root read-write after preinit
     * mounted it read-only to let ext4 replay its journal first. */
    if (flags & MS_REMOUNT) {
        if (nargs < 1) {
            dprintf(STDERR_FILENO, "usage: mount -o remount,rw <dir>\n");
            return 2;
        }
        const char *at = args[nargs - 1];
        long rc = lp_mount(nargs > 1 ? args[0] : "none", at, type,
                           flags, data);
        if (rc < 0) {
            dprintf(STDERR_FILENO, "mount: remount %s: failed (%ld)\n",
                    at, -rc);
            return 1;
        }
        return 0;
    }

    if (nargs < 2) {
        dprintf(STDERR_FILENO,
                "usage: mount [-t type] [-o options] [-L label] [-w]"
                " <source> <dir>\n");
        return 2;
    }

    const char *src = args[0], *dst = args[1];

    /* A bind mount has no filesystem type. It makes a directory visible at
     * a second place, so it keeps the type of the original. */
    if (flags & MS_BIND) {
        long rc = lp_mount(src, dst, NULL, flags, NULL);
        if (rc < 0) {
            dprintf(STDERR_FILENO, "mount: bind %s -> %s: failed (%ld)\n",
                    src, dst, -rc);
            return 1;
        }
        return 0;
    }

    if (type) {
        long rc = lp_mount(src, dst, type, flags, data);
        if (rc < 0) {
            dprintf(STDERR_FILENO, "mount: %s -> %s (%s): failed (%ld)\n",
                    src, dst, type, -rc);
            return 1;
        }
        return 0;
    }

    /* -w: wait for the device to turn up.
     *
     * USB takes about a second to enumerate, and /etc/rc runs long
     * before that. Without this, booting from a USB stick left /boot
     * unmounted - and /boot is where authorized_keys, firewall.conf and
     * e2fsck come from, so the machine came up with no way to log into
     * it. expandfs has had the same flag for the same reason; mounting
     * needed it too and did not have it.
     *
     * Only worth passing on the USB attempt. The others are either
     * there immediately or not at all, and four seconds of waiting for
     * each would be four seconds added to every boot. */
    if (strncmp(src, "/dev/", 5) == 0 && wait_for_it) {
        long waited = 0;
        for (; !lp_exists(src) && waited < 4000; waited += 50)
            lp_sleep_ms(50);

        /* The node existing is not the same as the disk answering.
         *
         * A USB stick registers its partitions as soon as the kernel
         * has read the table, and /dev/sda1 appears then - but the
         * device can still refuse reads for a moment after that while
         * the SCSI layer finishes with it. Waiting only for the node
         * meant the label was read out of a device that had nothing to
         * say yet, which came back as no label at all, and the mount
         * was refused with
         *
         *     /dev/sda1 is not labelled LPZERO - leaving it alone
         *
         * on a stick that is labelled LPZERO. On a board booted from
         * USB that is /boot missing for the whole session: no
         * authorized_keys, no firewall.conf, and no e2fsck or
         * resize2fs - so the data partition is neither checked nor
         * grown, at that boot or any later one.
         *
         * So wait for the answer, not for the node. When a label is
         * wanted, the label matching IS the device answering; when one
         * is not, a readable first sector is. Same four seconds, and a
         * device that is genuinely there costs one 50ms tick at most. */
        for (; waited < 4000; waited += 50) {
            if (want_label) {
                if (label_matches(src, want_label))
                    break;
            } else {
                long fd = lp_open(src, O_RDONLY, 0);
                if (fd >= 0) {
                    char probe[512];
                    long n = lp_read((int)fd, probe, sizeof probe);
                    lp_close((int)fd);
                    if (n == (long)sizeof probe)
                        break;
                }
            }
            lp_sleep_ms(50);
        }
    }

    /* A device that is not there is not an error worth a kernel log
     * entry. /etc/rc tries four names for the disk on purpose, knowing
     * three of them will not exist, and the boot screen should not fill
     * up with the kernel saying so. */
    if (strncmp(src, "/dev/", 5) == 0 && !lp_exists(src)) {
        dprintf(STDERR_FILENO, "mount: %s: no such device\n", src);
        return 1;
    }

    if (want_label && !label_matches(src, want_label)) {
        dprintf(STDERR_FILENO,
                "mount: %s is not labelled %s - leaving it alone\n",
                src, want_label);
        return 1;
    }

    /* Ask the device what it holds before asking the kernel to guess. */
    const char *sniffed = sniff_type(src);
    if (sniffed) {
        long rc = lp_mount(src, dst, sniffed, flags, data);
        if (rc == 0) {
            printf("mount: %s -> %s (%s)\n", src, dst, sniffed);
            return 0;
        }
        /* It said one thing and would not mount as it: fall through and
         * try the rest, rather than trusting our own reading over the
         * filesystem driver's. */
    }

    /* Guess the type. */
    long last = -1;
    for (int i = 0; AUTO_TYPES[i]; i++) {
        long rc = lp_mount(src, dst, AUTO_TYPES[i], flags, data);
        if (rc == 0) {
            printf("mount: %s -> %s (%s)\n", src, dst, AUTO_TYPES[i]);
            return 0;
        }
        last = rc;
    }

    dprintf(STDERR_FILENO, "mount: %s -> %s: no filesystem fits (%ld)\n",
            src, dst, -last);
    return 1;
}
