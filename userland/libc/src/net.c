/* net.c - socket wrappers and interface configuration.
 *
 * Interfaces are configured through the old ioctl interface rather than
 * netlink. We only need an IPv4 address, a netmask, the flags and a
 * default route; ioctl covers that in a fraction of the code. netlink
 * starts to matter with IPv6 or several addresses per interface. */
#include "net.h"
#include "syscall.h"
#include "string.h"
#include "unistd.h"
#include "stdio.h"
#include "stdlib.h"
#include "tls.h"

long lp_socket(int family, int type, int proto)
{
    return sys_call3(SYS_socket, family, type, proto);
}

long lp_bind(int fd, const void *addr, u32 addrlen)
{
    return sys_call3(SYS_bind, fd, (long)addr, (long)addrlen);
}

long lp_connect(int fd, const void *addr, u32 addrlen)
{
    return sys_call3(SYS_connect, fd, (long)addr, (long)addrlen);
}

long lp_sendto(int fd, const void *buf, size_t n, int flags,
               const void *addr, u32 addrlen)
{
    return sys_call6(SYS_sendto, fd, (long)buf, (long)n, flags,
                     (long)addr, (long)addrlen);
}

long lp_recvfrom(int fd, void *buf, size_t n, int flags,
                 void *addr, u32 *addrlen)
{
    return sys_call6(SYS_recvfrom, fd, (long)buf, (long)n, flags,
                     (long)addr, (long)addrlen);
}

long lp_setsockopt(int fd, int level, int opt, const void *val, u32 len)
{
    return sys_call5(SYS_setsockopt, fd, level, opt, (long)val, (long)len);
}

/* Close one direction of a socket while leaving the other open.
 *
 * `nc host 80 < request` has to say "that is all I am sending" or the
 * server waits forever for a request that has already finished. how is
 * 0 read, 1 write, 2 both. */
long lp_shutdown(int fd, int how)
{
    return sys_call2(SYS_shutdown, fd, how);
}

long lp_listen(int fd, int backlog)
{
    return sys_call2(SYS_listen, fd, backlog);
}

/* accept4, not accept, on both machines.
 *
 * aarch64 uses the asm-generic system call table, and that table has no
 * accept at all - only accept4, which is accept plus a flags word. So
 * there is no shared "call accept" to write; one of the two machines
 * has to use accept4 regardless. Both do, so the flags argument is
 * available everywhere and there is a single code path to think about.
 * Passing 0 for flags is exactly accept. */
long lp_accept(int fd, void *addr, u32 *addrlen, int flags)
{
    return sys_call4(SYS_accept4, fd, (long)addr, (long)addrlen, flags);
}

/* ppoll, because arm64 has no bare poll - see syscall-arm64.h.
 *
 * ppoll takes a struct timespec rather than a count of milliseconds,
 * and a NULL one means "wait forever", which is how a negative timeout
 * is expressed here. The last two arguments are the signal mask and its
 * size; passing NULL leaves the mask alone, which is what a caller that
 * simply wants to wait wants.
 *
 * The kernel may update the timespec on some architectures, so it is a
 * local rather than a caller's buffer. */
long lp_poll(lp_pollfd_t *fds, unsigned n, int timeout_ms)
{
    s64 ts[2];
    long tsp = 0;

    if (timeout_ms >= 0) {
        ts[0] = timeout_ms / 1000;
        ts[1] = (s64)(timeout_ms % 1000) * 1000000;
        tsp = (long)ts;
    }
    return sys_call5(SYS_ppoll, (long)fds, (long)n, tsp, 0, 0);
}

/* ── Interface ioctls ─────────────────────────────────────────────
 *
 * struct ifreq is a 16-byte name followed by a union. The union overlays
 * a sockaddr, the flags, the index and more. We lay it out ourselves
 * rather than pulling in kernel headers.
 *
 *   offset 0..15   ifr_name
 *   offset 16..    ifr_addr / ifr_flags / ifr_ifindex / ifr_hwaddr
 */
#define IFREQ_SIZE   40
#define IFR_UNION    16

typedef u8 ifreq_t[IFREQ_SIZE];

static void ifreq_init(ifreq_t r, const char *ifname)
{
    memset(r, 0, IFREQ_SIZE);
    strlcpy((char *)r, ifname, IFNAMSIZ);
}

/* Open an AF_INET socket to carry the ioctl. The socket is never used
 * to communicate - it is just the handle the ioctl rides on. */
static long if_ioctl(unsigned long req, ifreq_t r)
{
    long fd = lp_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return fd;

    long rc = sys_call3(SYS_ioctl, (long)fd, (long)req, (long)r);
    lp_close((int)fd);
    return rc;
}

