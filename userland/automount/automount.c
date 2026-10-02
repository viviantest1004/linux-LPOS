/* automount - notice drives being plugged in, and mount them; and keep
 * the machine alive when the card it boots from is pulled out.
 *
 *   automount              mount everything attached right now, once
 *   automount -d           the same, then keep watching (this is a daemon)
 *   automount -l           what is mounted, and what was skipped
 *   automount -u <name>    unmount one, by mount point or device
 *   automount -a           also mount drives that are not removable
 *   automount -r           check the data card now and put /data back
 *
 * Those are two jobs in one program, and they are here together because
 * they are the same job seen from two sides: a block device appearing
 * or going away, and something sensible being done about it. The second
 * one - the SD card this system boots from being pulled out of a
 * running board, and put back - is much the more serious of the two,
 * and it has its own long note further down, by the code that does it.
 *
 * ── Why this exists ──
 * `disk` could already list every block device the kernel found, and
 * `mount` could mount one by hand. So plugging in a USB stick worked,
 * as long as you were logged in, knew it was called sdb1, and knew what
 * filesystem was on it. Nobody plugs a drive into a headless board and
 * then goes to look up its device name.
 *
 * ── How a drive announces itself ──
 * The kernel sends a message on a netlink socket every time a device
 * appears or goes away - the same messages udev listens to. They are
 * plain text: an "add@/devices/..." line followed by NUL-separated
 * KEY=VALUE pairs. We want four of them: ACTION, SUBSYSTEM, DEVNAME and
 * DEVTYPE. No library, no rules files, no daemon protocol.
 *
 * The alternative is polling /sys/block, which means a choice between
 * noticing late and burning CPU on a board that has neither to spare.
 *
 * ── What it will not touch ──
 * The disk this system booted from. Mounting a second copy of /boot or
 * /data under /media would be confusing at best, and at worst gives two
 * paths to the same bytes with different mount options - which is how
 * you get a corrupted filesystem rather than a full one.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"
#include "disk.h"

#define MEDIA_ROOT      "/media"
#define MNT_ROOT        "/mnt"
#define STORAGE_CONF    "/data/storage.conf"
#define MAX_MOUNTS      16
#define UEVENT_BUF      4096

/* Netlink, the kernel's own object-event group. */
#define AF_NETLINK              16
#define SOCK_DGRAM_             2
#define NETLINK_KOBJECT_UEVENT  15
#define SOL_SOCKET_             1
#define SO_RCVBUF_              8

typedef struct {
    u16 nl_family;
    u16 nl_pad;
    u32 nl_pid;
    u32 nl_groups;
} sockaddr_nl_t;

static bool opt_all;            /* -a: 고정 디스크도 대상에 넣는다 */

/* ── which drives count as "plugged in" ──────────────────────────
 *
 * /sys/block/<dev>/removable is the obvious answer and it is wrong
 * often enough to matter. It reports the *media* as removable, which is
 * true of a card reader and false of most USB hard drives and of plenty
 * of USB sticks - QEMU's emulated USB disk reports 0 as well. Trusting
 * it alone means the drives people most want mounted are the ones that
 * get skipped, with no message.
 *
 * So we also look at how the device is attached. /sys/block/<dev> is a
 * symlink into the device tree, and for anything behind a USB host
 * controller that path contains "/usb". That is the question we
 * actually mean: did somebody just plug this in. */
static bool is_external(const blk_t *d)
{
    if (d->removable)
        return true;

    char link[512];
    char path[80];
    snprintf(path, sizeof path, "/sys/block/%s", d->name);

    long n = lp_readlink(path, link, sizeof link - 1);
    if (n <= 0)
        return false;
    link[n] = '\0';

    return strstr(link, "/usb") != NULL;
}

/* ── what we must never touch ────────────────────────────────────── */

/* The whole disk that carries a given mount point, or empty. */
static void disk_behind(const char *mountpoint, char *out, size_t n)
{
    out[0] = '\0';

    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0)
        return;

    char buf[4096];
    long got = lp_read((int)fd, buf, sizeof buf - 1);
    lp_close((int)fd);
    if (got <= 0)
        return;
    buf[got] = '\0';

    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char dev[64] = "", mnt[64] = "";
        int i = 0;
        while (line[i] && line[i] != ' ' && i < 63) { dev[i] = line[i]; i++; }
        dev[i] = '\0';
        if (line[i] == ' ') {
            i++;
            int j = 0;
            while (line[i] && line[i] != ' ' && j < 63) mnt[j++] = line[i++];
            mnt[j] = '\0';
        }

        if (strcmp(mnt, mountpoint) == 0 && dev[0] == '/') {
            if (!disk_whole(dev, out, n))
                strlcpy(out, dev, n);
            return;
        }
        line = nl ? nl + 1 : NULL;
    }
}

/* The disk this system lives on. Everything on it is off limits. */
static bool is_system_disk(const char *disk_path)
{
    char boot_disk[40], data_disk[40];
    disk_behind("/boot", boot_disk, sizeof boot_disk);
    disk_behind("/data", data_disk, sizeof data_disk);

    if (boot_disk[0] && strcmp(disk_path, boot_disk) == 0)
        return true;
    if (data_disk[0] && strcmp(disk_path, data_disk) == 0)
        return true;
    return false;
}

/* ── choosing a name ─────────────────────────────────────────────── */

/* Turn a filesystem label into something safe to use as a directory.
 *
 * Labels come from whoever formatted the drive, which means they can
 * contain spaces, slashes and anything else. A label of "../../etc"
 * must not become a mount at /etc. Anything that is not a letter,
 * digit, dash, dot or underscore becomes a dash, and a leading dot is
 * dropped so nothing lands as a hidden directory. */
