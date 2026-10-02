/* net - set up the network, and say where it broke.
 *
 *   net                       what the network is doing right now
 *   net test                  walk every step and name the one that fails
 *   net scan                  WiFi networks in range
 *   net wifi <ssid> [pass]    join one, and remember it
 *   net wifi forget           stop joining it
 *   net dhcp [interface]      ask the router for an address
 *   net static <ip> <mask> <gateway> [dns]
 *   net dns <server>...       set the nameservers
 *
 * ── Why this exists ──
 * Everything here was already possible with ifconfig, route, dhcp and a
 * text editor. But joining a WiFi network meant writing
 * wpa_supplicant.conf by hand, with the right quoting, in the right
 * place - and getting the place wrong is silent, because the copy on
 * the boot partition overwrites the one on /data at every boot.
 *
 * And when it does not work, "the network is broken" is five different
 * problems: no link, no address, no route, no DNS, or nothing
 * listening. `net test` walks them in order and stops at the first one,
 * which is nearly always the answer.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

#define WPA_LIVE  "/etc/wpa.conf"
#define WPA_BOOT  "/boot/wpa_supplicant.conf"
#define WPA_DATA  "/data/wpa_supplicant.conf"
#define RESOLV    "/etc/resolv.conf"

/* ── running the tools we already have ───────────────────────────── */

static int run(const char *path, char *const argv[], bool quiet)
{
    pid_t pid = lp_fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (quiet) {
            long null = lp_open("/dev/null", O_WRONLY, 0);
            if (null >= 0) {
                lp_dup2((int)null, STDOUT_FILENO);
                lp_dup2((int)null, STDERR_FILENO);
                lp_close((int)null);
            }
        }
        char *envp[] = { (char *)"PATH=/bin:/data/bin", NULL };
        lp_execve(path, argv, envp);
        lp_exit(127);
    }
    int status = 0;
    lp_waitpid(pid, &status, 0);
    return LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : -1;
}

/* ── reading the current state ───────────────────────────────────── */

/* Every interface the kernel has, from /proc/net/dev. */
static int interfaces(char names[][IFNAMSIZ], int max)
{
    long fd = lp_open("/proc/net/dev", O_RDONLY, 0);
    if (fd < 0)
        return 0;

    char line[512];
    int n = 0;
    /* The first two lines are column headings. */
    readline((int)fd, line, sizeof line);
    readline((int)fd, line, sizeof line);

    while (n < max && readline((int)fd, line, sizeof line) >= 0) {
        char *p = line;
        while (*p == ' ') p++;
        char *colon = strchr(p, ':');
        if (!colon)
            continue;
        *colon = '\0';
        strlcpy(names[n], p, IFNAMSIZ);
        n++;
    }
    lp_close((int)fd);
    return n;
}

/* The default gateway, from /proc/net/route. 0 when there is none.
 *
 * The addresses in that file are hex, little endian, and already in
 * network byte order once read as a word - so the value read is the
 * u32 to hand to ipv4_format directly. */
