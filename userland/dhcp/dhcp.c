/* dhcp - a minimal DHCP client (RFC 2131), and the thing that keeps
 * this machine on a network.
 *
 *   dhcp                  ask on whatever is plugged in, once
 *   dhcp <interface>      ask on that one, once
 *   dhcp -d               manage every interface, for ever (the service)
 *   dhcp -d <interface>   manage only that one, for ever
 *
 * Why write our own: DHCP is a plain UDP protocol with no cryptography.
 * Unlike SSH it is not dangerous to implement yourself, so this removes
 * one external dependency. Smaller than udhcpc or dhcpcd, and the part
 * that matters most here - noticing that a cable has been plugged in -
 * is ours to get right rather than somebody else's to configure.
 *
 * The exchange:
 *   DISCOVER (broadcast)  ->  OFFER  (the server proposes an address)
 *   REQUEST  (broadcast)  ->  ACK    (it is settled)
 * Once settled we set the address and netmask on the interface, add the
 * default route and write /etc/resolv.conf.
 *
 * Not handled: DECLINE, and the rebind half of the renewal state
 * machine. Both exist for networks with several DHCP servers, which
 * neither a home router nor a cable to one PC is.
 *
 * The long note further down, by run_daemon_all(), is about the other
 * half of this program: watching every interface at once, which is what
 * makes a cable pushed in five minutes after boot work by itself.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

#define DHCP_SERVER_PORT   67
#define DHCP_CLIENT_PORT   68
#define DHCP_MAGIC         0x63825363u

/* op */
#define BOOTREQUEST  1
#define BOOTREPLY    2

/* Option 53 values */
#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6

/* Option numbers */
#define OPT_SUBNET_MASK   1
#define OPT_ROUTER        3
#define OPT_DNS           6
#define OPT_REQUESTED_IP 50
#define OPT_LEASE_TIME   51
#define OPT_MSG_TYPE     53
#define OPT_SERVER_ID    54
#define OPT_PARAM_LIST   55
#define OPT_END         255

#define BOOTP_FIXED_LEN  236        /* fixed part, before the options */
#define PKT_SIZE         576        /* the RFC's minimum DHCP message size */

/* BOOTP/DHCP header. This is a wire layout, so it must be packed. */
typedef struct __attribute__((packed)) {
    u8  op, htype, hlen, hops;
    u32 xid;
    u16 secs, flags;
    u32 ciaddr, yiaddr, siaddr, giaddr;
    u8  chaddr[16];
    u8  sname[64];
    u8  file[128];
    u32 magic;
    u8  options[PKT_SIZE - BOOTP_FIXED_LEN - 4];
} dhcp_pkt_t;

typedef struct {
    u32 addr;       /* the address we got (network order) */
    u32 mask;
    u32 router;
    u32 dns;
    u32 server;
    u32 lease;      /* seconds (host order) */
} lease_t;

/* ── Building options ── */

static u8 *put_opt(u8 *p, u8 code, u8 len, const void *val)
{
    *p++ = code;
    *p++ = len;
    if (len) { memcpy(p, val, len); p += len; }
    return p;
}

/* ── Parsing options ──
 * Trusting the length field would let a crafted packet read past the
 * buffer. Always check it against the end. */
static bool parse_options(const u8 *opt, size_t len, lease_t *out, u8 *msg_type)
{
    const u8 *end = opt + len;
    *msg_type = 0;

    while (opt < end) {
        u8 code = *opt++;

        if (code == 0) continue;            /* padding */
        if (code == OPT_END) break;
        if (opt >= end) return false;       /* no length byte */

        u8 olen = *opt++;
        if (opt + olen > end) return false; /* the value runs past the buffer */

        switch (code) {
        case OPT_MSG_TYPE:
            if (olen >= 1) *msg_type = opt[0];
            break;
        case OPT_SUBNET_MASK:
            if (olen >= 4) memcpy(&out->mask, opt, 4);
            break;
        case OPT_ROUTER:
            if (olen >= 4) memcpy(&out->router, opt, 4);
            break;
        case OPT_DNS:
            if (olen >= 4) memcpy(&out->dns, opt, 4);
            break;
        case OPT_SERVER_ID:
            if (olen >= 4) memcpy(&out->server, opt, 4);
            break;
        case OPT_LEASE_TIME:
            if (olen >= 4) {
                u32 v; memcpy(&v, opt, 4);
                out->lease = ntohl(v);
            }
            break;
        default:
            break;
        }
        opt += olen;
    }
    return true;
}

/* ── Building and sending packets ── */

static size_t build_packet(dhcp_pkt_t *p, u8 type, u32 xid, const u8 mac[6],
                           u32 req_addr, u32 server)
{
    memset(p, 0, sizeof(*p));
    p->op    = BOOTREQUEST;
    p->htype = 1;               /* Ethernet */
    p->hlen  = 6;
    p->xid   = xid;
    p->flags = htons(0x8000);   /* ask for a broadcast reply */
    memcpy(p->chaddr, mac, 6);
    p->magic = htonl(DHCP_MAGIC);

    u8 *o = p->options;
    o = put_opt(o, OPT_MSG_TYPE, 1, &type);

    if (req_addr) o = put_opt(o, OPT_REQUESTED_IP, 4, &req_addr);
    if (server)   o = put_opt(o, OPT_SERVER_ID,    4, &server);

    /* What we are asking the server for */
    static const u8 want[] = { OPT_SUBNET_MASK, OPT_ROUTER, OPT_DNS };
    o = put_opt(o, OPT_PARAM_LIST, sizeof(want), want);

    *o++ = OPT_END;

    return (size_t)(o - (u8 *)p);
}

static long send_bcast(int fd, const void *buf, size_t len)
{
    sockaddr_in_t dst = {
        .sin_family = AF_INET,
        .sin_port   = htons(DHCP_SERVER_PORT),
        .sin_addr   = 0xFFFFFFFFu,          /* 255.255.255.255 */
    };
    return lp_sendto(fd, buf, len, 0, &dst, sizeof(dst));
}

