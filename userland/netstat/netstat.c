/* netstat - what this machine is listening on, and what it is talking to.
 *
 *   netstat              listening sockets and established connections
 *   netstat -l           only what is listening
 *   netstat -a           every socket, whatever state it is in
 *   netstat -t / -u      TCP only / UDP only
 *   netstat -n           numbers (this is what it always does - see below)
 *   netstat -p           which process holds the socket, pid and name
 *   netstat -r           the routing table
 *   netstat -i           the interfaces and their counters
 *   netstat -s           per-protocol statistics
 *
 * Everything here comes out of /proc: net/tcp, net/tcp6, net/udp,
 * net/udp6, net/route, net/dev and net/snmp. No netlink, no sockets
 * opened to ask the kernel anything - the kernel has already written
 * all of it down.
 *
 * BYTE ORDER, which is the one thing that quietly goes wrong here.
 * /proc/net/tcp prints an address as eight hex digits of a u32 held in
 * the HOST's byte order. Both architectures this system builds for -
 * x86-64 and aarch64 - are little-endian, so the four bytes of that u32
 * sit in memory as 7F 00 00 01 for the text "0100007F", which is
 * already network order, which is what ipv4_format wants. So the value
 * strtol gives back is passed straight through with no swapping at all.
 * Checked against this machine's own table: the loopback line reads
 * 0100007F and prints 127.0.0.1, and route.c has been doing the same
 * thing with /proc/net/route since it was written. Get this wrong and
 * you do not get an error, you get 1.0.0.127 - an address that looks
 * perfectly reasonable and is not the one on the wire.
 *
 * The port after the colon is different again: it is plain hex of the
 * port number, no byte order involved.
 *
 * NAMES ARE NEVER LOOKED UP. -n is accepted and does nothing, because
 * numeric is all this ever prints. A board on a home network has no
 * reverse DNS worth the wait, and a netstat that sits for thirty
 * seconds per line against a resolver that is not answering is a
 * netstat nobody runs twice. The usage text says so rather than
 * pretending there is a lookup to turn off.
 *
 * -p IS THE EXPENSIVE ONE. Nothing in /proc/net says which process owns
 * a socket; the only link is the socket's inode, which turns up again
 * as the target of a /proc/<pid>/fd/<n> symlink reading "socket:[NNN]".
 * Finding it means a readlink per open file descriptor of every process
 * on the machine - hundreds of syscalls - so the map is only built when
 * -p is given, and it is built once for all four socket files rather
 * than per line. Descriptors of other users' processes cannot be read
 * unless you are root, and those sockets simply show "-".
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

/* getdents64 records: reclen at byte 16, the name at 19 (as ls does). */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

/* Column widths. An IPv4 address and port is at most 21 characters, so
 * 24 holds one with room to spare and the whole table with -p still
 * fits in 80 columns. A long IPv6 address overflows its column and
 * pushes that one row across; that beats truncating an address, which
 * would be a lie about what is connected. */
#define ADDRW 24

/* ── small parsing helpers ──────────────────────────────────────── */

static u32 hex_digits(const char *s, int n)
{
    u32 v = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        u32  d;
        if      (c >= '0' && c <= '9') d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A') + 10;
        else break;
        v = v * 16 + d;
    }
    return v;
}