static u32 default_gateway(char *iface, size_t ifn)
{
    if (ifn) iface[0] = '\0';

    long fd = lp_open("/proc/net/route", O_RDONLY, 0);
    if (fd < 0)
        return 0;

    char line[512];
    u32 gw = 0;
    readline((int)fd, line, sizeof line);        /* headings */

    while (readline((int)fd, line, sizeof line) >= 0) {
        char *fields[4];
        int nf = 0;
        char *p = line;
        while (*p && nf < 4) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            fields[nf++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
        if (nf < 3)
            continue;
        if (strcmp(fields[1], "00000000") != 0)
            continue;                            /* not the default route */
        gw = (u32)strtol(fields[2], NULL, 16);
        if (ifn) strlcpy(iface, fields[0], ifn);
        break;
    }
    lp_close((int)fd);
    return gw;
}

static int nameservers(char out[][20], int max)
{
    long fd = lp_open(RESOLV, O_RDONLY, 0);
    if (fd < 0)
        return 0;
    char line[256];
    int n = 0;
    while (n < max && readline((int)fd, line, sizeof line) >= 0) {
        if (strncmp(line, "nameserver", 10) != 0)
            continue;
        char *p = line + 10;
        while (*p == ' ' || *p == '\t') p++;
        char *end = p;
        while (*end && *end != ' ' && *end != '\n') end++;
        *end = '\0';
        if (*p) strlcpy(out[n++], p, 20);
    }
    lp_close((int)fd);
    return n;
}

static bool wpa_running(void)
{
    /* wpa_supplicant makes a control socket per interface. Its presence
     * is a better answer than scanning /proc for the name, and it is
     * the thing wpa_cli actually needs. */
    return lp_exists("/var/run/wpa_supplicant") ||
           lp_exists("/var/run/wpa_supplicant/wlan0");
}

/* ── net (no arguments): what is going on ────────────────────────── */

static int status(void)
{
    char names[8][IFNAMSIZ];
    int n = interfaces(names, 8);

    printf("interfaces\n");
    bool any_addr = false;
    for (int i = 0; i < n; i++) {
        u32 addr = 0;
        bool up = net_if_is_up(names[i]);
        bool has = net_get_addr(names[i], &addr) >= 0 && addr != 0;
        if (has) any_addr = true;

        char a[20] = "-";
        if (has) ipv4_format(addr, a);

        u8 mac[6];
        char m[20] = "";
        if (net_if_hwaddr(names[i], mac) >= 0)
            snprintf(m, sizeof m, "%02x:%02x:%02x:%02x:%02x:%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        printf("  %-8s %-16s %-6s %s\n",
               names[i], a, up ? "up" : "down", m);
    }

    char gwif[IFNAMSIZ];
    u32 gw = default_gateway(gwif, sizeof gwif);
    printf("\ngateway    ");
    if (gw) {
        char g[20];
        ipv4_format(gw, g);
        printf("%s via %s\n", g, gwif);
    } else {
        printf("none - nothing can be reached beyond this network\n");
    }

    char ns[4][20];
    int nn = nameservers(ns, 4);
    printf("nameserver ");
    if (nn == 0) {
        printf("none - names cannot be looked up\n");
    } else {
        for (int i = 0; i < nn; i++)
            printf("%s%s", i ? ", " : "", ns[i]);
        printf("\n");
    }

    printf("wifi       %s\n",
           wpa_running() ? "wpa_supplicant is running"
                         : "not running");

    if (!any_addr) {
        printf("\nNo interface has an address.\n");
        printf("  net wifi <name> <password>   join a WiFi network\n");
        printf("  net dhcp                     ask the router for one\n");
    } else {
        printf("\n  net test    check every step from here to the internet\n");
    }
    return 0;
}

/* ── net test: find the first thing that is wrong ────────────────── */

static int test(void)
{
    int step = 0;

    /* 1. Is there an interface with an address? */
    char names[8][IFNAMSIZ];
    int n = interfaces(names, 8);
    char live[IFNAMSIZ] = "";
    u32  myaddr = 0;

    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], "lo") == 0)
            continue;
        u32 a = 0;
        if (net_get_addr(names[i], &a) >= 0 && a != 0) {
            strlcpy(live, names[i], sizeof live);
            myaddr = a;
            break;
        }
    }

    step++;
    if (!live[0]) {
        printf("%d. address       FAILED\n\n", step);
        printf("No interface other than loopback has an address.\n");
        if (n <= 1) {
            printf("There is no network interface at all - no WiFi chip and\n");
            printf("no Ethernet adapter that this kernel recognised.\n");
        } else {
            printf("Interfaces exist but none is configured:\n");
            for (int i = 0; i < n; i++)
                if (strcmp(names[i], "lo") != 0)
                    printf("  %-8s %s\n", names[i],
                           net_if_is_up(names[i]) ? "up, no address" : "down");
            printf("\n  net wifi <name> <password>\n");
            printf("  net dhcp %s\n", n > 1 ? names[1] : "<interface>");
        }
        return 1;
    }
    char a[20];
    ipv4_format(myaddr, a);
    printf("%d. address       ok    %s on %s\n", step, a, live);

    /* 2. Is there a route out? */
    char gwif[IFNAMSIZ];
    u32 gw = default_gateway(gwif, sizeof gwif);
    step++;
    if (!gw) {
        printf("%d. gateway       FAILED\n\n", step);
        printf("This machine has an address but no default route, so it can\n");
        printf("talk to its own network and nothing beyond it. DHCP normally\n");
        printf("supplies one.\n\n");
        printf("  net dhcp %s\n", live);
        printf("  route add default gw <router address>\n");
        return 1;
    }
    char g[20];
    ipv4_format(gw, g);
    printf("%d. gateway       ok    %s\n", step, g);

    /* 3. Does the gateway answer? */
    step++;
    {
        char *argv[] = { (char *)"/bin/ping", (char *)"-c", (char *)"1",
                         g, NULL };
        int rc = run("/bin/ping", argv, true);
        if (rc != 0) {
            printf("%d. router        FAILED\n\n", step);
            printf("%s does not answer. The address and the route are set,\n", g);
            printf("but nothing is there - a wrong static address, the wrong\n");
            printf("WiFi network, or a cable that is not in.\n\n");
            printf("  net dhcp %s    ask the network what the answer is\n", live);
            return 1;
        }
        printf("%d. router        ok    %s answers\n", step, g);
    }

    /* 4. Can names be looked up? */
    char ns[4][20];
    int nn = nameservers(ns, 4);
    step++;
    if (nn == 0) {
        printf("%d. dns           FAILED\n\n", step);
        printf("No nameserver is configured, so nothing can be reached by\n");
        printf("name even though the network itself works.\n\n");
        printf("  net dns 1.1.1.1\n");
        return 1;
    }
    u32 resolved = net_resolve("example.com");
    if (resolved == 0) {
        printf("%d. dns           FAILED\n\n", step);
        printf("The nameserver at %s did not answer.\n", ns[0]);
        printf("The network works - the router answers - but name lookups\n");
        printf("do not, which usually means the wrong nameserver address.\n\n");
        printf("  net dns 1.1.1.1 8.8.8.8\n");
        return 1;
    }
    {
        char r[20];
        ipv4_format(resolved, r);
        printf("%d. dns           ok    example.com is %s\n", step, r);
    }

    /* 5. Does anything out there actually answer? */
    step++;
    {
        long got = net_http_get("http://example.com/", "/tmp/.nettest");
        lp_unlink("/tmp/.nettest");
        if (got < 0) {
            printf("%d. internet      FAILED\n\n", step);
            printf("Names resolve and the router answers, but an HTTP request\n");
            printf("did not get through. On a network that filters outbound\n");
            printf("traffic that is expected; otherwise check the firewall.\n\n");
            printf("  firewall      what this machine is blocking\n");
            return 1;
        }
        printf("%d. internet      ok    %ld bytes from example.com\n",
               step, got);
    }

    printf("\nEverything works.\n");
    return 0;
}

