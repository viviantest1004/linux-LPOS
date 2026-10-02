/* tls.c - the decisions around BearSSL.
 *
 * BearSSL does the arithmetic. Everything that can be got wrong in a
 * way this project would never notice - a bignum, a padding check, a
 * comparison that takes a different amount of time when it fails - is
 * its code, not ours, and that is the whole reason it is here. What is
 * ours is the part with judgement in it:
 *
 *   which certificates count as trusted   tls-roots.c, compiled in
 *   what the clock says                   lp_time(), passed explicitly
 *   where the randomness comes from       getrandom(2), and no fallback
 *   what a failure reads like             the table at the bottom
 *
 * ── One connection at a time ──
 * The buffers are static and there is one of them. TLS needs a 16KB
 * receive buffer because a peer may send a full-size record and the
 * whole record has to be there before any of it can be decrypted; two
 * directions makes it about 33KB. Nothing in this system fetches two
 * URLs at once, and a 33KB allocation that can fail is worse than a
 * 33KB one that cannot. lp_tls_open refuses a second caller rather
 * than quietly handing out the first one's buffers.
 *
 * Because it is static and the linker is given -fdata-sections and
 * --gc-sections, a program that never speaks TLS does not carry it.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "tls.h"
#include "bearssl.h"

/* This libc has no errno.h and does not need one: syscalls return the
 * negative error directly. 4 is EINTR - a signal landed mid-call, which
 * on this system means the alarm behind a timeout, and the read has to
 * be started again rather than reported as a broken connection. */
#define LP_EINTR 4

/* Generated from Mozilla's root list. See tls-roots.c. */
extern const br_x509_trust_anchor lp_tls_tas[];
extern const size_t lp_tls_tas_num;

struct lp_tls {
    br_ssl_client_context   sc;
    br_x509_minimal_context xc;
    br_sslio_context        io;
    int   fd;
    bool  open;
    char  host[256];
    char  err[192];
};

static struct lp_tls T;
static bool in_use = false;

/* ── the socket, as BearSSL wants to see it ─────────────────────── */

static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    for (;;) {
        long n = lp_read(fd, buf, len);
        if (n == -LP_EINTR)
            continue;
        if (n <= 0)
            return -1;
        return (int)n;
    }
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    for (;;) {
        long n = lp_write(fd, buf, len);
        if (n == -LP_EINTR)
            continue;
        if (n <= 0)
            return -1;
        return (int)n;
    }
}

/* ── what went wrong, in a sentence ─────────────────────────────── */

/* BearSSL numbers TLS errors 1..31 and X.509 errors 32..63 in one
 * space, so one table covers both.
 *
 * Only the ones somebody can act on are spelled out. The rest come out
 * as a number, which is not helpful but is honest, and the number is
 * findable in bearssl_ssl.h. The two that matter most on this machine
 * are the clock and the name: a board with no real-time clock starts
 * in 1970 until ntp answers, and every certificate on earth is "not
 * valid yet" at that point - saying "expired" and stopping there sends
 * people looking at the server. */
static void say(struct lp_tls *t, int err, const char *host)
{
    switch (err) {
    case BR_ERR_X509_EXPIRED:
        snprintf(t->err, sizeof t->err,
                 "the certificate for %s is not valid at %ld, which is what"
                 " this machine thinks the time is - run ntp first",
                 host, (long)lp_time());
        return;
    case BR_ERR_X509_NOT_TRUSTED:
        snprintf(t->err, sizeof t->err,
                 "%s presented a certificate signed by an authority this"
                 " system carries no root for", host);
        return;
    case BR_ERR_X509_BAD_SERVER_NAME:
        snprintf(t->err, sizeof t->err,
                 "the certificate is valid, but not for %s", host);
        return;
    case BR_ERR_X509_UNSUPPORTED:
        snprintf(t->err, sizeof t->err,
                 "%s used a certificate of a kind this TLS build does not"
                 " handle", host);
        return;
    case BR_ERR_X509_CRITICAL_EXTENSION:
        snprintf(t->err, sizeof t->err,
                 "%s's certificate insists on an extension this build does"
                 " not implement, so it cannot be honoured", host);
        return;
    case BR_ERR_IO:
        snprintf(t->err, sizeof t->err,
                 "the connection to %s broke", host);
        return;
    case BR_ERR_UNSUPPORTED_VERSION:
    case BR_ERR_BAD_VERSION:
        snprintf(t->err, sizeof t->err,
                 "%s offered no TLS version this build speaks (it does"
                 " 1.0 through 1.2)", host);
        return;
    case BR_ERR_BAD_CIPHER_SUITE:
        snprintf(t->err, sizeof t->err,
                 "%s and this build have no cipher suite in common", host);
        return;
    case BR_ERR_NO_RANDOM:
        snprintf(t->err, sizeof t->err,
                 "no randomness - getrandom(2) refused, so nothing was"
                 " sent rather than sending something guessable");
        return;
    default:
        snprintf(t->err, sizeof t->err,
                 "the TLS connection to %s failed (BearSSL error %d)",
                 host, err);
        return;
    }
}