/* Wait for a reply of the wanted type. The timeout is a socket option. */
static bool wait_reply(int fd, u32 xid, u8 want_type, lease_t *out)
{
    dhcp_pkt_t pkt;

    for (int tries = 0; tries < 8; tries++) {
        long n = lp_recvfrom(fd, &pkt, sizeof(pkt), 0, NULL, NULL);
        if (n < 0)
            return false;                    /* timed out, or an error */
        if ((size_t)n < BOOTP_FIXED_LEN + 4)
            continue;                        /* too short */
        if (pkt.op != BOOTREPLY || pkt.xid != xid)
            continue;                        /* not a reply to our request */
        if (ntohl(pkt.magic) != DHCP_MAGIC)
            continue;

        u8 type = 0;
        size_t optlen = (size_t)n - BOOTP_FIXED_LEN - 4;
        if (!parse_options(pkt.options, optlen, out, &type))
            continue;

        if (type == DHCPNAK)
            return false;
        if (type != want_type)
            continue;

        out->addr = pkt.yiaddr;
        if (!out->server)
            out->server = pkt.siaddr;
        return true;
    }
    return false;
}

static void write_resolv_conf(u32 dns_be)
{
    if (!dns_be)
        return;

    char ip[16];
    ipv4_format(dns_be, ip);

    long fd = lp_open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "dhcp: cannot write /etc/resolv.conf (%ld)\n", -fd);
        return;
    }
    dprintf((int)fd, "nameserver %s\n", ip);
    lp_close((int)fd);
}

/* Put a lease on the wire.
 *
 * `route_held_by` is the interface that already owns the default route
 * and /etc/resolv.conf, or NULL when this one is to take them.
 *
 * There is one default route and one resolv.conf on this machine, and
 * two interfaces that both got a lease cannot both be right about where
 * "everything else" is. The rule is first come, first served: whoever
 * got an address first keeps the route until its link goes away. A
 * second interface still gets its address and netmask, so it is
 * reachable on its own network - which is the whole point of a cable
 * from a laptop to a board whose wireless is already working - it just
 * does not get to redirect the traffic of the one that was there first.
 *
 * Two exceptions to "first come". A cable beats a radio: a laptop on
 * Wi-Fi that is plugged into a USB-C Ethernet adapter goes out through
 * the cable, as every other desktop does (service() decides that). And
 * a route whose link has gone is taken back off the kernel - a carrier
 * lost does not remove it (it only reads "linkdown"), and the kernel
 * kept sending everything into the dead link: a laptop whose Wi-Fi had
 * dropped got an address on the adapter and still no internet, because
 * the new route was refused as a duplicate of the dead one (EEXIST,
 * which below was read as "already there and already right").
 * clear_stale_routes() removes those before the new one goes in. */
static bool has_carrier(const char *ifname);
static bool is_wireless(const char *ifname);

/* Take every default route through ifname off the kernel. */
static void drop_route(const char *ifname)
{
    for (int i = 0; i < 4 && net_del_default_route(ifname) == 0; i++)
        ;
}

/* Before `ifname` installs the default route: remove the ones through
 * other interfaces that cannot carry it any more (no carrier, or gone),
 * and - when `ifname` is a cable - the radio's. A live cable's route
 * that this program did not put there (a fixed address from
 * /boot/network.conf) is left alone. */
static void clear_stale_routes(const char *ifname)
{
    char tab[4096];
    if (proc_read("/proc/net/route", tab, sizeof tab) <= 0)
        return;
    bool wired = !is_wireless(ifname);
    char *line = strchr(tab, '\n');        /* past the header */
    while (line && *++line) {
        char *end = strchr(line, '\n');
        if (end)
            *end = '\0';
        char dev[IFNAMSIZ];
        size_t n = 0;
        while (line[n] && line[n] != '\t' && line[n] != ' ')
            n++;
        const char *rest = line + n;
        while (*rest == '\t' || *rest == ' ')
            rest++;
        /* Iface, then Destination; the mask is the eighth field, and a
         * destination of 0 with a gateway flag is the default route. */
        if (n > 0 && n < sizeof dev && strncmp(rest, "00000000", 8) == 0) {
            memcpy(dev, line, n);
            dev[n] = '\0';
            if (strcmp(dev, ifname) != 0 &&
                (!has_carrier(dev) || (wired && is_wireless(dev)))) {
                printf("dhcp: %s: taking the default route from %s\n", ifname, dev);
                drop_route(dev);
            }
        }
        line = end;
    }
}

static int apply_lease(const char *ifname, const lease_t *l,
                       const char *route_held_by)
{
    char a[16], m[16], g[16];
    ipv4_format(l->addr, a);
    ipv4_format(l->mask, m);
    ipv4_format(l->router, g);

    long rc = net_set_addr(ifname, l->addr);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "dhcp: cannot set the address (%ld)\n", -rc);
        return 1;
    }
    if (l->mask) {
        rc = net_set_netmask(ifname, l->mask);
        if (rc < 0)
            dprintf(STDERR_FILENO, "dhcp: cannot set the netmask (%ld)\n", -rc);
    }
    if (l->router && !route_held_by) {
        clear_stale_routes(ifname);
        rc = net_add_default_route(ifname, l->router);
        /* EEXIST is the normal answer on a renewal: the route is
         * already there and already right. */
        if (rc < 0 && -rc != 17)
            dprintf(STDERR_FILENO, "dhcp: cannot add the default route (%ld)\n", -rc);
    }

    if (!route_held_by)
        write_resolv_conf(l->dns);

    printf("dhcp: %s  address %s  netmask %s  gateway %s  lease %us\n",
           ifname, a, m, l->router ? g : "(none)", l->lease);
    if (route_held_by)
        printf("dhcp:   %s already has the default route, so %s reaches"
               " its own network only\n", route_held_by, ifname);
    return 0;
}

/* One full exchange: DISCOVER, OFFER, REQUEST, ACK. Returns 0 when an
 * address was applied.
 *
 * `known` is the address we already hold, or 0. A renewal names it in
 * the REQUEST, which is how a server knows to give the same one back -
 * and getting the same address back is the whole point: everything that
 * reaches this machine does so by the address the router handed out.
 *
 * `route_held_by` goes straight to apply_lease; see the note there.
 *
 * `verbose` is about the console, not about the protocol. Typing `dhcp
 * eth0` and watching it count "no reply (1/4)" is exactly what somebody
 * standing at the board wants. The daemon that has this same
 * conversation on every interface every time it backs off does not: it
 * says once that the cable is in and nothing is answering, and then
 * stays quiet until that changes. A true sentence repeated for ever is
 * indistinguishable from a fault. */