/* ── WiFi ────────────────────────────────────────────────────────── */

/* wpa_supplicant reads a quoted string. A password may legitimately
 * contain a quote or a backslash, and both have to be escaped or the
 * file silently means something else. */
static void quote_into(const char *in, char *out, size_t n)
{
    size_t o = 0;
    if (n == 0) return;
    for (size_t i = 0; in[i] && o + 3 < n; i++) {
        if (in[i] == '"' || in[i] == '\\')
            out[o++] = '\\';
        out[o++] = in[i];
    }
    out[o] = '\0';
}

static bool write_wpa(const char *ssid, const char *pass)
{
    char qs[128], qp[128];
    quote_into(ssid, qs, sizeof qs);
    quote_into(pass ? pass : "", qp, sizeof qp);

    char body[512];
    if (pass && pass[0]) {
        snprintf(body, sizeof body,
                 "# written by 'net wifi'\n"
                 "ctrl_interface=/var/run/wpa_supplicant\n"
                 "update_config=1\n"
                 "country=00\n"
                 "\n"
                 "network={\n"
                 "    ssid=\"%s\"\n"
                 "    psk=\"%s\"\n"
                 "}\n", qs, qp);
    } else {
        snprintf(body, sizeof body,
                 "# written by 'net wifi' - open network, no password\n"
                 "ctrl_interface=/var/run/wpa_supplicant\n"
                 "update_config=1\n"
                 "country=00\n"
                 "\n"
                 "network={\n"
                 "    ssid=\"%s\"\n"
                 "    key_mgmt=NONE\n"
                 "}\n", qs);
    }

    /* Three copies, and the order matters.
     *
     * /boot wins at every boot, so writing only the others would look
     * like it worked and then be undone by the next reboot. /boot is
     * mounted read-only, so this remounts it for the one write and puts
     * it back. */
    bool wrote_any = false;

    bool boot_rw = lp_mount(NULL, "/boot", NULL, MS_REMOUNT, NULL) == 0;
    if (boot_rw) {
        long fd = lp_open(WPA_BOOT, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            lp_write((int)fd, body, strlen(body));
            lp_close((int)fd);
            wrote_any = true;
            printf("net: saved to %s\n", WPA_BOOT);
        }
        lp_sync();
        lp_mount(NULL, "/boot", NULL, MS_REMOUNT | MS_RDONLY, NULL);
    }

    for (int i = 0; i < 2; i++) {
        const char *path = i ? WPA_LIVE : WPA_DATA;
        long fd = lp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0)
            continue;
        lp_write((int)fd, body, strlen(body));
        lp_close((int)fd);
        wrote_any = true;
        if (!boot_rw && i == 0)
            printf("net: saved to %s\n", path);
    }
    lp_sync();

    if (!wrote_any)
        dprintf(STDERR_FILENO, "net: could not save the settings anywhere\n");
    return wrote_any;
}

