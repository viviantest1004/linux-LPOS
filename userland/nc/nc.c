/* nc - netcat: connect a program to a socket and get out of the way.
 *
 *   nc host 80                 connect, then shuttle bytes both ways
 *   nc -l -p 8080              listen, take one connection
 *   nc -z host 22              is that port open? (exit code answers)
 *   nc -u host 53              UDP instead of TCP
 *
 * On a machine reached only over SSH this is the tool that answers "is
 * the port even open" without a browser, and the one that moves a file
 * between two machines when nothing else is installed on either.
 *
 * There is deliberately no -e. The real netcat's -e hands a shell to
 * whoever connects, and that is not a feature on a machine left on the
 * internet, it is the back door people go looking for. Anything that
 * genuinely needs a remote shell here already has one: dropbear is
 * running and is the thing that asks for a key first.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

static const char *prog = "nc";
static bool verbose = false;
static bool udp = false;

static void say(const char *fmt, const char *a, int b)
{
    if (verbose) dprintf(STDERR_FILENO, fmt, a, b);
}

/* "10.0.0.1" -> the address in network order. false if it is a name we
 * cannot resolve without a resolver here. */
static bool parse_ip(const char *s, u32 *out)
{
    u32 v = 0;
    int part = 0;
    for (;;) {
        if (*s < '0' || *s > '9') return false;
        int n = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (*s++ - '0');
            if (n > 255) return false;
        }
        v = (v << 8) | (u32)n;
        part++;
        if (*s == '\0') break;
        if (*s != '.' || part == 4) return false;
        s++;
    }
    if (part != 4) return false;
    *out = htonl(v);
    return true;
}

/* Names go through /etc/hosts and then the resolver the rest of this
 * system already uses. */
static bool resolve(const char *host, u32 *out)
{
    if (parse_ip(host, out)) return true;
    u32 a = net_resolve(host);
    if (a) { *out = a; return true; }
    dprintf(STDERR_FILENO, "%s: %s: Name or service not known\n", prog, host);
    return false;
}

/* Move bytes in both directions until one side is finished.
 *
 * Both halves have to be watched at once. Reading the socket first and
 * stdin second deadlocks the moment the far end says nothing and the
 * person types, which is every interactive use of this program. */
static int shuttle(int sock, bool half_close)
{
    lp_pollfd_t fds[2];
    char buf[65536];
    bool stdin_open = true;

    for (;;) {
        int n = 0;
        fds[n].fd = sock; fds[n].events = LP_POLLIN; fds[n].revents = 0; n++;
        if (stdin_open) {
            fds[n].fd = STDIN_FILENO; fds[n].events = LP_POLLIN; fds[n].revents = 0; n++;
        }

        long r = lp_poll(fds, (unsigned)n, -1);
        if (r < 0) {
            if (r == -4) continue;              /* EINTR: nothing happened */
            return 1;
        }

        if (fds[0].revents & (LP_POLLIN | LP_POLLHUP | LP_POLLERR)) {
            long got = lp_read(sock, buf, sizeof buf);
            if (got <= 0) return 0;             /* the far end is done */
            lp_write(STDOUT_FILENO, buf, (size_t)got);
        }
        if (n > 1 && (fds[1].revents & (LP_POLLIN | LP_POLLHUP))) {
            long got = lp_read(STDIN_FILENO, buf, sizeof buf);
            if (got <= 0) {
                /* Our input ran out. Tell the far end so a server that
                 * waits for end-of-input can answer, but keep reading:
                 * the answer is the whole point. */
                stdin_open = false;
                if (half_close) lp_shutdown(sock, 1);
                else            return 0;
                continue;
            }
            lp_write(sock, buf, (size_t)got);
        }
    }
}