static void safe_name(const char *label, const char *fallback,
                      char *out, size_t n)
{
    size_t w = 0;
    if (label && label[0]) {
        for (size_t i = 0; label[i] && w + 1 < n; i++) {
            char c = label[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      (c == '.' && w > 0);
            out[w++] = ok ? c : '-';
        }
        /* Trailing dashes read as a typo. */
        while (w > 0 && out[w - 1] == '-') w--;
    }
    if (w == 0) {
        strlcpy(out, fallback, n);
        return;
    }
    out[w] = '\0';
}

/* /media/<name>, or /media/<name>-2 when that is taken by something
 * else. Two USB sticks both labelled "UNTITLED" is the normal case,
 * not a rare one. */
/* /media/<name>, or /media/<name>-2 when that is taken. Two USB sticks
 * both labelled "UNTITLED" is the normal case, not a rare one. */
static bool dir_exists(const char *path)
{
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return false;
    lp_close((int)fd);
    return true;
}

/* Is anything mounted here right now? An empty leftover directory is
 * fine to reuse; one with a filesystem on it is not. */
static bool is_mountpoint(const char *path)
{
    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0)
        return false;
    char buf[4096];
    long got = lp_read((int)fd, buf, sizeof buf - 1);
    lp_close((int)fd);
    if (got <= 0)
        return false;
    buf[got] = '\0';

    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *sp = strchr(line, ' ');
        if (sp) {
            char *sp2 = strchr(sp + 1, ' ');
            if (sp2) *sp2 = '\0';
            if (strcmp(sp + 1, path) == 0)
                return true;
        }
        line = nl ? nl + 1 : NULL;
    }
    return false;
}

static bool pick_mountpoint(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", MEDIA_ROOT, name);
    if (!dir_exists(out) || !is_mountpoint(out))
        return true;

    for (int suffix = 2; suffix < 20; suffix++) {
        snprintf(out, n, "%s/%s-%d", MEDIA_ROOT, name, suffix);
        if (!dir_exists(out) || !is_mountpoint(out))
            return true;
    }
    return false;
}

/* ── drives the system has been told to keep ─────────────────────
 *
 * `storage adopt` records a drive by filesystem label and gives it a
 * name. Such a drive belongs at /mnt/<name>, not under /media, because
 * things on the machine are configured to look for it there.
 *
 * Both programs read the same file rather than one telling the other,
 * so there is a single source of truth and no protocol between them to
 * fall out of step. */
