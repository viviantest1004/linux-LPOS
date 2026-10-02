/* expandfs - grow the data partition to the end of the card.
 *
 * Our image is 256MB. Written to a 32GB card that leaves 31.7GB
 * unallocated and idle. This does what Raspberry Pi OS does on first boot.
 *
 * Steps:
 *   1) ask the block device for its real size (BLKGETSIZE64)
 *   2) rewrite MBR entry 2 to reach the end of the device
 *   3) tell the kernel to re-read the partition table (BLKRRPART)
 *   4) mount it and grow the ext4 (EXT4_IOC_RESIZE_FS)
 *
 * We do not pull in resize2fs because the kernel grows a mounted ext4
 * itself: one ioctl and it adds the block groups. Calling that ioctl is
 * most of what the userspace tool does anyway.
 *
 * Safety: this only ever grows. There is no shrink path at all.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "syscall.h"
#include "disk.h"

/* Defaults to the SD card. Pass arguments to use another device, e.g.
 *   expandfs /dev/vda /dev/vda2 */
static const char *dev_disk = "/dev/mmcblk0";
static const char *dev_part = "/dev/mmcblk0p2";
#define DEV_DISK      dev_disk
#define DEV_PART      dev_part
#define MOUNT_POINT   "/data"

/* Where the ext4 that is being grown is reachable from.
 *
 * Growing an ext4 is an ioctl on any directory inside it, so what
 * matters is that the filesystem is mounted somewhere - not that it is
 * mounted at /data. On the disk-rooted amd64 image the partition being
 * grown IS the root, already mounted at /, and mounting a second copy
 * of the running root at /data to grow it would be both unnecessary and
 * a good way to confuse everything that reads /proc/mounts. So this is
 * set to wherever the partition already is, and only falls back to
 * mounting it at /data when it is not mounted at all. */
static char grow_at[64] = MOUNT_POINT;
#define PART_INDEX    2            /* 1-based */
#define SECTOR_SIZE   512
#define MBR_SIZE      512
#define PART_TABLE_OFF 446
#define PART_ENTRY_LEN 16
#define PART_TYPE_LINUX 0x83

/* ── Making sure this is our disk ─────────────────────────────────────
 *
 * This program rewrites a partition table. Pointed at the wrong disk it
 * destroys somebody else's data, and it is the only thing in this
 * system that can damage something other than itself.
 *
 * Which is not hypothetical. /etc/rc tries four device names because
 * the disk is called something different depending on whether it is an
 * SD card, a USB stick, NVMe or a virtual disk. Boot from USB with an
 * unrelated SD card left in the slot and the SD card is the first name
 * tried - a card from another Raspberry Pi would pass every check we
 * had, because it also has a Linux partition in slot 2.
 *
 * So the filesystem is asked what it is called. mksdcard.sh labels the
 * data partition LPZERO-DATA, and nothing without that label is touched.
 * The label sits in the ext4 superblock, which starts 1024 bytes into
 * the partition, 120 bytes in and 16 bytes long - no library needed to
 * read it.
 *
 * An unlabelled filesystem is accepted, with a warning. Images written
 * before labels existed are still out there and refusing to grow them
 * would be worse than the risk: the check is here to stop us touching
 * something that is definitely not ours, and an empty label is not
 * evidence either way.
 */
#define EXT_SB_OFFSET     1024
#define EXT_MAGIC_OFF       56      /* within the superblock */
#define EXT_LABEL_OFF      120
#define EXT_LABEL_LEN       16
#define EXT_MAGIC       0xEF53

/* The labels this project writes on a partition it is allowed to grow.
 *
 * LPZERODATA is the data partition of a RAM-rooted image; LPROOT is the
 * root of the disk-rooted amd64 one. Both are ours. Anything else -
 * somebody's photo drive that happened to be plugged in when the board
 * booted - is refused, which is the whole reason this check exists. */
#define OUR_LABEL  "LPZERODATA"   /* what mksdcard.sh writes */
#define OUR_LABEL2 "LPROOT"       /* what mkdisk.sh writes */