static int get_lease(const char *ifname, u32 known, lease_t *out,
                     const char *route_held_by, bool verbose)
{
    u8 mac[6];
    if (net_if_hwaddr(ifname, mac) < 0) {
        dprintf(STDERR_FILENO,
                "dhcp: cannot read the MAC address of %s\n", ifname);
        return 1;
    }

    if (net_if_up(ifname) < 0)
        dprintf(STDERR_FILENO,
                "dhcp: could not bring %s up (trying anyway)\n", ifname);

    long fd = lp_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "dhcp: cannot create a socket (%ld)\n", -fd);
        return 1;
    }

    int one = 1;
    lp_setsockopt((int)fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    lp_setsockopt((int)fd, SOL_SOCKET, SO_BINDTODEVICE, ifname,
                  (u32)strlen(ifname) + 1);

    s64 tv[2] = { 3, 0 };
    lp_setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof(tv));

    sockaddr_in_t me = {
        .sin_family = AF_INET,
        .sin_port   = htons(DHCP_CLIENT_PORT),
        .sin_addr   = 0,
    };
    if (lp_bind((int)fd, &me, sizeof(me)) < 0) {
        dprintf(STDERR_FILENO, "dhcp: cannot bind port %d\n", DHCP_CLIENT_PORT);
        lp_close((int)fd);
        return 1;
    }

    u32 xid = 0;
    if (lp_getrandom(&xid, sizeof(xid), 0) != (long)sizeof(xid) || xid == 0)
        xid = (u32)lp_getpid() * 2654435761u;   /* fallback */

    dhcp_pkt_t pkt;
    lease_t lease;

    for (int attempt = 1; attempt <= 4; attempt++) {
        memset(&lease, 0, sizeof(lease));

        size_t len = build_packet(&pkt, DHCPDISCOVER, xid, mac, known, 0);
        if (send_bcast((int)fd, &pkt, len) < 0) {
            dprintf(STDERR_FILENO, "dhcp: sending DISCOVER failed\n");
            lp_sleep_ms(1000);
            continue;
        }

        if (!wait_reply((int)fd, xid, DHCPOFFER, &lease)) {
            if (verbose)
                printf("dhcp: no reply (%d/4)\n", attempt);
            continue;
        }

        len = build_packet(&pkt, DHCPREQUEST, xid, mac, lease.addr, lease.server);
        if (send_bcast((int)fd, &pkt, len) < 0)
            continue;

        lease_t ack;
        memset(&ack, 0, sizeof(ack));
        if (!wait_reply((int)fd, xid, DHCPACK, &ack)) {
            if (verbose)
                printf("dhcp: no ACK (%d/4)\n", attempt);
            continue;
        }

        if (!ack.mask)   ack.mask   = lease.mask;
        if (!ack.router) ack.router = lease.router;
        if (!ack.dns)    ack.dns    = lease.dns;

        lp_close((int)fd);
        if (out)
            *out = ack;
        return apply_lease(ifname, &ack, route_held_by);
    }

    lp_close((int)fd);
    if (verbose)
        dprintf(STDERR_FILENO, "dhcp: got no address on %s\n", ifname);
    return 1;
}

/* ── Staying on the network ───────────────────────────────────────────
 *
 * A lease is a loan with a deadline. A home router usually lends an
 * address for a day or a week, and expects to be asked again before
 * then. Ask, and it hands back the same address; do not ask, and it
 * takes the address back and eventually gives it to something else.
 *
 * A board that only asks once at boot therefore works perfectly for
 * days and then, silently, is not on the network any more. Nothing here
 * notices: the machine has not crashed, so the watchdog is content;
 * memory and temperature are fine, so guard is content. It is the one
 * failure with no automatic recovery at all - and this is a system whose
 * entire purpose is being left alone for months.
 *
 * The protocol says renew at half the lease and rebind at seven eighths.
 * We do the first: at half time, ask again. That is the part that keeps
 * the address, and the rest of the state machine exists for networks
 * with several servers, which a home router is not.
 *
 * If the renewal fails we retry, backing off, and keep the address in
 * the meantime - it usually still works, because the router has not
 * handed it to anyone else yet. Giving it up early would guarantee the
 * outage we are trying to avoid.
 */

/* How long to sleep between looks at the interfaces.
 *
 * This was 60 seconds, and 60 seconds is the wrong number for the thing
 * it is now measuring. It used to be "how long before we try DHCP again
 * on a board that is not on any network", where a minute is nothing.
 * Now it is "how long after somebody pushes a cable in before the board
 * does something about it", and a person standing over a board with a
 * cable in their hand gives up long before a minute is out. Two seconds
 * costs one small sysfs read per interface, which is not measurable
 * even on an armv6 board.
 *
 * It is not the only way an interface is noticed - see run_daemon_all -
 * but it is the one that always works. */
#define TICK_MS         2000

/* How long to wait before asking again on an interface that is plugged
 * in and got no answer. It doubles up to the ceiling, because the two
 * cases behind "no answer" want opposite things: a DHCP server that is
 * still booting wants to be asked again in a moment, and a switch with
 * no DHCP server on it at all wants to be left alone. A DISCOVER round
 * takes about twelve seconds of this process's attention, so asking a
 * dead network every few seconds for ever would be most of what the
 * daemon does. Carrier coming back resets it to zero. */
#define RETRY_MIN_MS    5000
#define RETRY_MAX_MS    60000

/* How many interfaces we will keep track of. A Pi has one or two; the
 * amd64 image on a machine with several NICs is the reason this is not
 * three. */
#define MAX_IFACES      8

#define UEVENT_BUF      4096

/* linux_dirent64 offsets, the same ones ls and blkid use. */
#define DIRENT_RECLEN   16
#define DIRENT_NAME     19

/* Netlink, the kernel's own object-event group. AF_NETLINK is not in
 * net.h: that header is about the internet, and this is the kernel
 * talking to itself. */
#define AF_NETLINK              16
#define NETLINK_KOBJECT_UEVENT  15
#define SO_RCVBUF               8
/* rtnetlink's link group: RTM_NEWLINK for every carrier and operstate
 * change. See run_daemon_all for why this is listened to as well. */
#define NETLINK_ROUTE           0
#define RTMGRP_LINK             1

typedef struct {
    u16 nl_family;
    u16 nl_pad;
    u32 nl_pid;
    u32 nl_groups;
} sockaddr_nl_t;

