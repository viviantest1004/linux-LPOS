/* lp-power - the one thing a desktop session is allowed to do as root.
 *
 * The session runs as uid 1000, because a compositor holds every
 * keystroke on the machine and must not be root. That is right, and it
 * has one consequence nobody expects: the person sitting at the machine
 * cannot switch it off. `poweroff` refuses a non-root caller, and it is
 * right to - it signals init, and being able to signal init is being
 * able to stop the machine.
 *
 * Elsewhere this is solved with logind and polkit: the session is
 * registered on a seat, polkit is asked whether that seat may shut down,
 * and systemd does the rest. This machine has neither, and adding both
 * to gain one button is not a trade worth making.
 *
 * So: this program, setuid root, and nothing else in the image is.
 *
 * ── What makes a setuid program safe is what it refuses to do ──
 *
 * It takes exactly one word and that word must be one of three. It
 * reads no configuration, opens no file, uses no environment variable,
 * and execs nothing - so there is no PATH to poison, no library to
 * preload into a child, and no filename to traverse. What it does is
 * kill(1, SIGUSR1) or kill(1, SIGUSR2), which is the same signal
 * /bin/poweroff sends, and then init does the actual work: stop the
 * services, sync twice, unmount /data, cut the power.
 *
 * ── Who may run it ──
 *
 * uid 0, and any uid at or above 1000 - which is to say, a person with
 * a real account rather than a service. That is the same rule Ubuntu
 * and Fedora arrive at through polkit: whoever is sitting at the
 * machine may switch it off. It is not a security boundary and does not
 * pretend to be one; somebody who can log in can also hold the power
 * button. The rule exists so that a compromised daemon running as
 * uid 103 cannot reboot the machine in a loop.
 */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        dprintf(STDERR_FILENO,
                "usage: lp-power off|restart|halt\n"
                "\n"
                "  off       stop everything and switch the machine off\n"
                "  restart   stop everything and start again\n"
                "  halt      stop everything and stay powered\n"
                "\n"
                "This exists so the desktop session, which runs as an\n"
                "ordinary account, can do those three things. It is the\n"
                "only setuid program on this machine.\n");
        return 2;
    }

    /* The real uid, not the effective one - the effective uid is root
     * for everybody here, which is the whole point of the file mode. */
    long uid = lp_getuid();
    if (uid != 0 && uid < 1000) {
        dprintf(STDERR_FILENO,
                "lp-power: uid %ld is a service account, not a person.\n"
                "lp-power:   Only an account at 1000 or above, or root,\n"
                "lp-power:   can stop this machine.\n", uid);
        return 1;
    }

    int sig;
    if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "halt") == 0)
        sig = SIGUSR1;
    else if (strcmp(argv[1], "restart") == 0)
        sig = SIGUSR2;
    else {
        dprintf(STDERR_FILENO,
                "lp-power: %s?  Only off, restart and halt.\n", argv[1]);
        return 2;
    }

    if (lp_kill(1, sig) != 0) {
        dprintf(STDERR_FILENO,
                "lp-power: init did not take the signal.\n"
                "lp-power:   Nothing has been stopped and nothing was\n"
                "lp-power:   written to disk.\n");
        return 1;
    }

    /* init prints the rest and never gives us the machine back. Ten
     * seconds of waiting and then saying so beats exiting 0 and looking
     * as if the button did nothing. */
    for (int i = 0; i < 100; i++)
        lp_sleep_ms(100);

    dprintf(STDERR_FILENO,
            "lp-power: init took the signal but the machine is still here.\n"
            "lp-power:   Something is refusing to stop.\n");
    return 1;
}
