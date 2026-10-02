/* nice - run something at a lower priority.
 *
 *   nice -n 19 ./backup
 *
 * A long job that must not slow down the thing people are waiting for.
 * Positive numbers are politer; only root may ask for a negative one.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    int adj = 10;
    int i = 1;

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) {
            printf("Usage: nice [OPTION] [COMMAND [ARG]...]\n"
                   "Run COMMAND with an adjusted niceness, which affects scheduling.\n"
                   "With no COMMAND, print the current niceness.\n\n"
                   "  -n, --adjustment=N   add integer N to the niceness (default 10)\n"
                   "      --help     display this help and exit\n");
            return 0;
        }
        if (strncmp(a, "--adjustment=", 13) == 0) { adj = atoi(a + 13); continue; }
        if (strcmp(a, "-n") == 0 && i + 1 < argc) { adj = atoi(argv[++i]); continue; }
        if (a[0] == '-' && a[1] == 'n' && a[2]) { adj = atoi(a + 2); continue; }
        /* `nice -5 cmd` is the old spelling and still in scripts. */
        if (a[0] == '-' && (a[1] == '-' || (a[1] >= '0' && a[1] <= '9'))) {
            if (a[1] == '-' && !a[2]) { i++; break; }
            adj = atoi(a + 1);
            continue;
        }
        break;
    }

    if (i >= argc) {
        printf("%d\n", lp_getpriority(lp_getpid()));
        return 0;
    }

    int now = lp_getpriority(lp_getpid());
    if (lp_setpriority(lp_getpid(), now + adj) < 0)
        dprintf(STDERR_FILENO, "nice: cannot set niceness: Permission denied\n");

    lp_execve(argv[i], &argv[i], environ);
    if (!strchr(argv[i], '/')) {
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
                strlcpy(full + n + 1, argv[i], sizeof full - n - 1);
                lp_execve(full, &argv[i], environ);
            }
            if (!e) break;
            p = e + 1;
        }
    }
    dprintf(STDERR_FILENO, "nice: '%s': No such file or directory\n", argv[i]);
    return 127;
}
