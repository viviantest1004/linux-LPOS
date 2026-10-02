/* route - where packets go that are not for this network.
 *
 *   route                                  the table
 *   route add default gw <address> [dev <if>]
 *
 * /proc/net/route holds it, in hexadecimal and little-endian, which is
 * why the numbers there look nothing like addresses until they are
 * turned back the right way round.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

/* The fields are 8 hex digits of a little-endian u32 - the same bytes an
 * address already is in network order, so no swapping is needed once it
 * is parsed. */
static u32 hex_addr(const char *s)
{
    return (u32)strtol(s, NULL, 16);
}

static int show_table(void)
{
    long fd = lp_open("/proc/net/route", O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "route: cannot read /proc/net/route\n");
        return 1;
    }

    printf("%-16s %-16s %-16s %s\n",
           "destination", "gateway", "netmask", "interface");

    char line[512];
    int  lineno = 0;

    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (++lineno == 1)
            continue;                      /* the header */

        /* Iface Destination Gateway Flags RefCnt Use Metric Mask ... */
        char *fields[9];
        int   n = 0;
        char *p = line;

        while (*p && n < 9) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            fields[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
        if (n < 8)
            continue;

        char dst[16], gw[16], mask[16];
        u32  d = hex_addr(fields[1]);
        ipv4_format(d, dst);
        ipv4_format(hex_addr(fields[2]), gw);
        ipv4_format(hex_addr(fields[7]), mask);

        printf("%-16s %-16s %-16s %s\n",
               d == 0 ? "default" : dst, gw, mask, fields[0]);
    }

    lp_close((int)fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return show_table();

    if (strcmp(argv[1], "-h") == 0) {
        printf("usage:\n");
        printf("  route                                  the table\n");
        printf("  route add default gw <address> [dev <if>]\n");
        return 0;
    }

    if (argc >= 5 && strcmp(argv[1], "add") == 0 &&
        strcmp(argv[2], "default") == 0 && strcmp(argv[3], "gw") == 0) {

        u32 gw = 0;
        if (!ipv4_parse(argv[4], &gw)) {
            dprintf(STDERR_FILENO, "route: %s is not an address\n", argv[4]);
            return 2;
        }

        const char *dev = "wlan0";
        if (argc >= 7 && strcmp(argv[5], "dev") == 0)
            dev = argv[6];

        long r = net_add_default_route(dev, gw);
        if (r < 0) {
            dprintf(STDERR_FILENO,
                    "route: cannot add it (%ld)%s\n", -r,
                    -r == 17 ? " - there is already a default route" : "");
            return 1;
        }
        printf("route: default via %s on %s\n", argv[4], dev);
        return show_table();
    }

    dprintf(STDERR_FILENO, "route: try 'route -h'\n");
    return 2;
}
