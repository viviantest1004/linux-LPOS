/* nohup - keep running after the terminal goes away.
 *
 *   nohup ./server &
 *
 * When an SSH session ends the kernel sends SIGHUP to everything in it.
 * This ignores that signal and, if output would go to the terminal,
 * sends it to nohup.out instead - because a process writing to a
 * terminal that no longer exists dies of SIGPIPE a moment later.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define SIGHUP 1

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: nohup COMMAND [ARG]...\n"
               "Run COMMAND, ignoring hangup signals.\n\n"
               "If standard output is a terminal, append output to 'nohup.out'.\n"
               "      --help     display this help and exit\n");
        return 0;
    }
    if (argc < 2) {
        dprintf(STDERR_FILENO, "nohup: missing operand\n");
        dprintf(STDERR_FILENO, "Try 'nohup --help' for more information.\n");
        return 125;
    }

    lp_signal_ignore(SIGHUP);

    if (lp_isatty(STDOUT_FILENO)) {
        const char *out = "nohup.out";
        long fd = lp_open(out, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd < 0) {
            const char *home = getenv("HOME");
            char alt[1024];
            if (home) {
                snprintf(alt, sizeof alt, "%s/nohup.out", home);
                fd = lp_open(alt, O_WRONLY | O_CREAT | O_APPEND, 0600);
                if (fd >= 0) out = alt;
            }
        }
        if (fd < 0) {
            dprintf(STDERR_FILENO, "nohup: failed to open 'nohup.out'\n");
            return 125;
        }
        dprintf(STDERR_FILENO, "nohup: ignoring input and appending output to '%s'\n", out);
        lp_dup2((int)fd, STDOUT_FILENO);
        if (lp_isatty(STDERR_FILENO)) lp_dup2((int)fd, STDERR_FILENO);
        if ((int)fd > 2) lp_close((int)fd);
    }

    lp_execve(argv[1], &argv[1], environ);
    if (!strchr(argv[1], '/')) {
        const char *path = getenv("PATH");
        if (!path) path = "/bin:/sbin:/usr/bin:/usr/sbin";
        char full[1024];
        const char *p = path;
        while (*p) {
            const char *e = strchr(p, ':');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            if (n && n < sizeof full - 2) {
                memcpy(full, p, n);
                full[n] = '/';
                strlcpy(full + n + 1, argv[1], sizeof full - n - 1);
                lp_execve(full, &argv[1], environ);
            }
            if (!e) break;
            p = e + 1;
        }
    }
    dprintf(STDERR_FILENO, "nohup: failed to run command '%s': "
                           "No such file or directory\n", argv[1]);
    return 127;
}