static void restart_wpa(void)
{
    /* Stop whatever is running, then start it on the config just
     * written. -B backgrounds it, which is what /etc/rc does too. */
    char *k[] = { (char *)"/bin/kill", (char *)"-9",
                  (char *)"wpa_supplicant", NULL };
    run("/bin/kill", k, true);
    lp_sleep_ms(300);

    char *argv[] = { (char *)"/bin/wpa_supplicant", (char *)"-B",
                     (char *)"-i", (char *)"wlan0",
                     (char *)"-c", (char *)WPA_LIVE, NULL };
    run("/bin/wpa_supplicant", argv, false);
}

static int wifi(const char *ssid, const char *pass)
{
    if (strcmp(ssid, "forget") == 0) {
        bool boot_rw = lp_mount(NULL, "/boot", NULL, MS_REMOUNT, NULL) == 0;
        if (boot_rw) {
            lp_unlink(WPA_BOOT);
            lp_mount(NULL, "/boot", NULL, MS_REMOUNT | MS_RDONLY, NULL);
        }
        lp_unlink(WPA_DATA);
        printf("net: forgotten. The next boot will not join a WiFi network.\n");
        return 0;
    }

    if (!lp_exists("/bin/wpa_supplicant")) {
        dprintf(STDERR_FILENO,
                "net: wpa_supplicant is not in this image, so WiFi cannot\n"
                "     be joined. Use Ethernet, or 'net static'.\n");
        return 1;
    }

    printf("net: joining \"%s\"%s\n", ssid,
           (pass && pass[0]) ? "" : " (open network)");
    if (!write_wpa(ssid, pass))
        return 1;

    restart_wpa();

    /* Association takes a moment, and asking for an address before it
     * has happened just fails. */
    printf("net: waiting for the network to associate\n");
    for (int waited = 0; waited < 15000; waited += 500) {
        lp_sleep_ms(500);
        if (net_if_is_up("wlan0")) {
            u32 addr = 0;
            if (net_get_addr("wlan0", &addr) >= 0 && addr != 0)
                break;
        }
    }

    char *argv[] = { (char *)"/bin/dhcp", (char *)"wlan0", NULL };
    if (run("/bin/dhcp", argv, false) != 0) {
        dprintf(STDERR_FILENO,
                "net: joined, but no address came back.\n"
                "     'net test' says which step is failing.\n");
        return 1;
    }
    return test();
}

static int scan(void)
{
    if (!lp_exists("/bin/wpa_cli")) {
        dprintf(STDERR_FILENO, "net: wpa_cli is not in this image\n");
        return 1;
    }
    if (!wpa_running()) {
        printf("net: starting wpa_supplicant so that it can scan\n");
        restart_wpa();
        lp_sleep_ms(1000);
    }

    char *s[] = { (char *)"/bin/wpa_cli", (char *)"scan", NULL };
    run("/bin/wpa_cli", s, true);
    printf("net: scanning\n");
    lp_sleep_ms(3000);

    char *r[] = { (char *)"/bin/wpa_cli", (char *)"scan_results", NULL };
    int rc = run("/bin/wpa_cli", r, false);
    if (rc != 0)
        dprintf(STDERR_FILENO,
                "net: the scan failed. There may be no WiFi interface -\n"
                "     'net' lists what interfaces exist.\n");
    else
        printf("\n  net wifi <name> <password>   to join one\n");
    return rc;
}