/* Split a line on runs of spaces and tabs, in place. */
static int split(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;

    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        f[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

typedef struct {
    u8  b[16];      /* network order, always */
    int len;        /* 4 or 16 */
    u16 port;
} addr_t;

/* "0100007F:91D5" or the 32-digit v6 form. false if it is neither. */
static bool parse_addr(const char *s, addr_t *a)
{
    const char *colon = strchr(s, ':');
    if (!colon)
        return false;

    int words = (int)(colon - s) / 8;
    if (words != 1 && words != 4)
        return false;

    for (int i = 0; i < words; i++) {
        u32 w = hex_digits(s + i * 8, 8);
        memcpy(a->b + i * 4, &w, 4);      /* little-endian host: see above */
    }
    a->len  = words * 4;
    a->port = (u16)hex_digits(colon + 1, 4);
    return true;
}

/* One IPv6 group, lower case, no leading zeros. */
static int put_hex16(char *out, u16 v)
{
    static const char d[] = "0123456789abcdef";
    char tmp[4];
    int  n = 0;

    if (v == 0) { out[0] = '0'; return 1; }
    while (v) { tmp[n++] = d[v & 0xF]; v >>= 4; }
    for (int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

/* The longest run of zero groups collapses to "::", as everyone writes
 * IPv6. Without it a listening socket prints as
 * 0:0:0:0:0:0:0:0 and the column is wider than the screen. */
static void fmt_v6(const u8 *b, char *out)
{
    u16 g[8];
    for (int i = 0; i < 8; i++)
        g[i] = (u16)((b[2 * i] << 8) | b[2 * i + 1]);

    int best = -1, best_len = 0;
    for (int i = 0; i < 8; ) {
        if (g[i]) { i++; continue; }
        int j = i;
        while (j < 8 && !g[j]) j++;
        if (j - i > best_len) { best_len = j - i; best = i; }
        i = j;
    }
    if (best_len < 2)
        best = -1;

    int o = 0;
    for (int i = 0; i < 8; i++) {
        if (best >= 0 && i >= best && i < best + best_len) {
            if (i == best) out[o++] = ':';
            continue;
        }
        if (i > 0) out[o++] = ':';
        o += put_hex16(out + o, g[i]);
    }
    if (best >= 0 && best + best_len == 8)
        out[o++] = ':';
    out[o] = '\0';
}

/* address:port, ready to print. 48 bytes is enough for the widest v6. */
static void fmt_addr(const addr_t *a, char *out, size_t cap)
{
    char host[46];
    const u8 *b = a->b;
    int len = a->len;

    /* ::ffff:a.b.c.d is an IPv4 socket the kernel filed under tcp6.
     * Printing it as IPv4 is what it actually is. */
    if (len == 16) {
        bool mapped = b[10] == 0xFF && b[11] == 0xFF;
        for (int i = 0; i < 10 && mapped; i++)
            if (b[i]) mapped = false;
        if (mapped) { b += 12; len = 4; }
    }

    if (len == 4) {
        u32 v;
        memcpy(&v, b, 4);
        ipv4_format(v, host);
        if (a->port)
            snprintf(out, cap, "%s:%u", host, (u32)a->port);
        else
            snprintf(out, cap, "%s:*", host);
        return;
    }

    fmt_v6(b, host);
    if (a->port)
        snprintf(out, cap, "[%s]:%u", host, (u32)a->port);
    else
        snprintf(out, cap, "[%s]:*", host);
}

/* The state numbers are the kernel's TCP state enum. UDP sockets use
 * the same field but only ever hold 1 (connected to a peer) or 7. */
static const char *tcp_state(u32 st)
{
    switch (st) {
    case 1:  return "ESTABLISHED";
    case 2:  return "SYN_SENT";
    case 3:  return "SYN_RECV";
    case 4:  return "FIN_WAIT1";
    case 5:  return "FIN_WAIT2";
    case 6:  return "TIME_WAIT";
    case 7:  return "CLOSE";
    case 8:  return "CLOSE_WAIT";
    case 9:  return "LAST_ACK";
    case 10: return "LISTEN";
    case 11: return "CLOSING";
    case 12: return "SYN_RECV";       /* NEW_SYN_RECV, a SYN cookie */
    default: return "UNKNOWN";
    }
}

/* ── inode -> process, only built for -p ────────────────────────── */

#define OWNER_MAX 256

typedef struct {
    u64  ino;
    int  pid;
    char name[16];
} owner_t;

static owner_t g_owner[OWNER_MAX];
static int     g_owners;
static bool    g_owner_full;

static void owner_add(u64 ino, int pid, const char *name)
{
    if (g_owners >= OWNER_MAX) { g_owner_full = true; return; }
    g_owner[g_owners].ino = ino;
    g_owner[g_owners].pid = pid;
    strlcpy(g_owner[g_owners].name, name, sizeof(g_owner[0].name));
    g_owners++;
}

static bool numeric(const char *s)
{
    if (!*s) return false;
    for (const char *c = s; *c; c++)
        if (*c < '0' || *c > '9') return false;
    return true;
}

static void scan_fds(int pid, const char *name)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fd", pid);

    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;                       /* another user's process, or gone */

    char buf[4096];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;

        for (long off = 0; off < n; ) {
            char       *rec  = buf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            const char *ent  = rec + DIRENT_NAME;
            off += len;

            if (!numeric(ent))
                continue;

            char link[64], target[64];
            snprintf(link, sizeof(link), "/proc/%d/fd/%s", pid, ent);
            long r = lp_readlink(link, target, sizeof(target) - 1);
            if (r <= 0)
                continue;
            target[r] = '\0';         /* readlink does not terminate it */

            if (strncmp(target, "socket:[", 8) != 0)
                continue;
            owner_add((u64)strtol(target + 8, NULL, 10), pid, name);
        }
    }
    lp_close((int)fd);
}

static void build_owner_map(void)
{
    long fd = lp_open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;

    char buf[8192];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;

        for (long off = 0; off < n; ) {
            char       *rec  = buf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            const char *ent  = rec + DIRENT_NAME;
            off += len;

            if (!numeric(ent))
                continue;

            int  pid = (int)strtol(ent, NULL, 10);
            char path[64], name[64];
            snprintf(path, sizeof(path), "/proc/%d/comm", pid);
            if (proc_read(path, name, sizeof(name)) <= 0)
                strlcpy(name, "?", sizeof(name));

            char *nl = strchr(name, '\n');
            if (nl) *nl = '\0';

            scan_fds(pid, name);
        }
    }
    lp_close((int)fd);
}

static void owner_of(u64 ino, char *out, size_t cap)
{
    for (int i = 0; i < g_owners; i++)
        if (g_owner[i].ino == ino) {
            snprintf(out, cap, "%d/%s", g_owner[i].pid, g_owner[i].name);
            return;
        }
    strlcpy(out, "-", cap);
}

/* ── the socket table ───────────────────────────────────────────── */

typedef struct {
    bool listening_only;
    bool all;
    bool want_pid;
} opts_t;

/* Returns the number of rows printed, or -1 when the file is not there
 * (no IPv6 in this kernel, say - not an error worth a message). */
static long show_file(const char *path, const char *proto, bool udp,
                      const opts_t *o)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return -1;

    char line[512];
    long rows   = 0;
    int  lineno = 0;

    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (++lineno == 1)
            continue;                 /* the column header */

        /* sl local rem st tx:rx tr:when retrnsmt uid timeout inode */
        char *f[12];
        if (split(line, f, 12) < 10)
            continue;

        addr_t local, remote;
        if (!parse_addr(f[1], &local) || !parse_addr(f[2], &remote))
            continue;

        u32 st = hex_digits(f[3], 2);

        /* A UDP socket has no handshake and so no real state. The
         * kernel writes 1 once connect() has pinned a peer to it and 7
         * otherwise; "unconnected" is what that second case means, and
         * calling it CLOSE - which is what the number says - reads as
         * if a working socket were dead. */
        const char *state = udp
            ? (st == 1 ? "ESTABLISHED" : "UNCONNECTED")
            : tcp_state(st);

        bool is_listen = udp ? (st != 1) : (st == 10);
        bool is_estab  = (st == 1);

        if (o->listening_only) {
            if (!is_listen) continue;
        } else if (!o->all && !is_listen && !is_estab) {
            continue;
        }

        char lbuf[52], rbuf[52];
        fmt_addr(&local,  lbuf, sizeof(lbuf));
        fmt_addr(&remote, rbuf, sizeof(rbuf));

        if (o->want_pid) {
            char who[32];
            owner_of((u64)strtol(f[9], NULL, 10), who, sizeof(who));
            printf("%-5s %-*s %-*s %-11s %s\n", proto,
                   ADDRW, lbuf, ADDRW, rbuf, state, who);
        } else {
            printf("%-5s %-*s %-*s %s\n", proto,
                   ADDRW, lbuf, ADDRW, rbuf, state);
        }
        rows++;
    }

    lp_close((int)fd);
    return rows;
}

