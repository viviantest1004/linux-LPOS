/* disk.c - block devices, partition tables and filesystem labels. */
#include "disk.h"
#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define BLKRRPART    0x125F      /* _IO(0x12, 95)  re-read the whole table */
/* LP_BLKGETSIZE64 is built from sizeof(size_t) in unistd.h: the number
 * is not the same on a 32-bit machine, and the constant that used to be
 * written out here was the 64-bit one. */
#define BLKPG        0x1269      /* _IO(0x12, 105) change one partition */

#define BLKPG_ADD_PARTITION     1
#define BLKPG_DEL_PARTITION     2
#define BLKPG_RESIZE_PARTITION  3

typedef struct {
    s64  start;                /* bytes */
    s64  length;               /* bytes */
    int  pno;
    char devname[64];
    char volname[64];
} blkpg_part_t;

/* struct blkpg_ioctl_arg, exactly as include/uapi/linux/blkpg.h has it:
 *
 *     int op; int flags; int datalen; void *data;
 *
 * There is NO padding field between datalen and data, and writing one
 * in - which is what this used to do - is not harmless. On a 64-bit
 * machine a pointer has to start on an 8-byte boundary, so the compiler
 * puts four bytes of padding there anyway and an explicit `int pad`
 * lands in exactly that hole: the layout comes out identical and the
 * ioctl works. On the Pi Zero W a pointer is four bytes and needs no
 * padding, so the kernel reads `data` from offset 12 - which was the
 * pad, and the pad is zero. Every BLKPG call on that board therefore
 * handed the kernel a null pointer and failed.
 *
 * What that cost: expandfs writes the new partition table and then has
 * to tell the running kernel about it. BLKPG is the call that works
 * while /boot is mounted, and /boot is mounted at that point because
 * resize2fs lives on it. With BLKPG failing, the fallback BLKRRPART
 * returned EBUSY for the same reason, the kernel went on believing the
 * partition was 124MB, and resize2fs - which asks the kernel, not the
 * table - found nothing to do. On a 64GB card in a Pi Zero W the
 * filesystem never grew, at any boot, ever.
 */
typedef struct {
    int   op;
    int   flags;
    int   datalen;
    void *data;
} blkpg_arg_t;

/* Where the MBR keeps its table. */
#define PART_TABLE_OFF  446
#define PART_ENTRY_LEN   16
#define MBR_SIG_OFF     510

/* ext2/3/4: superblock 1024 bytes in, magic at +56, label at +120. */
#define EXT_SB_OFFSET  1024
#define EXT_MAGIC_OFF    56
#define EXT_LABEL_OFF   120
#define EXT_LABEL_LEN    16
#define EXT_MAGIC    0xEF53

/* FAT: the boot sector names itself, and carries a label. */
#define FAT32_ID_OFF     0x52
#define FAT16_ID_OFF     0x36
#define FAT32_LABEL_OFF    71
#define FAT16_LABEL_OFF    43
#define FAT_LABEL_LEN      11

/* dirent, as getdents64 lays it out */
#define DIRENT_RECLEN 16
#define DIRENT_TYPE   18
#define DIRENT_NAME   19

static long dev_ioctl(const char *path, unsigned long req, void *arg, int flags)
{
    long fd = lp_open(path, flags, 0);
    if (fd < 0)
        return fd;
    long rc = sys_call3(SYS_ioctl, (long)fd, (long)req, (long)arg);
    lp_close((int)fd);
    return rc;
}

u64 disk_bytes(const char *dev)
{
    u64 n = 0;
    if (dev_ioctl(dev, LP_BLKGETSIZE64, &n, O_RDONLY) < 0)
        return 0;
    return n;
}

/* ── the MBR ─────────────────────────────────────────────────────── */

bool mbr_valid(const u8 *mbr)
{
    return mbr[MBR_SIG_OFF] == 0x55 && mbr[MBR_SIG_OFF + 1] == 0xAA;
}

