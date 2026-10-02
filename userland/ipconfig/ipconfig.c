/* ipconfig - a fixed address, from a file on the boot partition.
 *
 *   ipconfig <file>      apply the settings in it
 *   ipconfig -s <file>   show what is in it, change nothing
 *
 * The file, which is optional - without it the machine uses DHCP:
 *
 *     # /boot/network.conf
 *     interface  wlan0
 *     address    192.168.0.50
 *     netmask    255.255.255.0
 *     gateway    192.168.0.1
 *     nameserver 192.168.0.1
 *
 * ── Why a fixed address is worth having ──
 * A DHCP lease is a loan. The router hands out an address and expects
 * to be asked again before it expires; a board that is not asked hands
 * the address to something else. dhcp -d renews it now, but a fixed
 * address does not need renewing at all, and on a network with no DHCP
 * server there is nothing to ask.
 *
 * It also matters for finding the machine. Something reached only over
 * SSH is reached by address, and an address that changes is a board you
 * have to go and look for.
 *
 * The file lives on the boot partition for the same reason the SSH key
 * and the WiFi settings do: it is FAT32, so any computer can write it,
 * and setting the machine up needs no Linux box.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "net.h"

typedef struct {
    char iface[32];
    u32  address;
    u32  netmask;
    u32  gateway;
    u32  nameserver;
    bool have_address;
} config_t;

/* One "key value" line. Blank lines and # comments are skipped. */
static bool parse_line(const char *line, config_t *c)
{
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#')
        return true;

    char key[32];
    size_t n = 0;
    while (*line && *line != ' ' && *line != '\t' && n < sizeof(key) - 1)
        key[n++] = *line++;
    key[n] = '\0';

    while (*line == ' ' || *line == '\t') line++;
    if (!*line)
        return true;                 /* a key with no value: ignore */

    char value[64];
    n = 0;
    while (*line && *line != ' ' && *line != '\t' && *line != '#' &&
           n < sizeof(value) - 1)
        value[n++] = *line++;
    value[n] = '\0';

    if (strcmp(key, "interface") == 0) {
        strlcpy(c->iface, value, sizeof(c->iface));
        return true;
    }

    u32 addr = 0;
    if (!ipv4_parse(value, &addr)) {
        dprintf(STDERR_FILENO, "ipconfig: %s: not an address: %s\n",
                key, value);
        return false;
    }

    if (strcmp(key, "address") == 0)     { c->address = addr;
                                           c->have_address = true; }
    else if (strcmp(key, "netmask") == 0)    c->netmask = addr;
    else if (strcmp(key, "gateway") == 0)    c->gateway = addr;
    else if (strcmp(key, "nameserver") == 0) c->nameserver = addr;
    else {
        dprintf(STDERR_FILENO, "ipconfig: unknown setting: %s\n", key);
        return false;
    }
    return true;
}

static bool read_config(const char *path, config_t *c)
{
    memset(c, 0, sizeof(*c));
    strlcpy(c->iface, "wlan0", sizeof(c->iface));

    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return false;                /* no file: DHCP, which is normal */

    char line[256];
    bool ok = true;
    while (readline((int)fd, line, sizeof(line)) >= 0)
        if (!parse_line(line, c))
            ok = false;

    lp_close((int)fd);
    return ok && c->have_address;
}

static void show(const config_t *c)
{
    char buf[16];

    printf("interface   %s\n", c->iface);
    ipv4_format(c->address, buf);    printf("address     %s\n", buf);
    ipv4_format(c->netmask, buf);    printf("netmask     %s\n", buf);
    if (c->gateway)    { ipv4_format(c->gateway, buf);
                         printf("gateway     %s\n", buf); }
    if (c->nameserver) { ipv4_format(c->nameserver, buf);
                         printf("nameserver  %s\n", buf); }
}

int main(int argc, char **argv)
{
    bool show_only = false;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) show_only = true;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: ipconfig [-s] <file>\n\n");
            printf("A fixed address, from a file like this:\n\n");
            printf("  interface  wlan0\n");
            printf("  address    192.168.0.50\n");
            printf("  netmask    255.255.255.0\n");
            printf("  gateway    192.168.0.1\n");
            printf("  nameserver 192.168.0.1\n\n");
            printf("Put it on the boot partition as network.conf and the\n");
            printf("machine uses it instead of DHCP. Without it, DHCP.\n");
            return 0;
        }
        else if (!path) path = argv[i];
    }

    if (!path) {
        dprintf(STDERR_FILENO, "usage: ipconfig [-s] <file>\n");
        return 2;
    }

    config_t c;
    if (!read_config(path, &c))
        return 1;                    /* no file, or no address in it */

    if (show_only) {
        show(&c);
        return 0;
    }

    /* A netmask nobody gave us: /24, which is what a home network is. */
    if (!c.netmask)
        ipv4_parse("255.255.255.0", &c.netmask);

    if (net_if_up(c.iface) < 0)
        dprintf(STDERR_FILENO,
                "ipconfig: could not bring %s up (trying anyway)\n", c.iface);

    long r = net_set_addr(c.iface, c.address);
    if (r < 0) {
        dprintf(STDERR_FILENO, "ipconfig: %s: cannot set the address (%ld)\n",
                c.iface, -r);
        return 1;
    }
    net_set_netmask(c.iface, c.netmask);

    if (c.gateway) {
        r = net_add_default_route(c.iface, c.gateway);
        /* EEXIST means a default route is already there, which is fine. */
        if (r < 0 && -r != 17)
            dprintf(STDERR_FILENO,
                    "ipconfig: could not add the default route (%ld)\n", -r);
    }

    if (c.nameserver) {
        long fd = lp_open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            char buf[16];
            ipv4_format(c.nameserver, buf);
            dprintf((int)fd, "nameserver %s\n", buf);
            lp_close((int)fd);
        }
    }

    printf("ipconfig: %s configured from %s\n", c.iface, path);
    show(&c);
    return 0;
}