/* -1 cannot read, 0 not ours, 1 ours, 2 unlabelled */
static int check_label(const char *part)
{
    long fd = lp_open(part, O_RDONLY, 0);
    if (fd < 0)
        return -1;

    u8 sb[512];
    memset(sb, 0, sizeof(sb));

    if (lp_lseek((int)fd, EXT_SB_OFFSET, 0) < 0) {
        lp_close((int)fd);
        return -1;
    }
    long n = lp_read((int)fd, sb, sizeof(sb));
    lp_close((int)fd);

    if (n < (long)sizeof(sb))
        return -1;

    u16 magic = (u16)(sb[EXT_MAGIC_OFF] | (sb[EXT_MAGIC_OFF + 1] << 8));
    if (magic != EXT_MAGIC)
        return -1;                   /* not ext at all */

    char label[EXT_LABEL_LEN + 1];
    memcpy(label, sb + EXT_LABEL_OFF, EXT_LABEL_LEN);
    label[EXT_LABEL_LEN] = '\0';

    if (label[0] == '\0')
        return 2;

    return (strcmp(label, OUR_LABEL) == 0 ||
            strcmp(label, OUR_LABEL2) == 0) ? 1 : 0;
}

/* The BLKPG and BLKRRPART calls that tell a running kernel a partition
 * changed size live in the libc, as disk_tell_kernel(). They used to be
 * copied out here as well, with their own copy of struct
 * blkpg_ioctl_arg - and that copy carried a padding field the kernel
 * does not have, which made every BLKPG call from this program fail on
 * a 32-bit machine and is why a 64GB card never grew in a Pi Zero W.
 * One definition now, in the one place that owns block devices, so
 * there is nowhere for the two to drift apart again.
 *
 * The size ioctl is LP_BLKGETSIZE64, from unistd.h. It used to be
 * written out as 0x80081272 here, which is its value only where size_t
 * is eight bytes - on the Pi Zero W the call returned ENOTTY and this
 * program quietly did nothing on every card ever put in one.
 */

/* ext4 online grow. _IOW('f', 16, __u64) */
#define EXT4_IOC_RESIZE_FS  0x40086610

/* struct statfs is read through lp_statfs(), not by reaching into a
 * byte buffer at fixed offsets.
 *
 * It used to be the latter, with a comment saying "(arm64)" - and that
 * is exactly what it was: the 64-bit layout, read as u64 at offsets 8
 * and 16, on every architecture. On the Pi Zero W the 32-bit statfs is
 * a different structure with 32-bit fields, so bsize and blocks came
 * back as garbage, `want <= blocks` was true against nonsense, and this
 * printed "the filesystem already fills the partition" and stopped.
 *
 * The partition had already been grown by then. So a 64GB card ended up
 * with a 60GB partition holding a 124MB filesystem, and every boot
 * afterwards said the partition was already at full size and the
 * filesystem already filled it. Nothing ever grew, and nothing ever
 * said anything was wrong.
 *
 * libc has had a per-architecture statfs since the ARM port; this file
 * simply never used it. */

/* Not worth growing for less than this. */
#define MIN_GROW_MB     16

/* ── Keeping the watchdog quiet while this runs ───────────────────────
 *
 * /etc/rc arms the hardware watchdog for 120 seconds before anything
 * else, so that a boot which hangs still recovers. Growing a filesystem
 * is the one step at boot that can legitimately take longer than that:
 * a large card behind a slow reader, on a first boot, resizing a
 * filesystem to a couple of hundred gigabytes.
 *
 * If the timer fires in the middle, it resets the board between writing
 * the new partition table and finishing the resize - which leaves a
 * partition that is bigger than the filesystem inside it. That usually
 * recovers on the next boot, because this runs again. Usually is not a
 * word to rely on when the alternative is four lines.
 *
 * So we pet it as we go. Not disarm it: a machine that wedges here
 * should still recover, and disarming would take that away for exactly
 * the operation most likely to wedge.
 */
#define WDIOC_KEEPALIVE  0x80045705

static int wd_fd = -1;