/* ── addresses ───────────────────────────────────────────────────── */

static int set_dns(int argc, char **argv, int first)
{
    long fd = lp_open(RESOLV, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "net: cannot write %s (%ld)\n", RESOLV, -fd);
        return 1;
    }
    for (int i = first; i < argc; i++) {
        u32 a;
        if (!ipv4_parse(argv[i], &a)) {
            dprintf(STDERR_FILENO,
                    "net: \"%s\" is not an address. A nameserver has to be\n"
                    "     numeric - there is nothing to look it up with yet.\n",
                    argv[i]);
            lp_close((int)fd);
            return 1;
        }
        dprintf((int)fd, "nameserver %s\n", argv[i]);
        printf("net: nameserver %s\n", argv[i]);
    }
    lp_close((int)fd);
    printf("net:   this lasts until the next boot. DHCP overwrites it.\n");
    printf("net:   for a permanent one, put it in /boot/network.conf\n");
    return 0;
}

static int set_static(int argc, char **argv, int first)
{
    if (argc - first < 3) {
        dprintf(STDERR_FILENO,
                "usage: net static <address> <netmask> <gateway> [dns]\n"
                "   eg: net static 192.168.0.50 255.255.255.0 192.168.0.1\n");
        return 2;
    }
    const char *ip = argv[first], *mask = argv[first + 1],
               *gw = argv[first + 2];

    /* Which interface? The first one that is not loopback and exists. */
    char names[8][IFNAMSIZ];
    int n = interfaces(names, 8);
    char iface[IFNAMSIZ] = "";
    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], "lo") != 0) { strlcpy(iface, names[i], IFNAMSIZ); break; }
    }
    if (!iface[0]) {
        dprintf(STDERR_FILENO, "net: there is no network interface\n");
        return 1;
    }

    u32 a, m, g;
    if (!ipv4_parse(ip, &a) || !ipv4_parse(mask, &m) || !ipv4_parse(gw, &g)) {
        dprintf(STDERR_FILENO, "net: one of those is not an IPv4 address\n");
        return 1;
    }

    if (net_if_up(iface) < 0) {
        dprintf(STDERR_FILENO, "net: cannot bring %s up\n", iface);
        return 1;
    }
    if (net_set_addr(iface, a) < 0 || net_set_netmask(iface, m) < 0) {
        dprintf(STDERR_FILENO, "net: cannot set the address on %s\n", iface);
        return 1;
    }
    net_add_default_route(iface, g);

    printf("net: %s is %s/%s via %s\n", iface, ip, mask, gw);

    if (argc - first >= 4)
        set_dns(argc, argv, first + 3);

    printf("net:   this lasts until the next boot. To make it permanent,\n");
    printf("net:   put it in /boot/network.conf - 'ipconfig' reads that.\n");
    return 0;
}


/* ── WiFi power save ──────────────────────────────────────────────────
 *
 * The chip is allowed to sleep between beacons. It saves a little power
 * and it costs you the machine: a board left idle stops answering,
 * while staying up - the LED is on, the USB link is fine, and ssh times
 * out. The router lost track of a station that was asleep when it
 * asked. On a board whose whole purpose is to sit somewhere and be
 * reachable, that is not a trade worth making.
 *
 * There is no module parameter and no sysfs file for it. brcmfmac
 * exposes it only through nl80211, which is generic netlink, which
 * means two round trips: ask the controller which family id "nl80211"
 * has today (it is assigned at load time and is not a constant), then
 * send NL80211_CMD_SET_POWER_SAVE to it.
 *
 * The numbers below are from the kernel's own uapi headers, read at the
 * version this image ships, not copied from memory.
 */
/* AF_NETLINK is not in net.h: this libc's socket layer is for the
 * internet, and netlink is the kernel talking to itself. */
