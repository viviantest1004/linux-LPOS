/* fsck - check and repair the data partition.
 *
 *   fsck [-f] <device>...
 *
 * Tries each device in turn and checks the first one that exists and
 * carries our label, which is how everything else in /etc/rc finds the
 * disk: the name depends on whether this is an SD card, a USB disk,
 * NVMe or a virtual machine, and more than one can be present.
 *
 * ── Why this exists ──
 * /data is mounted errors=remount-ro, so ext4 corruption is noticed and
 * the filesystem stops taking writes. That is the right first move and
 * it is also the whole of what the system could do about it: there was
 * no way to repair it without pulling the card and finding another
 * computer. On a board sitting headless on a shelf, that is the same as
 * unrecoverable.
 *
 * ── Where e2fsck lives ──
 * On the boot partition, not in this filesystem image.
 *
 *   Not on /data     that is the thing being repaired
 *   Not in the image the initramfs is unpacked into RAM and stays there
 *                    for the life of the machine. 1.4MB of memory,
 *                    permanently, for a program that runs for a
 *                    fraction of a second at boot and does nothing at
 *                    all when the filesystem is clean
 *   On /boot         costs no memory, and it is read-only mounted, so
 *                    the tool cannot be damaged by the failure it is
 *                    there to fix
 *
 * ── Why the binary is checked before it is run ──
 * /boot is a FAT partition. Pull the card, put it in any PC, and it is
 * writable - that is the whole point of it, and it is how the WiFi
 * password and the SSH key get onto the machine. It also means that
 * "the e2fsck on the boot partition" is not, by itself, a reason to
 * trust a binary this system runs as root.
 *
 * Worse, until recently /boot was mounted without checking its label.
 * Boot from USB with a stranger's SD card in the slot and their FAT
 * partition became /boot - and then this ran their e2fsck as root.
 * mount -L closed that, and this is the second lock on the same door.
 *
 * boot_tool() in the libc does that check - the expected hash is in
 * /etc/boot-tools.sha256, which is inside the initramfs: part of the
 * kernel image, unpacked into RAM at boot, and reachable from no
 * filesystem at all. If it does not match, this does not run it. The
 * cost of being wrong that way is losing automatic repair, which the
 * code already handles; the cost of the other choice is running
 * somebody else's program as root.
 *
 * ── Preen mode ──
 * -p: fix what can be fixed without asking, and do nothing at all when
 * the filesystem is already clean. A clean check reads the superblock
 * and exits, so running this on every boot costs milliseconds. Without
 * -p, e2fsck asks questions, and there is nobody at the console at boot
 * to answer them.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "disk.h"

/* ── Checking that the partition is ours ──
 *
 * This takes a list of device names and checks the first one that
 * exists, because the disk is called something different on an SD card,
 * a USB stick, NVMe and a virtual machine. "The first one that exists"
 * is the wrong rule when two of them do.
 *
 * Boot from USB with an unrelated SD card in the slot and the card is
 * tried first - so e2fsck ran, in -p mode, against a stranger's
 * filesystem, and -p means it repairs what it finds without asking.
 * mount and expandfs both check the label before touching anything;
 * this did not, and it is the one of the three that writes to a
 * filesystem nobody has looked at yet.
 *
 * The label is in the ext4 superblock, 1024 bytes into the partition
 * and 120 bytes into that, NUL padded to 16. An unlabelled filesystem
 * is accepted, as elsewhere, so that older cards still work. */
#define OUR_LABEL "LPZERODATA"

/* Which e2fsck we ended up running. boot_tool fills this in. */
static char E2FSCK[64] = "/boot/e2fsck";

/* 1 = ours, 0 = somebody else's, 2 = ext4 with no label at all. */
static int check_label(const char *dev)
{
    char fs[8], label[24];
    if (!disk_identify(dev, fs, sizeof fs, label, sizeof label))
        return 0;
    if (strcmp(fs, "ext4") != 0)
        return 0;                      /* not ext4: not ours to repair */
    if (!label[0])
        return 2;
    return strcmp(label, OUR_LABEL) == 0 ? 1 : 0;
}

#define FSCK_CORRECTED   1
#define FSCK_REBOOT      2
#define FSCK_UNCORRECTED 4

static int run_e2fsck(const char *dev, bool force)
{
    pid_t pid = lp_fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        char *argv[5];
        int   n = 0;
        argv[n++] = (char *)E2FSCK;
        argv[n++] = (char *)"-p";      /* fix what is safe, ask nothing */
        if (force)
            argv[n++] = (char *)"-f";  /* check even when marked clean */
        argv[n++] = (char *)dev;
        argv[n]   = NULL;

        char *envp[] = { NULL };
        lp_execve(E2FSCK, argv, envp);
        lp_exit(127);
    }

    int status = 0;
    lp_waitpid(pid, &status, 0);
    return LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : -1;
}

int main(int argc, char **argv)
{
    bool force = false;
    int  first = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0) {
            force = true;
            first = i + 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: fsck [-f] <device>...\n");
            printf("  checks the first device in the list that exists\n");
            printf("  -f  check even when the filesystem says it is clean\n");
            printf("\ne2fsck itself lives on the boot partition (%s),\n",
                   E2FSCK);
            printf("so mount /boot before this runs.\n");
            return 0;
        }
    }

    if (first >= argc) {
        dprintf(STDERR_FILENO, "usage: fsck [-f] <device>...\n");
        return 2;
    }

    /* boot_tool checks that /boot/e2fsck is the one this image was
     * built with before we agree to run it as root. It prints why when
     * it refuses; a boot that cannot repair the filesystem is not a
     * boot worth stopping. */
    if (!boot_tool("e2fsck", E2FSCK, sizeof E2FSCK))
        return 0;

    for (int i = first; i < argc; i++) {
        if (!lp_exists(argv[i]))
            continue;                  /* a different kind of disk */

        int owned = check_label(argv[i]);
        if (owned == 0)
            continue;                  /* somebody else's. Not ours to repair. */
        if (owned == 2)
            dprintf(STDERR_FILENO,
                    "fsck: %s has no label. Checking it anyway, but a\n"
                    "fsck:   labelled partition is how this tells our disk\n"
                    "fsck:   from one that happens to be plugged in.\n",
                    argv[i]);

        int rc = run_e2fsck(argv[i], force);

        if (rc == 0)
            return 0;                  /* clean, and quiet about it */

        if (rc < 0) {
            dprintf(STDERR_FILENO, "fsck: could not run %s\n", E2FSCK);
            return 1;
        }

        if (rc & FSCK_UNCORRECTED) {
            dprintf(STDERR_FILENO,
                    "fsck: ** %s is damaged and could not be repaired\n"
                    "fsck:    automatically. Run this by hand to answer\n"
                    "fsck:    its questions:  %s -f %s\n",
                    argv[i], E2FSCK, argv[i]);
            return 1;
        }

        if (rc & FSCK_CORRECTED)
            printf("fsck: %s had errors, and they were repaired\n", argv[i]);

        if (rc & FSCK_REBOOT)
            printf("fsck:   the repair wants a reboot to settle\n");

        return 0;
    }

    /* None of the named devices exist. Normal: the list covers SD, USB,
     * NVMe and virtio, and only one of them is ever there. */
    return 0;
}
