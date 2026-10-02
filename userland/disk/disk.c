/* disk - what storage is attached, and what is on it.
 *
 *   disk                 every disk the kernel found
 *   disk <device>        one disk, and its partitions in detail
 *   disk -a              the same, including the empty table slots
 *
 * ── Why this exists ──
 * /etc/rc looks for the data partition under four names - mmcblk0p2,
 * vda2, nvme0n1p2, sda2 - because that covers an SD card, a virtual
 * disk, NVMe and USB, and only one of them is normally there. When none
 * of them is, the boot prints four "no such device" lines and carries
 * on in RAM, and there is no way from inside the machine to find out
 * whether the disk is missing, named something else, partitioned
 * differently, or simply not ours.
 *
 * This answers that. It lists what the kernel actually has, straight
 * out of /sys/block, rather than what we hoped would be there.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "disk.h"

static void print_disk_line(const blk_t *d)
{
    char size[12];
    disk_human(d->bytes, size, sizeof size);

    printf("  %-12s %7s  %-11s %s%s\n",
           d->name, size,
           d->removable ? "removable" : "fixed",
           d->model[0] ? d->model : "",
           d->mount[0] ? "" : "");
}

/* One partition, indented under its disk. */
static void print_part_line(const blk_t *p, bool ours)
{
    char size[12];
    disk_human(p->bytes, size, sizeof size);

    printf("    %-14s %7s  %-11s %-8s %-12s %s\n",
           p->name, size,
           part_type_name(p->type),
           p->fs[0] ? p->fs : "-",
           p->label[0] ? p->label : "-",
           p->mount[0] ? p->mount : (ours ? "(not mounted)" : ""));
}

static bool is_ours(const blk_t *p)
{
    return strcmp(p->label, "LPZERODATA") == 0 ||
           strcmp(p->label, "LPZERO") == 0;
}

static void show_one(const blk_t *d, bool all)
{
    char size[12];
    disk_human(d->bytes, size, sizeof size);

    printf("\n%s  %s%s%s\n", d->path, size,
           d->removable ? "  removable" : "",
           d->mount[0] ? "  mounted on " : "");
    if (d->mount[0])
        printf("  mounted on %s\n", d->mount);
    if (d->model[0])
        printf("  %s\n", d->model);

    blk_t parts[DISK_PARTS];
    int n = disk_parts(d->path, parts, DISK_PARTS);

    if (n < 0) {
        printf("  cannot read the first sector\n");
        return;
    }
    if (n == 0) {
        if (d->fs[0]) {
            /* A filesystem written straight onto the disk with no
             * partition table at all. Unusual, but it works, and it is
             * what some tools produce when told to format a whole
             * disk. */
            printf("  no partition table - %s%s%s written directly\n",
                   d->fs,
                   d->label[0] ? ", labelled " : "",
                   d->label[0] ? d->label : "");
        } else {
            printf("  no partition table (blank disk)\n");
            printf("  'part %s new 1 linux 1 rest' makes one\n", d->path);
        }
        return;
    }

    printf("    %-14s %7s  %-11s %-8s %-12s %s\n",
           "partition", "size", "type", "fs", "label", "mounted");
    for (int i = 0; i < n; i++)
        print_part_line(&parts[i], is_ours(&parts[i]));

    if (all && n < DISK_PARTS)
        printf("    %d of %d table slots used\n", n, DISK_PARTS);
}

static int show_all(bool all)
{
    blk_t disks[DISK_MAX];
    int n = disk_list(disks, DISK_MAX);

    if (n == 0) {
        printf("No disks.\n\n");
        printf("The kernel found no block device at all. On this board that\n");
        printf("means the card is not seated, or the machine was started\n");
        printf("with no disk attached - the system runs from RAM either\n");
        printf("way, so it boots and then has nowhere to keep anything.\n");
        return 1;
    }

    printf("  %-12s %7s  %-11s %s\n", "disk", "size", "kind", "model");
    for (int i = 0; i < n; i++)
        print_disk_line(&disks[i]);

    for (int i = 0; i < n; i++)
        show_one(&disks[i], all);

    /* The question this command usually exists to answer. */
    printf("\n");
    bool found_data = false;
    for (int i = 0; i < n; i++) {
        blk_t parts[DISK_PARTS];
        int np = disk_parts(disks[i].path, parts, DISK_PARTS);
        for (int j = 0; j < np; j++) {
            if (strcmp(parts[j].label, "LPZERODATA") == 0) {
                found_data = true;
                printf("/data is %s%s%s\n", parts[j].path,
                       parts[j].mount[0] ? ", mounted on " : " - not mounted",
                       parts[j].mount[0] ? parts[j].mount : "");
            }
        }
    }
    if (!found_data) {
        printf("Nothing here is labelled LPZERODATA, so /data is in RAM and\n");
        printf("everything written to it is lost on the next boot.\n");
        printf("  datadisk                 what could become /data\n");
        printf("  datadisk <partition>     use an existing partition\n");
        printf("  datadisk <disk> --format wipe a disk and make one\n");
    }
    return 0;
}

static void usage(void)
{
    printf("disk - what storage is attached\n\n");
    printf("  disk                 every disk, and what is on it\n");
    printf("  disk <device>        one disk in detail\n");
    printf("  disk -a              show unused table slots too\n\n");
    printf("Related:\n");
    printf("  part      change the partition table\n");
    printf("  datadisk  choose which partition becomes /data\n");
    printf("  df        how full the mounted filesystems are\n");
}

int main(int argc, char **argv)
{
    bool all = false;
    const char *target = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            all = true;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else {
            target = argv[i];
        }
    }

    if (!target)
        return show_all(all);

    char path[64];
    if (strncmp(target, "/dev/", 5) == 0)
        strlcpy(path, target, sizeof path);
    else
        snprintf(path, sizeof path, "/dev/%s", target);

    if (!lp_exists(path)) {
        dprintf(STDERR_FILENO, "disk: %s is not there.\n", path);
        dprintf(STDERR_FILENO, "      'disk' with no arguments lists what is.\n");
        return 1;
    }

    blk_t d;
    memset(&d, 0, sizeof d);
    strlcpy(d.path, path, sizeof d.path);
    strlcpy(d.name, path + 5, sizeof d.name);
    d.bytes = disk_bytes(path);
    disk_identify(path, d.fs, sizeof d.fs, d.label, sizeof d.label);
    disk_mountpoint(path, d.mount, sizeof d.mount);

    show_one(&d, all);
    return 0;
}