#define AF_NETLINK             16
#define NETLINK_GENERIC        16
#define GENL_ID_CTRL           16
#define CTRL_CMD_GETFAMILY      3
#define CTRL_ATTR_FAMILY_ID     1
#define CTRL_ATTR_FAMILY_NAME   2

#define NL80211_CMD_SET_POWER_SAVE  61
#define NL80211_CMD_GET_POWER_SAVE  62
#define NL80211_ATTR_IFINDEX         3
#define NL80211_ATTR_PS_STATE       93
#define NL80211_PS_DISABLED          0
#define NL80211_PS_ENABLED           1

#define NLM_F_REQUEST  0x01
#define NLM_F_ACK      0x04
#define NLMSG_ERROR    0x02
#define NLMSG_DONE     0x03

typedef struct { u32 len; u16 type; u16 flags; u32 seq; u32 pid; } nlhdr_t;
typedef struct { u8 cmd; u8 version; u16 reserved; } genlhdr_t;
typedef struct { u16 nla_len; u16 nla_type; } nlattr_t;

static u32 nl_seq;

/* Netlink pads every attribute to four bytes. */
static u32 nla_put(u8 *buf, u32 off, u16 type, const void *val, u16 len)
{
    nlattr_t a = { (u16)(sizeof a + len), type };
    memcpy(buf + off, &a, sizeof a);
    memcpy(buf + off + sizeof a, val, len);
    u32 total = (u32)(sizeof a + len);
    u32 padded = (total + 3u) & ~3u;
    for (u32 i = total; i < padded; i++)
        buf[off + i] = 0;
    return off + padded;
}

/* Send one message and read the reply into buf. Returns the reply
 * length, or -errno. */
static long nl_talk(int fd, u8 *buf, u32 len, u8 *reply, u32 cap)
{
    if (lp_write(fd, buf, len) < 0)
        return -1;
    for (;;) {
        long n = lp_read(fd, reply, cap);
        if (n <= 0)
            return -1;
        nlhdr_t h;
        memcpy(&h, reply, sizeof h);
        if (h.type == NLMSG_ERROR) {
            s32 err;
            memcpy(&err, reply + sizeof h, sizeof err);
            return err;             /* 0 is the ACK we asked for */
        }
        return n;
    }
}

/* The family id the kernel gave nl80211 this boot. */
static int nl80211_family(int fd)
{
    u8 buf[512], rep[8192];
    memset(buf, 0, sizeof buf);

    nlhdr_t  h = { 0, GENL_ID_CTRL, NLM_F_REQUEST, ++nl_seq, 0 };
    genlhdr_t g = { CTRL_CMD_GETFAMILY, 1, 0 };
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, &g, sizeof g);
    u32 off = nla_put(buf, (u32)(sizeof h + sizeof g), CTRL_ATTR_FAMILY_NAME,
                      "nl80211", 8);
    h.len = off;
    memcpy(buf, &h, sizeof h);

    long n = nl_talk(fd, buf, off, rep, sizeof rep);
    if (n <= 0)
        return -1;

    /* Walk the attributes for the id. */
    u32 p = (u32)(sizeof(nlhdr_t) + sizeof(genlhdr_t));
    while (p + sizeof(nlattr_t) <= (u32)n) {
        nlattr_t a;
        memcpy(&a, rep + p, sizeof a);
        if (a.nla_len < sizeof a)
            break;
        if (a.nla_type == CTRL_ATTR_FAMILY_ID && a.nla_len >= sizeof a + 2) {
            u16 id;
            memcpy(&id, rep + p + sizeof a, sizeof id);
            return (int)id;
        }
        p += (a.nla_len + 3u) & ~3u;
    }
    return -1;
}