void mbr_get(const u8 *mbr, int slot, u8 *type, u32 *start, u32 *count,
             bool *bootable)
{
    if (slot < 1 || slot > DISK_PARTS) {
        if (type) *type = 0;
        if (start) *start = 0;
        if (count) *count = 0;
        if (bootable) *bootable = false;
        return;
    }
    const u8 *e = mbr + PART_TABLE_OFF + (slot - 1) * PART_ENTRY_LEN;
    if (bootable) *bootable = (e[0] == 0x80);
    if (type)     *type = e[4];
    if (start)    memcpy(start, e + 8, 4);
    if (count)    memcpy(count, e + 12, 4);
}

void mbr_set(u8 *mbr, int slot, u8 type, u32 start, u32 count, bool bootable)
{
    if (slot < 1 || slot > DISK_PARTS)
        return;
    u8 *e = mbr + PART_TABLE_OFF + (slot - 1) * PART_ENTRY_LEN;
    memset(e, 0, PART_ENTRY_LEN);
    e[0] = bootable ? 0x80 : 0x00;
    e[4] = type;
    memcpy(e + 8, &start, 4);
    memcpy(e + 12, &count, 4);
    /* The CHS fields stay zero. Nothing has booted from CHS geometry in
     * twenty years, and a wrong value there is worse than none: some
     * firmware trusts it over the LBA fields sitting right beside it. */
}

void mbr_init(u8 *mbr)
{
    memset(mbr + PART_TABLE_OFF, 0, DISK_PARTS * PART_ENTRY_LEN);
    mbr[MBR_SIG_OFF]     = 0x55;
    mbr[MBR_SIG_OFF + 1] = 0xAA;
    /* A disk signature, which Windows and Linux both use to tell one
     * disk from another. Ours spells LP0Z. */
    u32 sig = 0x4C50305A;
    memcpy(mbr + 440, &sig, 4);
}

bool disk_read_mbr(const char *dev, u8 mbr[DISK_SECTOR])
{
    long fd = lp_open(dev, O_RDONLY, 0);
    if (fd < 0)
        return false;
    long n = lp_read((int)fd, mbr, DISK_SECTOR);
    lp_close((int)fd);
    return n == DISK_SECTOR;
}

bool disk_write_mbr(const char *dev, const u8 mbr[DISK_SECTOR])
{
    long fd = lp_open(dev, O_RDWR, 0);
    if (fd < 0)
        return false;
    long n = lp_write((int)fd, mbr, DISK_SECTOR);
    lp_close((int)fd);
    lp_sync();
    return n == DISK_SECTOR;
}

long disk_tell_kernel(const char *disk, int pno, u64 start, u64 len)
{
    blkpg_part_t part;
    memset(&part, 0, sizeof part);
    part.start  = (s64)start;
    part.length = (s64)len;
    part.pno    = pno;

    blkpg_arg_t arg;
    memset(&arg, 0, sizeof arg);
    arg.op      = BLKPG_RESIZE_PARTITION;
    arg.datalen = (int)sizeof part;
    arg.data    = &part;

    long rc = dev_ioctl(disk, BLKPG, &arg, O_RDONLY);
    if (rc < 0)
        rc = dev_ioctl(disk, BLKRRPART, NULL, O_RDONLY);
    return rc;
}

long disk_reread(const char *disk)
{
    return dev_ioctl(disk, BLKRRPART, NULL, O_RDONLY);
}