/* ── Reading what sysfs knows about an interface ─────────────────── */

/* One value out of /sys/class/net/<if>/<file>, with the trailing
 * newline cut off. false when there is nothing to read.
 *
 * Note what that means for `carrier`: the kernel answers EINVAL for an
 * interface that is administratively down, not "0". So a down
 * interface reads as "no cable", which is why everything below brings
 * an interface up before it asks whether anything is plugged into it. */
static bool net_sysfs(const char *ifname, const char *file,
                      char *out, size_t n)
{
    char path[96];
    snprintf(path, sizeof path, "/sys/class/net/%s/%s", ifname, file);

    long got = proc_read(path, out, n);
    if (got <= 0)
        return false;

    for (size_t i = 0; out[i]; i++) {
        if (out[i] == '\n' || out[i] == ' ') { out[i] = '\0'; break; }
    }
    return out[0] != '\0';
}

/* Is there a cable in it, or - on a wireless interface - is it
 * associated AND keyed?
 *
 * The second half is operstate. A wireless link has carrier from the
 * moment it is associated, which is before the WPA handshake; a DISCOVER
 * sent then goes out unencrypted and the access point drops it, so the
 * first round was always lost and the address came a round late. The
 * wpa daemon (and wpa_supplicant, the fallback) set the interface's link
 * mode to dormant, which makes the kernel report an associated-but-not-
 * yet-keyed link as operstate "dormant" and switch it to "up" when the
 * supplicant says the keys are in (RFC 2863). So "dormant" here means
 * "not yet" - for a wired port nobody sets that mode and operstate is
 * "up" whenever there is carrier, so nothing changes for a cable. */
static bool has_carrier(const char *ifname)
{
    char v[16];
    if (!net_sysfs(ifname, "carrier", v, sizeof v) || strcmp(v, "1") != 0)
        return false;
    return !(net_sysfs(ifname, "operstate", v, sizeof v) &&
             strcmp(v, "dormant") == 0);
}

/* A radio: the kernel gives every wireless interface a "wireless"
 * directory (cfg80211). Used only to let a cable win the route. */
static bool is_wireless(const char *ifname)
{
    char path[96];
    snprintf(path, sizeof path, "/sys/class/net/%s/wireless", ifname);
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return false;
    lp_close((int)fd);
    return true;
}

/* ── Which interfaces this manages ────────────────────────────────────
 *
 * Everything under /sys/class/net that could carry DHCP at all, which
 * is a shorter list than it sounds. Three tests, and each one is here
 * because of what it removes:
 *
 *   type is 1.  /sys/class/net/<if>/type is the ARP hardware type and 1
 *      is Ethernet. DHCP is defined over Ethernet framing - the packets
 *      built above say htype 1, hlen 6 - so anything else cannot be
 *      asked in the first place. This single test removes lo (772) and
 *      every pseudo-device the kernel invents by itself when a module
 *      loads: sit0 is 776, tunl0 768, ip6tnl0 769. No name list to keep
 *      up to date, and no chance of the next one slipping through.
 *
 *   no bridge/ directory.  A bridge is made out of other interfaces.
 *      Nothing in this system builds one, so a bridge that is here was
 *      built by hand and gets to be configured by hand.
 *
 *   no master link.  An interface enslaved to a bridge or a bond has no
 *      address of its own; the address belongs to the master. A lease
 *      taken on the slave is a lease the machine cannot use, and on a
 *      home network it is also an address taken away from something
 *      that could have used it.
 *
 * What is deliberately *not* a test is the name. eth0 and wlan0 are
 * what a Pi calls them, but the amd64 image on real hardware gets
 * enp0s3, an arm64 board can get end0, and a USB adapter is named by
 * whatever its driver felt like. A name list would have to be edited
 * every time somebody plugs in a new kind of adapter, which is exactly
 * the thing this is meant to stop being necessary. The list below is
 * only an order of preference, not the set.
 *
 * And the test that does most of the work is none of these: it is
 * carrier. An interface with nothing plugged into it is never asked for
 * an address, so a machine full of interfaces costs one small sysfs
 * read each per tick and nothing else. */
static bool is_candidate(const char *ifname)
{
    char v[16];
    if (!net_sysfs(ifname, "type", v, sizeof v) || strcmp(v, "1") != 0)
        return false;

    char path[96];
    snprintf(path, sizeof path, "/sys/class/net/%s/bridge", ifname);
    if (lp_exists(path))
        return false;

    snprintf(path, sizeof path, "/sys/class/net/%s/master", ifname);
    if (lp_exists(path))
        return false;

    return true;
}

/* The order interfaces are looked at in, which decides only one thing:
 * who gets the default route when more than one of them works.
 *
 * Wired first, and on purpose. A cable is faster, does not drop, and
 * cannot be knocked off the air by the wireless firmware falling over -
 * which on this board is not a hypothetical, it is the failure that
 * /etc/rc's usb0 section exists for. If a board has both, the cable
 * should be the way out. Anything found that is not in this list is
 * looked at after everything that is. */
static const char *PREFERRED[] = { "eth0", "eth1", "usb0", "usb1", "wlan0" };

/* Every interface worth managing, preferred names first. */
static int list_ifaces(char names[][IFNAMSIZ], int max)
{
    int n = 0;

    for (size_t i = 0; i < sizeof PREFERRED / sizeof *PREFERRED; i++) {
        if (n >= max)
            return n;
        char path[96];
        snprintf(path, sizeof path, "/sys/class/net/%s", PREFERRED[i]);
        if (lp_exists(path) && is_candidate(PREFERRED[i]))
            strlcpy(names[n++], PREFERRED[i], IFNAMSIZ);
    }

    long fd = lp_open("/sys/class/net", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return n;

    char buf[4096];
    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0)
            break;
        for (long off = 0; off < got && n < max; ) {
            char *rec  = buf + off;
            u16   len  = *(u16 *)(rec + DIRENT_RECLEN);
            char *name = rec + DIRENT_NAME;
            if (len == 0)
                break;
            off += len;
            if (name[0] == '.')
                continue;

            bool already = false;
            for (int i = 0; i < n; i++)
                if (strcmp(names[i], name) == 0)
                    already = true;
            if (already || !is_candidate(name))
                continue;

            strlcpy(names[n++], name, IFNAMSIZ);
        }
    }
    lp_close((int)fd);
    return n;
}