static void watchdog_open(void)
{
    long fd = lp_open("/dev/watchdog", O_WRONLY, 0);
    wd_fd = (fd < 0) ? -1 : (int)fd;
}

static void watchdog_pet(void)
{
    if (wd_fd < 0)
        return;
    int dummy = 0;
    lp_ioctl(wd_fd, WDIOC_KEEPALIVE, &dummy);
}

/* Closing /dev/watchdog would normally disarm it - but only if 'V' is
 * written first, which we deliberately do not do. The timer keeps
 * running and the daemon started from /etc/services takes over. */
static void watchdog_close(void)
{
    if (wd_fd >= 0) {
        lp_close(wd_fd);
        wd_fd = -1;
    }
}

/* The resize is one ioctl that does not return until it is finished, so
 * there is nowhere in this process to pet from while it runs. A second
 * process does it instead, and is stopped when the work is done.
 *
 * Returns the pid, or -1 when there is no watchdog to pet. */
static pid_t watchdog_petter_start(void)
{
    watchdog_open();
    if (wd_fd < 0)
        return -1;

    pid_t pid = lp_fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        for (;;) {
            watchdog_pet();
            lp_sleep_ms(5000);
        }
    }
    return pid;
}

static void watchdog_petter_stop(pid_t pid)
{
    if (pid > 0) {
        lp_kill(pid, SIGKILL);
        int status = 0;
        lp_waitpid(pid, &status, 0);
    }
    watchdog_close();
}

static long dev_ioctl(const char *path, unsigned long req, void *arg, int flags)
{
    long fd = lp_open(path, flags, 0);
    if (fd < 0)
        return fd;
    long rc = sys_call3(SYS_ioctl, (long)fd, (long)req, (long)arg);
    lp_close((int)fd);
    return rc;
}

/* Read one MBR partition entry. */
static bool read_part_entry(const u8 *mbr, int index, u8 *type,
                            u32 *start, u32 *count)
{
    if (index < 1 || index > 4)
        return false;
    const u8 *e = mbr + PART_TABLE_OFF + (index - 1) * PART_ENTRY_LEN;
    *type = e[4];
    memcpy(start, e + 8, 4);
    memcpy(count, e + 12, 4);
    return true;
}

static void write_part_count(u8 *mbr, int index, u32 count)
{
    u8 *e = mbr + PART_TABLE_OFF + (index - 1) * PART_ENTRY_LEN;
    memcpy(e + 12, &count, 4);
    /* The end CHS cannot express more than 1023 cylinders. Write the
     * conventional maximum: both Linux and the Pi boot ROM read LBA, so
     * this value is a formality. */
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
}