/* ── Making a table the kernel has not read yet take effect ───────────
 *
 * Writing the partition table to sector 0 changes nothing about the
 * running machine: the kernel decided what /dev/sda1 and /dev/sda2 are
 * when it last looked, and it does not look again on its own.
 *
 * BLKRRPART asks it to look again at the whole table, and is the right
 * answer when it works. It refuses with EBUSY while ANYTHING on the
 * disk is mounted - which is the common case, not the rare one: you
 * partition the free space on a stick that already has one partition
 * mounted, or you grow the data partition on the card you booted from.
 * "Written. Reboot and it will pick it up" is not an answer for a USB
 * drive somebody just plugged in.
 *
 * BLKPG is the per-partition version and does not care what else is
 * mounted. So each slot is deleted and added back to match the table we
 * just wrote. Deleting a slot that is not there fails harmlessly and is
 * ignored; deleting one that is MOUNTED fails with EBUSY, and that one
 * is reported, because the caller has just written a table the kernel
 * is refusing to adopt for a reason the person needs to hear.
 *
 * Returns the number of slots the kernel would not accept: 0 means the
 * table is live. */
int disk_apply_table(const char *disk, const u8 *mbr)
{
    /* The cheap path first. When nothing on the disk is mounted this
     * re-reads all four slots in one call and gets the extended
     * partitions right too, which the loop below does not attempt. */
    if (dev_ioctl(disk, BLKRRPART, NULL, O_RDONLY) >= 0)
        return 0;

    int refused = 0;

    for (int slot = 1; slot <= DISK_PARTS; slot++) {
        u8  type; u32 start, count; bool boot;
        mbr_get(mbr, slot, &type, &start, &count, &boot);

        blkpg_part_t part;
        memset(&part, 0, sizeof part);
        part.pno = slot;

        blkpg_arg_t arg;
        memset(&arg, 0, sizeof arg);
        arg.datalen = (int)sizeof part;
        arg.data    = &part;

        /* Out with the old. A slot the kernel does not have gives
         * ENXIO, which is exactly the state we want it in anyway. */
        arg.op = BLKPG_DEL_PARTITION;
        long rc = dev_ioctl(disk, BLKPG, &arg, O_RDONLY);
        if (rc < 0 && rc != -6 /* ENXIO */ && rc != -22 /* EINVAL */) {
            refused++;
            continue;               /* mounted, almost certainly */
        }

        if (type == PART_TYPE_EMPTY || count == 0)
            continue;               /* the table says nothing is there */

        part.start  = (s64)start * DISK_SECTOR;
        part.length = (s64)count * DISK_SECTOR;
        arg.op      = BLKPG_ADD_PARTITION;
        if (dev_ioctl(disk, BLKPG, &arg, O_RDONLY) < 0)
            refused++;
    }

    return refused;
}

const char *part_type_name(u8 type)
{
    switch (type) {
    case PART_TYPE_EMPTY:   return "-";
    case 0x01:              return "FAT12";
    case 0x04: case 0x06:   return "FAT16";
    case 0x07:              return "NTFS/exFAT";
    case 0x0b:              return "FAT32";
    case PART_TYPE_FAT32:   return "FAT32";
    case 0x0e:              return "FAT16";
    case PART_TYPE_EXTEND:
    case PART_TYPE_EXTEND2: return "extended";
    case PART_TYPE_SWAP:    return "swap";
    case PART_TYPE_LINUX:   return "Linux";
    case 0x8e:              return "Linux LVM";
    case 0xa5:              return "FreeBSD";
    case PART_TYPE_GPT:     return "GPT";
    case 0xef:              return "EFI system";
    default:                return "unknown";
    }
}

/* ── what is on it ───────────────────────────────────────────────── */

/* Copy a fixed-width label out and trim the padding. FAT pads with
 * spaces, ext with NULs, so both are trimmed from the right. */
static void trim_label(const u8 *src, size_t len, char *out, size_t outn)
{
    if (len >= outn)
        len = outn - 1;
    memcpy(out, src, len);
    out[len] = '\0';
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\0'))
        out[--len] = '\0';
}

