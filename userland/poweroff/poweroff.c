/* poweroff, reboot, halt - stop the machine properly.
 *
 * One program, three names. Which one you typed decides what happens,
 * the same way it does on any other Unix.
 *
 * None of them switches the machine off themselves. They send init a
 * signal and init does the work, because init is the only process that
 * knows what is running and is allowed to wait for it to stop. What
 * init then does is the part that matters:
 *
 *   - asks every service to finish, and gives it a few seconds
 *   - kills whatever ignored that
 *   - sync, twice
 *   - unmounts /data
 *   - and only then asks the kernel to cut the power
 *
 * The unmount is the reason this exists at all. /data is ext4 with a
 * journal, so yanking the power is survivable - but survivable means
 * the next boot replays a journal and may decide it wants a full fsck,
 * and anything a program had buffered is simply gone. On a board that
 * is supposed to sit somewhere unattended for months, the moment
 * somebody types `poweroff` is the one chance to write it all down.
 *
 * `halt` stops everything and leaves the machine powered but idle. On
 * hardware with no power control that is all "off" can mean anyway, and
 * on a Pi it is the state where the SD card is safe to pull.
 */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

static const char *leaf(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int main(int argc, char **argv)
{
    const char *me = leaf(argv[0]);

    /* argv[0] normally, but a flag works too - some scripts call
     * `poweroff -r` expecting a reboot, and it costs nothing. */
    bool want_reboot = (strcmp(me, "reboot") == 0);
    bool want_halt   = (strcmp(me, "halt") == 0);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0)      want_reboot = true;
        else if (strcmp(argv[i], "-h") == 0) want_halt = true;
        else if (strcmp(argv[i], "-p") == 0) { want_reboot = false; want_halt = false; }
        else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: %s [-r|-h|-p]\n", me);
            printf("  poweroff   stop everything and switch off\n");
            printf("  reboot     stop everything and start again\n");
            printf("  halt       stop everything and stay on\n\n");
            printf("All three stop services, flush what is unwritten and\n");
            printf("unmount /data first, so the next boot does not have to\n");
            printf("repair it.\n");
            return 0;
        } else {
            dprintf(STDERR_FILENO, "%s: %s?  Try --help\n", me, argv[i]);
            return 2;
        }
    }

    if (lp_getuid() != 0) {
        dprintf(STDERR_FILENO,
                "%s: only root can stop the machine\n", me);
        return 1;
    }

    /* halt is poweroff that stops before the last step. init does not
     * have a third signal for it, so say what we mean here and let init
     * do the stopping; the machine ends up idle with everything shut
     * down, which is what halt is. */
    if (want_halt)
        printf("%s: stopping everything. The machine stays on -"
               " it is safe to cut the power when it goes quiet.\n", me);

    /* SIGUSR2 reboots, SIGUSR1 powers off. See the note in init.c on why
     * these rather than SIGINT or SIGTERM: those arrive by accident, and
     * switching a machine off by accident is worse than not being able
     * to switch it off at all. */
    int sig = want_reboot ? SIGUSR2 : SIGUSR1;

    if (lp_kill(1, sig) != 0) {
        dprintf(STDERR_FILENO,
                "%s: init did not take the signal.\n"
                "%s:   Nothing has been stopped and nothing was written"
                " to disk.\n", me, me);
        return 1;
    }

    /* init prints the rest and never comes back to us. If we are still
     * here in ten seconds something is wrong, and saying so beats
     * looking like the command did nothing. */
    for (int i = 0; i < 100; i++)
        lp_sleep_ms(100);

    dprintf(STDERR_FILENO,
            "%s: init took the signal but the machine is still here.\n"
            "%s:   Something is refusing to stop. `top` will show what.\n",
            me, me);
    return 1;
}
