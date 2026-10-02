/* tls.h - one TLS connection at a time, which is all this system needs.
 *
 * The arithmetic underneath is BearSSL's (thirdparty/bearssl). This is
 * the part that decides things: which certificates are trusted, what
 * the clock says, where the randomness comes from, and what to print
 * when it goes wrong.
 *
 *   int fd = <a connected TCP socket>;
 *   lp_tls_t *t = lp_tls_open(fd, "example.com");
 *   if (!t) ... lp_tls_error() said why on stderr already
 *   lp_tls_write(t, req, n); lp_tls_flush(t);
 *   while ((n = lp_tls_read(t, buf, sizeof buf)) > 0) ...
 *   lp_tls_close(t);
 *
 * The socket is yours: this does not close it.
 */
#ifndef LP_TLS_H
#define LP_TLS_H

#include "types.h"

typedef struct lp_tls lp_tls_t;

/* Handshake with the host on fd. `host` is both the name sent in SNI
 * and the name the certificate has to match - passing the IP address
 * you connected to instead of the name you asked for is how a checked
 * connection quietly becomes an unchecked one, so there is no way to
 * pass one and not the other.
 *
 * Returns NULL and has already printed a sentence saying what failed. */
lp_tls_t *lp_tls_open(int fd, const char *host);

/* Bytes out of the connection. 0 is a clean close, -1 an error. */
long lp_tls_read(lp_tls_t *t, void *buf, size_t n);

/* Bytes in. Buffered until lp_tls_flush. */
long lp_tls_write(lp_tls_t *t, const void *buf, size_t n);
int  lp_tls_flush(lp_tls_t *t);

/* Send close_notify and let go of the buffers. Does not close the fd. */
void lp_tls_close(lp_tls_t *t);

/* The last failure, as a sentence, or NULL if there has not been one. */
const char *lp_tls_error(const lp_tls_t *t);

#endif /* LP_TLS_H */