static int set_powersave(const char *iface, bool on)
{
    int idx = 0;
    if (net_if_index(iface, &idx) < 0 || idx <= 0) {
        dprintf(STDERR_FILENO, "net: no interface called %s\n", iface);
        return 1;
    }

    long fd = lp_socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "net: cannot open netlink (%ld)\n", -fd);
        return 1;
    }

    int fam = nl80211_family((int)fd);
    if (fam < 0) {
        dprintf(STDERR_FILENO,
                "net: this kernel has no nl80211 - no WiFi driver is loaded\n");
        lp_close((int)fd);
        return 1;
    }

    u8 buf[512], rep[8192];
    memset(buf, 0, sizeof buf);
    nlhdr_t  h = { 0, (u16)fam, NLM_F_REQUEST | NLM_F_ACK, ++nl_seq, 0 };
    genlhdr_t g = { NL80211_CMD_SET_POWER_SAVE, 0, 0 };
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, &g, sizeof g);
    u32 off = (u32)(sizeof h + sizeof g);
    u32 ifidx = (u32)idx;
    off = nla_put(buf, off, NL80211_ATTR_IFINDEX, &ifidx, sizeof ifidx);
    u32 state = on ? NL80211_PS_ENABLED : NL80211_PS_DISABLED;
    off = nla_put(buf, off, NL80211_ATTR_PS_STATE, &state, sizeof state);
    h.len = off;
    memcpy(buf, &h, sizeof h);

    long r = nl_talk((int)fd, buf, off, rep, sizeof rep);
    lp_close((int)fd);

    if (r != 0) {
        dprintf(STDERR_FILENO,
                "net: the driver refused to change power save (%ld)\n", r);
        return 1;
    }
    printf("net: power save %s on %s\n", on ? "on" : "off", iface);
    if (!on)
        printf("net:   the chip stays awake, so the router does not lose it\n");
    return 0;
}

static void usage(void)
{
    printf("net - set up the network, and say where it broke\n\n");
    printf("  net                       what the network is doing\n");
    printf("  net test                  check every step, name the failure\n");
    printf("  net scan                  WiFi networks in range\n");
    printf("  net wifi <name> [pass]    join one, and remember it\n");
    printf("  net wifi forget           stop joining it\n");
    printf("  net dhcp [interface]      ask the router for an address\n");
    printf("  net static <ip> <mask> <gateway> [dns]\n");
    printf("  net dns <server>...       set the nameservers\n");
    printf("  net powersave off|on [if]  keep the WiFi chip awake\n\n");
    printf("'net wifi' saves to the boot partition as well as here,\n");
    printf("because that copy wins at every boot - writing only the local\n");
    printf("one looks like it worked until you reboot.\n\n");
    printf("Underneath: ifconfig, route, dhcp, nslookup, wpa_supplicant.\n");
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return status();

    const char *cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "help") == 0) { usage(); return 0; }

    if (strcmp(cmd, "status") == 0) return status();
    if (strcmp(cmd, "test") == 0)   return test();
    if (strcmp(cmd, "scan") == 0)   return scan();

    if (strcmp(cmd, "wifi") == 0) {
        if (argc < 3) {
            dprintf(STDERR_FILENO,
                    "usage: net wifi <name> [password]\n"
                    "       net wifi forget\n"
                    "       net scan          to see what is in range\n");
            return 2;
        }
        return wifi(argv[2], argc > 3 ? argv[3] : NULL);
    }

    if (strcmp(cmd, "dhcp") == 0) {
        char *a[] = { (char *)"/bin/dhcp",
                      (char *)(argc > 2 ? argv[2] : "eth0"), NULL };
        int rc = run("/bin/dhcp", a, false);
        if (rc == 0) return test();
        if (argc <= 2)
            dprintf(STDERR_FILENO,
                    "net: no address on eth0. Name the interface, or use\n"
                    "     'net wifi' - 'net' lists what interfaces exist.\n");
        return rc;
    }

    if (strcmp(cmd, "static") == 0) return set_static(argc, argv, 2);
    if (strcmp(cmd, "powersave") == 0) {
        if (argc < 3 || (strcmp(argv[2], "off") != 0 &&
                         strcmp(argv[2], "on") != 0)) {
            dprintf(STDERR_FILENO, "usage: net powersave off|on [interface]\n");
            return 2;
        }
        return set_powersave(argc > 3 ? argv[3] : "wlan0",
                             strcmp(argv[2], "on") == 0);
    }
    if (strcmp(cmd, "dns") == 0) {
        if (argc < 3) {
            dprintf(STDERR_FILENO, "usage: net dns <server>...\n");
            return 2;
        }
        return set_dns(argc, argv, 2);
    }

    dprintf(STDERR_FILENO, "net: no idea what \"%s\" means\n", cmd);
    usage();
    return 2;
}
