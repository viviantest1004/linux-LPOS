/* eapol.h - the socket the four-way handshake happens on.
 *
 * On a fullmac chip - which is what this board has - the firmware does
 * the scan, the authentication and the association by itself, and the
 * host never sees a management frame. The one thing left for the host
 * is the four-way handshake, and those four messages arrive as ordinary
 * Ethernet frames on the wireless interface with ethertype 0x888E.
 *
 * So this is a packet socket and nothing more exotic. SOCK_DGRAM rather
 * than SOCK_RAW: the kernel then strips the Ethernet header on the way
 * in and writes one on the way out from the address handed to sendto,
 * which is exactly what we want - the handshake cares about the EAPOL
 * payload and about who sent it, and nothing else in the header means
 * anything to it.
 *
 * The socket is opened BEFORE the connection is asked for, always. The
 * access point sends message 1 of 4 the instant the association
 * completes, and a socket opened after that races it: the frame is
 * delivered to no one, the handshake never starts, and what the log
 * shows is an association followed by silence. That is precisely the
 * symptom this whole stack was rewritten to explain, so the ordering is
 * a rule here rather than a detail.
 */
#ifndef LP_EAPOL_H
#define LP_EAPOL_H

#include "types.h"

#define ETH_P_PAE      0x888E      /* 802.1X Port Access Entity        */
#define EAPOL_MAX      1024        /* a key frame is ~130 bytes        */

typedef struct {
    int fd;
    u32 ifindex;
    u8  our_mac[6];
} eapol_t;

/* Open and bind. false on failure; the reason is printed by the caller
 * from `err`, which is a positive errno. */
bool eapol_open(eapol_t *e, const char *ifname, int *err);
void eapol_close(eapol_t *e);

/* Send one EAPOL frame to `dst`. */
bool eapol_send(eapol_t *e, const u8 dst[6], const void *buf, size_t len);

/* Receive one. Returns the length, 0 on timeout, -1 on error.
 * `from` gets the sender's address, which the handshake checks against
 * the access point it thinks it is talking to - an EAPOL frame from
 * anyone else is somebody else's business or somebody's attempt. */
long eapol_recv(eapol_t *e, void *buf, size_t size, u8 from[6],
                int timeout_ms);

/* Throw away anything already queued. Called before a fresh
 * association so that a stale frame from the last attempt cannot be
 * mistaken for the answer to this one. */
void eapol_flush(eapol_t *e);

#endif
