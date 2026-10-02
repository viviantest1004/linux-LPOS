/* arch - the machine name, the one the kernel gives.
 *
 * This prints exactly what `uname -m` prints, and nothing else. It has
 * its own command because that is what configure scripts, shell
 * profiles and package installers call, and a machine without it is a
 * machine where those stop at their first line.
 *
 * The answer comes from the uname syscall, so it is the kernel's word
 * for the machine and not the userland's: "x86_64" on a PC, "aarch64"
 * on the Pi. That distinction matters when it comes to picking which
 * binary to download - a 32-bit userland on a 64-bit kernel still reads
 * "aarch64" here, because that is what the kernel will run.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

/* struct utsname is six fixed 65-byte fields; the machine is the fifth. */
#define FIELD    65
#define MACHINE  (4 * FIELD)

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = { { "help", 0, 'H' }, { 0, 0, 0 } };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "", lo);

    for (int c; (c = lp_getopt(&g)) != -1; ) {
        if (c == 'H') {
            printf("Usage: arch [OPTION]...\n"
                   "Print machine architecture.\n\n"
                   "      --help     display this help and exit\n");
            return 0;
        }
        lp_getopt_err("arch", &g);
        return 1;
    }

    if (g.ind < argc) {
        dprintf(STDERR_FILENO, "arch: extra operand '%s'\n", argv[g.ind]);
        dprintf(STDERR_FILENO, "Try 'arch --help' for more information.\n");
        return 1;
    }

    char uts[FIELD * 6];
    memset(uts, 0, sizeof uts);
    if (lp_uname(uts) < 0) {
        dprintf(STDERR_FILENO, "arch: cannot get system name\n");
        return 1;
    }
    printf("%s\n", uts + MACHINE);
    return 0;
}
