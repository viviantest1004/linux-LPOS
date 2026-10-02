/* watchdog - reboot the board if this program stops running.
 *
 *   watchdog          run in the foreground (init supervises it)
 *   watchdog -t <s>   timeout in seconds (default 15)
 *   watchdog -x       disarm the watchdog and exit
 *   watchdog -1       arm it and exit without petting - the board then
 *                     reboots after the timeout. This is how the reboot
 *                     path gets verified; it is not a way to run.
 *
 * The BCM2835 has a hardware countdown timer. Once armed, it reboots
 * the board when it reaches zero. Something has to keep resetting it -
 * that is all this program does.
 *
 * Why it matters: this is meant to be a device left running. If the
 * kernel wedges, or memory runs out badly enough that nothing can be
 * scheduled, SSH is gone and the only fix is walking over and pulling
 * the power. With this, the board reboots itself in 15 seconds.
 *
 * The kernel driver was already in the build - what was missing was
 * anyone to pet it, so the timer was never armed and it never fired.
 *
 * Closing /dev/watchdog normally *disarms* the timer. That is the
 * "magic close" convention, and it is wrong for us: if this program
 * dies, we want the reboot, not a quiet disarm. WDIOS_DISABLECARD is
 * therefore never sent on the way out unless -x asked for it.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define DEV_WATCHDOG  "/dev/watchdog"

/* linux/watchdog.h ioctls. _IOWR('W', n, int) etc. */
#define WDIOC_GETSUPPORT   0x80285700
#define WDIOC_KEEPALIVE    0x80045705
#define WDIOC_SETTIMEOUT   0xc0045706
#define WDIOC_GETTIMEOUT   0x80045707
#define WDIOC_SETOPTIONS   0x80045704

#define WDIOS_DISABLECARD  0x0001
#define WDIOS_ENABLECARD   0x0002

#define DEFAULT_TIMEOUT    15

/* Pet it well before the deadline. If we only just made it each time,
 * one slow moment would reboot a healthy board. */
static int pet_interval_ms(int timeout_s)
{
    int ms = timeout_s * 1000 / 3;
    return ms < 1000 ? 1000 : ms;
}

int main(int argc, char **argv)
{
    int  timeout = DEFAULT_TIMEOUT;
    bool disable = false;
    bool arm_only = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            timeout = atoi(argv[++i]);
            if (timeout < 2 || timeout > 120) {
                dprintf(STDERR_FILENO,
                        "watchdog: timeout must be between 2 and 120 seconds\n");
                return 2;
            }
        } else if (strcmp(argv[i], "-x") == 0) {
            disable = true;
        } else if (strcmp(argv[i], "-1") == 0) {
            arm_only = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: watchdog [-t seconds] [-x]\n");
            printf("  -t <s>   reboot if not petted for this long (default %d)\n",
                   DEFAULT_TIMEOUT);
            printf("  -x       disarm the watchdog and exit\n");
        printf("  -1       arm it and exit without petting. /etc/rc uses\n");
        printf("           this to cover the boot itself, before this\n");
        printf("           program is running to pet it\n\n");
            printf("Runs in the foreground; init keeps it alive.\n");
            return 0;
        } else {
            dprintf(STDERR_FILENO, "watchdog: unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    long fd = lp_open(DEV_WATCHDOG, O_WRONLY, 0);
    if (fd < 0) {
        /* No device node at all means no watchdog hardware here - a
         * virtual machine, or a board whose driver is not in the build.
         * That is a fact about the machine, not a failure of this
         * program, and it will not change while the machine is running.
         * So say it once and exit in a way that tells init not to keep
         * asking. Anything else (busy, permission) is worth retrying. */
        if (-fd == 2) {                 /* ENOENT - no such file */
            printf("watchdog: no watchdog hardware on this machine"
                   " - nothing to arm\n");
            printf("watchdog:   (a real Pi has one; a virtual machine"
                   " does not)\n");
            return LP_EXIT_NO_HARDWARE;
        }
        dprintf(STDERR_FILENO,
                "watchdog: cannot open %s (%ld)\n"
                "          the kernel needs a watchdog driver for this board\n",
                DEV_WATCHDOG, -fd);
        return 1;
    }

    if (disable) {
        int opt = WDIOS_DISABLECARD;
        if (lp_ioctl((int)fd, WDIOC_SETOPTIONS, &opt) < 0)
            dprintf(STDERR_FILENO, "watchdog: could not disarm it\n");
        else
            printf("watchdog: disarmed\n");
        lp_close((int)fd);
        return 0;
    }

    /* Ask for our timeout. The hardware may not offer exactly what we
     * asked for, so read back what it actually took. */
    int want = timeout;
    if (lp_ioctl((int)fd, WDIOC_SETTIMEOUT, &want) < 0)
        dprintf(STDERR_FILENO,
                "watchdog: could not set the timeout, using the default\n");

    int actual = 0;
    if (lp_ioctl((int)fd, WDIOC_GETTIMEOUT, &actual) == 0 && actual > 0)
        timeout = actual;

    int opt = WDIOS_ENABLECARD;
    lp_ioctl((int)fd, WDIOC_SETOPTIONS, &opt);

    int interval = pet_interval_ms(timeout);

    if (arm_only) {
        /* Leave it armed and walk away. Closing the device would
         * normally disarm it, but only if 'V' is written first - the
         * kernel's "magic close". We deliberately do not, so the timer
         * keeps counting and the board reboots in `timeout` seconds. */
        printf("watchdog: armed for %ds and exiting - the board reboots"
               " unless something takes over petting it\n", timeout);
        lp_close((int)fd);
        return 0;
    }

    printf("watchdog: armed, %ds timeout, petting every %dms\n",
           timeout, interval);

    /* From here the board reboots itself unless we keep going. */
    for (;;) {
        int dummy = 0;
        if (lp_ioctl((int)fd, WDIOC_KEEPALIVE, &dummy) < 0) {
            /* Some drivers only take a write. Try that before giving up. */
            if (lp_write((int)fd, "\0", 1) != 1) {
                dprintf(STDERR_FILENO,
                        "watchdog: cannot pet it any more - "
                        "the board will reboot in about %ds\n", timeout);
                return 1;
            }
        }
        lp_sleep_ms(interval);
    }
}
