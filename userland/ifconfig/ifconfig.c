/* ifconfig - look at the network interfaces, and set one up.
 *
 *   ifconfig                        every interface
 *   ifconfig <if>                   just that one
 *   ifconfig <if> up | down
 *   ifconfig <if> <address> [netmask <mask>]
 *
 * dhcp does all of this by itself when there is a router to ask. This is
 * for when there is not: a cable straight to another machine, a network
 * with no DHCP server, or working out why dhcp came back empty.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "net.h"

/* /proc/net/dev, whose first two lines are a header:
 *   iface: rx_bytes rx_packets ... tx_bytes tx_packets ... */
static void show_one(const char *name, const char *stats)
{
    printf("%s\n", name);

    u32 addr = 0;
    if (net_get_addr(name, &addr) == 0 && addr != 0) {
        char buf[16];
        ipv4_format(addr, buf);
        printf("    address  %s\n", buf);
    } else {
        printf("    address  none\n");
    }

    u8 mac[6];
    if (net_if_hwaddr(name, mac) == 0 &&
        (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]))
        printf("    hardware %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    printf("    state    %s\n", net_if_is_up(name) ? "up" : "down");

    if (stats) {
        /* rx bytes is field 1, tx bytes field 9. */
        const char *p = stats;
        long v[16];
        int  n = 0;
        while (*p && n < 16) {
            while (*p == ' ') p++;
            if (!*p) break;
            v[n++] = strtol(p, (char **)&p, 10);
        }
        if (n >= 9)
            printf("    traffic  %ld KB in, %ld KB out\n",
                   v[0] / 1024, v[8] / 1024);
    }
    printf("\n");
}

static void show_all(const char *only)
{
    long fd = lp_open("/proc/net/dev", O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "ifconfig: cannot read /proc/net/dev\n");
        return;
    }

    char line[512];
    int  lineno = 0;

    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (++lineno <= 2)
            continue;               /* two header lines */

        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = '\0';

        char *name = line;
        while (*name == ' ') name++;

        if (only && strcmp(name, only) != 0)
            continue;

        show_one(name, colon + 1);
    }
    lp_close((int)fd);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-h") == 0) {
        printf("usage:\n");
        printf("  ifconfig                     every interface\n");
        printf("  ifconfig <if>                one of them\n");
        printf("  ifconfig <if> up|down\n");
        printf("  ifconfig <if> <address> [netmask <mask>]\n\n");
        printf("dhcp does all of this when there is a router to ask.\n");
        return 0;
    }

    if (argc == 1) {
        show_all(NULL);
        return 0;
    }

    const char *iface = argv[1];

    if (argc == 2) {
        show_all(iface);
        return 0;
    }

    if (strcmp(argv[2], "up") == 0) {
        long r = net_if_up(iface);
        if (r < 0) {
            dprintf(STDERR_FILENO, "ifconfig: %s: cannot bring it up (%ld)\n",
                    iface, -r);
            return 1;
        }
        printf("ifconfig: %s is up\n", iface);
        return 0;
    }

    if (strcmp(argv[2], "down") == 0) {
        long r = net_if_down(iface);
        if (r < 0) {
            dprintf(STDERR_FILENO, "ifconfig: %s: cannot take it down (%ld)\n",
                    iface, -r);
            return 1;
        }
        printf("ifconfig: %s is down\n", iface);
        return 0;
    }

    /* An address, and optionally a netmask. */
    u32 addr = 0;
    if (!ipv4_parse(argv[2], &addr)) {
        dprintf(STDERR_FILENO, "ifconfig: %s is not an address\n", argv[2]);
        return 2;
    }

    /* The interface has to be up before an address will stick. */
    net_if_up(iface);

    long r = net_set_addr(iface, addr);
    if (r < 0) {
        dprintf(STDERR_FILENO, "ifconfig: %s: cannot set the address (%ld)\n",
                iface, -r);
        return 1;
    }

    u32 mask = 0;
    if (argc > 4 && strcmp(argv[3], "netmask") == 0) {
        if (!ipv4_parse(argv[4], &mask)) {
            dprintf(STDERR_FILENO, "ifconfig: %s is not a netmask\n", argv[4]);
            return 2;
        }
    } else {
        /* No mask given: /24, which is what a small network is. */
        ipv4_parse("255.255.255.0", &mask);
    }
    net_set_netmask(iface, mask);

    show_all(iface);
    return 0;
}
