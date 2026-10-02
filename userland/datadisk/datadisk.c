/* datadisk - choose which partition holds /data.
 *
 *   datadisk                      what is there, and what could be /data
 *   datadisk <partition>          use this one from the next boot
 *   datadisk <partition> --now    use it, and mount it straight away
 *   datadisk <disk> --format      wipe the disk and make one
 *   datadisk --none               go back to looking for it automatically
 *
 * ── Why this exists ──
 * /etc/rc used to look for the data partition under four fixed names -
 * mmcblk0p2, vda2, nvme0n1p2, sda2 - and take the first one labelled
 * LPZERODATA. That covers the card this system wrote and nothing else.
 * Attach your own disk and there was no way to say "put /data there":
 * it is not one of the four names, it does not carry the label, and
 * nothing on the machine could give it one.
 *
 * ── The label is the real name ──
 * Whichever partition is chosen gets labelled LPZERODATA, because that
 * is what everything else already looks for. The device name is
 * recorded too, in /boot/data.conf, but only as a hint about which disk
 * to try first - the label is what decides, so moving the card from the
 * SD slot to a USB reader does not break anything.
 *
 * ── What --format destroys ──
 * Everything on that disk. It writes a fresh partition table with one
 * partition filling the disk and puts an empty ext4 on it. It asks
 * first, it names the disk and its size, and it refuses outright if
 * anything on that disk is mounted - which is what stops you doing it
 * to the disk you booted from.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "disk.h"

#define OUR_LABEL   "LPZERODATA"
#define CONF_BOOT   "/boot/data.conf"
#define CONF_DATA   "/data/data.conf"
#define MOUNT_POINT "/data"

/* ── where /data is now ──────────────────────────────────────────── */

static void show_current(void)
{
    char dev[64] = "";
    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd >= 0) {
        char line[512];
        while (readline((int)fd, line, sizeof line) >= 0) {
            char *sp = strchr(line, ' ');
            if (!sp) continue;
            char *point = sp + 1;
            char *end = strchr(point, ' ');
            if (end) *end = '\0';
            if (strcmp(point, MOUNT_POINT) == 0) {
                *sp = '\0';
                strlcpy(dev, line, sizeof dev);
                break;
            }
        }
        lp_close((int)fd);
    }

    if (dev[0] && strncmp(dev, "/dev/", 5) == 0) {
        u64 free_b = 0, total_b = 0;
        lp_fs_space(MOUNT_POINT, &free_b, &total_b);
        char f[12], t[12];
        disk_human(free_b, f, sizeof f);
        disk_human(total_b, t, sizeof t);
        printf("/data is on %s  -  %s free of %s\n", dev, f, t);
    } else {
        printf("/data is in RAM. Nothing written to it survives a reboot.\n");
    }

    char conf[128] = "";
    long cf = lp_open(CONF_BOOT, O_RDONLY, 0);
    if (cf < 0) cf = lp_open(CONF_DATA, O_RDONLY, 0);
    if (cf >= 0) {
        readline((int)cf, conf, sizeof conf);
        lp_close((int)cf);
        if (conf[0])
            printf("Next boot will try %s first (from data.conf).\n", conf);
    }
}

/* ── what could become /data ─────────────────────────────────────── */

static int list_candidates(void)
{
    blk_t disks[DISK_MAX];
    int nd = disk_list(disks, DISK_MAX);

    if (nd == 0) {
        printf("\nNo disks at all. The kernel found no block device.\n");
        return 1;
    }

    printf("\n  %-16s %8s  %-8s %-12s %s\n",
           "partition", "size", "fs", "label", "");
    int shown = 0;

    for (int i = 0; i < nd; i++) {
        blk_t parts[DISK_PARTS];
        int np = disk_parts(disks[i].path, parts, DISK_PARTS);

        if (np == 0) {
            char sz[12];
            disk_human(disks[i].bytes, sz, sizeof sz);
            printf("  %-16s %8s  %-8s %-12s %s\n",
                   disks[i].path, sz, "-", "-",
                   disks[i].mount[0] ? "in use" : "blank disk - use --format");
            shown++;
            continue;
        }
        for (int j = 0; j < np; j++) {
            char sz[12];
            disk_human(parts[j].bytes, sz, sizeof sz);
            const char *note = "";
            if (parts[j].mount[0])
                note = "mounted";
            else if (strcmp(parts[j].label, OUR_LABEL) == 0)
                note = "already ours";
            else if (parts[j].fs[0])
                note = "has a filesystem on it";
            printf("  %-16s %8s  %-8s %-12s %s\n",
                   parts[j].path, sz,
                   parts[j].fs[0] ? parts[j].fs : "-",
                   parts[j].label[0] ? parts[j].label : "-",
                   note);
            shown++;
        }
    }

    printf("\n  datadisk <partition>          use it from the next boot\n");
    printf("  datadisk <partition> --now    and mount it now\n");
    printf("  datadisk <disk> --format      wipe a disk and make one\n");
    return shown ? 0 : 1;
}

