/* ping - is it there, and how far away.
 *
 *   ping [-c count] [-i ms] [-s bytes] [-q] <host>
 *
 * The oldest question on a network, and the first one worth asking when
 * something does not work: it separates "the network is broken" from
 * "that one service is broken", which are very different afternoons.
 *
 * ICMP echo over a raw socket. Raw sockets need root, which everything
 * here is - there is one user. The kernel fills in the IP header on the
 * way out and hands it back to us on the way in, which is why the reply
 * is read past an IP header and the request is not written with one.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

#define IPPROTO_ICMP   1
#define ICMP_ECHO      8
#define ICMP_ECHOREPLY 0
#define ICMP_DEST_UNREACH 3
#define ICMP_TIME_EXCEEDED 11

/* The internet checksum: ones' complement of the ones' complement sum of
 * 16-bit words. The same routine covers IP, ICMP, UDP and TCP. */
static u16 checksum(const void *data, size_t len)
{
    const u8 *p = data;
    u32 sum = 0;

    while (len > 1) {
        sum += (u32)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (u32)(p[0] << 8);

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (u16)~sum;
}

int main(int argc, char **argv)
{
    long count    = 4;
    long interval = 1000;
    long payload  = 56;          /* what every other ping sends */
    bool quiet    = false;
    const char *host = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            count = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
            interval = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            payload = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "-q") == 0)
            quiet = true;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: ping [-c count] [-i ms] [-s bytes] [-q] <host>\n");
            printf("  -c  how many to send (default 4, 0 for no limit)\n");
            printf("  -i  milliseconds between them (default 1000)\n");
            printf("  -s  payload bytes (default 56)\n");
            printf("  -q  the summary only\n");
            return 0;
        }
        else if (!host)
            host = argv[i];
    }

    if (!host) {
        dprintf(STDERR_FILENO, "usage: ping [-c count] <host>\n");
        return 2;
    }
    if (payload < 0)   payload = 0;
    if (payload > 1400) payload = 1400;

    u32 addr = net_resolve(host);
    if (addr == 0) {
        dprintf(STDERR_FILENO, "ping: cannot resolve %s\n", host);
        return 1;
    }

    char addr_str[16];
    ipv4_format(addr, addr_str);

    long fd = lp_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "ping: cannot open a raw socket (%ld)\n"
                "      raw sockets need root\n", -fd);
        return 1;
    }

    /* Without a timeout a lost packet stops us for good. */
    s64 tv[2] = { 2, 0 };
    lp_setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof(tv));

    sockaddr_in_t to = { 0 };
    to.sin_family = AF_INET;
    to.sin_addr   = addr;

    u16 id = (u16)(lp_getpid() & 0xFFFF);

    printf("PING %s (%s) %ld bytes\n", host, addr_str, payload);

    static u8 packet[1500];
    static u8 reply[2048];

    long sent = 0, received = 0;
    long best = -1, worst = -1, total_ms = 0;

    for (long seq = 1; count == 0 || seq <= count; seq++) {
        memset(packet, 0, sizeof(packet));
        packet[0] = ICMP_ECHO;
        packet[1] = 0;
        /* checksum goes in 2..3 */
        packet[4] = (u8)(id >> 8);   packet[5] = (u8)id;
        packet[6] = (u8)(seq >> 8);  packet[7] = (u8)seq;

        /* A recognisable payload, so a reply that is not ours is easy to
         * spot in a capture. */
        for (long i = 0; i < payload; i++)
            packet[8 + i] = (u8)('a' + (i % 26));

        size_t len = 8 + (size_t)payload;
        u16 ck = checksum(packet, len);
        packet[2] = (u8)(ck >> 8);
        packet[3] = (u8)ck;

        s64 start = lp_monotonic_ms();

        if (lp_sendto((int)fd, packet, len, 0, &to, sizeof(to)) < 0) {
            dprintf(STDERR_FILENO, "ping: cannot send\n");
            break;
        }
        sent++;

        /* Wait for one that is ours. Another program's ping, or an
         * unrelated ICMP message, can arrive on this socket too. */
        bool got = false;
        for (;;) {
            long n = lp_recvfrom((int)fd, reply, sizeof(reply), 0, NULL, NULL);
            if (n <= 0)
                break;                  /* timed out */

            /* The IP header is variable length: the low nibble of the
             * first byte counts 32-bit words. */
            int ihl = (reply[0] & 0x0F) * 4;
            if (n < ihl + 8)
                continue;

            u8 *icmp = reply + ihl;
            u16 rid  = (u16)((icmp[4] << 8) | icmp[5]);
            u16 rseq = (u16)((icmp[6] << 8) | icmp[7]);

            if (icmp[0] == ICMP_DEST_UNREACH) {
                printf("from %s: unreachable\n", addr_str);
                got = true;
                break;
            }
            if (icmp[0] != ICMP_ECHOREPLY || rid != id || rseq != seq)
                continue;               /* not ours */

            s64 ms = lp_monotonic_ms() - start;
            received++;
            total_ms += (long)ms;
            if (best  < 0 || ms < best)  best  = (long)ms;
            if (worst < 0 || ms > worst) worst = (long)ms;

            if (!quiet) {
                char from[16];
                u32  src;
                memcpy(&src, reply + 12, 4);
                ipv4_format(src, from);
                printf("%ld bytes from %s: seq=%ld ttl=%d time=%lld ms\n",
                       n - ihl, from, seq, reply[8], (long long)ms);
            }
            got = true;
            break;
        }

        if (!got && !quiet)
            printf("seq=%ld  no reply\n", seq);

        if (count == 0 || seq < count)
            lp_sleep_ms(interval);
    }

    lp_close((int)fd);

    printf("\n--- %s ---\n", host);
    printf("%ld sent, %ld received, %ld%% lost\n",
           sent, received,
           sent ? (sent - received) * 100 / sent : 0);
    if (received > 0)
        printf("round trip  min %ld ms  avg %ld ms  max %ld ms\n",
               best, total_ms / received, worst);

    return received > 0 ? 0 : 1;
}
