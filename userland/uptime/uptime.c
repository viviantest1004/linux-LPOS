/* uptime - how long it has been running, and how busy.
 *
 * The load average is the number of processes wanting to run, averaged
 * over one, five and fifteen minutes. On four cores, 4.00 means every
 * core is busy and nothing is waiting; above that, things are queueing.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-h") == 0) {
        printf("usage: uptime\n");
        return 0;
    }

    char buf[256];
    long up = 0;
    if (proc_read("/proc/uptime", buf, sizeof(buf)) > 0)
        up = strtol(buf, NULL, 10);

    long days  = up / 86400;
    long hours = (up % 86400) / 3600;
    long mins  = (up % 3600) / 60;
    long secs  = up % 60;

    printf("up ");
    if (days)  printf("%ldd ", days);
    if (days || hours) printf("%ldh ", hours);
    printf("%ldm %lds", mins, secs);

    char load[128];
    if (proc_read("/proc/loadavg", load, sizeof(load)) > 0) {
        char *nl = strchr(load, '\n');
        if (nl) *nl = '\0';
        /* The first three numbers; the rest is process counts. */
        int spaces = 0;
        for (char *p = load; *p; p++) {
            if (*p == ' ' && ++spaces == 3) { *p = '\0'; break; }
        }
        printf(",  load %s", load);
    }
    printf("\n");
    return 0;
}