/* ── writing the choice down ─────────────────────────────────────── */

static bool write_conf(const char *dev)
{
    /* /boot first: it is FAT, so the choice can be undone from any PC
     * with a card reader, which is the repair path that still works
     * when the machine will not boot. /boot is mounted read-only
     * though, so this remounts it for the one write. */
    bool boot_rw = lp_mount(NULL, "/boot", NULL, MS_REMOUNT, NULL) == 0;
    const char *path = boot_rw ? CONF_BOOT : CONF_DATA;

    long fd = lp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 && boot_rw) {
        path = CONF_DATA;
        fd = lp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (fd < 0) {
        dprintf(STDERR_FILENO, "datadisk: cannot write %s (%ld)\n", path, -fd);
        if (boot_rw)
            lp_mount(NULL, "/boot", NULL, MS_REMOUNT | MS_RDONLY, NULL);
        return false;
    }

    dprintf((int)fd, "%s\n", dev);
    lp_close((int)fd);
    lp_sync();

    if (boot_rw)
        lp_mount(NULL, "/boot", NULL, MS_REMOUNT | MS_RDONLY, NULL);

    printf("datadisk: recorded in %s\n", path);
    return true;
}

static int forget(void)
{
    bool boot_rw = lp_mount(NULL, "/boot", NULL, MS_REMOUNT, NULL) == 0;
    lp_unlink(CONF_BOOT);
    if (boot_rw)
        lp_mount(NULL, "/boot", NULL, MS_REMOUNT | MS_RDONLY, NULL);
    lp_unlink(CONF_DATA);
    printf("datadisk: forgotten. The boot will look for the %s label again,\n"
           "          on the SD card, a virtual disk, NVMe and USB in turn.\n",
           OUR_LABEL);
    return 0;
}

/* ── labelling ───────────────────────────────────────────────────── */

/* Write the ext4 label straight into the superblock.
 *
 * e2label would do this, but that is a third program to ship on the
 * boot partition for sixteen bytes at a known offset. The filesystem
 * must not be mounted: ext4 keeps the superblock in memory and writes
 * it back on unmount, which would undo this. */
static bool set_label(const char *dev)
{
    long fd = lp_open(dev, O_RDWR, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "datadisk: cannot open %s (%ld)\n", dev, -fd);
        return false;
    }

    u8 sb[1024];
    if (lp_lseek((int)fd, 1024, 0) < 0 ||
        lp_read((int)fd, sb, sizeof sb) != (long)sizeof sb) {
        dprintf(STDERR_FILENO, "datadisk: cannot read the superblock\n");
        lp_close((int)fd);
        return false;
    }

    u16 magic = (u16)(sb[56] | (sb[57] << 8));
    if (magic != 0xEF53) {
        dprintf(STDERR_FILENO,
                "datadisk: %s does not hold an ext4 filesystem.\n"
                "          Make one with:  datadisk %s --format\n", dev, dev);
        lp_close((int)fd);
        return false;
    }

    memset(sb + 120, 0, 16);
    memcpy(sb + 120, OUR_LABEL, strlen(OUR_LABEL));

    if (lp_lseek((int)fd, 1024, 0) < 0 ||
        lp_write((int)fd, sb, sizeof sb) != (long)sizeof sb) {
        dprintf(STDERR_FILENO, "datadisk: could not write the label\n");
        lp_close((int)fd);
        return false;
    }
    lp_close((int)fd);
    lp_sync();
    return true;
}

static bool confirm(const char *question, bool assume_yes)
{
    if (assume_yes)
        return true;
    printf("%s [y/N] ", question);

    long fd = lp_open("/dev/tty", O_RDONLY, 0);
    if (fd < 0)
        fd = STDIN_FILENO;
    char c = 0;
    long n = lp_read((int)fd, &c, 1);
    if (fd != STDIN_FILENO)
        lp_close((int)fd);
    printf("\n");
    return n == 1 && (c == 'y' || c == 'Y');
}