static int show_sockets(bool tcp, bool udp, const opts_t *o)
{
    if (o->want_pid)
        build_owner_map();

    if (o->want_pid)
        printf("%-5s %-*s %-*s %-11s %s\n", "proto",
               ADDRW, "local address", ADDRW, "foreign address",
               "state", "pid/program");
    else
        printf("%-5s %-*s %-*s %s\n", "proto",
               ADDRW, "local address", ADDRW, "foreign address", "state");

    int any = 0;

    if (tcp) {
        if (show_file("/proc/net/tcp",  "tcp",  false, o) >= 0) any++;
        if (show_file("/proc/net/tcp6", "tcp6", false, o) >= 0) any++;
    }
    if (udp) {
        if (show_file("/proc/net/udp",  "udp",  true, o) >= 0) any++;
        if (show_file("/proc/net/udp6", "udp6", true, o) >= 0) any++;
    }

    if (!any) {
        dprintf(STDERR_FILENO,
                "netstat: cannot read /proc/net - is /proc mounted?\n");
        dprintf(STDERR_FILENO, "         try 'mount -t proc proc /proc'\n");
        return 1;
    }

    if (o->want_pid && g_owner_full)
        dprintf(STDERR_FILENO,
                "netstat: more than %d open sockets on this machine, so some "
                "rows show '-'\n", OWNER_MAX);
    return 0;
}

