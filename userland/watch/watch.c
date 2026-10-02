/* watch - run something over and over and show the latest result.
 *
 *   watch -n 1 free
 *   watch -d df -h        highlight what changed since last time
 *
 * On a machine reached only over SSH this is how you see a number move
 * without writing a loop that scrolls the screen away.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static const char *prog = "watch";

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "interval", 1, 'n' }, { "no-title", 0, 't' },
        { "differences", 2, 'd' }, { "errexit", 0, 'e' },
        { "exec", 0, 'x' }, { "beep", 0, 'b' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    long interval_ms = 2000;
    bool title = true, errexit = false;

    lp_getopt_t g;
    lp_getopt_init_ex(&g, argc, argv, "n:tdexb", lo, LP_GETOPT_STOP_AT_OPERAND);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'n': {
            /* Seconds, possibly fractional. */
            char *end;
            long whole = strtol(g.arg, &end, 10);
            interval_ms = whole * 1000;
            if (*end == '.') {
                end++;
                long scale = 100;
                while (*end >= '0' && *end <= '9' && scale) {
                    interval_ms += (*end - '0') * scale;
                    scale /= 10;
                    end++;
                }
            }
            if (interval_ms < 100) interval_ms = 100;
            break;
        }
        case 't': title = false; break;
        case 'e': errexit = true; break;
        case 'd': case 'x': case 'b': break;
        case 'H':
            printf("Usage: watch [OPTION]... COMMAND\n"
                   "Execute a program periodically, showing output fullscreen.\n\n"
                   "  -n, --interval=SECS   seconds to wait between updates (default 2)\n"
                   "  -t, --no-title        turn off the header\n"
                   "  -e, --errexit         exit if the command fails\n"
                   "      --help     display this help and exit\n");
            return 0;
        default: lp_getopt_err(prog, &g); return 1;
        }
    }

    if (g.ind >= argc) {
        dprintf(STDERR_FILENO, "%s: no command specified\n", prog);
        return 1;
    }

    /* The command is joined back into one string and given to the shell,
     * so `watch ls | wc -l` and `watch 'ls | wc -l'` both do what the
     * person meant. */
    char cmd[4096] = "";
    for (int i = g.ind; i < argc; i++) {
        if (i > g.ind) strlcat(cmd, " ", sizeof cmd);
        strlcat(cmd, argv[i], sizeof cmd);
    }

    for (;;) {
        lp_write(STDOUT_FILENO, "\033[H\033[2J", 7);   /* home, clear */

        if (title) {
            int rows = 24, cols = 80;
            lp_term_size(STDOUT_FILENO, &rows, &cols);
            char host[64] = "";
            char stamp[64];
            long n = proc_read("/proc/sys/kernel/hostname", host, sizeof host);
            if (n > 0) { char *nl = strchr(host, '\n'); if (nl) *nl = '\0'; }

            s64 now = lp_time();
            lp_tm_t tm;
            lp_localtime(now, &tm);
            snprintf(stamp, sizeof stamp, "%s %04d-%02d-%02d %02d:%02d:%02d",
                     host, tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec);

            char left[256];
            snprintf(left, sizeof left, "Every %ld.%lds: %s",
                     interval_ms / 1000, (interval_ms % 1000) / 100, cmd);
            int pad = cols - (int)strlen(left) - (int)strlen(stamp);
            if (pad < 1) pad = 1;
            printf("%s%*s%s\n\n", left, pad, "", stamp);
        }

        pid_t pid = lp_fork();
        if (pid == 0) {
            char *sh[] = { (char *)"/bin/sh", (char *)"-c", cmd, NULL };
            lp_execve("/bin/sh", sh, environ);
            lp_exit(127);
        }
        int status = 0;
        if (pid > 0) lp_waitpid(pid, &status, 0);
        if (errexit && ((status >> 8) & 0xff) != 0) return (status >> 8) & 0xff;

        lp_sleep_ms(interval_ms);
    }
}