/* Grow the partition table. false when it is already at the maximum. */
static bool grow_partition(u64 *new_bytes_out)
{
    u64 dev_bytes = 0;
    long rc = dev_ioctl(DEV_DISK, LP_BLKGETSIZE64, &dev_bytes, O_RDONLY);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "expandfs: cannot get the size of %s (%ld)\n",
                DEV_DISK, -rc);
        return false;
    }

    long fd = lp_open(DEV_DISK, O_RDWR, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "expandfs: cannot open %s (%ld)\n",
                DEV_DISK, -fd);
        return false;
    }

    u8 mbr[MBR_SIZE];
    if (lp_read((int)fd, mbr, sizeof(mbr)) != (long)sizeof(mbr)) {
        dprintf(STDERR_FILENO, "expandfs: could not read the MBR\n");
        lp_close((int)fd);
        return false;
    }

    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        dprintf(STDERR_FILENO, "expandfs: no MBR signature\n");
        lp_close((int)fd);
        return false;
    }

    u8  type; u32 start, count;
    read_part_entry(mbr, PART_INDEX, &type, &start, &count);

    if (type != PART_TYPE_LINUX) {
        dprintf(STDERR_FILENO,
                "expandfs: partition %d is not a Linux partition (0x%02x)."
                " Stopping, to be safe\n", PART_INDEX, type);
        lp_close((int)fd);
        return false;
    }

    u64 total_sectors = dev_bytes / SECTOR_SIZE;
    if (total_sectors <= start) {
        dprintf(STDERR_FILENO, "expandfs: the device is smaller than the partition\n");
        lp_close((int)fd);
        return false;
    }

    u64 max_count = total_sectors - start;
    if (max_count > 0xFFFFFFFFULL)
        max_count = 0xFFFFFFFFULL;      /* MBR is 32-bit, about 2TB */

    /* Grow only. */
    if (max_count <= count + (MIN_GROW_MB * 1024 * 1024 / SECTOR_SIZE)) {
        printf("expandfs: already at full size (%lu MB)\n",
               (unsigned long)(count / 2048));
        lp_close((int)fd);
        return false;
    }

    printf("expandfs: growing partition %d from %lu MB to %lu MB\n",
           PART_INDEX,
           (unsigned long)(count / 2048),
           (unsigned long)(max_count / 2048));

    write_part_count(mbr, PART_INDEX, (u32)max_count);

    if (lp_lseek((int)fd, 0, 0) < 0 ||
        lp_write((int)fd, mbr, sizeof(mbr)) != (long)sizeof(mbr)) {
        dprintf(STDERR_FILENO, "expandfs: writing the MBR failed\n");
        lp_close((int)fd);
        return false;
    }
    lp_sync();
    lp_close((int)fd);

    /* Now tell the running kernel about it, or the filesystem resize
     * below asks for more blocks than the block device admits to having
     * and comes back ENOSPC.
     *
     * BLKPG first: it resizes the one partition and works with other
     * partitions on the same disk mounted, which is always the case
     * here because /boot is. BLKRRPART is the fallback for a kernel or
     * a device that does not do BLKPG; it re-reads the whole table and
     * returns EBUSY whenever anything on the disk is in use. */
    rc = disk_tell_kernel(DEV_DISK, PART_INDEX,
                          (u64)start * SECTOR_SIZE,
                          (u64)max_count * SECTOR_SIZE);

    if (rc < 0) {
        /* The table on the card is right; the kernel just has not taken
         * it. Growing the filesystem now would fail, so say what is
         * true and let the next boot - which runs this again, and by
         * then reads the new size from the card - finish the job. */
        dprintf(STDERR_FILENO,
                "expandfs: the partition was grown on the card, but this"
                " kernel would not take the new size (%ld).\n"
                "expandfs: reboot once and the filesystem will follow.\n",
                -rc);
        return false;
    }

    /* Wait for the partition device to actually report the new size.
     *
     * Telling the kernel and the kernel having done it are not the same
     * instant: the block device is torn down and rebuilt, and on a
     * board that means devtmpfs removing and recreating
     * /dev/mmcblk0p2. Open it too early and the very next thing -
     * resize2fs - opens either nothing or the old device, and prints
     * "No such file or directory" about a partition that is right
     * there. Three seconds is far longer than it takes and costs
     * nothing on the normal path, which finishes on the first look. */
    u64 want = max_count * SECTOR_SIZE;
    for (int i = 0; i < 60; i++) {
        u64 have = 0;
        long fd = lp_open(DEV_PART, O_RDONLY, 0);
        if (fd >= 0) {
            sys_call3(SYS_ioctl, (long)fd, (long)LP_BLKGETSIZE64, (long)&have);
            lp_close((int)fd);
            if (have == want)
                break;
        }
        lp_sleep_ms(50);
    }

    *new_bytes_out = want;
    return true;
}

/* ── Growing it offline, which is the path that actually runs ────────
 *
 * resize2fs on the unmounted partition. This is what `expandfs` does on
 * a normal boot, and the online ioctl below is only for the case where
 * something else already has /data mounted.
 *
 * It used to be the other way round - mount it, then EXT4_IOC_RESIZE_FS
 * - and that ioctl asks for two things a board cannot be assumed to
 * have: CAP_SYS_RESOURCE, and a kernel built with online resize. If
 * either is missing the call returns EPERM, expandfs prints one line
 * and gives up, and the card is left with a 60GB partition holding a
 * 124MB filesystem. Nothing on the next boot fixes it, because by then
 * the partition is already at full size.
 *
 * resize2fs needs none of that. It opens the block device and writes to
 * it. With no size argument it grows the filesystem to fill whatever
 * the partition now is, which is exactly the question being asked.
 *
 * It is on the boot partition rather than in the system image, like
 * e2fsck and mke2fs, and boot_tool() checks its SHA-256 against the
 * list compiled into the kernel before running it as root.
 */
