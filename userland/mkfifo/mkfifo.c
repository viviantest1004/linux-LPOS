/* mkfifo - a named pipe.
 *
 * Two programs that were never written to know about each other can be
 * joined through one, without either of them being started by a shell
 * that could have used a | instead.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "mode", 1, 'm' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    mode_t mode = 0666;
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "m:", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'm': mode = (mode_t)strtol(g.arg, NULL, 8); break;
        case 'H':
            printf("Usage: mkfifo [OPTION]... NAME...\n"
                   "Create named pipes (FIFOs) with the given NAMEs.\n\n"
                   "  -m, --mode=MODE    set file permission bits to MODE, not a=rw - umask\n"
                   "      --help     display this help and exit\n");
            return 0;
        default: lp_getopt_err("mkfifo", &g); return 1;
        }
    }
    if (g.ind >= argc) {
        dprintf(STDERR_FILENO, "mkfifo: missing operand\n");
        dprintf(STDERR_FILENO, "Try 'mkfifo --help' for more information.\n");
        return 1;
    }
    int rc = 0;
    for (int i = g.ind; i < argc; i++) {
        long r = lp_mknod(argv[i], LP_S_IFIFO_MODE | mode, 0);
        if (r < 0) {
            dprintf(STDERR_FILENO, "mkfifo: cannot create fifo '%s': %s\n",
                    argv[i], lp_strerror((int)-r));
            rc = 1;
        }
    }
    return rc;
}