/* ── format ──────────────────────────────────────────────────────── */

static int run(const char *path, char *const argv[])
{
    pid_t pid = lp_fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char *envp[] = { (char *)"PATH=/bin:/data/bin", NULL };
        lp_execve(path, argv, envp);
        lp_exit(127);
    }
    int status = 0;
    lp_waitpid(pid, &status, 0);
    return LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : -1;
}

static int do_format(const char *disk, bool assume_yes)
{
    char mke2fs[64];
    if (!boot_tool("mke2fs", mke2fs, sizeof mke2fs))
        return 1;

    u64 total = disk_bytes(disk);
    if (total == 0) {
        dprintf(STDERR_FILENO, "datadisk: cannot get the size of %s\n", disk);
        return 1;
    }

    /* Anything mounted on this disk stops us, which is also what keeps
     * this from being pointed at the disk we booted from. */
    char busy[96];
    if (disk_mountpoint(disk, busy, sizeof busy)) {
        dprintf(STDERR_FILENO,
                "datadisk: %s is mounted on %s. Not formatting it.\n",
                disk, busy);
        return 1;
    }
    blk_t parts[DISK_PARTS];
    int np = disk_parts(disk, parts, DISK_PARTS);
    for (int i = 0; i < np; i++) {
        if (parts[i].mount[0]) {
            dprintf(STDERR_FILENO,
                    "datadisk: %s is mounted on %s. Not formatting %s.\n"
                    "          Unmount it first, or pick a different disk.\n",
                    parts[i].path, parts[i].mount, disk);
            return 1;
        }
    }

    char sz[12];
    disk_human(total, sz, sizeof sz);
    printf("\nThis will erase everything on %s (%s).\n", disk, sz);
    if (np > 0) {
        printf("It currently holds:\n");
        for (int i = 0; i < np; i++) {
            char psz[12];
            disk_human(parts[i].bytes, psz, sizeof psz);
            printf("  %-16s %8s  %-8s %s\n", parts[i].path, psz,
                   parts[i].fs[0] ? parts[i].fs : "-",
                   parts[i].label[0] ? parts[i].label : "");
        }
    }
    if (!confirm("Erase it?", assume_yes)) {
        printf("Nothing was changed.\n");
        return 1;
    }

    /* One partition, starting at 1MB so that it lands on a flash erase
     * boundary, filling the rest of the disk. */
    u8 mbr[DISK_SECTOR];
    if (!disk_read_mbr(disk, mbr))
        memset(mbr, 0, sizeof mbr);
    mbr_init(mbr);

    u64 start = 2048;
    u64 sectors = total / DISK_SECTOR;
    if (sectors > 0xFFFFFFFFULL)
        sectors = 0xFFFFFFFFULL;       /* MBR counts sectors in 32 bits */
    if (sectors <= start + 2048) {
        dprintf(STDERR_FILENO, "datadisk: %s is too small to be useful\n", disk);
        return 1;
    }
    mbr_set(mbr, 1, PART_TYPE_LINUX, (u32)start, (u32)(sectors - start), false);

    if (!disk_write_mbr(disk, mbr)) {
        dprintf(STDERR_FILENO, "datadisk: could not write to %s\n", disk);
        return 1;
    }

    long rc = disk_tell_kernel(disk, 1, start * DISK_SECTOR,
                               (sectors - start) * DISK_SECTOR);
    if (rc < 0)
        rc = disk_reread(disk);

    /* Work out what the kernel will call the new partition. */
    const char *base = disk + (strncmp(disk, "/dev/", 5) == 0 ? 5 : 0);
    size_t bl = strlen(base);
    bool needs_p = bl > 0 && base[bl - 1] >= '0' && base[bl - 1] <= '9';
    char part[64];
    snprintf(part, sizeof part, "/dev/%s%s1", base, needs_p ? "p" : "");

    /* The device node appears when udev - or in our case devtmpfs -
     * catches up with the kernel, which is not instant. */
    for (int waited = 0; !lp_exists(part) && waited < 4000; waited += 50)
        lp_sleep_ms(50);

    if (!lp_exists(part)) {
        dprintf(STDERR_FILENO,
                "datadisk: the table was written, but %s has not appeared.\n"
                "          Reboot and run:  datadisk %s\n", part, part);
        return 1;
    }

    printf("datadisk: making an ext4 filesystem on %s\n", part);
    char *argv[] = {
        mke2fs, (char *)"-t", (char *)"ext4",
        (char *)"-L", (char *)OUR_LABEL,
        (char *)"-m", (char *)"0",     /* no reserved blocks: this is not / */
        (char *)"-q",
        part, NULL
    };
    int frc = run(mke2fs, argv);
    if (frc != 0) {
        dprintf(STDERR_FILENO, "datadisk: mke2fs failed (%d)\n", frc);
        return 1;
    }

    printf("datadisk: %s is ready.\n", part);
    write_conf(part);
    printf("\nReboot, and /data will be on it.\n");
    printf("Or mount it now:  datadisk %s --now\n", part);
    return 0;
}