static bool grow_offline(void)
{
    char tool[128];
    if (!boot_tool("resize2fs", tool, sizeof tool))
        return false;

    printf("expandfs: growing the filesystem on %s to fill the partition\n",
           DEV_PART);
    /* Say it takes a while before it takes a while.
     *
     * Growing to 60GB means writing a few hundred block group
     * descriptors and their inode tables, and on a 1GHz board with an
     * SD card that is minutes, not seconds. Silence for that long on
     * the first boot reads as a hang, and somebody pulls the power -
     * in the middle of a filesystem resize, which is the one moment
     * where that actually costs them the card. */
    printf("expandfs:   on a large card this takes a few minutes."
           " Do not cut the power.\n");

    char *args[] = { (char *)"resize2fs", (char *)DEV_PART, NULL };
    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "expandfs: cannot start %s\n", tool);
        return false;
    }
    if (pid == 0) {
        extern char **environ;
        lp_execve(tool, args, environ);
        lp_exit(127);
    }
    int status = 0;
    lp_waitpid(pid, &status, 0);
    int rc = LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : -1;
    if (rc != 0) {
        dprintf(STDERR_FILENO,
                "expandfs: resize2fs exited %d.\n"
                "expandfs:   `fsck %s` and then `expandfs` again -"
                " resize2fs refuses a filesystem that has not been"
                " checked.\n", rc, DEV_PART);
        return false;
    }
    printf("expandfs: done\n");
    return true;
}

/* Grow a mounted ext4. */
static bool grow_filesystem(u64 part_bytes)
{
    lp_statfs_t fs;
    long rc = lp_statfs(grow_at, &fs);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "expandfs: statfs failed (%ld)\n", -rc);
        return false;
    }

    u64 bsize  = fs.bsize;
    u64 blocks = fs.blocks;
    if (bsize == 0) {
        dprintf(STDERR_FILENO, "expandfs: cannot determine the block size\n");
        return false;
    }

    u64 want = part_bytes / bsize;
    if (want <= blocks) {
        printf("expandfs: the filesystem already fills the partition\n");
        return true;
    }

    printf("expandfs: growing the filesystem from %lu MB to %lu MB\n",
           (unsigned long)(blocks * bsize / 1048576),
           (unsigned long)(want * bsize / 1048576));

    /* The kernel adds the block groups. This works while mounted. */
    long fd = lp_open(grow_at, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "expandfs: cannot open %s (%ld)\n",
                grow_at, -fd);
        return false;
    }

    rc = sys_call3(SYS_ioctl, (long)fd, EXT4_IOC_RESIZE_FS, (long)&want);
    lp_close((int)fd);

    if (rc < 0) {
        dprintf(STDERR_FILENO, "expandfs: grow failed (%ld)\n", -rc);
        return false;
    }

    printf("expandfs: done\n");
    return true;
}

/* Wait for a device node to show up.
 *
 * An SD card and a virtio disk are there before init runs. A USB disk is
 * not: the controller resets the port, asks the device what it is, spins
 * it up and only then does /dev/sda appear, which takes the better part
 * of a second. Without this the boot looks at an empty /dev, decides
 * there is no storage and carries on in RAM. */
/* The node existing is not the same as the disk answering. A USB stick
 * registers its partitions the moment the kernel has read the table, so
 * the node appears then - but reads can still fail for a moment after
 * that while the SCSI layer finishes with it, and everything this
 * program does starts by reading the partition table. So wait for a
 * readable first sector, not for a name in /dev. */