/* ── The gadget port ──────────────────────────────────────────────────
 *
 * A Pi plugged into a PC by its USB port is a network device to that
 * PC, and usb0 on this side is the other end of it. /etc/rc gives that
 * interface a link-local address at boot, and used to be the only thing
 * that did - so plugging the cable into the PC a minute after boot got
 * you an interface with no address and no way in.
 *
 * This belongs here rather than in a second daemon of its own for one
 * reason: this is already the program with a table of interfaces, a
 * uevent socket and a carrier check. A separate watcher would be a
 * second copy of that loop for one interface. And it has to know about
 * usb0 in any case, because the one thing that must not happen to the
 * gadget port is a DHCP round: there is no server on a cable to one PC,
 * so it would fail, and it would fail slowly - four DISCOVER attempts,
 * twelve seconds - on the one interface whose entire job is to be the
 * way in when the wireless has already failed.
 *
 * The address scheme is /etc/rc's, exactly: 169.254, then the last two
 * bytes of the interface's own MAC, with 0 and 255 nudged aside because
 * they are the network and broadcast of their own /24 and because
 * 169.254.0.x and 169.254.255.x are reserved by RFC 3927. It has to
 * stay exactly that, because a PC that has talked to this board before
 * has an ARP entry, a known_hosts line and probably a shortcut with
 * that address in it.
 *
 * A link-local address is enough: there is no router on this cable, and
 * 169.254.0.0/16 is what both ends would negotiate anyway. */

/* Is this the gadget port, or a USB Ethernet adapter that happened to
 * be called usbN?
 *
 * The difference matters: a phone sharing its connection also comes up
 * as usb0, and that link does have a DHCP server on it, which we should
 * use. A gadget interface's parent device is the USB device controller
 * rather than a device behind a host controller, so the `device`
 * symlink points at something with "gadget" or "udc" in the path.
 * Nothing at all behind it is also the gadget: on some kernels the
 * gadget netdev has no parent device link.
 *
 * Getting this wrong in either direction is survivable, which is why a
 * test this rough is acceptable. Both kinds get the link-local address
 * first, immediately, so the way in is there within a second either
 * way; the only difference is whether a DHCP round is attempted
 * afterwards, and on the gadget port that round costs twelve seconds
 * and changes nothing. */
static bool is_gadget_port(const char *ifname)
{
    if (strncmp(ifname, "usb", 3) != 0)
        return false;

    char path[96], link[256];
    snprintf(path, sizeof path, "/sys/class/net/%s/device", ifname);

    long n = lp_readlink(path, link, sizeof link - 1);
    if (n <= 0)
        return true;
    link[n] = '\0';

    return strstr(link, "gadget") != NULL || strstr(link, "udc") != NULL;
}

/* 169.254.<mac[4]>.<mac[5]>, netmask 255.255.0.0. `shown` gets the
 * address as text so the caller can say it. */
static bool set_linklocal(const char *ifname, char *shown, size_t sn)
{
    u8 mac[6];
    if (net_if_hwaddr(ifname, mac) < 0)
        return false;

    int a = mac[4], b = mac[5];
    if (a == 0)   a = 1;
    if (a == 255) a = 254;
    if (b == 0)   b = 1;
    if (b == 255) b = 254;

    char text[16];
    snprintf(text, sizeof text, "169.254.%d.%d", a, b);

    u32 addr = 0, mask = 0;
    if (!ipv4_parse(text, &addr) || !ipv4_parse("255.255.0.0", &mask))
        return false;

    if (net_if_up(ifname) < 0)
        return false;
    if (net_set_addr(ifname, addr) < 0)
        return false;
    net_set_netmask(ifname, mask);

    strlcpy(shown, text, sn);
    return true;
}

/* ── The table ───────────────────────────────────────────────────── */

typedef struct {
    char    name[IFNAMSIZ];
    bool    present;        /* still under /sys/class/net */
    bool    carrier;        /* what we last saw, so a change is a change */
    bool    have_lease;
    bool    linklocal;      /* the gadget address is on it */
    bool    said_silence;   /* we have already complained about this link */
    lease_t lease;
    s64     renew_at;       /* lp_monotonic_ms */
    s64     retry_at;
    u32     backoff_ms;
} iface_t;

static iface_t IFTAB[MAX_IFACES];
static int     IFN;

/* Who installed the default route and wrote /etc/resolv.conf. Empty
 * when nobody has. See apply_lease for why there is only one. */
static char    route_owner[IFNAMSIZ];

/* Everything this interface was holding is no longer true. Called when
 * the link goes down, when it comes back, and when the interface itself
 * disappears - the three ways an address stops meaning anything. */
static void forget(iface_t *it)
{
    /* The route and the address it put in go with it. Left there, the
     * route is still the kernel's way out, into a link that is not there
     * (see apply_lease); and the address keeps its subnet's route, which
     * on a home network is the same subnet the other link is on - the
     * Wi-Fi and the cable both on the router's 192.168.0.0/24 - so even
     * the router itself was looked for through the dead link. Gone with
     * the interface, both are already gone. A link-local address (usbN)
     * is not a lease and is left alone. */
    if (it->have_lease) {
        if (it->lease.router)
            drop_route(it->name);
        net_set_addr(it->name, 0);
    }

    it->have_lease   = false;
    it->linklocal    = false;
    it->said_silence = false;
    it->backoff_ms   = 0;
    it->retry_at     = 0;
    it->renew_at     = 0;
    memset(&it->lease, 0, sizeof it->lease);

    if (strcmp(route_owner, it->name) == 0)
        route_owner[0] = '\0';
}

/* Half the lease, and never longer than a day: a router that offers a
 * week-long lease is not a reason to go a week without checking that
 * the network still works. */
static s64 renew_delay_ms(u32 lease_secs)
{
    u32 wait = (lease_secs ? lease_secs : 600) / 2;
    if (wait < 30)    wait = 30;
    if (wait > 43200) wait = 43200;
    return (s64)wait * 1000;
}

/* One interface, one tick. Everything about it is decided from its own
 * state, so an interface that is failing cannot stop another one from
 * working - which was the whole trouble with the version of this that
 * picked one interface at boot and never looked at the others again. */