bool disk_identify(const char *dev, char *fs, size_t fsn,
                   char *label, size_t labeln)
{
    if (fs && fsn)       fs[0] = '\0';
    if (label && labeln) label[0] = '\0';

    long fd = lp_open(dev, O_RDONLY, 0);
    if (fd < 0)
        return false;

    /* One read covers all three: the FAT boot sector at 0, the ext
     * superblock at 1024, and swap's signature at the end of the first
     * 4096-byte page. */
    u8 head[4096];
    memset(head, 0, sizeof head);
    long n = lp_read((int)fd, head, sizeof head);
    lp_close((int)fd);
    if (n < 2048)
        return false;               /* too small to hold any of them */

    u16 magic = (u16)(head[EXT_SB_OFFSET + EXT_MAGIC_OFF] |
                      (head[EXT_SB_OFFSET + EXT_MAGIC_OFF + 1] << 8));
    if (magic == EXT_MAGIC) {
        if (fs && fsn) strlcpy(fs, "ext4", fsn);
        if (label && labeln)
            trim_label(head + EXT_SB_OFFSET + EXT_LABEL_OFF,
                       EXT_LABEL_LEN, label, labeln);
        return true;
    }

    if (head[510] == 0x55 && head[511] == 0xAA) {
        bool f32 = memcmp(head + FAT32_ID_OFF, "FAT", 3) == 0;
        bool f16 = memcmp(head + FAT16_ID_OFF, "FAT", 3) == 0;
        if (f32 || f16) {
            if (fs && fsn) strlcpy(fs, "vfat", fsn);
            if (label && labeln)
                trim_label(head + (f32 ? FAT32_LABEL_OFF : FAT16_LABEL_OFF),
                           FAT_LABEL_LEN, label, labeln);
            return true;
        }
    }

    /* swap keeps its signature in the last ten bytes of the first page,
     * so it is only there to read on a device at least that big. */
    if (n >= 4096 && memcmp(head + 4086, "SWAPSPACE2", 10) == 0) {
        if (fs && fsn) strlcpy(fs, "swap", fsn);
        return true;
    }

    return true;        /* readable, just not something we recognise */
}

bool disk_mountpoint(const char *dev, char *out, size_t n)
{
    if (n) out[0] = '\0';

    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0)
        return false;

    char line[512];
    bool found = false;
    while (readline((int)fd, line, sizeof line) >= 0) {
        char *sp = strchr(line, ' ');
        if (!sp)
            continue;
        *sp = '\0';
        if (strcmp(line, dev) != 0)
            continue;
        char *point = sp + 1;
        char *end = strchr(point, ' ');
        if (end) *end = '\0';
        strlcpy(out, point, n);
        found = true;
        break;
    }
    lp_close((int)fd);
    return found;
}

bool root_is_persistent(void)
{
    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0)
        return false;          /* cannot tell: assume the cautious answer */

    char line[512];
    bool persistent = false;
    while (readline((int)fd, line, sizeof line) >= 0) {
        /* device mountpoint fstype ... - we want the third field of the
         * line whose second is exactly "/". */
        char *sp = strchr(line, ' ');
        if (!sp)
            continue;
        char *point = sp + 1;
        char *end = strchr(point, ' ');
        if (!end)
            continue;
        *end = '\0';
        if (strcmp(point, "/") != 0)
            continue;

        char *fs = end + 1;
        char *fend = strchr(fs, ' ');
        if (fend) *fend = '\0';

        persistent = strcmp(fs, "rootfs") != 0 &&
                     strcmp(fs, "ramfs")  != 0 &&
                     strcmp(fs, "tmpfs")  != 0;
        break;
    }
    lp_close((int)fd);
    return persistent;
}

bool disk_whole(const char *part, char *out, size_t n)
{
    size_t len = strlen(part);
    if (len == 0 || len + 1 > n)
        return false;

    /* Trailing digits are the partition number. What comes before them
     * is the disk - except that mmcblk and nvme put a "p" in between,
     * and that p belongs to the partition, not to the disk name. */
    size_t i = len;
    while (i > 0 && part[i - 1] >= '0' && part[i - 1] <= '9')
        i--;
    if (i == len)
        return false;                    /* no number: not a partition */

    if (i > 1 && part[i - 1] == 'p' &&
        part[i - 2] >= '0' && part[i - 2] <= '9')
        i--;                             /* mmcblk0p2 -> mmcblk0 */

    memcpy(out, part, i);
    out[i] = '\0';
    return true;
}