static bool wait_for_dev(const char *path, long timeout_ms)
{
    for (long waited = 0; ; waited += 50) {
        if (lp_exists(path)) {
            long fd = lp_open(path, O_RDONLY, 0);
            if (fd >= 0) {
                char probe[512];
                long n = lp_read((int)fd, probe, sizeof probe);
                lp_close((int)fd);
                if (n == (long)sizeof probe)
                    return true;
            }
        }
        if (waited >= timeout_ms)
            return lp_exists(path);   /* there, but not talking - let the
                                       * caller fail with a real error */
        lp_sleep_ms(50);
    }
}

#define WAIT_MS 4000

int main(int argc, char **argv)
{
    bool wait = false;

    /* -w: the device may not be there yet. Only worth passing for USB. */
    if (argc >= 2 && strcmp(argv[1], "-w") == 0) {
        wait = true;
        argc--;
        argv++;
    }

    if (argc >= 3) {
        dev_disk = argv[1];
        dev_part = argv[2];
    } else if (argc == 2) {
        dprintf(STDERR_FILENO,
                "usage: expandfs [-w] [disk partition]\n"
                "  e.g.  expandfs /dev/mmcblk0 /dev/mmcblk0p2\n"
                "  -w    wait up to 4s for the device (USB takes a moment)\n");
        return 2;
    }

    if (wait && !wait_for_dev(DEV_PART, WAIT_MS))
        return 1;

    /* Before anything is written: is this ours? */
    int owned = check_label(DEV_PART);
    if (owned == 0) {
        dprintf(STDERR_FILENO,
                "expandfs: %s belongs to something else - not touching it\n"
                "expandfs:   (its label is neither %s nor %s)\n",
                DEV_PART, OUR_LABEL, OUR_LABEL2);
        return 1;
    }
    if (owned == 2)
        dprintf(STDERR_FILENO,
                "expandfs: %s has no label. Growing it anyway, but a\n"
                "expandfs:   labelled partition is how this tells our disk\n"
                "expandfs:   from one that happens to be plugged in.\n",
                DEV_PART);

    u64 part_bytes = 0;
    bool grew = grow_partition(&part_bytes);

    /* Even when the partition did not change, the filesystem can be behind
     * (say a re-read failed last time and a reboot picked it up). */
    if (!grew) {
        long fd = lp_open(DEV_PART, O_RDONLY, 0);
        if (fd >= 0) {
            u64 sz = 0;
            sys_call3(SYS_ioctl, (long)fd, (long)LP_BLKGETSIZE64, (long)&sz);
            lp_close((int)fd);
            part_bytes = sz;
        }
    }

    if (part_bytes == 0)
        return 1;

    /* Growing requires it to be mounted - somewhere, not necessarily
     * here. Ask where it already is before deciding to mount it. */
    bool mounted_here = false;
    char where[64] = "";
    if (disk_mountpoint(DEV_PART, where, sizeof where) && where[0]) {
        strlcpy(grow_at, where, sizeof grow_at);
        printf("expandfs: %s is already mounted at %s\n", DEV_PART, grow_at);
        u64 pb = part_bytes;
        pid_t p = watchdog_petter_start();
        bool k = grow_filesystem(pb);
        watchdog_petter_stop(p);
        return k ? 0 : 1;
    }

    /* Nothing has it mounted, so grow it offline - see grow_offline().
     * This is the normal boot path: /etc/rc runs expandfs before it
     * mounts /data, exactly so that this can happen. */
    (void)mounted_here;

    /* Growing a large filesystem can outlast the 120 seconds /etc/rc
     * arms the watchdog for, and a reset partway through leaves the
     * partition bigger than the filesystem in it. */
    pid_t petter = watchdog_petter_start();

    bool ok = grow_offline();

    watchdog_petter_stop(petter);

    /* If we mounted it, we unmount it. The rc script mounts it again.
     *
     * And it has to be said when that fails: /data stays mounted, rc's
     * own mount then fails with EBUSY down every candidate device, and
     * rc reports "no data partition - continuing in RAM" while /data is
     * in fact mounted. */
    if (mounted_here && sys_call2(SYS_umount2, (long)grow_at, 0) < 0)
        dprintf(STDERR_FILENO,
                "expandfs: could not unmount %s - it stays mounted, and"
                " the boot may report there is no data partition\n",
                grow_at);

    return ok ? 0 : 1;
}