static long if_set_flags(const char *ifname, u16 set, u16 clear)
{
    ifreq_t r;
    ifreq_init(r, ifname);

    long rc = if_ioctl(SIOCGIFFLAGS, r);
    if (rc < 0)
        return rc;

    u16 flags = *(u16 *)(r + IFR_UNION);
    flags = (u16)((flags | set) & ~clear);
    *(u16 *)(r + IFR_UNION) = flags;

    return if_ioctl(SIOCSIFFLAGS, r);
}

long net_if_up(const char *ifname)
{
    return if_set_flags(ifname, IFF_UP | IFF_RUNNING, 0);
}

long net_if_down(const char *ifname)
{
    return if_set_flags(ifname, 0, IFF_UP);
}

bool net_if_is_up(const char *ifname)
{
    ifreq_t r;
    ifreq_init(r, ifname);
    if (if_ioctl(SIOCGIFFLAGS, r) < 0)
        return false;
    return (*(u16 *)(r + IFR_UNION) & IFF_UP) != 0;
}

long net_if_index(const char *ifname, int *index_out)
{
    ifreq_t r;
    ifreq_init(r, ifname);

    long rc = if_ioctl(SIOCGIFINDEX, r);
    if (rc < 0)
        return rc;

    if (index_out)
        *index_out = *(int *)(r + IFR_UNION);
    return 0;
}

long net_if_hwaddr(const char *ifname, u8 mac[6])
{
    ifreq_t r;
    ifreq_init(r, ifname);

    long rc = if_ioctl(SIOCGIFHWADDR, r);
    if (rc < 0)
        return rc;

    /* ifr_hwaddr is a sockaddr: 2 bytes of sa_family, then the data. */
    memcpy(mac, r + IFR_UNION + 2, 6);
    return 0;
}

/* Setting an address or netmask puts a sockaddr_in in the union. */
static long if_set_inaddr(const char *ifname, unsigned long req, u32 addr_be)
{
    ifreq_t r;
    ifreq_init(r, ifname);

    sockaddr_in_t *sa = (sockaddr_in_t *)(r + IFR_UNION);
    sa->sin_family = AF_INET;
    sa->sin_port   = 0;
    sa->sin_addr   = addr_be;

    return if_ioctl(req, r);
}

long net_get_addr(const char *ifname, u32 *addr_be)
{
    ifreq_t r;
    ifreq_init(r, ifname);

    long rc = if_ioctl(SIOCGIFADDR, r);
    if (rc < 0)
        return rc;

    sockaddr_in_t *sa = (sockaddr_in_t *)(r + IFR_UNION);
    if (addr_be) *addr_be = sa->sin_addr;
    return 0;
}

long net_set_addr(const char *ifname, u32 addr_be)
{
    return if_set_inaddr(ifname, SIOCSIFADDR, addr_be);
}

long net_set_netmask(const char *ifname, u32 mask_be)
{
    return if_set_inaddr(ifname, SIOCSIFNETMASK, mask_be);
}

/* struct rtentry, the kernel's layout (include/uapi/linux/route.h),
 * written as a struct so the compiler places every field for the
 * machine: rt_dev is at 88 on a 64-bit kernel and 68 on a 32-bit one.
 * The offsets used to be written out by hand, with rt_dev at 72 - which
 * is rt_pad4 - so the device was never named and the kernel chose one
 * by the gateway: with a dead link and a live one on the same subnet it
 * chose the dead one, and the new default route went into it. */
typedef struct {
    unsigned long  rt_pad1;
    sockaddr_in_t  rt_dst;
    sockaddr_in_t  rt_gateway;
    sockaddr_in_t  rt_genmask;
    unsigned short rt_flags;
    short          rt_pad2;
    unsigned long  rt_pad3;
    void          *rt_pad4;
    short          rt_metric;
    const char    *rt_dev;
    unsigned long  rt_mtu;
    unsigned long  rt_window;
    unsigned short rt_irtt;
} rtentry_t;

#define RTF_UP       0x0001
#define RTF_GATEWAY  0x0002

long net_add_default_route(const char *ifname, u32 gw_be)
{
    rtentry_t rt;
    memset(&rt, 0, sizeof(rt));

    /* The default route: destination 0.0.0.0, netmask 0.0.0.0, via the gateway. */
    rt.rt_dst.sin_family     = AF_INET;
    rt.rt_genmask.sin_family = AF_INET;
    rt.rt_gateway.sin_family = AF_INET;
    rt.rt_gateway.sin_addr   = gw_be;
    rt.rt_flags = RTF_UP | RTF_GATEWAY;
    rt.rt_dev   = ifname;

    long fd = lp_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return fd;

    long rc = sys_call3(SYS_ioctl, (long)fd, SIOCADDRT, (long)&rt);
    lp_close((int)fd);
    return rc;
}

