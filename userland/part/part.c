/* part - change the partition table.
 *
 *   part <disk>                            show what is there
 *   part <disk> new <n> <type> <start> <size>
 *   part <disk> del <n>
 *   part <disk> type <n> <name>
 *   part <disk> boot <n>
 *   part <disk> clear
 *
 * Sizes are in MB, or "rest" for everything left. Start may be "next",
 * which means "just after the last partition".
 *
 *   part /dev/sda new 1 fat32 1 64        a 64MB boot partition
 *   part /dev/sda new 2 linux next rest   the rest of the disk
 *
 * ── MBR only ──
 * Not GPT. The images this system writes are MBR because the Raspberry
 * Pi's GPU firmware reads an MBR to find the boot partition, and a disk
 * carrying both schemes is a way to lose data rather than a feature. A
 * GPT disk is recognised and refused rather than half-edited.
 *
 * ── What it will not do ──
 * Touch a disk with anything mounted on it. That covers the important
 * case without a special rule for it: the disk this system booted from
 * has /boot and /data mounted, so repartitioning the ground you are
 * standing on is refused by the same check that stops you doing it to a
 * disk somebody else's files are open on.
 *
 * Everything is shown and confirmed before a single byte is written.
 * The table is the only thing written - no filesystem is created and no
 * data is erased, so a mistake is recoverable by writing the old table
 * back, as long as you know what it was. That is what the "before"
 * listing is for.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "disk.h"

/* Partitions start on a 1MB boundary. Not tradition: an SD card or SSD
 * erases in blocks much larger than a sector, and a filesystem whose
 * blocks straddle those boundaries makes the device read-modify-write
 * on every commit. 1MB divides evenly into every erase block in use. */
#define ALIGN_SECTORS  2048

typedef struct { const char *name; u8 type; } typename_t;

static const typename_t TYPES[] = {
    { "linux", PART_TYPE_LINUX },
    { "ext4",  PART_TYPE_LINUX },
    { "fat32", PART_TYPE_FAT32 },
    { "fat",   PART_TYPE_FAT32 },
    { "boot",  PART_TYPE_FAT32 },
    { "swap",  PART_TYPE_SWAP  },
    { "efi",   0xef },
    { NULL, 0 }
};

static bool parse_type(const char *s, u8 *out)
{
    for (int i = 0; TYPES[i].name; i++) {
        if (strcmp(s, TYPES[i].name) == 0) {
            *out = TYPES[i].type;
            return true;
        }
    }
    /* A raw hex byte, for a type this list does not name. */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        long v = strtol(s + 2, NULL, 16);
        if (v > 0 && v <= 0xFF) { *out = (u8)v; return true; }
    }
    return false;
}

static void show(const char *disk, const u8 *mbr)
{
    u64 total = disk_bytes(disk);
    char size[12];
    disk_human(total, size, sizeof size);
    printf("%s  %s\n\n", disk, size);

    printf("  %-4s %-6s %-11s %10s %10s %8s  %s\n",
           "slot", "boot", "type", "start MB", "size MB", "fs", "label");

    bool any = false;
    for (int slot = 1; slot <= DISK_PARTS; slot++) {
        u8 type; u32 start, count; bool boot;
        mbr_get(mbr, slot, &type, &start, &count, &boot);
        if (type == PART_TYPE_EMPTY && count == 0) {
            printf("  %-4d %-6s %-11s\n", slot, "", "-");
            continue;
        }
        any = true;

        char part[48], fs[8] = "", label[24] = "";
        const char *base = disk + (strncmp(disk, "/dev/", 5) == 0 ? 5 : 0);
        size_t bl = strlen(base);
        bool needs_p = bl > 0 && base[bl - 1] >= '0' && base[bl - 1] <= '9';
        snprintf(part, sizeof part, "/dev/%s%s%d", base, needs_p ? "p" : "", slot);
        if (lp_exists(part))
            disk_identify(part, fs, sizeof fs, label, sizeof label);

        printf("  %-4d %-6s %-11s %10lu %10lu %8s  %s\n",
               slot, boot ? "*" : "", part_type_name(type),
               (unsigned long)((u64)start * DISK_SECTOR / 1048576),
               (unsigned long)((u64)count * DISK_SECTOR / 1048576),
               fs[0] ? fs : "-", label[0] ? label : "");
    }
    if (!any)
        printf("\n  The table is empty.\n");
}