/* ── listing what the kernel found ───────────────────────────────── */

/* Read a one-line file out of /sys. Empty string when it is not there. */
static void sysfs_read(const char *path, char *out, size_t n)
{
    out[0] = '\0';
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return;
    long got = lp_read((int)fd, out, n - 1);
    lp_close((int)fd);
    if (got <= 0) { out[0] = '\0'; return; }
    out[got] = '\0';
    for (long i = got - 1; i >= 0; i--) {
        if (out[i] == '\n' || out[i] == '\r' || out[i] == ' ')
            out[i] = '\0';
        else
            break;
    }
}

/* Things in /sys/block that are not disks you would put data on. */
static bool is_virtual(const char *name)
{
    static const char *skip[] = { "loop", "ram", "zram", "dm-", "md", 0 };
    for (int i = 0; skip[i]; i++)
        if (strncmp(name, skip[i], strlen(skip[i])) == 0)
            return true;
    return false;
}

static void fill_disk(blk_t *b, const char *name)
{
    memset(b, 0, sizeof *b);
    strlcpy(b->name, name, sizeof b->name);
    snprintf(b->path, sizeof b->path, "/dev/%s", name);

    char p[160], v[64];
    snprintf(p, sizeof p, "/sys/block/%s/size", name);
    sysfs_read(p, v, sizeof v);
    /* /sys reports in 512-byte sectors whatever the hardware's real
     * sector size is - that is the one place the kernel is always
     * consistent about it. */
    b->bytes = (u64)strtol(v, 0, 10) * 512ULL;
    if (b->bytes == 0)
        b->bytes = disk_bytes(b->path);

    snprintf(p, sizeof p, "/sys/block/%s/removable", name);
    sysfs_read(p, v, sizeof v);
    b->removable = (v[0] == '1');

    snprintf(p, sizeof p, "/sys/block/%s/device/model", name);
    sysfs_read(p, b->model, sizeof b->model);
    if (!b->model[0]) {
        /* NVMe puts it here instead. */
        snprintf(p, sizeof p, "/sys/block/%s/device/device/model", name);
        sysfs_read(p, b->model, sizeof b->model);
    }
    if (!b->model[0]) {
        snprintf(p, sizeof p, "/sys/block/%s/device/name", name);
        sysfs_read(p, b->model, sizeof b->model);
    }

    disk_identify(b->path, b->fs, sizeof b->fs, b->label, sizeof b->label);
    disk_mountpoint(b->path, b->mount, sizeof b->mount);
}

int disk_list(blk_t *out, int max)
{
    long fd = lp_open("/sys/block", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 0;

    char buf[4096];
    int  n = 0;

    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0)
            break;
        for (long off = 0; off < got && n < max; ) {
            char *rec  = buf + off;
            u16   len  = *(u16 *)(rec + DIRENT_RECLEN);
            char *name = rec + DIRENT_NAME;
            if (len == 0)
                break;
            off += len;

            if (name[0] == '.' || is_virtual(name))
                continue;
            fill_disk(&out[n], name);
            n++;
        }
        if (n >= max)
            break;
    }
    lp_close((int)fd);
    return n;
}