static bool adopted_name_for(const char *label, char *out, size_t n)
{
    if (!label || !label[0])
        return false;

    long fd = lp_open(STORAGE_CONF, O_RDONLY, 0);
    if (fd < 0)
        return false;

    char buf[2048];
    long got = lp_read((int)fd, buf, sizeof buf - 1);
    lp_close((int)fd);
    if (got <= 0)
        return false;
    buf[got] = '\0';

    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        while (*line == ' ' || *line == '\t') line++;
        if (*line && *line != '#') {
            char *sp = strchr(line, ' ');
            if (sp) {
                *sp = '\0';
                if (strcmp(sp + 1, label) == 0) {
                    strlcpy(out, line, n);
                    return true;
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    return false;
}

/* Labels this system uses for its own partitions. A drive carrying one
 * of these is not somebody's USB stick - it is a second copy of our own
 * card, and mounting it under /media would give two live paths to
 * filesystems that each expect to be the only one. */
static bool is_ours(const char *label)
{
    return label && label[0] &&
           (strcmp(label, "LPZERODATA") == 0 || strcmp(label, "LPZERO") == 0);
}

/* ── mounting ────────────────────────────────────────────────────── */

/* Filesystems worth trying, most likely first. A drive from a PC is
 * almost always one of these three. */
static const char *fs_candidates[] = { "ext4", "vfat", "exfat", "ext3", "ext2" };

static bool mount_one(const blk_t *p, bool quiet)
{
    char here[64];
    if (disk_mountpoint(p->path, here, sizeof here)) {
        if (!quiet)
            printf("  %-12s already mounted at %s\n", p->name, here);
        return false;
    }

    char whole[40];
    if (disk_whole(p->path, whole, sizeof whole) && is_system_disk(whole)) {
        if (!quiet)
            printf("  %-12s skipped - this is the system disk\n", p->name);
        return false;
    }

    if (is_ours(p->label)) {
        if (!quiet)
            printf("  %-12s skipped - one of our own partitions (%s)\n",
                   p->name, p->label);
        return false;
    }

    /* A drive the system has adopted goes to its fixed place. */
    char name[40], point[96], adopted[40];
    if (adopted_name_for(p->label, adopted, sizeof adopted)) {
        snprintf(point, sizeof point, "%s/%s", MNT_ROOT, adopted);
        lp_mkdir(MNT_ROOT, 0755);
    } else {
        safe_name(p->label, p->name, name, sizeof name);
        if (!pick_mountpoint(name, point, sizeof point)) {
            printf("  %-12s no free mount point under %s\n",
                   p->name, MEDIA_ROOT);
            return false;
        }
        lp_mkdir(MEDIA_ROOT, 0755);
    }

    if (lp_mkdir(point, 0755) < 0 && !dir_exists(point)) {
        printf("  %-12s cannot create %s\n", p->name, point);
        return false;
    }

    /* nosuid,nodev for the same reason /data has them: this filesystem
     * was written by another machine, and a setuid root binary or a
     * device node on it would be that machine's decision, not ours. */
    unsigned long flags = MS_NOSUID | MS_NODEV;

    /* Try what the superblock says first; fall back to the usual list,
     * because a filesystem we could not identify is not necessarily one
     * the kernel cannot mount. */
    if (p->fs[0] && lp_mount(p->path, point, p->fs, flags, NULL) == 0) {
        printf("  %-12s -> %s  (%s)\n", p->name, point, p->fs);
        return true;
    }

    for (u32 i = 0; i < sizeof fs_candidates / sizeof *fs_candidates; i++) {
        if (p->fs[0] && strcmp(p->fs, fs_candidates[i]) == 0)
            continue;                       /* already tried */
        if (lp_mount(p->path, point, fs_candidates[i], flags, NULL) == 0) {
            printf("  %-12s -> %s  (%s)\n", p->name, point, fs_candidates[i]);
            return true;
        }
    }

    /* Read-only is better than nothing: a dirty NTFS or an unclean
     * unmount often mounts read-only when it will not mount writable. */
    if (p->fs[0] &&
        lp_mount(p->path, point, p->fs, flags | MS_RDONLY, NULL) == 0) {
        printf("  %-12s -> %s  (%s, read-only)\n", p->name, point, p->fs);
        return true;
    }

    lp_rmdir(point);
    if (!quiet) {
        if (p->fs[0])
            printf("  %-12s %s is not a filesystem this kernel can mount\n",
                   p->name, p->fs);
        else
            printf("  %-12s no filesystem found on it\n", p->name);
    }
    return false;
}

static bool unmount_one(const char *point, bool quiet)
{
    long r = lp_umount(point, 0);
    if (r < 0) {
        /* Busy is the common case - somebody's shell is sitting in it.
         * Detach it from the tree so the drive can be pulled safely and
         * the last user's file handles die with them. */
        r = lp_umount(point, 2 /* MNT_DETACH */);
        if (r < 0) {
            if (!quiet)
                printf("  %s: cannot unmount (%ld)\n", point, -r);
            return false;
        }
        if (!quiet)
            printf("  %s: was busy, detached\n", point);
    }
    lp_rmdir(point);
    return true;
}

/* ── scanning what is here now ───────────────────────────────────── */

static int scan_and_mount(bool quiet)
{
    blk_t disks[DISK_MAX];
    int nd = disk_list(disks, DISK_MAX);
    int mounted = 0;

    for (int i = 0; i < nd; i++) {
        if (is_system_disk(disks[i].path))
            continue;
        if (!is_external(&disks[i]) && !opt_all)
            continue;

        blk_t parts[DISK_PARTS];
        int np = disk_parts(disks[i].path, parts, DISK_PARTS);

        if (np <= 0) {
            /* No partition table. A drive formatted whole is common on
             * USB sticks, so try the disk itself. */
            if (mount_one(&disks[i], quiet))
                mounted++;
            continue;
        }
        for (int j = 0; j < np; j++)
            if (mount_one(&parts[j], quiet))
                mounted++;
    }
    return mounted;
}

/* A device went away. Unmount anything under /media that no longer has
 * a device behind it.
 *
 * The device is already gone by the time we hear about it, so we cannot
 * ask what it was - we can only look at what is mounted and check which
 * of those devices still exist. */
static int drop_vanished(void)
{
    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0)
        return 0;

    char buf[4096];
    long got = lp_read((int)fd, buf, sizeof buf - 1);
    lp_close((int)fd);
    if (got <= 0)
        return 0;
    buf[got] = '\0';

    int dropped = 0;
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char dev[64] = "", mnt[80] = "";
        int i = 0, j = 0;
        while (line[i] && line[i] != ' ' && j < 63) dev[j++] = line[i++];
        dev[j] = '\0';
        if (line[i] == ' ') {
            i++; j = 0;
            while (line[i] && line[i] != ' ' && j < 79) mnt[j++] = line[i++];
            mnt[j] = '\0';
        }

        if (strncmp(mnt, MEDIA_ROOT "/", sizeof MEDIA_ROOT) == 0 &&
            dev[0] == '/' && lp_access(dev, F_OK) != 0) {
            printf("automount: %s went away, releasing %s\n", dev, mnt);
            if (unmount_one(mnt, true))
                dropped++;
        }
        line = nl ? nl + 1 : NULL;
    }
    return dropped;
}

/* ── listening ───────────────────────────────────────────────────── */


/* ── The card going away, and coming back ─────────────────────────────
 *
 * This is the half of this program that has nothing to do with USB
 * sticks. It is about the one card the machine cannot do without.
 *
 * Pulling the card out of a running board does not stop it. The root
 * filesystem is a cpio archive inside the kernel image, unpacked into
 * RAM at boot, so the shell, SSH and the network all carry on. What
 * goes is everything that was mounted off the card - and on this system
 * that is very nearly the whole of the filesystem a person can type a
 * path into:
 *
 *   /data       the log, the SSH host key, the saved clock, /data/bin
 *   /root       a bind mount of /data/root; the home directory
 *   /boot       the FAT partition, where e2fsck and authorized_keys are
 *   /bin /sbin /lib /usr /opt /srv
 *               six overlays whose upper layer is /data/persist - see
 *               persist.c. Their lower layer is the RAM root and is
 *               perfectly healthy; the mount as a whole is not.
 *
 * So the state after the card is pulled is not "a board without /data".
 * It is a board where every command lives on a filesystem whose disk is
 * gone: `ls` runs for as long as it is still in the page cache, and then
 * it does not. That is the "and there is no recovery" in the complaint
 * this was written for. There is nothing left to run, so nothing on the
 * machine can be the thing that puts it back.
 *
 * Which decides the shape of everything below.
 *
 *   It has to be already running. `automount -d` is started from
 *   /etc/services at boot and its own pages come from the RAM root:
 *   /bin/automount resolves through the overlay to the file in the
 *   lower layer, which is memory. Nothing this process needs to keep
 *   executing is on the card.
 *
 *   It has to take the mounts down with the umount2 syscall itself, not
 *   by running /bin/umount. /bin/umount is on the card. Forking it is
 *   the one approach that certainly does not work.
 *
 *   MNT_DETACH, not a plain unmount. Every one of these mounts is busy:
 *   a login shell has its cwd in /root, and every running program has
 *   its executable open under /bin. A plain umount returns EBUSY and
 *   leaves the machine exactly as wedged as it was. MNT_DETACH takes
 *   the mount out of the tree now and lets it die when its last user
 *   does, which is the right answer for a disk that has already left.
 *
 * Once the six overlays are detached, /bin is the pristine RAM root
 * again - every program the image ships is there and works - and from
 * that point ordinary fork+exec is available again, which is how e2fsck
 * and `persist on` get run when the card comes back.
 *
 * ── How we find out ──
 * Two ways, and both are needed.
 *
 *   The uevent. The kernel sends "remove" for the partitions and for
 *   the disk the moment the card goes. On a Pi that is the MMC core
 *   polling the slot - CONFIG_MMC_BCM2835 builds bcm2835.c, which sets
 *   MMC_CAP_NEEDS_POLL, so a card leaving is noticed without anything
 *   having to touch it. In a VM it is the device being unplugged.
 *
 *   A timer. A uevent can be missed: the socket buffer can overflow in
 *   a burst, and this process can be busy running e2fsck when one
 *   arrives. A missed uevent must not be the difference between a board
 *   that recovers by itself and a board that is wedged until somebody
 *   walks over to it. So the same check runs every few seconds as well,
 *   and both paths call the same function.
 *
 * ── What "gone" means, and the check that used to be here ──
 * The old test was: read /proc/mounts, and if /data is listed and not
 * marked ro, all is well. That is wrong, and wrong in precisely the way
 * that mattered - pulling the card does not change /proc/mounts at all.
 * The entry stays, and it stays marked rw, because ext4 only switches
 * to read-only when something tries to write and the write fails.
 * Nothing was writing, so nothing failed, so the entry never changed,
 * so the check reported a healthy machine and the recovery it guarded
 * returned immediately having done nothing - for as long as the board
 * stayed up.
 *
 * The mount table says what the kernel was asked to do, not what the
 * hardware is doing. So the question has to be put to the device: is
 * the block device this mount names still in /sys/class/block, and does
 * it still answer a read. Both of those stop being true the moment the
 * card leaves, whether or not anybody has written anything.
 */
#define DATA_LABEL      "LPZERODATA"
#define BOOT_LABEL      "LPZERO"
#define DATA_POINT      "/data"
#define BOOT_POINT      "/boot"
#define HOME_POINT      "/root"
#define HOME_SOURCE     "/data/root"

/* The same options /etc/rc mounts it with, and they have to stay the
 * same: two mounts of one filesystem with different options is how a
 * filesystem gets corrupted rather than merely full. errors=remount-ro
 * so ext4 stops writing the moment it sees damage; nosuid,nodev because
 * a data partition has no business carrying a setuid binary or a device
 * node, and a card can be written on any other machine. */
#define DATA_OPTS       "errors=remount-ro"
#define DATA_FLAGS      (MS_NOSUID | MS_NODEV)

/* The six directories persist(1) overlays, in the order it lists them.
 * If that list ever changes, this one has to change with it - the two
 * are checked against each other by nothing but this comment. */
static const char *PERSIST_DIRS[] = {
    "/bin", "/sbin", "/lib", "/usr", "/opt", "/srv", NULL
};

/* How often the timer check runs. Three seconds is short enough that
 * nobody watching the console thinks the board has died, and the check
 * itself is one read of /proc/mounts plus one cached read of a sector,
 * so running it this often costs nothing measurable. */
#define TICK_MS         3000

/* How long to wait before trying a card that has already failed once.
 * Without this, a card that is present but unrepairable would have
 * e2fsck run against it every three seconds forever. A card that is
 * simply absent is retried on every tick instead - that costs a scan of
 * /sys/block and nothing else. */
#define RETRY_MS        30000

/* Ceilings on the two programs this runs. e2fsck on a large, badly
 * damaged filesystem legitimately takes minutes; a wedged one takes
 * forever, and a daemon that is the only thing that can recover the
 * machine must not be the thing that hangs. */
#define FSCK_MS         600000
#define PERSIST_MS      60000

typedef enum {
    CARD_UP,            /* mounted, writable, and the device answers */
    CARD_GONE,          /* nothing of ours is attached */
    CARD_STRANGER,      /* a card is here and it is not ours */
    CARD_BROKEN         /* ours, and it will not check or will not mount */
} card_state;

/* What we last told the console. Everything below says things only when
 * this changes, because both callers run repeatedly and a true sentence
 * repeated every three seconds is indistinguishable from a fault. */
static card_state card = CARD_UP;
static s64 retry_after = 0;         /* lp_monotonic_ms, for CARD_BROKEN */

/* Console and log, because the two audiences are different and neither
 * is optional. The console is what somebody with a serial cable sees
 * while it is happening; the log is the only copy that is still there
 * afterwards - and lp_log writes to /dev/kmsg, which is a file
 * descriptor into the kernel's own ring buffer and so keeps working
 * with no filesystem underneath it at all. logd picks it up and puts it
 * in /data/log/messages once there is a /data again. */
static void say(const char *msg)
{
    printf("automount: %s\n", msg);
    lp_log("automount", msg);
}

/* ── asking the hardware, not the mount table ────────────────────── */

/* What /proc/mounts says is behind a mount point, and whether the
 * kernel has it read-only. false when nothing is mounted there. */
static bool mounted_device(const char *point, char *dev, size_t n, bool *ro)
{
    if (ro) *ro = false;
    if (n)  dev[0] = '\0';

    char buf[8192];
    if (proc_read("/proc/mounts", buf, sizeof buf) <= 0)
        return false;

    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* device point type options 0 0 */
        char *sp1 = strchr(line, ' ');
        if (sp1) {
            *sp1 = '\0';
            char *m = sp1 + 1;
            char *sp2 = strchr(m, ' ');
            if (sp2) {
                *sp2 = '\0';
                if (strcmp(m, point) == 0) {
                    strlcpy(dev, line, n);
                    char *sp3 = strchr(sp2 + 1, ' ');   /* past the type */
                    if (sp3 && ro) {
                        const char *opts = sp3 + 1;
                        /* "ro" is always the first option when it is set. */
                        *ro = strncmp(opts, "ro,", 3) == 0 ||
                              strncmp(opts, "ro ", 3) == 0;
                    }
                    return true;
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    return false;
}

/* Is the block device behind a mount still there?
 *
 * This is the question the old check never asked, and it is the whole of
 * the bug. Two answers are needed because either one alone can lie.
 *
 * /sys/class/block/<name> is created and removed by the kernel with the
 * gendisk, so it is the honest record of what exists. lp_exists follows
 * the symlink, so a link left pointing at a device directory that has
 * gone reads as absent, which is what we want.
 *
 * And then a read, because a device node can outlive the device - a
 * /dev built by hand with mknod rather than by devtmpfs keeps its nodes
 * whatever happens to the hardware. Opening a removed block device
 * fails with ENXIO or ENODEV; reading from one that is on its way out
 * fails with EIO. Either is the answer we are looking for. The read is
 * served from the block device's own page cache while the card is
 * healthy, so this costs microseconds on the path that runs every
 * three seconds. */
static bool device_still_there(const char *dev)
{
    if (strncmp(dev, "/dev/", 5) != 0)
        return true;            /* rootfs, tmpfs, overlay: no device */

    char sysfs[128];
    snprintf(sysfs, sizeof sysfs, "/sys/class/block/%s", dev + 5);
    if (!lp_exists(sysfs))
        return false;

    long fd = lp_open(dev, O_RDONLY, 0);
    if (fd < 0)
        return false;
    char probe[512];
    long got = lp_read((int)fd, probe, sizeof probe);
    lp_close((int)fd);
    return got == (long)sizeof probe;
}

/* Run a program, wait for it, and give up on it after `limit_ms`.
 *
 * Returns the exit status, -1 when it could not be run or died to a
 * signal, and -2 when it had to be killed for taking too long. That
 * last one is the case this exists for: everything below is the only
 * thing on the machine that can put the filesystems back, so it must
 * not be possible for a child of it to hang forever. */
#define RUN_TIMED_OUT   (-2)

static int run_wait_ms(const char *path, char *const argv[], long limit_ms)
{
    pid_t pid = (pid_t)lp_fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        lp_execve(path, argv, environ);
        lp_exit(127);
    }

    for (long waited = 0; waited < limit_ms; waited += 100) {
        int status = 0;
        if (lp_waitpid(pid, &status, WNOHANG) == pid)
            return (status & 0x7f) ? -1 : ((status >> 8) & 0xff);
        lp_sleep_ms(100);
    }

    lp_kill(pid, SIGKILL);
    int status = 0;
    lp_waitpid(pid, &status, 0);
    return RUN_TIMED_OUT;
}

/* A partition of ours, by label and filesystem type.
 *
 * An unlabelled filesystem of the right type is accepted, which is the
 * same rule mount(1), fsck(1) and expandfs(1) follow: cards written
 * before the labels existed are still in use, and refusing them would
 * break a machine that has been working for a year. A filesystem
 * carrying somebody *else's* label is refused, and its name is handed
 * back in `stranger` so the caller can say what it saw rather than just
 * "no card" - the difference between "put the card back in" and "that
 * is not this machine's card" is the whole of what the person standing
 * over the board needs to know. */
static bool find_ours(const char *want_label, const char *want_fs,
                      char *out, size_t n, char *stranger, size_t sn)
{
    if (sn) stranger[0] = '\0';

    blk_t disks[DISK_MAX];
    int nd = disk_list(disks, DISK_MAX);

    for (int i = 0; i < nd; i++) {
        blk_t parts[DISK_PARTS];
        int np = disk_parts(disks[i].path, parts, DISK_PARTS);
        for (int j = 0; j < np; j++) {
            if (strcmp(parts[j].fs, want_fs) != 0)
                continue;
            if (!parts[j].label[0] ||
                strcmp(parts[j].label, want_label) == 0) {
                strlcpy(out, parts[j].path, n);
                return true;
            }
            if (sn && !stranger[0])
                snprintf(stranger, sn, "%s is labelled %s",
                         parts[j].path, parts[j].label);
        }
    }
    return false;
}

/* ── taking it all down ──────────────────────────────────────────── */

/* Detach everything that was on the card, in the order that leaves the
 * machine usable at every step.
 *
 * The order is not cosmetic. /root first, because it is a bind mount of
 * a directory *inside* /data and holding it keeps /data in use. Then
 * the six overlays, because their upper and work directories are inside
 * /data too. Then /data itself. Then /boot, but only when its own
 * device has gone as well - when the trouble is nothing worse than ext4
 * having gone read-only, the card is still in the slot and /boot is
 * where e2fsck lives, so throwing it away would be throwing away the
 * tool needed to fix the thing that is wrong.
 *
 * Every one of these is umount2(MNT_DETACH) and every one of them
 * therefore succeeds - the only failure is EINVAL for a path that was
 * not a mount point, which is the normal answer when this runs twice.
 * Nothing here can leave the machine half-detached, and nothing here
 * runs a program.
 *
 * `headline` is the whole first sentence rather than a reason to append
 * to a fixed one, because the three cases are not the same news: a card
 * that has been pulled out, a card that is still in the slot with a
 * filesystem that has stopped taking writes, and a device that has
 * stopped answering without saying why. Somebody reading the console
 * needs to know which. */
static void tear_down(const char *headline)
{
    say(headline);
    say("  /data, /root and the overlays on /bin /sbin /lib /usr /opt"
        " /srv were all on it");

    lp_umount(HOME_POINT, MNT_DETACH);
    for (int i = 0; PERSIST_DIRS[i]; i++)
        lp_umount(PERSIST_DIRS[i], MNT_DETACH);
    lp_umount(DATA_POINT, MNT_DETACH);

    char bootdev[64];
    if (mounted_device(BOOT_POINT, bootdev, sizeof bootdev, NULL) &&
        !device_still_there(bootdev))
        lp_umount(BOOT_POINT, MNT_DETACH);

    say("  detached. /bin is the system image again, so every command"
        " on the machine works");
    say("  nothing written anywhere is being saved until it is back -"
        " put the card in and this mounts it again by itself");
}

/* ── putting it all back ─────────────────────────────────────────── */

/* Returns true when /data ended up mounted and writable. Says why when
 * it did not. Safe to call when some or all of it is already done: each
 * step looks before it acts, so a card that is pulled again halfway
 * through this leaves a partly-restored machine that the next call
 * finishes or tears down again. */
static bool bring_back(void)
{
    char dev[64], stranger[80], msg[256];

    /* /boot first, and for one reason: e2fsck is on it.
     *
     * The tool that repairs the data partition cannot live on the data
     * partition, and putting it in the system image would cost 1.4MB of
     * RAM for the life of the machine - so it is on the FAT partition,
     * and the FAT partition has to be back before the check below can
     * run. Read-only and label-checked, exactly as /etc/rc mounts it:
     * FAT has no journal, we only ever read from here, and /boot is
     * where authorized_keys and firewall.conf are read from, so a
     * stranger's FAT partition becoming /boot is not a filesystem
     * mix-up but handing somebody the machine. Root-only, as there too:
     * the private SSH key authkey leaves here logs in as root. */
    if (!is_mountpoint(BOOT_POINT)) {
        if (find_ours(BOOT_LABEL, "vfat", dev, sizeof dev,
                      stranger, sizeof stranger)) {
            lp_mkdir(BOOT_POINT, 0755);
            if (lp_mount(dev, BOOT_POINT, "vfat", MS_RDONLY,
                         "fmask=0077,dmask=0077") == 0) {
                snprintf(msg, sizeof msg, "  %s is back at /boot", dev);
                say(msg);
            } else {
                say("  the boot partition will not mount - the filesystem"
                    " check cannot run without it");
            }
        } else if (stranger[0]) {
            snprintf(msg, sizeof msg,
                     "  not mounting /boot: %s, not %s", stranger, BOOT_LABEL);
            say(msg);
        }
    }

    /* Now the data partition. */
    if (!find_ours(DATA_LABEL, "ext4", dev, sizeof dev,
                   stranger, sizeof stranger)) {
        if (stranger[0]) {
            /* A card is in the slot and it is not ours. Mounting it over
             * /data would put this machine's log, SSH host key and home
             * directory onto somebody else's filesystem, and would put
             * their files where this machine's programs expect to find
             * its own. Refuse, loudly, and keep waiting. */
            if (card != CARD_STRANGER) {
                snprintf(msg, sizeof msg,
                         "there is a card here but it is not this"
                         " machine's: %s, not %s", stranger, DATA_LABEL);
                say(msg);
                say("  leaving it alone. /data stays in RAM until the"
                    " right card is back in the slot.");
                card = CARD_STRANGER;
                retry_after = lp_monotonic_ms() + RETRY_MS;
            }
        } else if (card != CARD_GONE) {
            say("no partition labelled " DATA_LABEL " is attached");
            say("  anything written to /data now is in RAM and will be"
                " gone at the next boot");
            card = CARD_GONE;
        }
        return false;
    }

    /* Check it before mounting it. The card was almost certainly pulled
     * out of a filesystem that was mid-write, which is exactly what
     * fsck is for; mounting first would replay a journal over damage
     * that this could have repaired. /bin/fsck picks the device by
     * label, verifies /boot/e2fsck against the hash recorded inside the
     * kernel image and runs it in preen mode - so this is safe to call
     * even when /boot could not be mounted above; it says so and does
     * nothing. */
    snprintf(msg, sizeof msg, "%s is back - checking it before mounting", dev);
    say(msg);

    char *fargv[] = { (char *)"fsck", dev, NULL };
    int frc = run_wait_ms("/bin/fsck", fargv, FSCK_MS);
    if (frc == RUN_TIMED_OUT)
        say("  the filesystem check did not finish and was stopped -"
            " trying to mount it anyway");

    /* Pulled again while we were checking it. Say so rather than
     * failing at a mount with an errno nobody can read. */
    if (!device_still_there(dev)) {
        say("  the card went away again during the check");
        card = CARD_GONE;
        return false;
    }

    lp_mkdir(DATA_POINT, 0755);
    long rc = lp_mount(dev, DATA_POINT, "ext4", DATA_FLAGS, DATA_OPTS);
    if (rc < 0) {
        if (card != CARD_BROKEN) {
            snprintf(msg, sizeof msg,
                     "%s is here but will not mount (%ld)%s", dev, -rc,
                     frc > 0 ? " - and the check could not repair it" : "");
            say(msg);
            if (-rc == 16 /* EBUSY */)
                /* Only reachable when the card never actually left -
                 * ext4 went read-only under it and we detached it. The
                 * old superblock is still alive because processes have
                 * files open on it, and the kernel will not mount the
                 * same block device twice with different options. Time
                 * does not fix this; the processes have to go. */
                say("  the filesystem this machine had is still open in"
                    " processes that were started before it went"
                    " read-only, so the kernel will not mount it again."
                    " A reboot clears that.");
            else
                say("  the data on it may be gone. `fsck -f " DATA_LABEL "`"
                    " from a shell will say more; a reboot retries from"
                    " scratch.");
            say("  the machine itself is fine and stays reachable -"
                " /data is in RAM until this is sorted out.");
            card = CARD_BROKEN;
        }
        retry_after = lp_monotonic_ms() + RETRY_MS;
        return false;
    }

    snprintf(msg, sizeof msg, "/data is back on %s", dev);
    say(msg);

    /* /root is a bind mount from the card and followed it out. Same
     * line /etc/rc uses, for the same reason: home is on the card so
     * that what is saved in it is still there after a reboot. */
    lp_mkdir(HOME_SOURCE, 0755);
    if (lp_mount(HOME_SOURCE, HOME_POINT, NULL, MS_BIND, NULL) < 0)
        say("  /root could not be bound back to /data/root - home is in"
            " RAM until the next reboot");

    /* And the six overlays. `persist on` is what /etc/rc runs at boot
     * and it is what runs here, rather than a second copy of the same
     * mount options in this file that could drift from it. It skips
     * anything already overlaid, so this is safe when only some of them
     * came down. */
    char *pargv[] = { (char *)"persist", (char *)"on", NULL };
    int prc = run_wait_ms("/bin/persist", pargv, PERSIST_MS);
    if (prc == 0) {
        say("  /bin /sbin /lib /usr /opt and /srv are kept on the card"
            " again");
    } else {
        say("  the overlays could not be put back, so anything installed"
            " into the system from now on is in RAM only");
        say("  everything else works. A reboot puts them back - persist"
            " does this cleanly at boot.");
    }

    say("the card is back and the machine is whole again");
    card = CARD_UP;
    retry_after = 0;
    return true;
}

/* Does a uevent's DEVNAME name the device /data is mounted on, or the
 * whole disk that device is a partition of?
 *
 * Both, because the kernel takes an SD card away one object at a time
 * and we may hear about any of them first: "remove" arrives for
 * mmcblk0p1, for mmcblk0p2 and for mmcblk0, in whatever order the
 * partition teardown happens to produce. Any one of the three is the
 * card leaving. */
static bool names_the_data_card(const char *dev, const char *devname)
{
    if (!devname || !devname[0] || strncmp(dev, "/dev/", 5) != 0)
        return false;
    if (strcmp(dev + 5, devname) == 0)
        return true;

    char whole[40];
    return disk_whole(dev, whole, sizeof whole) &&
           strncmp(whole, "/dev/", 5) == 0 &&
           strcmp(whole + 5, devname) == 0;
}

/* ── the one check both callers make ─────────────────────────────────
 *
 * Cheap on the path that runs every three seconds: one read of
 * /proc/mounts, one stat in /sys/class/block and one cached sector.
 * Everything expensive is behind a state that is not CARD_UP.
 *
 * `urgent` means a uevent brought us here rather than the timer, so a
 * card that has already failed to mount gets one more try immediately -
 * somebody standing at the board and pushing the card in again should
 * not have to wait out the back-off.
 *
 * `gone` is the DEVNAME from a "remove" uevent, or NULL. It exists
 * because of an ordering detail that would otherwise cost three seconds
 * every time: the kernel broadcasts KOBJ_REMOVE from device_del() and
 * only then takes the sysfs directory down, so at the instant this runs
 * from a remove event /sys/class/block/<name> can still be there and
 * the sector we read can still be in the block device's page cache.
 * Both tests would say the card is fine, and only the next timer tick
 * would find out otherwise. The kernel has just told us in so many
 * words that the device is being removed, so believe it. */
static void check_card(bool urgent, const char *gone)
{
    /* On a disk-rooted machine - the amd64 desktop image - / is a real
     * filesystem, /data is an ordinary directory on it and there are no
     * overlays and no bind mount to put back. There is nothing here to
     * recover, and doing it anyway would mount a data partition over a
     * directory that already persists. */
    if (root_is_persistent())
        return;

    char dev[64];
    bool ro = false;

    if (mounted_device(DATA_POINT, dev, sizeof dev, &ro)) {
        bool leaving = names_the_data_card(dev, gone);

        if (!ro && !leaving && device_still_there(dev)) {
            if (card != CARD_UP) {
                card = CARD_UP;         /* somebody else fixed it */
                retry_after = 0;
            }
            return;                     /* the normal path ends here */
        }

        /* Something is wrong with a /data that is still in the mount
         * table. Take it down before anything else: while the overlays
         * are up, every command on the machine is being served by this
         * filesystem, so nothing - including e2fsck - can be run. */
        tear_down(leaving
                  ? "the data card has been pulled out - the kernel says"
                    " the device /data was on has been removed"
                  : ro
                  ? "/data has gone read-only - ext4 found damage on it"
                    " and stopped writing"
                  : "the data card is gone - the block device behind"
                    " /data does not answer any more");

        /* CARD_GONE now, and that is what keeps bring_back() from
         * following the four lines above with "no partition labelled
         * LPZERODATA is attached". It is true, and it has just been
         * said better. On a board that came up with no card at all
         * nothing was torn down, card is still CARD_UP, and that line
         * is the first thing said - which is right, because there it is
         * the whole of the news. */
        card = CARD_GONE;
        retry_after = 0;
    }

    /* /data is not mounted. Either it never was, or we just detached
     * it. Either way, look for the card. */
    if (!urgent && retry_after && lp_monotonic_ms() < retry_after)
        return;                         /* a card we already failed on */

    bring_back();
}

/* One uevent. Returns true when it is a block device appearing or
 * disappearing - which is the only kind we act on. */
static bool parse_uevent(const char *msg, size_t len,
                         char *action, size_t an,
                         char *devname, size_t dn)
{
    action[0] = '\0';
    devname[0] = '\0';
    bool is_block = false;

    /* Fields are NUL-separated, after a first line we do not need. */
    for (size_t i = 0; i < len; ) {
        const char *field = msg + i;
        size_t flen = strlen(field);

        if (strncmp(field, "ACTION=", 7) == 0)
            strlcpy(action, field + 7, an);
        else if (strncmp(field, "DEVNAME=", 8) == 0)
            strlcpy(devname, field + 8, dn);
        else if (strncmp(field, "SUBSYSTEM=", 10) == 0)
            is_block = strcmp(field + 10, "block") == 0;

        i += flen + 1;
        if (flen == 0)
            break;
    }
    return is_block && action[0] && devname[0];
}

static int watch(void)
{
    long fd = lp_socket(AF_NETLINK, SOCK_DGRAM_, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "automount: cannot open the kernel's event socket (%ld)\n", -fd);
        return 1;
    }

    /* A burst of events arrives when a drive with several partitions is
     * plugged in, and the default receive buffer is small enough to drop
     * some of them. A dropped event means a partition that never gets
     * mounted and no sign of why. */
    int rcvbuf = 1 << 18;
    lp_setsockopt((int)fd, SOL_SOCKET_, SO_RCVBUF_, &rcvbuf, sizeof rcvbuf);

    sockaddr_nl_t sa;
    memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_pid    = 0;               /* let the kernel assign */
    sa.nl_groups = 1;               /* the uevent broadcast group */

    if (lp_bind((int)fd, &sa, sizeof sa) < 0) {
        dprintf(STDERR_FILENO,
                "automount: cannot listen for device events\n");
        lp_close((int)fd);
        return 1;
    }

    printf("automount: watching for drives\n");

    /* Once at the start, for the card that was already back before this
     * began watching - a restart of this service, or a board that came
     * up with no card and had one pushed in before the daemon ran. */
    check_card(true, NULL);

    char buf[UEVENT_BUF];
    for (;;) {
        /* poll with a timeout rather than a blocking read, and that
         * timeout is the whole reason this is not still a plain
         * recvfrom.
         *
         * A uevent can go missing. The socket buffer is 256KB and can
         * still overflow in a burst; more to the point, this process
         * spends whole minutes inside run_wait_ms() while e2fsck runs,
         * and a card pulled during that is a card whose "remove" event
         * may be dropped before we come back to read it. A missed event
         * used to mean nothing at all happened - the board stayed wedged
         * on a filesystem that was not there until somebody power-cycled
         * it. Now the worst a missed event costs is TICK_MS. */
        lp_pollfd_t pfd;
        pfd.fd      = (int)fd;
        pfd.events  = LP_POLLIN;
        pfd.revents = 0;

        long pr = lp_poll(&pfd, 1, TICK_MS);
        if (pr <= 0) {
            /* Timed out, or a signal arrived. Either way the question
             * to ask is the same one. */
            check_card(false, NULL);
            continue;
        }

        long got = lp_recvfrom((int)fd, buf, sizeof buf - 1, 0, NULL, NULL);
        if (got <= 0)
            continue;
        buf[got] = '\0';

        char action[16], devname[48];
        if (!parse_uevent(buf, (size_t)got, action, sizeof action,
                          devname, sizeof devname))
            continue;

        if (strcmp(action, "add") == 0) {
            /* The device node may not exist for a moment after the
             * event - devtmpfs creates it around the same time. Give it
             * a beat rather than reporting a drive that is really there
             * as missing. */
            char path[64];
            snprintf(path, sizeof path, "/dev/%s", devname);
            for (int t = 0; t < 20 && lp_access(path, F_OK) != 0; t++)
                lp_sleep_ms(50);

            printf("automount: %s appeared\n", devname);
            /* Before /media: if this is the system's own data
             * partition coming back, it belongs at /data, and
             * scan_and_mount would rightly refuse to touch it. */
            check_card(true, NULL);
            scan_and_mount(true);
        } else if (strcmp(action, "remove") == 0) {
            drop_vanished();
            /* The device name goes with it, because at this instant
             * sysfs may not have caught up yet - see check_card. */
            check_card(true, devname);
        }
    }
}

/* ── listing ─────────────────────────────────────────────────────── */

static void list_mounts(void)
{
    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0) {
        printf("cannot read /proc/mounts\n");
        return;
    }
    char buf[4096];
    long got = lp_read((int)fd, buf, sizeof buf - 1);
    lp_close((int)fd);
    if (got <= 0)
        return;
    buf[got] = '\0';

    int n = 0;
    printf("mounted under %s:\n", MEDIA_ROOT);
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, "/dev/", 5) == 0 && strstr(line, MEDIA_ROOT "/")) {
            printf("  %s\n", line);
            n++;
        }
        line = nl ? nl + 1 : NULL;
    }
    if (n == 0)
        printf("  (nothing)\n");
}

