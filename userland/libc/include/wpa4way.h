/* wpa4way.h - the WPA2-PSK four-way and group-key handshakes, as pure
 * functions.
 *
 * This is the top half of our own wireless stack. nl80211.h gets the
 * radio associated and installs keys; eapol.h carries the frames; this
 * file decides what the frames mean and what to answer. It never opens
 * a socket, never reads the clock and never asks for randomness: every
 * input arrives as an argument - the frame, the PMK, the addresses, the
 * fresh nonce - and every output leaves in a struct. That is the whole
 * design, and the reason is the failure this stack was written to
 * explain. On the Raspberry Pi the association succeeded and the
 * handshake never happened, inside a supplicant whose state we could
 * not see. A handshake made of pure functions can be fed a real capture
 * on a machine with no radio, fuzzed at every truncation, and checked
 * against published vectors - and when it fails on the board, the
 * reason it gives is a sentence about one field of one frame.
 *
 * The cryptography is BearSSL's (SHA-1, HMAC, AES), the same library
 * TLS uses here. What is ours is PBKDF2, the 802.11 PRF, the RFC 3394
 * key wrap built on single AES blocks, and all of the parsing. Those
 * are thin enough to be checked against test vectors byte for byte,
 * and the vectors are in the test program next to this file's user.
 *
 * ── What is supported, and what is refused ──
 *   AKM          00-0F-AC:2, PSK. Not PSK-SHA256, not SAE (WPA3), not
 *                FT. The caller refuses those networks before it gets
 *                here, with a sentence saying why.
 *   pairwise     CCMP-128 only. Descriptor version 2: HMAC-SHA1-128
 *                MIC and AES key wrap.
 *   group        CCMP-128 or TKIP. An old router in WPA/WPA2 mixed mode
 *                hands out a TKIP group key over a CCMP pairwise one,
 *                and refusing it would refuse half the cafes there are.
 *   management   no 802.11w. We never advertise MFP, so an AP that
 *   frames       requires it is refused before association.
 *
 * ── Byte order ──
 * Every multi-byte field in an EAPOL-Key frame is big-endian - the body
 * length, the key information, the key length, the replay counter, the
 * key data length. The Key RSC is the exception: it is a little-endian
 * packet number, and it is handed to nl80211 exactly as it arrived.
 * Nothing in the implementation casts a pointer into a frame: the Pi
 * Zero W is a 32-bit ARMv6, where a misaligned load does not fault but
 * rotates the word, and a replay counter read that way is wrong in a
 * way no test on a PC would ever show.
 */
#ifndef LP_WPA4WAY_H
#define LP_WPA4WAY_H

#include "types.h"

#define WPA_PMK_LEN        32
#define WPA_NONCE_LEN      32
#define WPA_REPLAY_LEN      8
#define WPA_MIC_LEN        16
#define WPA_KCK_LEN        16
#define WPA_KEK_LEN        16
#define WPA_TK_LEN         16      /* CCMP-128                         */
#define WPA_GTK_MAX        32      /* TKIP; CCMP is 16                 */
#define WPA_RSC_LEN         6      /* what nl80211 is given            */
#define WPA_IE_MAX        128      /* an RSN IE is 22 bytes in practice */
#define WPA_FRAME_MAX     512      /* the largest EAPOL-Key we build   */

/* Suite selectors, as 32-bit numbers the way nl80211.h spells them. */
#define WPA_SUITE_TKIP     0x000FAC02u
#define WPA_SUITE_CCMP     0x000FAC04u
#define WPA_SUITE_PSK      0x000FAC02u

/* ── The EAPOL-Key frame, field by field ──────────────────────────────
 *
 * Offsets into the whole EAPOL frame, header included. The MIC is at a
 * fixed place because the MIC length is fixed for the one AKM we do:
 *
 *    0  protocol version   1    2  body length (BE)   2
 *    1  packet type (3)    1
 *    4  descriptor type    1    (2 = RSN; 254 = the old WPA1 one)
 *    5  key information    2 BE
 *    7  key length         2 BE
 *    9  replay counter     8 BE
 *   17  key nonce         32
 *   49  key IV            16
 *   65  key RSC            8    little-endian packet number
 *   73  reserved           8
 *   81  key MIC           16
 *   97  key data length    2 BE
 *   99  key data           n
 */