/* Where the next partition would naturally start: after the last one,
 * rounded up to the alignment, and never before the first megabyte -
 * the partition table itself lives in sector 0. */
static u32 next_free(const u8 *mbr)
{
    u32 end = ALIGN_SECTORS;
    for (int slot = 1; slot <= DISK_PARTS; slot++) {
        u8 type; u32 start, count;
        mbr_get(mbr, slot, &type, &start, &count, NULL);
        if (type == PART_TYPE_EMPTY || count == 0)
            continue;
        u32 e = start + count;
        if (e > end) end = e;
    }
    return (end + ALIGN_SECTORS - 1) / ALIGN_SECTORS * ALIGN_SECTORS;
}

/* Does [start,start+count) run into a partition that already exists? */
static int overlaps(const u8 *mbr, int ignore_slot, u64 start, u64 count)
{
    for (int slot = 1; slot <= DISK_PARTS; slot++) {
        if (slot == ignore_slot)
            continue;
        u8 type; u32 s, c;
        mbr_get(mbr, slot, &type, &s, &c, NULL);
        if (type == PART_TYPE_EMPTY || c == 0)
            continue;
        if (start < (u64)s + c && (u64)s < start + count)
            return slot;
    }
    return 0;
}

/* Anything mounted on this disk stops us. */
static bool disk_is_busy(const char *disk, char *what, size_t n)
{
    if (disk_mountpoint(disk, what, n))
        return true;

    blk_t parts[DISK_PARTS];
    int np = disk_parts(disk, parts, DISK_PARTS);
    for (int i = 0; i < np; i++) {
        if (parts[i].mount[0]) {
            snprintf(what, n, "%s on %s", parts[i].path, parts[i].mount);
            return true;
        }
    }
    return false;
}