static void service(iface_t *it)
{
    /* Carrier cannot be read from an interface that is down (the kernel
     * gives EINVAL, which reads as "no cable"), so bring it up first. A
     * USB adapter that has just been plugged in arrives down, and
     * nothing else on this system is going to bring it up. */
    if (!net_if_is_up(it->name))
        net_if_up(it->name);

    bool live = has_carrier(it->name);

    if (live != it->carrier) {
        it->carrier = live;
        if (live) {
            printf("dhcp: %s: the cable is in\n", it->name);
            /* Start again from nothing rather than renewing what we
             * had. The usual reason for a cable to be unplugged and
             * plugged back in is that it is in a different socket now,
             * and a REQUEST naming an address from the old network gets
             * a NAK at best and silence at worst. A fresh DISCOVER is
             * both correct and no slower. */
            forget(it);
        } else {
            printf("dhcp: %s: the cable is out%s\n", it->name,
                   (it->have_lease || it->linklocal)
                       ? " - the address it had is no longer good" : "");
            forget(it);
        }
    }

    if (!live)
        return;

    /* usbN gets its link-local address before anything else is tried on
     * it, so that there is a way into the machine within a second of
     * the cable going into the PC. See the long note above. */
    if (strncmp(it->name, "usb", 3) == 0 && !it->have_lease && !it->linklocal) {
        char shown[16];
        if (set_linklocal(it->name, shown, sizeof shown)) {
            it->linklocal = true;
            printf("dhcp: %s  address %s  netmask 255.255.0.0  (link-local,"
                   " no router on this cable)\n", it->name, shown);
            printf("dhcp:   ssh to %s over the USB cable\n", shown);
        } else if (!it->said_silence) {
            it->said_silence = true;
            dprintf(STDERR_FILENO,
                    "dhcp: %s: could not put a link-local address on it\n",
                    it->name);
        }
        if (is_gadget_port(it->name))
            return;     /* nothing on the other end of this serves DHCP */
    }

    s64 now = lp_monotonic_ms();
    if (it->have_lease ? now < it->renew_at : now < it->retry_at)
        return;

    /* The route and resolv.conf go to the first interface that gets an
     * address and stay with it until its link goes away - except that a
     * cable takes them from a radio (apply_lease). */
    const char *held = (route_owner[0] && strcmp(route_owner, it->name) != 0)
                       ? route_owner : NULL;
    if (held && is_wireless(held) && !is_wireless(it->name))
        held = NULL;

    lease_t fresh;
    memset(&fresh, 0, sizeof fresh);

    if (get_lease(it->name, it->have_lease ? it->lease.addr : 0,
                  &fresh, held, false) == 0) {
        if (it->have_lease && fresh.addr != it->lease.addr) {
            char before[16], after[16];
            ipv4_format(it->lease.addr, before);
            ipv4_format(fresh.addr, after);
            printf("dhcp: %s: the address changed: %s -> %s\n",
                   it->name, before, after);
        }
        it->lease        = fresh;
        it->have_lease   = true;
        it->linklocal    = false;   /* DHCP won; that address is gone */
        it->said_silence = false;
        it->backoff_ms   = 0;
        it->renew_at     = now + renew_delay_ms(fresh.lease);

        if (!held && fresh.router) {
            if (route_owner[0] && strcmp(route_owner, it->name) != 0)
                printf("dhcp: %s: the default route moves here from %s\n",
                       it->name, route_owner);
            strlcpy(route_owner, it->name, sizeof route_owner);
        }
        return;
    }

    if (it->have_lease) {
        /* The renewal failed. Keep the address - the router has almost
         * certainly not given it to anybody else yet - and ask again
         * sooner. Giving it up now would guarantee the outage we are
         * renewing to avoid. */
        dprintf(STDERR_FILENO,
                "dhcp: %s: could not renew - keeping the address and"
                " retrying\n", it->name);
        it->lease.lease = it->lease.lease > 120 ? it->lease.lease / 2 : 120;
        it->renew_at    = now + renew_delay_ms(it->lease.lease);
        return;
    }

    /* Plugged in, and nothing answered. Say so once - it is the one
     * thing somebody standing at the board needs to know, and it is
     * also the one that would be unbearable repeated every few seconds
     * for the life of the machine. */
    if (!it->said_silence) {
        it->said_silence = true;
        printf("dhcp: %s: the cable is in but nothing answered - still"
               " asking\n", it->name);
    }
    it->backoff_ms = it->backoff_ms ? it->backoff_ms * 2 : RETRY_MIN_MS;
    if (it->backoff_ms > RETRY_MAX_MS)
        it->backoff_ms = RETRY_MAX_MS;
    it->retry_at = now + it->backoff_ms;
}

/* Bring the table up to date with what exists, then service every entry
 * in it. */
static void pass(void)
{
    char names[MAX_IFACES][IFNAMSIZ];
    int  n = list_ifaces(names, MAX_IFACES);

    for (int i = 0; i < IFN; i++)
        IFTAB[i].present = false;

    for (int i = 0; i < n; i++) {
        iface_t *it = NULL;
        for (int j = 0; j < IFN; j++)
            if (strcmp(IFTAB[j].name, names[i]) == 0)
                it = &IFTAB[j];

        if (!it) {
            if (IFN >= MAX_IFACES)
                continue;
            it = &IFTAB[IFN++];
            memset(it, 0, sizeof *it);
            strlcpy(it->name, names[i], IFNAMSIZ);
            printf("dhcp: managing %s\n", it->name);
        }
        it->present = true;
    }

    /* An interface that has gone - a USB adapter pulled out - keeps its
     * slot but loses everything it was holding, so that the same
     * adapter plugged back in starts clean rather than trying to renew
     * an address from before it left. */
    for (int i = 0; i < IFN; i++) {
        if (!IFTAB[i].present) {
            if (IFTAB[i].carrier || IFTAB[i].have_lease ||
                IFTAB[i].linklocal) {
                printf("dhcp: %s has been unplugged\n", IFTAB[i].name);
                IFTAB[i].carrier = false;
                forget(&IFTAB[i]);
            }
            continue;
        }
        service(&IFTAB[i]);
    }

    /* Nobody holds the route - its link went - but another interface is
     * still up with a lease of its own (the Wi-Fi, when the cable it
     * gave way to is pulled out): that one takes it now, rather than at
     * its next renewal hours from now. A cable first. */
    if (!route_owner[0]) {
        iface_t *best = NULL;
        for (int i = 0; i < IFN; i++) {
            iface_t *it = &IFTAB[i];
            if (!it->present || !it->have_lease || !it->lease.router ||
                !has_carrier(it->name))
                continue;
            if (!best || (is_wireless(best->name) && !is_wireless(it->name)))
                best = it;
        }
        if (best) {
            clear_stale_routes(best->name);
            long rc = net_add_default_route(best->name, best->lease.router);
            if (rc == 0 || -rc == 17) {
                write_resolv_conf(best->lease.dns);
                strlcpy(route_owner, best->name, sizeof route_owner);
                printf("dhcp: %s has the default route again\n", best->name);
            }
        }
    }
}