static void usage(int fd)
{
    dprintf(fd, "Usage: nc [OPTION]... HOST PORT\n"
                "   or: nc -l [-p] PORT\n"
                "Read and write bytes over a network connection.\n\n"
                "  -l            listen for a connection instead of making one\n"
                "  -p PORT       the port to listen on\n"
                "  -u            UDP instead of TCP\n"
                "  -w SECONDS    give up on a connection after this long\n"
                "  -z            only find out whether the port is open; send nothing\n"
                "  -k            keep listening after a connection closes\n"
                "  -n            do not look up names (they are addresses)\n"
                "  -v            say what is happening\n"
                "  -q SECONDS    after end of input, wait this long then quit\n"
                "      --help    display this help and exit\n\n"
                "There is no -e. Handing a shell to whoever connects is the back\n"
                "door people go looking for on a machine left on the internet;\n"
                "dropbear is already here and asks for a key first.\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "listen", 0, 'l' }, { "udp", 0, 'u' }, { "verbose", 0, 'v' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    bool listening = false, scan = false, keep = false;
    long wait_s = 0, quit_after = -1;
    const char *lport = NULL;

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "lp:uw:zknvq:", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'l': listening = true; break;
        case 'p': lport = g.arg; break;
        case 'u': udp = true; break;
        case 'w': wait_s = strtol(g.arg, NULL, 10); break;
        case 'z': scan = true; break;
        case 'k': keep = true; break;
        case 'n': break;
        case 'v': verbose = true; break;
        case 'q': quit_after = strtol(g.arg, NULL, 10); break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default: lp_getopt_err(prog, &g); return 2;
        }
    }
    (void)quit_after;

    int type = udp ? SOCK_DGRAM : SOCK_STREAM;

    /* ── listening ── */
    if (listening) {
        const char *ps = lport ? lport : (g.ind < argc ? argv[g.ind] : NULL);
        if (!ps) {
            dprintf(STDERR_FILENO, "%s: no port given to listen on\n", prog);
            return 2;
        }
        int port = atoi(ps);
        long s = lp_socket(AF_INET, type, 0);
        if (s < 0) { dprintf(STDERR_FILENO, "%s: cannot make a socket\n", prog); return 1; }

        int one = 1;
        lp_setsockopt((int)s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

        sockaddr_in_t a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port   = htons((u16)port);
        a.sin_addr   = 0;                       /* every interface */
        if (lp_bind((int)s, &a, sizeof a) < 0) {
            dprintf(STDERR_FILENO, "%s: cannot bind to port %d: %s\n",
                    prog, port, "Address already in use or not permitted");
            return 1;
        }

        if (udp) {
            /* UDP has no connection to accept. Take the first packet,
             * remember who sent it, and talk to them from then on. */
            say("%s: listening on UDP port %d\n", "", port);
            char buf[65536];
            sockaddr_in_t from;
            u32 flen = sizeof from;
            long n = lp_recvfrom((int)s, buf, sizeof buf, 0, &from, &flen);
            if (n < 0) return 1;
            lp_write(STDOUT_FILENO, buf, (size_t)n);
            if (lp_connect((int)s, &from, sizeof from) < 0) return 1;
            return shuttle((int)s, false);
        }

        if (lp_listen((int)s, 1) < 0) {
            dprintf(STDERR_FILENO, "%s: cannot listen\n", prog);
            return 1;
        }
        say("%s: listening on port %d\n", "", port);

        do {
            sockaddr_in_t from;
            u32 flen = sizeof from;
            long c = lp_accept((int)s, &from, &flen, 0);
            if (c < 0) {
                if (c == -4) continue;          /* EINTR */
                return 1;
            }
            if (verbose) {
                u32 h = ntohl(from.sin_addr);
                dprintf(STDERR_FILENO, "%s: connection from %u.%u.%u.%u:%u\n",
                        prog, (h >> 24) & 255, (h >> 16) & 255,
                        (h >> 8) & 255, h & 255, ntohs(from.sin_port));
            }
            shuttle((int)c, true);
            lp_close((int)c);
        } while (keep);
        lp_close((int)s);
        return 0;
    }

    /* ── connecting ── */
    if (argc - g.ind < 2) {
        usage(STDERR_FILENO);
        return 2;
    }
    const char *host = argv[g.ind];
    int port = atoi(argv[g.ind + 1]);

    u32 addr;
    if (!resolve(host, &addr)) return 1;

    long s = lp_socket(AF_INET, type, 0);
    if (s < 0) { dprintf(STDERR_FILENO, "%s: cannot make a socket\n", prog); return 1; }

    if (wait_s > 0) {
        /* The only lever on how long connect() takes is how many SYNs
         * it sends; there is no connect timeout to set. */
        int tries = (int)(wait_s / 3);
        if (tries < 1) tries = 1;
        lp_setsockopt((int)s, IPPROTO_TCP, IPPROTO_TCP_SYNCNT, &tries, sizeof tries);
    }

    sockaddr_in_t a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons((u16)port);
    a.sin_addr   = addr;

    if (lp_connect((int)s, &a, sizeof a) < 0) {
        if (!scan || verbose)
            dprintf(STDERR_FILENO, "%s: connect to %s port %d (%s) failed: "
                                   "Connection refused\n",
                    prog, host, port, udp ? "udp" : "tcp");
        lp_close((int)s);
        return 1;
    }

    if (scan) {
        /* -z is the question "is this open", and the exit code is the
         * answer. Saying so out loud is what -v is for. */
        if (verbose)
            dprintf(STDERR_FILENO,
                    "%s: Connection to %s %d port [%s/*] succeeded!\n",
                    prog, host, port, udp ? "udp" : "tcp");
        lp_close((int)s);
        return 0;
    }

    int rc = shuttle((int)s, true);
    lp_close((int)s);
    return rc;
}