static bool confirm(bool assume_yes)
{
    if (assume_yes)
        return true;

    printf("\nWrite this table? [y/N] ");
    /* Read the answer from the terminal, not from stdin, so that this
     * still asks when the output is being piped somewhere. */
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

static int commit(const char *disk, u8 *mbr, bool assume_yes)
{
    printf("\nAfter:\n\n");
    show(disk, mbr);

    if (!confirm(assume_yes)) {
        printf("Nothing was written.\n");
        return 1;
    }

    if (!disk_write_mbr(disk, mbr)) {
        dprintf(STDERR_FILENO, "part: could not write to %s\n", disk);
        return 1;
    }

    /* Written is not the same as in effect. The kernel decided what
     * the partitions were when it last looked at this disk, and it does
     * not look again on its own - so without this the new partition has
     * no device node and nothing can be done with it until a reboot,
     * which for a USB drive somebody just plugged in is not an answer.
     *
     * disk_apply_table re-reads the whole table when the disk is idle,
     * and goes slot by slot with BLKPG when something on it is mounted.
     * It returns how many slots the kernel would not take. */
    int refused = disk_apply_table(disk, mbr);
    if (refused == 0) {
        printf("part: written, and the kernel has it. The partitions are"
               " there now.\n");
    } else {
        printf("part: written, but the kernel would not take %d of the"
               " four slots.\n", refused);
        printf("      That is what it says when a partition is mounted:"
               " unmount everything\n"
               "      on %s and run the same command again, or reboot"
               " and it picks the\n"
               "      table up on the way.\n", disk);
    }
    return 0;
}

static void usage(void)
{
    printf("part - change the partition table (MBR)\n\n");
    printf("  part <disk>                        show what is there\n");
    printf("  part <disk> new <n> <type> <start> <size>\n");
    printf("  part <disk> del <n>\n");
    printf("  part <disk> type <n> <type>\n");
    printf("  part <disk> boot <n>               mark it bootable\n");
    printf("  part <disk> clear                  empty the table\n\n");
    printf("start and size are in MB. size may be 'rest', start may be\n");
    printf("'next' - just after the last partition. Both are rounded to a\n");
    printf("1MB boundary, which is what flash storage erases in.\n\n");
    printf("types: linux ext4 fat32 fat boot swap efi, or 0x<hex>\n\n");
    printf("  part /dev/sda new 1 fat32 1 64     a 64MB boot partition\n");
    printf("  part /dev/sda new 2 linux next rest\n\n");
    printf("Only the table is written - no filesystem is made and no data\n");
    printf("is erased. 'datadisk' does the rest for a data partition.\n");
    printf("A disk with anything mounted on it is refused.\n");
}

int main(int argc, char **argv)
{
    bool assume_yes = false;
    const char *args[8];
    int nargs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-y") == 0) {
            assume_yes = true;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (nargs < 8) {
            args[nargs++] = argv[i];
        }
    }

    if (nargs < 1) { usage(); return 2; }

    char disk[64];
    if (strncmp(args[0], "/dev/", 5) == 0)
        strlcpy(disk, args[0], sizeof disk);
    else
        snprintf(disk, sizeof disk, "/dev/%s", args[0]);

    if (!lp_exists(disk)) {
        dprintf(STDERR_FILENO, "part: %s is not there. 'disk' lists what is.\n",
                disk);
        return 1;
    }

    u8 mbr[DISK_SECTOR];
    bool have = disk_read_mbr(disk, mbr);
    if (!have) {
        dprintf(STDERR_FILENO, "part: cannot read %s\n", disk);
        return 1;
    }

    /* A GPT disk keeps a fake MBR entry of type 0xEE covering the whole
     * disk, so that old tools see a full disk rather than a blank one.
     * Editing it as if it were a real MBR would leave the disk with two
     * disagreeing tables. */
    for (int slot = 1; slot <= DISK_PARTS; slot++) {
        u8 t; mbr_get(mbr, slot, &t, NULL, NULL, NULL);
        if (t == PART_TYPE_GPT) {
            dprintf(STDERR_FILENO,
                    "part: %s is a GPT disk, and this only writes MBR.\n"
                    "      Repartition it on another computer, or\n"
                    "      'part %s clear' to start over as MBR - which\n"
                    "      throws away everything on it.\n", disk, disk);
            return 1;
        }
    }

    if (!mbr_valid(mbr))
        mbr_init(mbr);          /* blank disk: start a table */

    if (nargs == 1) {
        show(disk, mbr);
        printf("\n  part %s new <n> <type> <start MB> <size MB|rest>\n", disk);
        return 0;
    }

    /* Everything past here writes. */
    char busy[96];
    if (disk_is_busy(disk, busy, sizeof busy)) {
        dprintf(STDERR_FILENO,
                "part: %s is in use - %s\n"
                "part:   Changing the table under a mounted filesystem is\n"
                "part:   how a disk gets corrupted. Unmount it first.\n",
                disk, busy);
        return 1;
    }

    printf("Before:\n\n");
    show(disk, mbr);

    const char *cmd = args[1];

    if (strcmp(cmd, "clear") == 0) {
        mbr_init(mbr);
        return commit(disk, mbr, assume_yes);
    }

    if (nargs < 3) { usage(); return 2; }
    int slot = atoi(args[2]);
    if (slot < 1 || slot > DISK_PARTS) {
        dprintf(STDERR_FILENO, "part: the slot has to be 1 to %d\n", DISK_PARTS);
        return 2;
    }

    if (strcmp(cmd, "del") == 0) {
        mbr_set(mbr, slot, PART_TYPE_EMPTY, 0, 0, false);
        return commit(disk, mbr, assume_yes);
    }

    if (strcmp(cmd, "boot") == 0) {
        /* Exactly one partition is bootable, so this moves the flag
         * rather than adding another. */
        for (int s = 1; s <= DISK_PARTS; s++) {
            u8 t; u32 st, c; bool b;
            mbr_get(mbr, s, &t, &st, &c, &b);
            if (t != PART_TYPE_EMPTY || c != 0)
                mbr_set(mbr, s, t, st, c, s == slot);
        }
        return commit(disk, mbr, assume_yes);
    }

    if (strcmp(cmd, "type") == 0) {
        if (nargs < 4) { usage(); return 2; }
        u8 type;
        if (!parse_type(args[3], &type)) {
            dprintf(STDERR_FILENO, "part: \"%s\" is not a type I know.\n"
                    "      linux ext4 fat32 fat boot swap efi, or 0x<hex>\n",
                    args[3]);
            return 2;
        }
        u8 t; u32 st, c; bool b;
        mbr_get(mbr, slot, &t, &st, &c, &b);
        if (c == 0) {
            dprintf(STDERR_FILENO, "part: slot %d is empty\n", slot);
            return 1;
        }
        mbr_set(mbr, slot, type, st, c, b);
        return commit(disk, mbr, assume_yes);
    }

    if (strcmp(cmd, "new") == 0) {
        if (nargs < 6) { usage(); return 2; }

        u8 type;
        if (!parse_type(args[3], &type)) {
            dprintf(STDERR_FILENO, "part: \"%s\" is not a type I know.\n"
                    "      linux ext4 fat32 fat boot swap efi, or 0x<hex>\n",
                    args[3]);
            return 2;
        }

        u64 total = disk_bytes(disk) / DISK_SECTOR;
        if (total == 0) {
            dprintf(STDERR_FILENO, "part: cannot get the size of %s\n", disk);
            return 1;
        }
        /* MBR counts sectors in 32 bits, so it cannot describe a disk
         * past about 2TB. Say so rather than silently wrapping. */
        if (total > 0xFFFFFFFFULL) {
            printf("part: %s is larger than MBR can describe.\n", disk);
            printf("      Only the first 2TB will be usable.\n");
            total = 0xFFFFFFFFULL;
        }

        u64 start;
        if (strcmp(args[4], "next") == 0) {
            start = next_free(mbr);
        } else {
            start = (u64)atoi(args[4]) * 1048576 / DISK_SECTOR;
            if (start < ALIGN_SECTORS)
                start = ALIGN_SECTORS;
            start = (start + ALIGN_SECTORS - 1) / ALIGN_SECTORS * ALIGN_SECTORS;
        }

        u64 count;
        if (strcmp(args[5], "rest") == 0) {
            if (start >= total) {
                dprintf(STDERR_FILENO, "part: no room left on %s\n", disk);
                return 1;
            }
            count = total - start;
        } else {
            count = (u64)atoi(args[5]) * 1048576 / DISK_SECTOR;
            count = count / ALIGN_SECTORS * ALIGN_SECTORS;
        }

        if (count == 0) {
            dprintf(STDERR_FILENO, "part: a size of 0 makes no partition\n");
            return 2;
        }
        if (start + count > total) {
            char h[12];
            disk_human((u64)total * DISK_SECTOR, h, sizeof h);
            dprintf(STDERR_FILENO,
                    "part: that runs past the end of the disk (%s)\n", h);
            return 1;
        }
        int hit = overlaps(mbr, slot, start, count);
        if (hit) {
            dprintf(STDERR_FILENO,
                    "part: that overlaps partition %d.\n"
                    "      'next' as the start puts it after the last one.\n",
                    hit);
            return 1;
        }

        mbr_set(mbr, slot, type, (u32)start, (u32)count, false);
        return commit(disk, mbr, assume_yes);
    }

    dprintf(STDERR_FILENO, "part: no idea what \"%s\" means\n", cmd);
    usage();
    return 2;
}
