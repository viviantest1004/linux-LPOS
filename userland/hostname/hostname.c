/* hostname - what this machine calls itself.
 *
 *   hostname            print it
 *   hostname <name>     set it, until the next reboot
 *
 * /etc/rc sets it at every boot, so to change it for good, change it
 * there. The name shows up in the shell prompt and in the log.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define SYSCTL "/proc/sys/kernel/hostname"

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-h") == 0) {
        printf("usage: hostname [name]\n");
        printf("  set it for good in /etc/rc - this lasts until reboot\n");
        return 0;
    }

    if (argc == 1) {
        char buf[128];
        if (proc_read(SYSCTL, buf, sizeof(buf)) <= 0) {
            dprintf(STDERR_FILENO, "hostname: cannot read it\n");
            return 1;
        }
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
        printf("%s\n", buf);
        return 0;
    }

    long fd = lp_open(SYSCTL, O_WRONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "hostname: cannot set it (%ld)\n", -fd);
        return 1;
    }
    lp_write((int)fd, argv[1], strlen(argv[1]));
    lp_close((int)fd);
    return 0;
}