/* ── -r ─────────────────────────────────────────────────────────── */

/* The route flags worth showing. 0x1 up, 0x2 via a gateway, 0x4 to a
 * single host rather than a network. */
static void route_flags(u32 fl, char *out)
{
    int o = 0;
    if (fl & 0x1) out[o++] = 'U';
    if (fl & 0x2) out[o++] = 'G';
    if (fl & 0x4) out[o++] = 'H';
    if (!o)       out[o++] = '-';
    out[o] = '\0';
}

static int show_routes(void)
{
    long fd = lp_open("/proc/net/route", O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "netstat: cannot read /proc/net/route\n");
        return 1;
    }

    printf("%-16s %-16s %-16s %-6s %s\n",
           "destination", "gateway", "netmask", "flags", "interface");

    char line[512];
    int  lineno = 0;

    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (++lineno == 1)
            continue;

        /* Iface Destination Gateway Flags RefCnt Use Metric Mask ... */
        char *f[12];
        if (split(line, f, 12) < 8)
            continue;

        char dst[16], gw[16], mask[16], flags[8];
        u32  d = (u32)strtol(f[1], NULL, 16);   /* already network order */
        ipv4_format(d, dst);
        ipv4_format((u32)strtol(f[2], NULL, 16), gw);
        ipv4_format((u32)strtol(f[7], NULL, 16), mask);
        route_flags((u32)strtol(f[3], NULL, 16), flags);

        printf("%-16s %-16s %-16s %-6s %s\n",
               d == 0 ? "default" : dst, gw, mask, flags, f[0]);
    }

    lp_close((int)fd);
    return 0;
}

/* ── -i ─────────────────────────────────────────────────────────── */

static int show_ifaces(void)
{
    long fd = lp_open("/proc/net/dev", O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "netstat: cannot read /proc/net/dev\n");
        return 1;
    }

    printf("%-10s %10s %6s %6s %10s %6s %6s %s\n",
           "interface", "rx-packets", "rx-err", "rx-drop",
           "tx-packets", "tx-err", "tx-drop", "state");

    char line[512];
    int  lineno = 0;

    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (++lineno <= 2)
            continue;                 /* two header lines */

        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = '\0';

        char *name = line;
        while (*name == ' ') name++;

        /* rx: bytes packets errs drop fifo frame compressed multicast
         * tx: bytes packets errs drop fifo colls carrier compressed */
        char *f[16];
        int   n = split(colon + 1, f, 16);
        if (n < 12)
            continue;

        printf("%-10s %10ld %6ld %6ld %10ld %6ld %6ld %s\n",
               name,
               strtol(f[1], NULL, 10), strtol(f[2], NULL, 10),
               strtol(f[3], NULL, 10),
               strtol(f[9], NULL, 10), strtol(f[10], NULL, 10),
               strtol(f[11], NULL, 10),
               net_if_is_up(name) ? "up" : "down");
    }

    lp_close((int)fd);
    return 0;
}

/* ── -s ─────────────────────────────────────────────────────────── */

/* /proc/net/snmp is pairs of lines: one naming the counters, the next
 * holding them, both prefixed with the protocol. So a counter is found
 * by its position in the first line, read out of the second. Doing it
 * that way rather than by fixed column means a kernel that grows a new
 * counter in the middle does not silently shift every number. */