#define WPA_EAPOL_HDR       4
#define WPA_OFF_DESC        4
#define WPA_OFF_INFO        5
#define WPA_OFF_KEYLEN      7
#define WPA_OFF_REPLAY      9
#define WPA_OFF_NONCE      17
#define WPA_OFF_IV         49
#define WPA_OFF_RSC        65
#define WPA_OFF_MIC        81
#define WPA_OFF_DATALEN    97
#define WPA_OFF_DATA       99
#define WPA_KEY_MIN        WPA_OFF_DATA

#define WPA_EAPOL_KEY       3       /* packet type                      */
#define WPA_DESC_RSN        2
#define WPA_DESC_WPA1     254

/* Key information bits (IEEE 802.11-2020 figure 12-33). */
#define WPA_KI_VERSION_MASK 0x0007  /* 1 MD5/RC4, 2 SHA1/AES, 3 CMAC    */
#define WPA_KI_PAIRWISE     0x0008
#define WPA_KI_INSTALL      0x0040
#define WPA_KI_ACK          0x0080
#define WPA_KI_MIC          0x0100
#define WPA_KI_SECURE       0x0200
#define WPA_KI_ERROR        0x0400
#define WPA_KI_REQUEST      0x0800
#define WPA_KI_ENCRYPTED    0x1000
#define WPA_KI_SMK          0x2000

#define WPA_KEY_VERSION_AES 2

/* ── Primitives ────────────────────────────────────────────────────── */

void wpa_hmac_sha1(const u8 *key, size_t keylen, const u8 *msg, size_t len,
                   u8 out[20]);

/* PBKDF2 with HMAC-SHA1 (RFC 8018). The WPA passphrase mapping is this
 * with the SSID as the salt, 4096 rounds and 32 bytes out. */
void wpa_pbkdf2_sha1(const u8 *pass, size_t passlen,
                     const u8 *salt, size_t saltlen,
                     u32 rounds, u8 *out, size_t outlen);

/* The IEEE 802.11 PRF (12.7.1.2): HMAC-SHA1 over label || 0 || data ||
 * counter, as many times as it takes. Any length is allowed; the
 * standard only ever asks for multiples of 8 bits. */
void wpa_prf_sha1(const u8 *key, size_t keylen, const char *label,
                  const u8 *data, size_t datalen, u8 *out, size_t outlen);

/* RFC 3394 AES key wrap with the default IV A6A6A6A6A6A6A6A6. kek is
 * 16, 24 or 32 bytes. wrap: n is a multiple of 8 and at least 16, out
 * gets n + 8. unwrap: n is a multiple of 8 and at least 24, out gets
 * n - 8. unwrap returns false - and leaves out zeroed - when the
 * integrity check fails, which is how a wrong KEK shows itself. */
bool wpa_aes_wrap(const u8 *kek, size_t keklen, const u8 *plain, size_t n,
                  u8 *out);
bool wpa_aes_unwrap(const u8 *kek, size_t keklen, const u8 *cipher, size_t n,
                    u8 *out);

/* Compare without leaking where the first difference is. */
bool wpa_ct_equal(const u8 *a, const u8 *b, size_t n);

/* ── The PSK ───────────────────────────────────────────────────────── */

typedef enum {
    WPA_PSK_OK = 0,
    WPA_PSK_SHORT,          /* a passphrase is at least 8 characters    */
    WPA_PSK_LONG,           /* ...and at most 63; 64 means hex          */
    WPA_PSK_NOT_ASCII,      /* printable ASCII only, 32..126            */
    WPA_PSK_BAD_HEX,        /* 64 characters that are not all hex       */
    WPA_PSK_NO_SSID         /* the SSID is the salt; it cannot be empty */
} wpa_psk_err_t;

/* A passphrase of 8..63 printable characters is run through PBKDF2 with
 * the SSID; exactly 64 hex digits are the PMK itself, which is what
 * wpa_passphrase prints and what some routers show. On failure pmk is
 * zeroed. */
wpa_psk_err_t wpa_pmk_from_psk(const char *psk, const u8 *ssid,
                               size_t ssid_len, u8 pmk[WPA_PMK_LEN]);
