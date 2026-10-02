/* bootcount - break a reboot loop.
 *
 * The watchdog reboots a board that has stopped answering. That is the
 * right thing to do exactly once. If whatever wedged the machine is in
 * the startup script, the watchdog will do it again, and again, and the
 * board spends the rest of its life rebooting - with no window to log in
 * and fix the file that is causing it. A headless board on a shelf
 * cannot be rescued from that state without pulling the card.
 *
 * So we count. Every boot bumps a number on the data partition. guard
 * clears it once the system has stayed up for five minutes, which is the
 * definition of a boot that worked. The number therefore only ever grows
 * when boots keep failing, and when it gets to LIMIT we say so and exit
 * non-zero. /etc/rc uses that to skip /data/rc.local - the one part of
 * the boot the user can change, and so the one most likely to be at
 * fault.
 *
 * There is deliberately no clock in any of this. At this point in the
 * boot the time has been restored from a file at best, and a board that
 * has never reached the network has no idea what year it is.
 *
 *   exit 0   carry on
 *   exit 1   we are in a loop - skip the startup script
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "syscall.h"

#define STATE_FILE  "/data/boot_count"
#define LIMIT       5      /* failed boots before we stop trusting rc.local */
#define CAP        99      /* do not let the number run away */

/* Safe mode, asked for rather than arrived at.
 *
 * The counter above is the automatic version: five boots that did not
 * last, and the startup script is skipped. This is the same thing on
 * purpose - add lpzero.safe to cmdline.txt on the boot partition, which
 * is FAT32 and editable from any PC, and the next boot comes up plain.
 *
 * That matters because the automatic version needs five failed boots
 * first, and each of those takes as long as the machine takes to hang.
 * When you already know what is wrong, waiting through five of them is
 * not a recovery procedure. */
#define SAFE_FLAG   "lpzero.safe"

static bool safe_mode(void)
{
    char cmdline[1024];
    if (proc_read("/proc/cmdline", cmdline, sizeof(cmdline)) <= 0)
        return false;
    return strstr(cmdline, SAFE_FLAG) != NULL;
}

static int read_count(void)
{
    char buf[32];
    long n = proc_read(STATE_FILE, buf, sizeof(buf));
    if (n <= 0)
        return 0;                  /* first boot, or no data partition */
    long v = strtol(buf, NULL, 10);
    if (v < 0)   v = 0;
    if (v > CAP) v = CAP;
    return (int)v;
}

static bool write_count(int v)
{
    long fd = lp_open(STATE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;              /* read-only, or running from RAM */

    char buf[16];
    int  n = snprintf(buf, sizeof(buf), "%d\n", v);
    bool ok = lp_write((int)fd, buf, (size_t)n) == n;
    lp_close((int)fd);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-c") == 0) {
        /* Clear it. This is what a boot that worked looks like. */
        if (!write_count(0)) {
            dprintf(STDERR_FILENO,
                    "bootcount: could not clear %s - the count still"
                    " stands\n", STATE_FILE);
            return 1;
        }
        return 0;
    }
    if (argc > 1) {
        printf("usage: bootcount [-c]\n");
        printf("  counts boots that did not last; -c clears the count\n");
        printf("  exits 1 once %d boots in a row have failed\n", LIMIT);
        printf("  exits 1 straight away when the kernel command line\n");
        printf("  carries %s (edit cmdline.txt on the boot partition)\n",
               SAFE_FLAG);
        return 2;
    }

    if (safe_mode()) {
        printf("[boot] safe mode (%s on the kernel command line)\n", SAFE_FLAG);
        printf("[boot]    skipping /data/rc.local. Remove the word from\n");
        printf("[boot]    cmdline.txt to boot normally again.\n");
        return 1;
    }

    int count = read_count() + 1;

    /* If the count cannot be written, refuse rather than carry on.
     *
     * The old reasoning - "without a data partition there is no
     * rc.local to skip either" - describes a case that is not the one
     * that happens. /data is mounted errors=remount-ro, which is a
     * state this system is designed to reach and has an fsck for, and a
     * full /data does the same thing. In both, rc.local is still there
     * and still readable, so it still runs; only the counter is gone.
     * The counter then never reaches the limit, and the one guard
     * against a board that reboots for ever fails open at exactly the
     * moment it is needed.
     *
     * Refusing skips rc.local. That is the safe direction: a board that
     * comes up plain and reachable can be fixed, and one stuck in a
     * reboot loop cannot. */
    if (!write_count(count)) {
        dprintf(STDERR_FILENO,
                "bootcount: cannot write %s - is /data full or read-only?\n"
                "bootcount:   Skipping /data/rc.local, because a boot that"
                " cannot be counted cannot be trusted to end.\n", STATE_FILE);
        return 1;
    }

    if (count < LIMIT)
        return 0;

    printf("[boot] ** %d boots in a row did not last five minutes.\n", count);
    printf("[boot]    Skipping /data/rc.local - it is the usual cause.\n");
    printf("[boot]    Fix or delete it, then: bootcount -c\n");
    return 1;
}