/* ── Hearing about it rather than finding out ─────────────────────────
 *
 * One uevent. true when it is a network interface appearing or going
 * away, which is the only kind we act on.
 *
 * Net uevents name the interface in INTERFACE=, not in DEVNAME= the way
 * the block devices automount listens for do. Everything else about the
 * message is the same: a first line we do not need, then NUL-separated
 * KEY=VALUE pairs. */
static bool parse_uevent(const char *msg, size_t len,
                         char *action, size_t an,
                         char *iface, size_t in)
{
    action[0] = '\0';
    iface[0]  = '\0';
    bool is_net = false;

    for (size_t i = 0; i < len; ) {
        const char *field = msg + i;
        size_t flen = strlen(field);

        if (strncmp(field, "ACTION=", 7) == 0)
            strlcpy(action, field + 7, an);
        else if (strncmp(field, "INTERFACE=", 10) == 0)
            strlcpy(iface, field + 10, in);
        else if (strncmp(field, "SUBSYSTEM=", 10) == 0)
            is_net = strcmp(field + 10, "net") == 0;

        i += flen + 1;
        if (flen == 0)
            break;
    }
    return is_net && action[0] && iface[0];
}

/* ── Managing every interface, for ever ───────────────────────────────
 *
 * `dhcp -d` with no interface named. What it used to do was walk wlan0,
 * eth0 and usb0 once, keep the first one that answered, and renew only
 * that one for the life of the machine. On a board whose wireless works
 * that is very nearly right and the "nearly" never shows. On a board
 * whose wireless does not - which is the board this was rewritten for -
 * every one of the gaps is the difference between a machine somebody
 * can reach and a machine they cannot:
 *
 *   A USB Ethernet adapter plugged in after boot was never brought up
 *   and never asked for an address. Nothing was watching for it, so it
 *   sat there dark until the next reboot.
 *
 *   With nothing answering anywhere the retry was every 60 seconds. A
 *   person who has just pushed a cable in does not wait a minute; they
 *   decide it is broken and pull the cable out again.
 *
 *   usb0 got its link-local address from /etc/rc, and only if it
 *   already existed when rc ran. Plugging the board into a PC later got
 *   an interface with no address on it.
 *
 *   An interface that had an address, lost carrier and got it back kept
 *   the old address, which is wrong the moment the cable has been moved
 *   to a different socket - and being moved is the usual reason for a
 *   cable to be unplugged.
 *
 * So now there is a table, every interface in it is looked at on every
 * tick, and each one is decided on its own. No interface is special and
 * none of them can starve another.
 *
 * ── How an interface is noticed ──
 * Two ways, and both are needed, for the same reason automount needs
 * both.
 *
 *   The kernel's uevent socket. Registering a network device
 *   broadcasts ACTION=add with SUBSYSTEM=net, so a USB Ethernet adapter
 *   being plugged in is known about in the time it takes the driver to
 *   probe it, rather than up to a tick later.
 *
 *   The tick. This is not a fallback, it is the main path, because of
 *   something worth being clear about: plugging a *cable* into an
 *   adapter that is already there produces no uevent at all. The kernel
 *   sends these for a device being registered or unregistered, not for
 *   carrier coming and going. So the uevent socket covers the adapter
 *   and the tick covers the cable, and the tick also covers a uevent
 *   that was dropped because the socket buffer overflowed in a burst or
 *   because this process was inside a twelve-second DISCOVER when it
 *   arrived. A missed event costs TICK_MS and nothing more.
 *
 *   Carrier and operstate changes also arrive on rtnetlink's link
 *   group, and that is listened to as a way to end the wait early -
 *   the cable, and a wireless link the wpa daemon has just keyed, are
 *   then seen at once instead of at the next tick. It only wakes the
 *   loop; the tick stays the thing that is guaranteed.
 */