long net_del_default_route(const char *ifname)
{
    rtentry_t rt;
    memset(&rt, 0, sizeof(rt));

    /* 0.0.0.0/0 on that device. No RTF_GATEWAY: the kernel then matches
     * the route by destination and device alone, so the caller need not
     * remember which gateway it went through. */
    rt.rt_dst.sin_family     = AF_INET;
    rt.rt_genmask.sin_family = AF_INET;
    rt.rt_flags = RTF_UP;
    rt.rt_dev   = ifname;

    long fd = lp_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return fd;

    long rc = sys_call3(SYS_ioctl, (long)fd, SIOCDELRT, (long)&rt);
    lp_close((int)fd);
    return rc;
}

/* ── Address strings ───────────────────────────────────────────── */

bool ipv4_parse(const char *s, u32 *out_be)
{
    u32 octets[4];

    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return false;

        u32 v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (u32)(*s++ - '0');
            if (v > 255)
                return false;
        }
        octets[i] = v;

        if (i < 3) {
            if (*s != '.') return false;
            s++;
        }
    }
    if (*s != '\0')
        return false;

    /* Network byte order: the first octet ends up in the lowest byte. */
    *out_be = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return true;
}

void ipv4_format(u32 addr_be, char *buf)
{
    u8 *o = (u8 *)&addr_be;
    char *p = buf;

    for (int i = 0; i < 4; i++) {
        u32 v = o[i];
        if (v >= 100) *p++ = (char)('0' + v / 100);
        if (v >= 10)  *p++ = (char)('0' + (v / 10) % 10);
        *p++ = (char)('0' + v % 10);
        if (i < 3) *p++ = '.';
    }
    *p = '\0';
}

/* ── Resolving a name ─────────────────────────────────────────────────
 *
 * A full resolver is a large thing: caching, search domains, CNAME
 * chains, TCP fallback for long answers, IPv6. None of that is needed to
 * turn one host name into one address, which is the only question anyone
 * on this machine asks. So this sends a single A query to the first
 * nameserver in /etc/resolv.conf and reads the first A record back.
 *
 * A numeric address is returned as it stands, so every caller can take a
 * name or an address without checking which it has. */

/* First nameserver from /etc/resolv.conf. 0 if we cannot read one. */
static u32 read_nameserver(void)
{
    char buf[512];
    long fd = lp_open("/etc/resolv.conf", O_RDONLY, 0);
    if (fd < 0)
        return 0;
    long n = lp_read((int)fd, buf, sizeof(buf) - 1);
    lp_close((int)fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';

    /* Look for a "nameserver 1.2.3.4" line. */
    for (char *p = buf; *p; ) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = '\0';

        static const char key[] = "nameserver";
        if (strncmp(line, key, sizeof(key) - 1) != 0)
            continue;
        char *ip = line + sizeof(key) - 1;
        while (*ip == ' ' || *ip == '\t') ip++;

        u32 addr = 0;
        if (ipv4_parse(ip, &addr))
            return addr;        /* network byte order */
    }
    return 0;
}

/* Encode a name for a DNS query: "a.b.com" -> 1'a' 1'b' 3'c''o''m' 0
 * Returns the bytes written, or 0 if it would not fit. */
static size_t encode_name(u8 *out, size_t cap, const char *host)
{
    size_t o = 0;
    const char *p = host;

    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        size_t len = (size_t)(dot - p);
        if (len == 0 || len > 63 || o + len + 1 >= cap)
            return 0;
        out[o++] = (u8)len;
        memcpy(out + o, p, len);
        o += len;
        p = (*dot == '.') ? dot + 1 : dot;
    }
    if (o + 1 > cap)
        return 0;
    out[o++] = 0;
    return o;
}