/* ── open ───────────────────────────────────────────────────────── */

lp_tls_t *lp_tls_open(int fd, const char *host)
{
    /* Where the record buffers live.
     *
     * Outside the struct on purpose: BR_SSL_BUFSIZE_BIDI is about 33KB
     * and putting it in a struct that gets zeroed by an accidental
     * memset is a 33KB memset on a 1GHz core. */
    static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];

    if (in_use) {
        dprintf(STDERR_FILENO,
                "tls: a connection is already open - this system does one"
                " at a time\n");
        return NULL;
    }

    struct lp_tls *t = &T;
    memset(t, 0, sizeof *t);
    t->fd = fd;
    strlcpy(t->host, host, sizeof t->host);

    br_ssl_client_init_full(&t->sc, &t->xc, lp_tls_tas, lp_tls_tas_num);
    br_ssl_engine_set_buffer(&t->sc.eng, iobuf, sizeof iobuf, 1);

    /* Seeding, and why there is no fallback.
     *
     * BearSSL is built here with its own /dev/urandom reader turned off
     * (BR_USE_URANDOM=0), so this is the only place entropy enters. If
     * getrandom(2) will not answer, the honest thing is to stop: a TLS
     * client whose "random" is the process id produces a session key
     * somebody else can work out, and it produces it while looking
     * exactly like a working connection. Falling back to the clock
     * would make this function succeed more often and mean less. */
    unsigned char seed[32];
    if (lp_getrandom(seed, sizeof seed, 0) != (long)sizeof seed) {
        dprintf(STDERR_FILENO,
                "tls: getrandom(2) would not fill 32 bytes, so there is no\n"
                "  key material worth the name. Refusing to connect.\n");
        return NULL;
    }
    br_ssl_engine_inject_entropy(&t->sc.eng, seed, sizeof seed);
    memset(seed, 0, sizeof seed);

    /* The clock the certificate dates are checked against.
     *
     * BearSSL counts days from 1 January of year 0 in the proleptic
     * Gregorian calendar; the Unix epoch is day 719528 of that count.
     * Passing it explicitly rather than letting BearSSL call time(3) is
     * what lets the error above name the clock. */
    s64 now = lp_time();
    if (now < 0) now = 0;
    br_x509_minimal_set_time(&t->xc,
                             (uint32_t)(now / 86400 + 719528),
                             (uint32_t)(now % 86400));

    if (!br_ssl_client_reset(&t->sc, host, 0)) {
        say(t, br_ssl_engine_last_error(&t->sc.eng), host);
        dprintf(STDERR_FILENO, "tls: %s\n", t->err);
        return NULL;
    }

    br_sslio_init(&t->io, &t->sc.eng, sock_read, &t->fd, sock_write, &t->fd);

    /* The handshake is not run here. BearSSL runs it on the first read
     * or write, and running it early would mean two places that can
     * fail with the same error. The first lp_tls_write is where a bad
     * certificate shows up. */
    t->open = true;
    in_use  = true;
    return t;
}

/* ── the traffic ────────────────────────────────────────────────── */

static long fail(struct lp_tls *t)
{
    int err = br_ssl_engine_last_error(&t->sc.eng);
    if (err == BR_ERR_OK)
        return 0;                       /* clean close, not a failure */
    if (!t->err[0]) {
        say(t, err, t->host);
        dprintf(STDERR_FILENO, "tls: %s\n", t->err);
    }
    return -1;
}

long lp_tls_read(lp_tls_t *t, void *buf, size_t n)
{
    if (!t || !t->open) return -1;
    int r = br_sslio_read(&t->io, buf, n);
    if (r < 0)
        return fail(t);
    return r;
}

long lp_tls_write(lp_tls_t *t, const void *buf, size_t n)
{
    if (!t || !t->open) return -1;
    if (br_sslio_write_all(&t->io, buf, n) < 0) {
        fail(t);
        return -1;
    }
    return (long)n;
}

int lp_tls_flush(lp_tls_t *t)
{
    if (!t || !t->open) return -1;
    if (br_sslio_flush(&t->io) < 0) {
        fail(t);
        return -1;
    }
    return 0;
}

void lp_tls_close(lp_tls_t *t)
{
    if (!t || !t->open) return;
    /* close_notify, so the other end can tell a finished transfer from
     * a cut cable. Its failure is not worth reporting: we are leaving. */
    br_sslio_close(&t->io);
    t->open = false;
    in_use  = false;
}

const char *lp_tls_error(const lp_tls_t *t)
{
    if (!t || !t->err[0]) return NULL;
    return t->err;
}