static int run_daemon_all(void)
{
    long fd = lp_socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (fd >= 0) {
        /* Plugging in one adapter produces a burst - the net device and
         * a queue object per hardware queue - and the default receive
         * buffer is small enough to drop some of it. */
        int rcvbuf = 1 << 18;
        lp_setsockopt((int)fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);

        sockaddr_nl_t sa;
        memset(&sa, 0, sizeof sa);
        sa.nl_family = AF_NETLINK;
        sa.nl_pid    = 0;           /* let the kernel assign */
        sa.nl_groups = 1;           /* the uevent broadcast group */

        if (lp_bind((int)fd, &sa, sizeof sa) < 0) {
            lp_close((int)fd);
            fd = -1;
        }
    }
    if (fd < 0)
        dprintf(STDERR_FILENO,
                "dhcp: no kernel event socket - an adapter plugged in will"
                " be noticed at the next check instead\n");

    /* Link changes: carrier coming and going, and a wireless link going
     * from dormant to up when the wpa daemon has installed the keys.
     * None of those is a uevent. Without this they wait for the tick,
     * which put up to two seconds of nothing between "keys installed"
     * and the first DISCOVER - the longest step of joining a network,
     * spent idle. Any message on it just ends the wait early; what
     * changed is read from sysfs as always, so a lost message costs a
     * tick and nothing else. */
    long rfd = lp_socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
    if (rfd >= 0) {
        sockaddr_nl_t sa;
        memset(&sa, 0, sizeof sa);
        sa.nl_family = AF_NETLINK;
        sa.nl_groups = RTMGRP_LINK;
        if (lp_bind((int)rfd, &sa, sizeof sa) < 0) {
            lp_close((int)rfd);
            rfd = -1;
        }
    }

    printf("dhcp: watching every interface for a cable\n");
    pass();

    char buf[UEVENT_BUF];
    for (;;) {
        /* Wait out one tick, but cut it short the moment the kernel
         * says an interface has appeared or gone. */
        s64 deadline = lp_monotonic_ms() + TICK_MS;
        bool now_please = false;

        while (!now_please) {
            s64 left = deadline - lp_monotonic_ms();
            if (left <= 0)
                break;

            if (fd < 0 && rfd < 0) {
                lp_sleep_ms(left);
                break;
            }

            lp_pollfd_t pfd[2];
            pfd[0].fd      = (int)fd;       /* -1 is ignored by poll */
            pfd[0].events  = LP_POLLIN;
            pfd[0].revents = 0;
            pfd[1].fd      = (int)rfd;
            pfd[1].events  = LP_POLLIN;
            pfd[1].revents = 0;

            if (lp_poll(pfd, 2, (int)left) <= 0)
                continue;           /* timed out, or a signal */

            if (pfd[1].revents & LP_POLLIN) {
                /* Read it all; a burst is one reason to look. The short
                 * pause lets the rest of a burst (carrier, then
                 * operstate) land before sysfs is read. */
                while (lp_recvfrom((int)rfd, buf, sizeof buf, 0x40 /* DONTWAIT */,
                                   NULL, NULL) > 0)
                    ;
                lp_sleep_ms(50);
                now_please = true;
            }
            if (!(pfd[0].revents & LP_POLLIN))
                continue;

            long got = lp_recvfrom((int)fd, buf, sizeof buf - 1, 0, NULL, NULL);
            if (got <= 0)
                continue;
            buf[got] = '\0';

            char action[16], iface[IFNAMSIZ];
            if (!parse_uevent(buf, (size_t)got, action, sizeof action,
                              iface, sizeof iface))
                continue;

            if (strcmp(action, "add") == 0) {
                /* The uevent goes out as the device is registered, so
                 * sysfs is there by now - but the driver may still be
                 * settling the link. Give it a beat rather than reading
                 * a carrier that has not been decided yet; the tick
                 * would catch it anyway, and this only saves a second. */
                lp_sleep_ms(200);
                printf("dhcp: %s appeared\n", iface);
                now_please = true;
            } else if (strcmp(action, "remove") == 0) {
                now_please = true;
            }
            /* Anything else - a rename, a change - is left to the tick,
             * which is about to happen anyway. */
        }

        pass();
    }
}

/* ── Managing one named interface, for ever ───────────────────────────
 *
 * `dhcp -d eth0`. This is deliberately the old loop, unchanged: a
 * person who names an interface has said which one they mean, and gets
 * exactly the behaviour they got before - including the console output,
 * which counts its attempts out loud. Everything the table above does -
 * looking at the others, watching carrier, the gadget address - would
 * be this program overruling somebody who has already been specific. */
static int run_daemon_one(const char *ifname)
{
    lease_t held;
    memset(&held, 0, sizeof held);

    if (get_lease(ifname, 0, &held, NULL, true) != 0)
        dprintf(STDERR_FILENO,
                "dhcp: no address yet on %s - will keep trying\n", ifname);

    for (;;) {
        s64 wait_ms = renew_delay_ms(held.lease);
        for (s64 slept = 0; slept < wait_ms; slept += 10000)
            lp_sleep_ms(10000);

        lease_t fresh;
        memset(&fresh, 0, sizeof fresh);

        if (get_lease(ifname, held.addr, &fresh, NULL, true) == 0) {
            if (held.addr && fresh.addr != held.addr) {
                char before[16], after[16];
                ipv4_format(held.addr, before);
                ipv4_format(fresh.addr, after);
                printf("dhcp: the address changed: %s -> %s\n", before, after);
            }
            held = fresh;
            continue;
        }

        /* The renewal failed. Keep what we have and try again sooner -
         * the address is very likely still ours. */
        dprintf(STDERR_FILENO,
                "dhcp: could not renew on %s - keeping the address and"
                " retrying\n", ifname);
        held.lease = held.lease > 120 ? held.lease / 2 : 120;
    }
}

/* ── dhcp with no arguments at all ────────────────────────────────────
 *
 * One round on whatever is actually plugged in. This used to be "try
 * wlan0", which on a board with no working wireless and a cable in it
 * was the one interface guaranteed not to answer. Now it asks the
 * interfaces that have a carrier, in the order in PREFERRED, and stops
 * at the first one that answers - which is what somebody typing `dhcp`
 * with nothing after it meant. */
static int one_shot_any(void)
{
    char names[MAX_IFACES][IFNAMSIZ];
    int  n = list_ifaces(names, MAX_IFACES);
    int  tried = 0;

    for (int i = 0; i < n; i++) {
        if (!net_if_is_up(names[i]))
            net_if_up(names[i]);
        if (!has_carrier(names[i]))
            continue;

        tried++;
        printf("dhcp: asking on %s\n", names[i]);
        if (get_lease(names[i], 0, NULL, NULL, true) == 0)
            return 0;
    }

    if (tried == 0) {
        dprintf(STDERR_FILENO,
                "dhcp: nothing is plugged in%s\n",
                n ? " - no interface has a cable or an association" : "");
        for (int i = 0; i < n; i++)
            dprintf(STDERR_FILENO, "dhcp:   %-8s no link\n", names[i]);
        if (n == 0)
            dprintf(STDERR_FILENO,
                    "dhcp:   and this kernel found no network interface"
                    " at all\n");
    }
    return 1;
}

static void usage(void)
{
    printf("usage: dhcp [-d] [interface]\n");
    printf("  dhcp                 ask on whatever is plugged in, once\n");
    printf("  dhcp <interface>     ask on that one, once\n");
    printf("  dhcp -d              manage every interface, for ever\n");
    printf("  dhcp -d <interface>  manage only that one, for ever\n\n");
    printf("  -d  stays running. It renews the lease before it expires -\n");
    printf("      without that the address is held until the router takes\n");
    printf("      it back, and the board falls off the network with\n");
    printf("      nothing to notice - and with no interface named it also\n");
    printf("      watches for a cable being plugged in and configures it.\n");
}

int main(int argc, char **argv)
{
    const char *ifname = NULL;
    bool daemon = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            daemon = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else if (!ifname) {
            ifname = argv[i];
        }
    }

    if (daemon)
        return ifname ? run_daemon_one(ifname) : run_daemon_all();

    if (ifname)
        return get_lease(ifname, 0, NULL, NULL, true);

    return one_shot_any();
}