/* ── choosing an existing partition ──────────────────────────────── */

static int choose(const char *dev, bool now, bool assume_yes)
{
    if (!lp_exists(dev)) {
        dprintf(STDERR_FILENO,
                "datadisk: %s is not there. 'disk' lists what is.\n", dev);
        return 1;
    }

    char where[64];
    if (disk_mountpoint(dev, where, sizeof where) &&
        strcmp(where, MOUNT_POINT) != 0) {
        dprintf(STDERR_FILENO,
                "datadisk: %s is mounted on %s. Unmount it first.\n",
                dev, where);
        return 1;
    }

    char fs[8], label[24];
    disk_identify(dev, fs, sizeof fs, label, sizeof label);

    if (strcmp(fs, "ext4") != 0) {
        dprintf(STDERR_FILENO,
                "datadisk: %s holds %s, and /data has to be ext4.\n",
                dev, fs[0] ? fs : "nothing recognisable");
        dprintf(STDERR_FILENO,
                "          To erase the whole disk and make one:\n"
                "            datadisk <disk> --format\n");
        return 1;
    }

    if (strcmp(label, OUR_LABEL) != 0) {
        if (label[0]) {
            char q[160];
            snprintf(q, sizeof q,
                     "%s is labelled \"%s\". Relabel it %s?",
                     dev, label, OUR_LABEL);
            printf("\nThat label is how this system tells its own disk from\n"
                   "somebody else's, so it has to be changed. The files on\n"
                   "the partition are not touched.\n");
            if (!confirm(q, assume_yes)) {
                printf("Nothing was changed.\n");
                return 1;
            }
        }
        if (!set_label(dev))
            return 1;
        printf("datadisk: %s is now labelled %s\n", dev, OUR_LABEL);
    }

    if (!write_conf(dev))
        return 1;

    if (!now) {
        printf("\nReboot, and /data will be on %s.\n", dev);
        printf("Or do it now without rebooting:  datadisk %s --now\n", dev);
        return 0;
    }

    /* Mounting over a /data that is already in use would hide whatever
     * is running from it rather than moving it. */
    char cur[64];
    if (disk_mountpoint("/dev/root", cur, sizeof cur))
        cur[0] = '\0';
    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    bool busy = false;
    if (fd >= 0) {
        char line[512];
        while (readline((int)fd, line, sizeof line) >= 0) {
            char *sp = strchr(line, ' ');
            if (!sp) continue;
            char *point = sp + 1;
            char *end = strchr(point, ' ');
            if (end) *end = '\0';
            if (strcmp(point, MOUNT_POINT) == 0) { busy = true; break; }
        }
        lp_close((int)fd);
    }
    if (busy) {
        printf("\n%s is already mounted. Mounting another filesystem over it\n"
               "would hide it rather than move it, and everything already\n"
               "reading from it - the log, Python, your home directory -\n"
               "would keep writing to the hidden one. Reboot instead.\n",
               MOUNT_POINT);
        return 0;
    }

    long rc = lp_mount(dev, MOUNT_POINT, "ext4", MS_NOSUID | MS_NODEV,
                       "errors=remount-ro");
    if (rc < 0) {
        dprintf(STDERR_FILENO, "datadisk: cannot mount %s (%ld)\n", dev, -rc);
        return 1;
    }
    printf("datadisk: %s is mounted on %s\n", dev, MOUNT_POINT);
    return 0;
}

/* ── what /etc/rc calls ──────────────────────────────────────────── */

/* Mount whatever data.conf names, having checked it first.
 *
 * Exits non-zero when there is nothing recorded or it cannot be used,
 * and /etc/rc then falls through to trying the four usual device names.
 * So a machine with no data.conf behaves exactly as it did before this
 * command existed.
 *
 * Only /boot is read here, not /data - /data is what this is trying to
 * mount. That is also the useful property: the choice lives on a FAT
 * partition any PC can edit, so a wrong one can be undone with a card
 * reader rather than a serial cable. */