const char   *wpa_psk_error(wpa_psk_err_t e);

/* ── Keys ──────────────────────────────────────────────────────────── */

typedef struct {
    u8 kck[WPA_KCK_LEN];    /* signs the frames (the MIC)               */
    u8 kek[WPA_KEK_LEN];    /* wraps the group key in message 3         */
    u8 tk[WPA_TK_LEN];      /* encrypts the traffic; what nl80211 gets  */
} wpa_ptk_t;

/* PRF-384("Pairwise key expansion", min(AA,SPA) || max(AA,SPA) ||
 * min(ANonce,SNonce) || max(ANonce,SNonce)). The ordering is by plain
 * byte comparison, which is why it does not matter who is who. */
void wpa_ptk_derive(const u8 pmk[WPA_PMK_LEN], const u8 aa[6],
                    const u8 spa[6], const u8 anonce[WPA_NONCE_LEN],
                    const u8 snonce[WPA_NONCE_LEN], wpa_ptk_t *ptk);

/* The RSN element this station sends in its association request and in
 * message 2: version 1, the AP's group cipher, CCMP pairwise, PSK, and
 * capabilities 0 - no pre-authentication, no MFP. Returns its length,
 * or 0 if cap is too small. */
size_t wpa_rsn_ie_build(u8 *out, size_t cap, u32 group_cipher);

/* ── Parsing an EAPOL-Key frame ────────────────────────────────────── */

typedef enum {
    WPA_KP_OK = 0,
    WPA_KP_SHORT_HEADER,    /* fewer than 4 bytes                       */
    WPA_KP_NOT_KEY,         /* an EAPOL packet, but not a Key           */
    WPA_KP_BODY_LEN,        /* the header claims more than arrived      */
    WPA_KP_SHORT_BODY,      /* a Key body is at least 95 bytes          */
    WPA_KP_DESC_WPA1,       /* the WPA1 descriptor - not ours           */
    WPA_KP_DESC_UNKNOWN,
    WPA_KP_DATA_LEN         /* key data length runs past the body       */
} wpa_kp_err_t;

typedef struct {
    u8        eapol_version;
    u16       body_len;
    u8        desc;
    u16       info;
    u16       key_len;
    u8        replay[WPA_REPLAY_LEN];
    u8        nonce[WPA_NONCE_LEN];
    u8        rsc[8];
    u8        mic[WPA_MIC_LEN];
    u16       data_len;
    const u8 *data;         /* points into the caller's frame           */
    size_t    frame_len;    /* 4 + body_len: what the MIC covers, which
                               excludes any link-layer padding after it */
} wpa_key_t;

/* Strict: every length is checked against what arrived before anything
 * is read, and a frame that fails any check is described, not repaired. */
wpa_kp_err_t wpa_key_parse(const u8 *frame, size_t len, wpa_key_t *k);
const char  *wpa_kp_error(wpa_kp_err_t e);

/* MIC over frame[0..len) with the MIC field treated as zero. len must
 * be at least WPA_KEY_MIN. */
void wpa_mic_compute(const u8 kck[WPA_KCK_LEN], const u8 *frame, size_t len,
                     u8 mic[WPA_MIC_LEN]);
bool wpa_mic_check(const u8 kck[WPA_KCK_LEN], const u8 *frame, size_t len);

/* ── Key data: elements and KDEs ───────────────────────────────────── */

typedef struct {
    const u8 *rsn_ie;       /* id and length byte included              */
    size_t    rsn_ie_len;
    const u8 *rsnxe;        /* the extension element, when an AP sends it */
    size_t    rsnxe_len;
    const u8 *gtk;          /* the key itself, without the KDE header   */
    size_t    gtk_len;
    int       gtk_idx;
    bool      gtk_tx;
    bool      have_gtk;
    bool      have_igtk;    /* the AP thinks we do MFP; we never said so */
    bool      have_pmkid;
} wpa_kde_t;

/* Walk decrypted key data. false when an element runs past the end -
 * which after a successful unwrap means the AP built it wrong. The
 * 0xDD 0x00... padding the key wrap needs ends the walk cleanly. */
bool wpa_kde_parse(const u8 *data, size_t len, wpa_kde_t *out);