/* Name to IPv4. The address in network order, or 0 on failure. */
u32 net_resolve(const char *host)
{
    /* Already a numeric address? Use it as is. */
    u32 direct = 0;
    if (ipv4_parse(host, &direct))
        return direct;

    u32 ns = read_nameserver();
    if (ns == 0)
        return 0;               /* no nameserver: run dhcp first */

    u8     query[512];
    size_t qlen = 0;

    /* 12-byte header: ID, flags (recursion desired), one question.
     *
     * The ID used to be the pid. That is the only thing distinguishing a
     * real answer from a forged one here, and /etc/rc caps pid_max at
     * 4096, so the search space was under twelve bits - and a daemon
     * started at boot has a small, stable pid across reboots. Anything
     * that could get a UDP packet to the port could answer first with
     * whatever address it liked, for every name this machine looks up:
     * ntp, beacon, pkg, and update, which has no signature check.
     *
     * getrandom, with the pid only as a fallback if that syscall is not
     * there. */
    u16 id;
    if (lp_getrandom(&id, sizeof id, 0) != (long)sizeof id)
        id = (u16)((lp_getpid() * 2654435761u) >> 13);
    query[qlen++] = (u8)(id >> 8);   query[qlen++] = (u8)id;
    query[qlen++] = 0x01;            query[qlen++] = 0x00;   /* RD */
    query[qlen++] = 0x00;            query[qlen++] = 0x01;   /* QDCOUNT=1 */
    query[qlen++] = 0x00;            query[qlen++] = 0x00;
    query[qlen++] = 0x00;            query[qlen++] = 0x00;
    query[qlen++] = 0x00;            query[qlen++] = 0x00;

    size_t nlen = encode_name(query + qlen, sizeof(query) - qlen - 4, host);
    if (nlen == 0)
        return 0;
    qlen += nlen;
    query[qlen++] = 0x00; query[qlen++] = 0x01;   /* QTYPE = A */
    query[qlen++] = 0x00; query[qlen++] = 0x01;   /* QCLASS = IN */

    long fd = lp_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    /* struct __kernel_sock_timeval { s64 tv_sec; s64 tv_usec; } */
    s64 tv[2] = { 3, 0 };
    lp_setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof(tv));

    sockaddr_in_t to = { 0 };
    to.sin_family = AF_INET;
    to.sin_port   = htons(53);
    to.sin_addr   = ns;

    /* connect() the socket so the kernel drops datagrams from anyone
     * but the nameserver we asked. Without it the reply was accepted
     * from any source address at all. */
    if (lp_connect((int)fd, &to, sizeof(to)) < 0) {
        lp_close((int)fd);
        return 0;
    }

    u32 result = 0;
    if (lp_sendto((int)fd, query, qlen, 0, &to, sizeof(to)) > 0) {
        u8   resp[512];
        long n = lp_recvfrom((int)fd, resp, sizeof(resp), 0, NULL, NULL);

        /* Skip the header and the question, then walk the answers. A name
         * compression pointer (0xC0) is 2 bytes; otherwise skip the label. */
        /* The ID must match, it must be a response (QR bit), and the
         * question echoed back must be the one we asked - a reply that
         * answers a different name is not an answer to this query. */
        bool same_question =
            n > 12 + (long)nlen + 4 &&
            memcmp(resp + 12, query + 12, nlen + 4) == 0;

        if (n > 12 && resp[0] == (u8)(id >> 8) && resp[1] == (u8)id &&
            (resp[2] & 0x80) && same_question) {
            u16 ancount = (u16)((resp[6] << 8) | resp[7]);
            long off = 12;

            /* The name in the question section */
            while (off < n && resp[off] != 0) {
                if ((resp[off] & 0xC0) == 0xC0) { off += 2; goto qtype; }
                off += resp[off] + 1;
            }
            off += 1;
qtype:
            off += 4;               /* QTYPE + QCLASS */

            for (u16 i = 0; i < ancount && off + 12 <= n; i++) {
                if ((resp[off] & 0xC0) == 0xC0) {
                    off += 2;
                } else {
                    while (off < n && resp[off] != 0) off += resp[off] + 1;
                    off += 1;
                }
                if (off + 10 > n) break;
                u16 type   = (u16)((resp[off] << 8) | resp[off + 1]);
                u16 rdlen  = (u16)((resp[off + 8] << 8) | resp[off + 9]);
                off += 10;
                if (type == 1 && rdlen == 4 && off + 4 <= n) {
                    memcpy(&result, resp + off, 4);   /* already network order */
                    break;
                }
                off += rdlen;
            }
        }
    }

    lp_close((int)fd);
    return result;
}

/* ── Fetching a file, over HTTP or HTTPS ──────────────────────────────
 *
 * GET or POST, one connection, HTTP/1.1 with Connection: close.
 *
 * 1.1 and not 1.0, which is what this used to send. The reason is not
 * taste: raw.githubusercontent.com answers an HTTP/1.0 request with
 *
 *     HTTP/1.1 426 Upgrade Required
 *
 * and so does everything else behind that kind of front end. A client
 * that speaks 1.0 cannot fetch a file from GitHub at all, which is
 * where the packages are. The price of 1.1 is that the server may send
 * the body in chunks instead of giving a length up front, so there is a
 * decoder for that below - about forty lines, and not optional.
 *
 * https:// works here, in this process, with the certificate checked
 * against the roots compiled into this libc (tls.c, tls-roots.c). It
 * used to be handed to python3 on the data partition, which meant that
 * fetching anything over TLS needed a Python that could only be
 * installed by fetching it over TLS. That is not a limitation, it is a
 * system that cannot start, and it is why `pkg` and `apt` did not work
 * on a freshly written card.
 *
 * Redirects are followed, up to five. Not a nicety: every download from
 * a GitHub release is a redirect to a storage host, so a client that
 * does not follow one cannot fetch a package at all.
 */

/* Split "http://host[:port]/path" or the https:// form apart.
 * false if it is neither. */