int disk_parts(const char *disk, blk_t *out, int max)
{
    u8 mbr[DISK_SECTOR];
    if (!disk_read_mbr(disk, mbr))
        return -1;
    if (!mbr_valid(mbr))
        return 0;                       /* blank disk, not an error */

    /* The kernel names a partition on mmcblk and nvme with a "p" in
     * front of the number, and everything else without one. */
    const char *base = disk;
    if (strncmp(disk, "/dev/", 5) == 0)
        base = disk + 5;
    size_t blen = strlen(base);
    bool needs_p = blen > 0 && base[blen - 1] >= '0' && base[blen - 1] <= '9';

    int n = 0;
    for (int slot = 1; slot <= DISK_PARTS && n < max; slot++) {
        u8 type; u32 start, count; bool boot;
        mbr_get(mbr, slot, &type, &start, &count, &boot);
        if (type == PART_TYPE_EMPTY || count == 0)
            continue;

        blk_t *b = &out[n];
        memset(b, 0, sizeof *b);
        b->index    = slot;
        b->type     = type;
        b->start    = start;
        b->bootable = boot;
        b->bytes    = (u64)count * DISK_SECTOR;

        snprintf(b->name, sizeof b->name, "%s%s%d",
                 base, needs_p ? "p" : "", slot);
        snprintf(b->path, sizeof b->path, "/dev/%s", b->name);

        if (lp_exists(b->path)) {
            /* Prefer what the device itself says over the table: the
             * type byte is a claim, the superblock is the thing. */
            disk_identify(b->path, b->fs, sizeof b->fs,
                          b->label, sizeof b->label);
            disk_mountpoint(b->path, b->mount, sizeof b->mount);
            u64 real = disk_bytes(b->path);
            if (real)
                b->bytes = real;
        }
        n++;
    }
    return n;
}

void disk_human(u64 bytes, char *buf, size_t n)
{
    static const char *unit[] = { "B", "K", "M", "G", "T" };
    u64 v = bytes;
    int u = 0;
    /* Switch units at 10000 rather than 1024 so that sizes stay at most
     * four digits and still read exactly where it matters - a 512MB
     * card says 512M, not 0.5G. */
    while (v >= 10000 && u < 4) { v /= 1024; u++; }
    snprintf(buf, n, "%lu%s", (unsigned long)v, unit[u]);
}

/* ── Tools on the boot partition ─────────────────────────────────── */

#define BOOT_TOOL_HASHES "/etc/boot-tools.sha256"

bool boot_tool(const char *name, char *path_out, size_t n)
{
    snprintf(path_out, n, "/boot/%s", name);

    if (!lp_exists(path_out)) {
        dprintf(STDERR_FILENO,
                "%s is not on the boot partition.\n"
                "  It is not in the system image either - it lives there so\n"
                "  that it costs no memory. Rebuild the card to get it back.\n",
                path_out);
        return false;
    }

    long fd = lp_open(BOOT_TOOL_HASHES, O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "%s: this image does not record what %s should be, so there\n"
                "  is no way to tell it from somebody else's. Not running it.\n",
                path_out, name);
        return false;
    }

    char want[80];
    want[0] = '\0';
    char line[160];
    while (readline((int)fd, line, sizeof line) >= 0) {
        /* "<64 hex>  <name>", the format sha256sum writes. */
        char *sp = strchr(line, ' ');
        if (!sp)
            continue;
        *sp = '\0';
        char *who = sp + 1;
        while (*who == ' ')
            who++;
        if (strcmp(who, name) == 0) {
            strlcpy(want, line, sizeof want);
            break;
        }
    }
    lp_close((int)fd);

    if (!want[0]) {
        dprintf(STDERR_FILENO,
                "%s: %s is not in %s, so there is nothing to check it\n"
                "  against. Not running it.\n",
                path_out, name, BOOT_TOOL_HASHES);
        return false;
    }

    char have[72];
    if (!lp_sha256_file(path_out, have)) {
        dprintf(STDERR_FILENO, "%s: cannot read it\n", path_out);
        return false;
    }

    if (strcmp(want, have) != 0) {
        dprintf(STDERR_FILENO,
                "** %s is not the one this image was built with.\n"
                "   Not running it. It would run as root, and the boot\n"
                "   partition is writable from any PC.\n"
                "   expected %s\n"
                "   found    %s\n",
                path_out, want, have);
        return false;
    }
    return true;
}