/* ── The supplicant ───────────────────────────────────────────────── */

typedef enum {
    WPA_SM_WAIT_1 = 0,      /* associated, waiting for message 1 of 4   */
    WPA_SM_WAIT_3,          /* sent 2 of 4, waiting for 3 of 4          */
    WPA_SM_DONE             /* keys installed; rekeys arrive here       */
} wpa_sm_state_t;

typedef struct {
    /* Set by the caller, once per association, before the first frame. */
    u8     pmk[WPA_PMK_LEN];
    u8     aa[6];                   /* the access point's address       */
    u8     spa[6];                  /* ours                             */
    u8     own_ie[WPA_IE_MAX];      /* the RSN IE we associated with    */
    size_t own_ie_len;
    u8     ap_ie[WPA_IE_MAX];       /* the AP's, from its beacon; 0 means
                                       unknown and is then not compared */
    size_t ap_ie_len;
    u32    group_cipher;            /* WPA_SUITE_CCMP or WPA_SUITE_TKIP */

    /* Everything below is the state machine's. */
    wpa_sm_state_t state;
    bool   replay_set;
    u8     replay[WPA_REPLAY_LEN];  /* the last one with a good MIC      */
    u8     anonce[WPA_NONCE_LEN];
    u8     snonce[WPA_NONCE_LEN];
    bool   snonce_set;              /* kept across retransmitted 1/4s    */
    wpa_ptk_t tptk;                 /* derived at 1/4, not yet installed */
    wpa_ptk_t ptk;                  /* installed                         */
    bool   ptk_set;
    u8     gtk[WPA_GTK_MAX];        /* installed, for the reinstall check */
    size_t gtk_len;
    int    gtk_idx;
    bool   gtk_set;

    /* Evidence for a diagnosis, not state. msg1_after_2 counts message
     * 1s that arrived after we had already answered one: the AP did not
     * accept our message 2, and by far the commonest reason is that the
     * MIC was made with the wrong passphrase. */
    int    msg1_seen;
    int    msg1_after_2;
    int    msg3_seen;
    int    group_rekeys;
    int    ptk_rekeys;

    char   why[200];                /* the last frame we refused, and why */
} wpa_sm_t;

/* What the caller has to do after one frame. */
#define WPA_DO_SEND         0x01    /* send out.tx to the AP              */
#define WPA_DO_SET_PTK      0x02    /* ...then install out.tk             */
#define WPA_DO_SET_GTK      0x04    /* ...then install out.gtk            */
#define WPA_DO_4WAY_DONE    0x08
#define WPA_DO_GROUP_DONE   0x10

typedef struct {
    unsigned flags;
    int      msg;                   /* 1 or 3 of 4; 11 for group 1 of 2  */
    u8       tx[WPA_FRAME_MAX];
    size_t   tx_len;
    u8       tk[WPA_TK_LEN];
    u8       gtk[WPA_GTK_MAX];      /* TKIP's MIC keys already swapped
                                       into the order the kernel wants  */
    size_t   gtk_len;
    int      gtk_idx;
    u8       gtk_rsc[WPA_RSC_LEN];
} wpa_out_t;

typedef enum {
    WPA_RX_IGNORED = 0,     /* not for us, or a replay; sm->why says   */
    WPA_RX_OK,              /* act on out->flags                        */
    WPA_RX_FATAL            /* the handshake cannot go on; sm->why says */
} wpa_rx_t;

/* Start (or restart) for a new association. Clears everything the state
 * machine owns; leaves the caller's fields alone. */
void wpa_sm_reset(wpa_sm_t *sm);

/* One EAPOL frame from the AP. fresh_nonce is 32 random bytes the caller
 * drew for this call; it is used only when the frame starts a handshake
 * and is otherwise ignored, which keeps this function free of any
 * randomness of its own. */
wpa_rx_t wpa_sm_rx(wpa_sm_t *sm, const u8 *frame, size_t len,
                   const u8 fresh_nonce[WPA_NONCE_LEN], wpa_out_t *out);

/* Forget key material in a struct that is going out of scope. The
 * compiler is not allowed to drop this one. */
void wpa_wipe(void *p, size_t n);

#endif /* LP_WPA4WAY_H */
