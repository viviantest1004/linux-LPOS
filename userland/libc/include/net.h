/* net.h - sockets and network interface configuration.
 *
 * IPv4 only. The kernel does TCP/IP, so all we write are the socket
 * wrappers and the interface ioctls. */
#ifndef _LP_NET_H
#define _LP_NET_H

#include "types.h"

/* Address families and socket types */
#define AF_INET         2
#define AF_PACKET      17
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3

#define IPPROTO_IP      0
#define IPPROTO_TCP     6
#define IPPROTO_UDP    17

/* setsockopt */
#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_BROADCAST    6
#define SO_BINDTODEVICE 25
#define SO_RCVTIMEO_NEW 66
#define SO_SNDTIMEO_NEW 67

/* How many times to retransmit a SYN before connect() gives up.
 *
 * There is no timeout on connect() and no poll() here to build one out
 * of, so this is the only lever. The default is six retries with the
 * interval doubling each time, which is a little over two minutes of a
 * program looking like it has hung. Three retries is about twenty
 * seconds, measured - long enough for a slow WiFi link to answer, and
 * short enough that a person waits for it rather than reaching for the
 * power. It matters most when a firewall is dropping the packets,
 * because then nothing ever answers at all. */
#define IPPROTO_TCP_SYNCNT 7

/* Interface ioctls */
#define SIOCGIFFLAGS    0x8913
#define SIOCSIFFLAGS    0x8914
#define SIOCGIFADDR     0x8915
#define SIOCSIFADDR     0x8916
#define SIOCSIFNETMASK  0x891C
#define SIOCGIFHWADDR   0x8927
#define SIOCGIFINDEX    0x8933
#define SIOCADDRT       0x890B
#define SIOCDELRT       0x890C

#define IFF_UP          0x1
#define IFF_RUNNING     0x40

#define IFNAMSIZ        16

/* struct sockaddr_in - exactly the layout the kernel expects */
typedef struct {
    u16 sin_family;
    u16 sin_port;       /* network byte order */
    u32 sin_addr;       /* network byte order */
    u8  sin_zero[8];
} sockaddr_in_t;

/* Byte order. AArch64 is little endian, so these really do swap. */
static inline u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }
static inline u32 htonl(u32 v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
static inline u32 ntohl(u32 v) { return htonl(v); }

/* ── Sockets ── */
long lp_socket(int family, int type, int proto);
long lp_bind(int fd, const void *addr, u32 addrlen);
long lp_connect(int fd, const void *addr, u32 addrlen);
long lp_sendto(int fd, const void *buf, size_t n, int flags,
               const void *addr, u32 addrlen);
long lp_recvfrom(int fd, void *buf, size_t n, int flags,
                 void *addr, u32 *addrlen);
long lp_setsockopt(int fd, int level, int opt, const void *val, u32 len);

/* ── Serving ──
 *
 * Nothing here listened for a connection until httpd, which is why
 * these two arrived so late. `backlog` is how many finished handshakes
 * the kernel will hold before it starts refusing; anything past a
 * couple of dozen only makes a queue nobody gets to in time. */
#define SOMAXCONN 128
long lp_listen(int fd, int backlog);
/* Half-close: 0 stops reading, 1 stops writing, 2 both. */
long lp_shutdown(int fd, int how);

/* Take the next connection off the queue. `addr` may be NULL when the
 * caller does not care who it is; when it is not, *addrlen must hold
 * the size of the buffer going in and comes back with the size used.
 *
 * This is accept4 on both machines, never accept. aarch64 uses the
 * asm-generic syscall table, which has no bare accept at all - accept4
 * with flags 0 is the only accept it has. Rather than call one thing on
 * one machine and another on the other, both go through accept4, and
 * the flags argument is then there for free: LP_SOCK_CLOEXEC on a
 * server that execs something is worth having. */
#define LP_SOCK_NONBLOCK 04000
#define LP_SOCK_CLOEXEC  02000000
long lp_accept(int fd, void *addr, u32 *addrlen, int flags);

/* ── Waiting on file descriptors ──
 *
 * Enough poll to wait on a handful of descriptors with a timeout.
 * timeout_ms below zero waits forever. Returns how many are ready, 0 on
 * timeout, or -errno; -EINTR when a signal arrived, which a caller in a
 * loop should treat as "nothing happened yet" rather than as a failure.
 *
 * This is ppoll underneath - see the note in syscall-arm64.h. */
#define LP_POLLIN   0x001
#define LP_POLLOUT  0x004
#define LP_POLLERR  0x008
#define LP_POLLHUP  0x010
#define LP_POLLNVAL 0x020

typedef struct {
    int fd;
    s16 events;
    s16 revents;
} lp_pollfd_t;

long lp_poll(lp_pollfd_t *fds, unsigned n, int timeout_ms);

/* ── Interface configuration ── */
long net_if_up(const char *ifname);
long net_if_down(const char *ifname);
bool net_if_is_up(const char *ifname);
long net_if_index(const char *ifname, int *index_out);
long net_if_hwaddr(const char *ifname, u8 mac[6]);
/* Read the current IPv4 address. Negative if it has none. */
long net_get_addr(const char *ifname, u32 *addr_be);
long net_set_addr(const char *ifname, u32 addr_be);      /* network order */
long net_set_netmask(const char *ifname, u32 mask_be);
long net_add_default_route(const char *ifname, u32 gw_be);
/* Remove one default route that goes out through ifname, whatever its
 * gateway. Negative (-ESRCH) when there is none left. */
long net_del_default_route(const char *ifname);

/* ── Names ── */
/* Resolve a host name to an IPv4 address in network byte order, by
 * asking the nameserver /etc/resolv.conf points at. A numeric address is
 * returned unchanged, so callers never have to tell the two apart.
 * 0 means it could not be resolved - no nameserver, or no answer. */
u32  net_resolve(const char *host);

/* ── HTTP ── */
/* Fetch an http:// or https:// URL into a file. Returns the bytes
 * written, or -1 with a message on stderr.
 *
 * TLS is done here, in this process, against the root certificates
 * compiled into this libc - see tls.c. Redirects are followed, which
 * matters because a download from a release page is always one. */
long net_http_get(const char *url, const char *dest);

/* POST a body and throw the reply away, or keep it in `dest`.
 * `dest` may be NULL when only "did it arrive" matters - a heartbeat,
 * for instance. Returns the reply's size, or -1. https:// works the
 * same way it does for net_http_get. */
long net_http_post(const char *url, const char *body, const char *dest);

/* "192.168.0.1" -> u32 in network order. false on failure. */
bool ipv4_parse(const char *s, u32 *out_be);
/* u32 in network order -> "192.168.0.1". buf must hold 16 bytes. */
void ipv4_format(u32 addr_be, char *buf);

#endif /* _LP_NET_H */