/* ── entry ───────────────────────────────────────────────────────── */

static void usage(void)
{
    printf("usage: automount [-d] [-a] [-l] [-r] [-u <name>]\n");
    printf("  (no options)  mount every drive attached right now\n");
    printf("  -d            keep watching for drives being plugged in\n");
    printf("  -a            include internal drives too, not just plugged-in ones\n");
    printf("  -l            list what is mounted under %s\n", MEDIA_ROOT);
    printf("  -u <name>     unmount one, by mount point or device\n");
    printf("  -r            check the data card now, and put /data, /root\n");
    printf("                and the overlays back if it has come back\n");
}

int main(int argc, char **argv)
{
    bool daemon = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            daemon = true;
        } else if (strcmp(argv[i], "-a") == 0) {
            opt_all = true;
        } else if (strcmp(argv[i], "-l") == 0) {
            list_mounts();
            return 0;
        } else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            const char *what = argv[++i];
            char point[96];
            if (what[0] == '/')
                strlcpy(point, what, sizeof point);
            else
                snprintf(point, sizeof point, "%s/%s", MEDIA_ROOT, what);
            return unmount_one(point, false) ? 0 : 1;
        } else if (strcmp(argv[i], "-r") == 0) {
            /* The same check the daemon makes, by hand.
             *
             * The daemon does this by itself and normally there is no
             * reason to type it. It is here for the two cases where
             * waiting is the wrong answer: somebody who has just pushed
             * the card back in and does not want to wait out the
             * back-off after a card that would not mount, and a board
             * where `automount -d` itself has died - init restarts it,
             * but init has to exec /bin/automount to do so, and while
             * the overlays are still up that is a file on the card that
             * is not there. Running this from a shell whose own binary
             * is still in the page cache is then the way back. */
            check_card(true, NULL);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else {
            dprintf(STDERR_FILENO, "automount: unknown option %s\n", argv[i]);
            usage();
            return 2;
        }
    }

    lp_mkdir(MEDIA_ROOT, 0755);

    printf("automount: looking at what is attached\n");
    int n = scan_and_mount(false);
    if (n == 0)
        printf("  nothing new to mount\n");

    if (daemon)
        return watch();
    return 0;
}