static bool url_split(const char *url, char *host, size_t hsize,
                      int *port, char *path, size_t psize, bool *tls)
{
    const char *p;

    if (strncmp(url, "https://", 8) == 0) {
        *tls  = true;
        *port = 443;
        p     = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        *tls  = false;
        *port = 80;
        p     = url + 7;
    } else {
        return false;
    }

    size_t i = 0;
    while (*p && *p != '/' && *p != ':' && i < hsize - 1)
        host[i++] = *p++;
    host[i] = '\0';
    if (i == 0)
        return false;

    if (*p == ':') {
        p++;
        *port = atoi(p);
        while (*p && *p != '/') p++;
    }

    strlcpy(path, *p ? p : "/", psize);
    return true;
}

/* ── one connection, either kind ─────────────────────────────────── */

typedef struct {
    int       fd;
    lp_tls_t *tls;          /* NULL on a plain connection */
} conn_t;

static bool conn_open(conn_t *c, const char *host, int port, bool want_tls)
{
    c->fd  = -1;
    c->tls = NULL;

    u32 addr = net_resolve(host);
    if (addr == 0) {
        dprintf(STDERR_FILENO, "cannot resolve %s\n", host);
        return false;
    }

    long fd = lp_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    s64 tv[2] = { 20, 0 };
    lp_setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof(tv));
    /* Give up on an unanswered SYN in about twenty seconds instead of
     * two minutes. Without this, one unreachable host - or one port the
     * firewall is dropping - makes the whole program look hung. */
    int syn_tries = 3;
    lp_setsockopt((int)fd, IPPROTO_TCP, IPPROTO_TCP_SYNCNT,
                  &syn_tries, sizeof(syn_tries));

    sockaddr_in_t sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((u16)port);
    sa.sin_addr   = addr;

    if (lp_connect((int)fd, &sa, sizeof(sa)) < 0) {
        dprintf(STDERR_FILENO, "cannot connect to %s:%d\n", host, port);
        lp_close((int)fd);
        return false;
    }

    c->fd = (int)fd;
    if (want_tls) {
        c->tls = lp_tls_open(c->fd, host);
        if (!c->tls) {
            lp_close(c->fd);
            c->fd = -1;
            return false;
        }
    }
    return true;
}

static long conn_write(conn_t *c, const void *b, size_t n)
{
    return c->tls ? lp_tls_write(c->tls, b, n) : lp_write(c->fd, b, n);
}

static long conn_read(conn_t *c, void *b, size_t n)
{
    return c->tls ? lp_tls_read(c->tls, b, n) : lp_read(c->fd, b, n);
}

static void conn_close(conn_t *c)
{
    if (c->tls) lp_tls_close(c->tls);
    if (c->fd >= 0) lp_close(c->fd);
    c->tls = NULL;
    c->fd  = -1;
}

/* ── headers ─────────────────────────────────────────────────────── */

