/* watch - run something over and over and keep the screen on the answer.
 *
 *   watch [-n seconds] <command> [args...]
 *
 * The screen is redrawn each time rather than scrolled, so what changes
 * is what you notice. Ctrl-C stops it.
 *
 * Built with the SDK against this system's own libc.
 */
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    long every = 2;
    int  first = 1;

    for (; first < argc; first++) {
        if (strcmp(argv[first], "-n") == 0 && first + 1 < argc) {
            every = strtol(argv[++first], 0, 10);
            if (every < 1) every = 1;
        } else if (strcmp(argv[first], "-h") == 0) {
            printf("usage: watch [-n seconds] <command> [args...]\n");
            printf("  -n  how often, in seconds (2 by default, 1 minimum)\n\n");
            printf("  watch -n 5 usage\n");
            printf("  watch service\n");
            return 0;
        } else break;
    }

    if (first >= argc) {
        dprintf(STDERR_FILENO, "usage: watch [-n seconds] <command> [args...]\n");
        return 2;
    }

    /* Resolve it once. Doing it every round would mean a command that
     * disappears halfway through is reported as a different error each
     * time rather than the same one. */
    char path[256];
    if (argv[first][0] == '/') {
        strlcpy(path, argv[first], sizeof path);
    } else {
        snprintf(path, sizeof path, "/bin/%s", argv[first]);
        if (!lp_exists(path))
            snprintf(path, sizeof path, "/data/bin/%s", argv[first]);
    }
    if (!lp_exists(path)) {
        dprintf(STDERR_FILENO, "watch: %s: not found\n", argv[first]);
        return 127;
    }

    for (;;) {
        /* Clear and go home. Redrawing over the old output rather than
         * scrolling is the whole point - a number that changed is in
         * the same place it was. */
        printf("\033[H\033[2J");
        printf("every %lds:", every);
        for (int i = first; i < argc; i++)
            printf(" %s", argv[i]);
        printf("\n\n");

        pid_t pid = lp_fork();
        if (pid == 0) {
            lp_signal_default(SIGINT);
            lp_execve(path, argv + first, environ);
            lp_exit(127);
        }
        int status = 0;
        lp_waitpid(pid, &status, 0);

        lp_sleep_ms(every * 1000);
    }
}