static int boot_mount(void)
{
    char dev[128] = "";
    long fd = lp_open(CONF_BOOT, O_RDONLY, 0);
    if (fd < 0)
        return 1;                      /* nothing recorded: not an error */
    readline((int)fd, dev, sizeof dev);
    lp_close((int)fd);

    /* Trim, and ignore a comment or a blank file. */
    for (int i = 0; dev[i]; i++)
        if (dev[i] == '\n' || dev[i] == '\r' || dev[i] == ' ')
            { dev[i] = '\0'; break; }
    if (!dev[0] || dev[0] == '#')
        return 1;

    if (!lp_exists(dev)) {
        /* USB takes about a second to enumerate and this runs long
         * before that. */
        for (int waited = 0; !lp_exists(dev) && waited < 4000; waited += 50)
            lp_sleep_ms(50);
    }
    if (!lp_exists(dev)) {
        dprintf(STDERR_FILENO,
                "datadisk: %s (from %s) is not there - looking for the %s\n"
                "datadisk:   label on the usual disks instead.\n",
                dev, CONF_BOOT, OUR_LABEL);
        return 1;
    }

    char fs[8], label[24];
    disk_identify(dev, fs, sizeof fs, label, sizeof label);
    if (strcmp(fs, "ext4") != 0 || strcmp(label, OUR_LABEL) != 0) {
        dprintf(STDERR_FILENO,
                "datadisk: %s is not our data partition any more\n"
                "datadisk:   (%s, labelled \"%s\") - ignoring %s.\n",
                dev, fs[0] ? fs : "unrecognised", label, CONF_BOOT);
        return 1;
    }

    /* Repair it before mounting, the same as the usual path does. A
     * filesystem that ext4 stopped writing to on the last boot is
     * exactly the one worth checking. */
    char e2fsck[64];
    if (boot_tool("e2fsck", e2fsck, sizeof e2fsck)) {
        char *argv[] = { e2fsck, (char *)"-p", (char *)dev, NULL };
        run(e2fsck, argv);
    }

    long rc = lp_mount(dev, MOUNT_POINT, "ext4", MS_NOSUID | MS_NODEV,
                       "errors=remount-ro");
    if (rc < 0) {
        dprintf(STDERR_FILENO,
                "datadisk: cannot mount %s on %s (%ld)\n", dev, MOUNT_POINT, -rc);
        return 1;
    }
    printf("datadisk: %s -> %s (chosen in %s)\n", dev, MOUNT_POINT, CONF_BOOT);
    return 0;
}

static void usage(void)
{
    printf("datadisk - choose which partition holds /data\n\n");
    printf("  datadisk                     what could be /data\n");
    printf("  datadisk <partition>         use it from the next boot\n");
    printf("  datadisk <partition> --now   use it, and mount it now\n");
    printf("  datadisk <disk> --format     wipe the disk and make one\n");
    printf("  datadisk --none              go back to finding it automatically\n\n");
    printf("/data is the only place that survives a reboot. Everything\n");
    printf("else is unpacked into RAM from the kernel image each time.\n\n");
    printf("The partition is labelled %s, which is what the boot\n", OUR_LABEL);
    printf("looks for. The device name is only a hint about which disk to\n");
    printf("try first, so moving the card to a different slot is fine.\n\n");
    printf("  disk   what storage is attached\n");
    printf("  part   change the partition table\n");
}

int main(int argc, char **argv)
{
    bool now = false, format = false, assume_yes = false;
    const char *target = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--now") == 0)          now = true;
        else if (strcmp(argv[i], "--format") == 0)  format = true;
        else if (strcmp(argv[i], "-y") == 0)        assume_yes = true;
        else if (strcmp(argv[i], "--none") == 0)    return forget();
        else if (strcmp(argv[i], "--boot") == 0)     return boot_mount();
        else if (strcmp(argv[i], "-h") == 0 ||
                 strcmp(argv[i], "--help") == 0)  { usage(); return 0; }
        else target = argv[i];
    }

    if (!target) {
        show_current();
        return list_candidates();
    }

    char path[64];
    if (strncmp(target, "/dev/", 5) == 0)
        strlcpy(path, target, sizeof path);
    else
        snprintf(path, sizeof path, "/dev/%s", target);

    if (format)
        return do_format(path, assume_yes);
    return choose(path, now, assume_yes);
}
