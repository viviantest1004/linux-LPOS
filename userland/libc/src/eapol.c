/* eapol.c - the packet socket the four-way handshake runs on.
 *
 * See eapol.h for why this is a packet socket and why it is opened
 * before the association is asked for rather than after.
 *
 * There is one thing worth knowing about the structure below. A
 * sockaddr_ll is the same shape on a 32-bit ARM and on a 64-bit one:
 * every member is a fixed-width integer or an array, and there is no
 * pointer and no long in it. That is not luck, it is why the kernel
 * defined it that way, and it means this file does not need the care
 * that the BLKPG ioctl needed - where an invented padding field put a
 * pointer four bytes past where the kernel reads it, and the Pi Zero W
 * silently did nothing for months. Every field here is written with
 * memcpy into a byte array all the same, because a cast to u32* at an
 * offset inside a buffer is the other half of that same mistake.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "net.h"
#include "eapol.h"

/* struct sockaddr_ll, from include/uapi/linux/if_packet.h:
 *
 *   u16 sll_family; u16 sll_protocol; s32 sll_ifindex; u16 sll_hatype;
 *   u8  sll_pkttype; u8 sll_halen;    u8  sll_addr[8];
 *
 * 20 bytes, identically on every architecture this system runs on. */
#define SLL_LEN        20
#define SLL_FAMILY      0
#define SLL_PROTOCOL    2
#define SLL_IFINDEX     4
#define SLL_HATYPE      8
#define SLL_PKTTYPE    10
#define SLL_HALEN      11
#define SLL_ADDR       12

static void sll_init(u8 *sa, u32 ifindex)
{
    memset(sa, 0, SLL_LEN);
    u16 fam = AF_PACKET;
    u16 pro = htons(ETH_P_PAE);
    u32 idx = ifindex;
    memcpy(sa + SLL_FAMILY,   &fam, 2);
    memcpy(sa + SLL_PROTOCOL, &pro, 2);
    memcpy(sa + SLL_IFINDEX,  &idx, 4);
}

bool eapol_open(eapol_t *e, const char *ifname, int *err)
{
    if (err) *err = 0;
    memset(e, 0, sizeof *e);
    e->fd = -1;

    int idx = 0;
    if (net_if_index(ifname, &idx) < 0 || idx <= 0) {
        if (err) *err = 19;                 /* ENODEV */
        return false;
    }
    e->ifindex = (u32)idx;
    net_if_hwaddr(ifname, e->our_mac);

    long fd = lp_socket(AF_PACKET, SOCK_DGRAM, (int)htons(ETH_P_PAE));
    if (fd < 0) {
        if (err) *err = (int)-fd;
        return false;
    }

    u8 sa[SLL_LEN];
    sll_init(sa, idx);
    long rc = lp_bind((int)fd, sa, SLL_LEN);
    if (rc < 0) {
        if (err) *err = (int)-rc;
        lp_close((int)fd);
        return false;
    }

    e->fd = (int)fd;
    return true;
}

void eapol_close(eapol_t *e)
{
    if (e->fd >= 0)
        lp_close(e->fd);
    e->fd = -1;
}

bool eapol_send(eapol_t *e, const u8 dst[6], const void *buf, size_t len)
{
    u8 sa[SLL_LEN];
    sll_init(sa, e->ifindex);
    sa[SLL_HALEN] = 6;
    memcpy(sa + SLL_ADDR, dst, 6);

    long n = lp_sendto(e->fd, buf, len, 0, sa, SLL_LEN);
    return n == (long)len;
}

long eapol_recv(eapol_t *e, void *buf, size_t size, u8 from[6],
                int timeout_ms)
{
    lp_pollfd_t p;
    p.fd = e->fd;
    p.events = LP_POLLIN;
    p.revents = 0;

    long r = lp_poll(&p, 1, timeout_ms);
    if (r == 0)
        return 0;                           /* nothing came */
    if (r < 0)
        return -1;

    u8 sa[SLL_LEN];
    u32 salen = SLL_LEN;
    memset(sa, 0, sizeof sa);

    long n = lp_recvfrom(e->fd, buf, size, 0, sa, &salen);
    if (n <= 0)
        return -1;

    if (from) {
        /* halen is 6 for Ethernet. A frame from something with a
         * different address length is not from an access point we are
         * talking to, and handing back six bytes of a shorter address
         * would be inventing them. */
        if (sa[SLL_HALEN] == 6)
            memcpy(from, sa + SLL_ADDR, 6);
        else
            memset(from, 0, 6);
    }
    return n;
}

void eapol_flush(eapol_t *e)
{
    u8 junk[EAPOL_MAX];
    for (;;) {
        lp_pollfd_t p;
        p.fd = e->fd;
        p.events = LP_POLLIN;
        p.revents = 0;
        if (lp_poll(&p, 1, 0) <= 0)
            return;
        if (lp_recvfrom(e->fd, junk, sizeof junk, 0, NULL, NULL) <= 0)
            return;
    }
}