static const char *snmp_line(const char *buf, const char *proto, int which)
{
    size_t plen = strlen(proto);
    int    seen = 0;

    for (const char *p = buf; p && *p; ) {
        if (strncmp(p, proto, plen) == 0 && p[plen] == ':') {
            if (seen++ == which)
                return p + plen + 1;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return NULL;
}

static const char *snmp_token(const char *line, int idx, size_t *len)
{
    for (const char *p = line; *p && *p != '\n'; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;

        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;

        if (idx-- == 0) {
            *len = (size_t)(p - start);
            return start;
        }
    }
    return NULL;
}

static long snmp_get(const char *buf, const char *proto, const char *key)
{
    const char *head = snmp_line(buf, proto, 0);
    const char *vals = snmp_line(buf, proto, 1);
    if (!head || !vals)
        return -1;

    size_t klen = strlen(key), len;
    for (int i = 0; ; i++) {
        const char *t = snmp_token(head, i, &len);
        if (!t)
            return -1;
        if (len == klen && strncmp(t, key, klen) == 0) {
            const char *v = snmp_token(vals, i, &len);
            return v ? strtol(v, NULL, 10) : -1;
        }
    }
}

static void snmp_row(const char *buf, const char *proto, const char *key,
                     const char *label)
{
    long v = snmp_get(buf, proto, key);
    if (v < 0)
        return;                       /* this kernel does not count it */
    printf("    %-24s %ld\n", label, v);
}

static int show_stats(void)
{
    static char buf[8192];

    if (proc_read("/proc/net/snmp", buf, sizeof(buf)) <= 0) {
        dprintf(STDERR_FILENO, "netstat: cannot read /proc/net/snmp\n");
        return 1;
    }

    printf("ip:\n");
    snmp_row(buf, "Ip", "InReceives",      "packets received");
    snmp_row(buf, "Ip", "OutRequests",     "packets sent");
    snmp_row(buf, "Ip", "ForwDatagrams",   "packets forwarded");
    snmp_row(buf, "Ip", "InDiscards",      "packets dropped in");
    snmp_row(buf, "Ip", "OutDiscards",     "packets dropped out");
    snmp_row(buf, "Ip", "InAddrErrors",    "not for this address");

    printf("icmp:\n");
    snmp_row(buf, "Icmp", "InMsgs",        "messages received");
    snmp_row(buf, "Icmp", "OutMsgs",       "messages sent");
    snmp_row(buf, "Icmp", "InErrors",      "bad messages in");
    snmp_row(buf, "Icmp", "InEchos",       "pings answered");

    printf("tcp:\n");
    snmp_row(buf, "Tcp", "ActiveOpens",    "connections started");
    snmp_row(buf, "Tcp", "PassiveOpens",   "connections accepted");
    snmp_row(buf, "Tcp", "AttemptFails",   "connections refused");
    snmp_row(buf, "Tcp", "CurrEstab",      "established now");
    snmp_row(buf, "Tcp", "InSegs",         "segments received");
    snmp_row(buf, "Tcp", "OutSegs",        "segments sent");
    snmp_row(buf, "Tcp", "RetransSegs",    "segments resent");
    snmp_row(buf, "Tcp", "OutRsts",        "resets sent");

    printf("udp:\n");
    snmp_row(buf, "Udp", "InDatagrams",    "datagrams received");
    snmp_row(buf, "Udp", "OutDatagrams",   "datagrams sent");
    snmp_row(buf, "Udp", "NoPorts",        "to a port nobody had");
    snmp_row(buf, "Udp", "InErrors",       "receive errors");
    snmp_row(buf, "Udp", "RcvbufErrors",   "dropped, buffer full");
    return 0;
}

/* ── ───────────────────────────────────────────────────────────── */

static void usage(void)
{
    printf("usage: netstat [-latunp] [-r] [-i] [-s]\n");
    printf("  -l  only what is listening   -a  every socket, any state\n");
    printf("  -t  tcp only                 -u  udp only\n");
    printf("  -p  which process holds it (walks /proc, so it is slower)\n");
    printf("  -n  accepted and ignored: this never looks a name up\n");
    printf("  -r  the routing table        -i  interfaces and counters\n");
    printf("  -s  per-protocol statistics\n\n");
    printf("with no options: what is listening, and what is connected\n");
}

int main(int argc, char **argv)
{
    opts_t o = { false, false, false };
    bool tcp = false, udp = false;
    bool routes = false, ifaces = false, stats = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') {
            dprintf(STDERR_FILENO,
                    "netstat: %s: this takes options, not names - "
                    "try 'netstat -h'\n", a);
            return 2;
        }
        /* Bundled options, so -ltnp works the way people type it. */
        for (const char *c = a + 1; *c; c++) {
            switch (*c) {
            case 'l': o.listening_only = true; break;
            case 'a': o.all            = true; break;
            case 't': tcp              = true; break;
            case 'u': udp              = true; break;
            case 'p': o.want_pid       = true; break;
            case 'n': break;          /* always numeric anyway */
            case 'r': routes = true;   break;
            case 'i': ifaces = true;   break;
            case 's': stats  = true;   break;
            case 'h': usage(); return 0;
            default:
                dprintf(STDERR_FILENO,
                        "netstat: -%c is not an option - try 'netstat -h'\n",
                        *c);
                return 2;
            }
        }
    }

    if (routes || ifaces || stats) {
        int rc = 0;
        if (routes) rc |= show_routes();
        if (ifaces) rc |= show_ifaces();
        if (stats)  rc |= show_stats();
        return rc ? 1 : 0;
    }

    if (!tcp && !udp)
        tcp = udp = true;

    return show_sockets(tcp, udp, &o);
}