static char lower_ch(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Find "Name:" in a set of HTTP headers and hand back what follows.
 * Case-insensitive on the name, because header names are.
 * The match has to start a line: "Content-Length:" must not be found
 * inside "X-Original-Content-Length:". */
static const char *header_find(const char *head, const char *name)
{
    size_t nlen = strlen(name);

    for (const char *p = head; *p; ) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               lower_ch(p[i]) == lower_ch(name[i]))
            i++;
        if (i == nlen) {
            const char *v = p + nlen;
            while (*v == ' ' || *v == '\t') v++;
            return v;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return NULL;
}

/* The number after a header, or -1 when it is not there or is not one. */
static long header_value_long(const char *head, const char *name)
{
    const char *v = header_find(head, name);
    if (!v || *v < '0' || *v > '9')
        return -1;
    return strtol(v, NULL, 10);
}

/* The text after a header, to the end of the line. */
static bool header_value_str(const char *head, const char *name,
                             char *out, size_t size)
{
    const char *v = header_find(head, name);
    if (!v)
        return false;
    size_t i = 0;
    while (v[i] && v[i] != '\r' && v[i] != '\n' && i < size - 1) {
        out[i] = v[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

/* ── chunked transfer encoding ───────────────────────────────────
 *
 * HTTP/1.1 lets a server start sending before it knows how much there
 * is, by cutting the body into pieces each prefixed with its length in
 * hex. It is not optional to understand: a server may use it for any
 * response, and GitHub does.
 *
 * This is a state machine rather than a loop over a whole body because
 * a chunk header can be split across two reads - the size of one read
 * has nothing to do with the size of a chunk - and a decoder that
 * assumes otherwise works on every small file and corrupts large ones.
 */
typedef enum {
    CH_SIZE,        /* reading the hex length line */
    CH_DATA,        /* copying chunk_remain bytes through */
    CH_CRLF,        /* the CRLF that follows a chunk's data */
    CH_TRAILER,     /* headers after the last chunk */
    CH_DONE
} chunk_state_t;

typedef struct {
    chunk_state_t state;
    u64  remain;
    char line[80];
    int  nline;
    bool blank;     /* the trailer line so far is empty */
} chunk_t;

static void chunk_init(chunk_t *c)
{
    c->state  = CH_SIZE;
    c->remain = 0;
    c->nline  = 0;
    c->blank  = true;
}

/* Feed `n` bytes; write the decoded body to `out`.
 * Returns how many body bytes were written, or -1 on a malformed body. */
static long chunk_feed(chunk_t *c, const char *p, long n, int out, bool write_it)
{
    long produced = 0;
    long i = 0;

    while (i < n && c->state != CH_DONE) {
        switch (c->state) {
        case CH_SIZE: {
            char ch = p[i++];
            if (ch == '\n') {
                c->line[c->nline] = '\0';
                /* "1a3f" or "1a3f;name=value" - the extension is
                 * allowed and means nothing to us. */
                u64 v = 0;
                bool any = false;
                for (int k = 0; c->line[k]; k++) {
                    char d = c->line[k];
                    int  digit;
                    if      (d >= '0' && d <= '9') digit = d - '0';
                    else if (d >= 'a' && d <= 'f') digit = d - 'a' + 10;
                    else if (d >= 'A' && d <= 'F') digit = d - 'A' + 10;
                    else break;
                    v = v * 16 + (u64)digit;
                    any = true;
                }
                if (!any)
                    return -1;
                c->nline = 0;
                if (v == 0) {
                    c->state = CH_TRAILER;
                    c->blank = true;
                } else {
                    c->remain = v;
                    c->state  = CH_DATA;
                }
            } else if (ch != '\r' && c->nline < (int)sizeof c->line - 1) {
                c->line[c->nline++] = ch;
            }
            break;
        }
        case CH_DATA: {
            long avail = n - i;
            long take  = (c->remain < (u64)avail) ? (long)c->remain : avail;
            if (write_it)
                lp_write(out, p + i, (size_t)take);
            produced   += take;
            i          += take;
            c->remain  -= (u64)take;
            if (c->remain == 0)
                c->state = CH_CRLF;
            break;
        }
        case CH_CRLF:
            if (p[i] == '\n')
                c->state = CH_SIZE;
            i++;
            break;
        case CH_TRAILER: {
            char ch = p[i++];
            if (ch == '\n') {
                if (c->blank)
                    c->state = CH_DONE;
                c->blank = true;
            } else if (ch != '\r') {
                c->blank = false;
            }
            break;
        }
        case CH_DONE:
            break;
        }
    }
    return produced;
}

/* Does the response say chunked?
 *
 * The value is a list - "gzip, chunked" is legal - and chunked, when it
 * is there at all, is always the last item. So looking for the word
 * anywhere in the value is enough, and a list parser is not needed.
 * Case-insensitive, because header values of this kind are. */
static bool header_says_chunked(const char *head)
{
    const char *v = header_find(head, "Transfer-Encoding:");
    if (!v)
        return false;
    for (; *v && *v != '\r' && *v != '\n'; v++) {
        static const char want[] = "chunked";
        size_t k = 0;
        while (want[k] && lower_ch(v[k]) == want[k])
            k++;
        if (want[k] == '\0')
            return true;
    }
    return false;
}

/* ── one request ─────────────────────────────────────────────────── */

/* Returns the number of body bytes written, or -1.
 *
 * A redirect is not a failure here: `redirect` comes back holding the
 * Location and the return is 0, so the caller can go round again. */
static long http_once(const char *method, const char *url,
                      const char *body, const char *dest,
                      char *redirect, size_t rsize)
{
    char host[128], path[512];
    int  port;
    bool want_tls;

    redirect[0] = '\0';

    if (!url_split(url, host, sizeof(host), &port, path, sizeof(path),
                   &want_tls)) {
        dprintf(STDERR_FILENO,
                "%s: this is not an http:// or https:// URL\n", url);
        return -1;
    }

    conn_t c;
    if (!conn_open(&c, host, port, want_tls))
        return -1;

    char req[768];
    int  rn;
    if (body) {
        rn = snprintf(req, sizeof(req),
                      "%s %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "User-Agent: lpzero\r\n"
                      "Accept-Encoding: identity\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %lu\r\n"
                      "Connection: close\r\n\r\n",
                      method, path, host, (unsigned long)strlen(body));
    } else {
        rn = snprintf(req, sizeof(req),
                      "%s %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "User-Agent: lpzero\r\n"
                      "Accept-Encoding: identity\r\n"
                      "Connection: close\r\n\r\n", method, path, host);
    }
    if (conn_write(&c, req, (size_t)rn) < 0 ||
        (body && conn_write(&c, body, strlen(body)) < 0) ||
        (c.tls && lp_tls_flush(c.tls) < 0)) {
        conn_close(&c);
        return -1;
    }

    /* A caller that only wants to know the request arrived passes no
     * destination - a heartbeat, for instance, where the reply is
     * "200" and nothing else.
     *
     * "-" means standard output. That is what makes `wget -O- <url> |
     * grep ...` work, and that one-liner is most of what anybody does
     * with an HTTP client on a server. Writing to a file first and
     * reading it back needs a writable directory, which a machine whose
     * root is in RAM does not always have where you are standing. */
    bool to_stdout = dest && dest[0] == '-' && dest[1] == '\0';
    long out;
    if (to_stdout)   out = STDOUT_FILENO;
    else if (dest)   out = lp_open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    else             out = lp_open("/dev/null", O_WRONLY, 0);
    if (out < 0) {
        dprintf(STDERR_FILENO, "cannot write %s\n", dest ? dest : "/dev/null");
        conn_close(&c);
        return -1;
    }

    /* Read past the headers. The blank line between them and the body may
     * land anywhere in a read, so we look for it as we go. */
    static char buf[8192];
    bool  in_body = false;
    int   match   = 0;          /* how much of \r\n\r\n we have seen */
    long  written = 0;
    int   status  = 0;
    bool  have_status = false;

    /* The header is collected before it is parsed. The status used to
     * be read as atoi(buf + 9) from whatever the first read returned,
     * which assumed the first read holds at least the status line - a
     * server is free to send it in smaller pieces, and one that wants
     * to can. Twelve bytes or fewer and have_status stayed false, which
     * skipped the status check entirely and handed a 404 page back as a
     * downloaded file; a later read and buf+9 pointed into the middle
     * of a header and gave a nonsense number.
     *
     * 2KB of header, not 512 bytes: a GitHub redirect carries a signed
     * URL in Location that is longer than that on its own, and a header
     * buffer that fills up before Location arrives turns a redirect
     * into "the server said 302" and a failed download. */
    char  head[2048];
    int   nhead = 0;
    long  content_length = -1;
    bool  bad_read = false;
    bool  chunked = false;
    chunk_t chunk;
    chunk_init(&chunk);

    for (;;) {
        long n = conn_read(&c, buf, sizeof(buf));
        if (n < 0) { bad_read = true; break; }
        if (n == 0)
            break;

        long i = 0;
        if (!in_body) {
            for (long k = 0; k < n && nhead < (int)sizeof head - 1; k++)
                head[nhead++] = buf[k];
            head[nhead] = '\0';
            for (; i < n; i++) {
                char ch = buf[i];
                if ((match == 0 || match == 2) && ch == '\r')      match++;
                else if ((match == 1 || match == 3) && ch == '\n') match++;
                else                                               match = (ch == '\r');
                if (match == 4) { i++; in_body = true; break; }
            }

            if (in_body && !have_status) {
                /* "HTTP/1.1 200 OK" - the digits start at byte 9. */
                if (nhead > 12 && strncmp(head, "HTTP/", 5) == 0) {
                    status      = atoi(head + 9);
                    have_status = true;
                }
                content_length = header_value_long(head, "Content-Length:");
                chunked        = header_says_chunked(head);
                if (status >= 300 && status < 400)
                    header_value_str(head, "Location:", redirect, rsize);
            }
        }

        if (in_body && i < n) {
            /* 200 이 아니면 몸통을 쓰지 않는다.
             *
             * 파일로 받을 때는 뒤에서 지우면 됐다. 표준출력으로 보낼
             * 때는 지울 수가 없어서, 401 오류 페이지가 파이프 저쪽의
             * 입력이 되어 버린다 - `wget -O- ... | grep` 이 오류
             * 페이지 안의 글자를 찾아 성공한 것처럼 구는 것이 가장
             * 나쁜 꼴이다. 상태 줄은 몸통보다 먼저 오므로 여기서
             * 이미 알고 있다. */
            bool keep = (!have_status || status == 200);
            if (chunked) {
                long got = chunk_feed(&chunk, buf + i, n - i, (int)out, keep);
                if (got < 0) {
                    dprintf(STDERR_FILENO,
                            "%s: the server's chunked body is malformed\n", url);
                    bad_read = true;
                    break;
                }
                written += got;
                if (chunk.state == CH_DONE)
                    break;
            } else {
                if (keep)
                    lp_write((int)out, buf + i, (size_t)(n - i));
                written += n - i;
            }
        }
    }

    if (!to_stdout)
        lp_close((int)out);
    conn_close(&c);

    if (!have_status) {
        dprintf(STDERR_FILENO,
                "%s: no HTTP status line in the reply\n", url);
        if (dest && !to_stdout) lp_unlink(dest);
        return -1;
    }

    /* A redirect is the caller's business, not a failure. */
    if (status >= 300 && status < 400 && redirect[0]) {
        if (dest && !to_stdout) lp_unlink(dest);
        return 0;
    }

    if (status != 200) {
        dprintf(STDERR_FILENO, "%s: the server said %d\n", url, status);
        if (dest && !to_stdout)
            lp_unlink(dest);
        return -1;
    }

    /* Did the whole body arrive?
     *
     * The loop above ends on any read that returns 0, and a connection
     * that is reset or simply stops mid-body looks exactly like a
     * finished one. Nothing checked, so nothing downstream COULD check:
     * `update` accepted a kernel image cut off after 18% of the file,
     * called it verified, and installed it as the thing the board
     * boots. That is the one failure this machine cannot repair by
     * itself - nothing runs before the GPU firmware, and it cannot be
     * told to try a second file.
     *
     * A server that sends no Content-Length leaves nothing to compare
     * against; that is not this code's fault, but it is worth saying so
     * the caller knows the difference. */
    if (bad_read) {
        dprintf(STDERR_FILENO, "%s: the connection broke mid-transfer\n", url);
        if (dest && !to_stdout) lp_unlink(dest);
        return -1;
    }
    /* A chunked body carries its own end marker - the zero-length chunk
     * - so a truncated one is detectable without a Content-Length, and
     * this is where it gets detected. */
    if (chunked && chunk.state != CH_DONE) {
        dprintf(STDERR_FILENO,
                "%s: the body stopped before its last chunk - discarding it\n",
                url);
        if (dest && !to_stdout) lp_unlink(dest);
        return -1;
    }
    if (!chunked && content_length >= 0 && written != content_length) {
        dprintf(STDERR_FILENO,
                "%s: got %ld bytes, the server said %ld - discarding it\n",
                url, written, content_length);
        if (dest && !to_stdout) lp_unlink(dest);
        return -1;
    }
    if (!chunked && content_length < 0 && dest && !to_stdout)
        dprintf(STDERR_FILENO,
                "%s: the server sent no length, so a short download"
                " cannot be detected here\n", url);

    return written;
}

/* ── redirects ───────────────────────────────────────────────────── */

/* Turn whatever a Location header said into a URL we can fetch.
 *
 * Servers are allowed to send a path instead of a whole URL, and some
 * do. Resolving it needs the URL we asked for, which is why this takes
 * both. Anything that is neither absolute nor rooted at "/" is refused
 * rather than guessed at - a wrong guess here fetches the wrong file
 * and says nothing. */
static bool resolve_location(const char *base, const char *loc,
                             char *out, size_t size)
{
    if (strncmp(loc, "http://", 7) == 0 || strncmp(loc, "https://", 8) == 0) {
        strlcpy(out, loc, size);
        return true;
    }
    if (loc[0] != '/')
        return false;

    /* Keep scheme://host[:port] from the URL we asked for. */
    const char *p = strstr(base, "://");
    if (!p)
        return false;
    p += 3;
    const char *slash = strchr(p, '/');
    size_t prefix = slash ? (size_t)(slash - base) : strlen(base);
    if (prefix + strlen(loc) + 1 > size)
        return false;
    memcpy(out, base, prefix);
    out[prefix] = '\0';
    strlcat(out, loc, size);
    return true;
}

/* One function behind both net_http_get and net_http_post: the two
 * differ by a word in the request line and whether a body follows. */
static long http_do(const char *method, const char *url,
                    const char *body, const char *dest)
{
    char current[1024];
    char next[1024];

    strlcpy(current, url, sizeof current);

    /* Five. Enough for the two or three a real download takes - a
     * release asset goes github.com -> objects.githubusercontent.com -
     * and few enough that a server redirecting to itself stops instead
     * of spinning. */
    for (int hop = 0; hop < 5; hop++) {
        char loc[1024];
        long n = http_once(method, current, body, dest, loc, sizeof loc);
        if (n < 0)
            return -1;
        if (!loc[0])
            return n;

        if (!resolve_location(current, loc, next, sizeof next)) {
            dprintf(STDERR_FILENO,
                    "%s: redirected somewhere this cannot follow: %s\n",
                    current, loc);
            return -1;
        }
        strlcpy(current, next, sizeof current);

        /* A POST that is redirected becomes a GET, which is what every
         * browser does with 302 and 303 and what servers expect. The
         * body is not sent again. */
        if (body) {
            method = "GET";
            body   = NULL;
        }
    }

    dprintf(STDERR_FILENO, "%s: too many redirects\n", url);
    return -1;
}

long net_http_get(const char *url, const char *dest)
{
    return http_do("GET", url, NULL, dest);
}

long net_http_post(const char *url, const char *body, const char *dest)
{
    return http_do("POST", url, body ? body : "", dest);
}
