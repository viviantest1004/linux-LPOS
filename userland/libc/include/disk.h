/* disk.h - block devices, partition tables and filesystem labels.
 *
 * Four programs needed to answer the same three questions - what disks
 * are here, what is on them, and is this one ours - and three of them
 * had grown their own copy of the answer. This is that code in one
 * place.
 *
 * Only MBR is handled, not GPT. The images this system writes are MBR
 * because the Raspberry Pi's GPU firmware reads an MBR to find the boot
 * partition, and a disk with two partition schemes on it is a way to
 * lose data rather than a feature. A GPT disk is recognised and left
 * alone rather than misread as an MBR one.
 */
#ifndef _LP_DISK_H
#define _LP_DISK_H

#include "types.h"

#define DISK_SECTOR   512
#define DISK_MAX      16      /* whole disks we will list */
#define DISK_PARTS     4      /* MBR holds four */

/* MBR partition type bytes we care about naming. */
#define PART_TYPE_EMPTY   0x00
#define PART_TYPE_FAT32   0x0c    /* FAT32 with LBA - what the Pi wants */
#define PART_TYPE_LINUX   0x83
#define PART_TYPE_SWAP    0x82
#define PART_TYPE_EXTEND  0x05
#define PART_TYPE_EXTEND2 0x0f
#define PART_TYPE_GPT     0xee    /* a GPT disk pretending to be MBR */

typedef struct {
    char name[32];      /* "sda", "mmcblk0p2" */
    char path[40];      /* "/dev/sda" */
    u64  bytes;
    int  index;         /* partition number; 0 for a whole disk */
    u8   type;          /* MBR type byte; 0 for a whole disk */
    u64  start;         /* first sector, for a partition */
    bool bootable;
    char fs[8];         /* "ext4", "vfat", or empty when unrecognised */
    char label[24];     /* filesystem label, or empty */
    char mount[32];     /* where it is mounted, or empty */
    char model[40];     /* what the hardware calls itself, or empty */
    bool removable;
} blk_t;

/* Every whole disk the kernel found, in the order /sys/block lists them
 * (which is alphabetical). Partitions, loop, ram and zram devices are
 * left out - this is "what could I put a filesystem on". */
int disk_list(blk_t *out, int max);

/* The partitions of one disk, read from its MBR. `disk` is a device
 * path like "/dev/sda". Entries with type 0 are skipped, so the count
 * is how many partitions exist, not how many table slots there are.
 * Returns -1 when the disk cannot be read, and 0 for a disk with no
 * partition table at all - which is not an error, just a blank disk. */
int disk_parts(const char *disk, blk_t *out, int max);

/* What kind of filesystem is on this device, and what is it called.
 * Both may come back empty. false when the device cannot be read. */
bool disk_identify(const char *dev, char *fs, size_t fsn,
                   char *label, size_t labeln);

/* Where this device is mounted, from /proc/mounts. false if nowhere. */
bool disk_mountpoint(const char *dev, char *out, size_t n);

/* Does the root filesystem survive a reboot?
 *
 * Two shapes of machine run this system and the difference decides what
 * half a dozen programs should do.
 *
 * The RAM-rooted one - every arm64 image, and the amd64 one meant for a
 * USB stick - has its whole root as a cpio inside the kernel, unpacked
 * into RAM at boot. Nothing written to /bin or /etc is there tomorrow,
 * which is why `persist` mounts overlays and why /root is a bind mount
 * off the data partition.
 *
 * The disk-rooted one boots a small initramfs that mounts a real ext4
 * partition and switches to it. There / is an ordinary writable
 * filesystem: `cp mytool /bin/` just works, /etc keeps what it is told,
 * and every one of those workarounds is not merely unnecessary but
 * actively wrong - an overlay over a directory that already persists
 * hides the real one behind a copy on itself.
 *
 * Told apart by the filesystem type mounted at /: rootfs, ramfs and
 * tmpfs are memory, anything else is a disk. */
bool root_is_persistent(void);

/* Size in bytes, straight from the kernel (BLKGETSIZE64). 0 on failure. */
u64 disk_bytes(const char *dev);

/* The whole disk a partition belongs to: /dev/sda2 -> /dev/sda, and
 * /dev/mmcblk0p2 -> /dev/mmcblk0. Returns false when `dev` does not
 * look like a partition. */
bool disk_whole(const char *part, char *out, size_t n);

/* Read and write the first sector. write_mbr syncs before returning. */
bool disk_read_mbr(const char *dev, u8 mbr[DISK_SECTOR]);
bool disk_write_mbr(const char *dev, const u8 mbr[DISK_SECTOR]);

/* One MBR table entry, by slot (1..4). */
void mbr_get(const u8 *mbr, int slot, u8 *type, u32 *start, u32 *count,
             bool *bootable);
void mbr_set(u8 *mbr, int slot, u8 type, u32 start, u32 count,
             bool bootable);
bool mbr_valid(const u8 *mbr);
/* Zero the table, write the 0x55AA signature, and set a disk signature.
 * Leaves the boot code area alone. */
void mbr_init(u8 *mbr);

/* Tell the running kernel that a partition changed size or appeared.
 * BLKPG first, because it works while other partitions on the same disk
 * are mounted; BLKRRPART as the fallback, which does not.
 * `start` and `len` are in bytes. Returns the kernel's -errno. */
long disk_tell_kernel(const char *disk, int pno, u64 start, u64 len);
/* Ask for the whole table to be re-read. -EBUSY when anything on the
 * disk is mounted, which is normal and usually fine to ignore. */
long disk_reread(const char *disk);

/* Make a table that has just been written take effect on the running
 * kernel. BLKRRPART when the disk is idle, and slot by slot with BLKPG
 * when it is not - which is most of the time, because the disk being
 * repartitioned usually has something mounted on it. Returns how many
 * slots the kernel refused; 0 means the new table is live and the
 * device nodes are there. */
int disk_apply_table(const char *disk, const u8 *mbr);

/* A human name for an MBR type byte: "Linux", "FAT32", "swap", ... */
const char *part_type_name(u8 type);

/* "1.4G", "188M", "512K". buf needs 8 bytes. */
void disk_human(u64 bytes, char *buf, size_t n);

/* ── Tools that live on the boot partition ──
 *
 * e2fsck and mke2fs are 2.6MB between them and do nothing at all on a
 * normal boot, so they sit on the FAT boot partition rather than in the
 * system image, where they would cost that much RAM for the life of the
 * machine.
 *
 * A FAT partition is writable from any PC - that is how the WiFi
 * password gets onto it - so "it is on /boot" is not by itself a reason
 * to run something as root. The hash each was built with is recorded in
 * /etc/boot-tools.sha256, which is inside the initramfs: part of the
 * kernel image, unpacked into RAM at boot, reachable from no filesystem
 * at all.
 *
 * Fills `path_out` with /boot/<name> and returns true only when the
 * file is there and its hash matches. Anything else prints why and
 * returns false, because the alternative is running somebody else's
 * program as root. */
bool boot_tool(const char *name, char *path_out, size_t n);

#endif /* _LP_DISK_H */
