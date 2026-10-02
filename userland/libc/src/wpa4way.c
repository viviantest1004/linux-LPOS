/* wpa4way.c - WPA2-PSK key handshakes: the arithmetic and the rules.
 *
 * wpa4way.h says what this is for and why it has no I/O. This file is
 * written to be read next to IEEE 802.11-2020 clause 12.7, and every
 * check below that refuses a frame names the field it looked at, so a
 * failure on the board comes back as a sentence rather than a silence.
 *
 * ── Where the arithmetic comes from ──
 * SHA-1, HMAC and the AES block cipher are BearSSL's, the same code
 * TLS runs on here. On top of them, four small things are ours:
 *
 *   PBKDF2-HMAC-SHA1    passphrase -> PMK, 4096 rounds with the SSID as
 *                       salt (RFC 8018, 802.11 J.4)
 *   the 802.11 PRF      PMK -> PTK, HMAC-SHA1 in counter mode (12.7.1.2)
 *   AES key wrap        RFC 3394, built from single AES blocks, which
 *                       is how message 3 carries the group key
 *   the MIC             HMAC-SHA1 over the frame, cut to 16 bytes
 *
 * Each of them is checked against published vectors - the 802.11
 * passphrase vectors, RFC 6070, RFC 2202, RFC 3394 section 4, and a
 * real handshake captured off the air - by the test program the wpa
 * daemon's directory points to. The AES used is BearSSL's constant-time
 * "ct" implementation, which is the right one for a 32-bit ARM with no
 * AES instructions and no reason to leak the KEK through the cache.
 *
 * ── The one rule about frames ──
 * A frame is a byte array, fields are read with explicit big-endian
 * loads, and nothing is ever cast to a u16* or u64*. See the header.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "wpa4way.h"
#include "bearssl.h"

/* ── Small helpers ─────────────────────────────────────────────────── */

static u16 be16(const u8 *p)
{
    return (u16)(((u16)p[0] << 8) | p[1]);
}

static void put_be16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

void wpa_wipe(void *p, size_t n)
{
    /* Through a volatile pointer, so the stores cannot be proved dead
     * and removed - which is exactly what an optimiser does to a
     * memset of a local that is about to go out of scope. */
    volatile u8 *v = p;
    while (n--)
        *v++ = 0;
}

bool wpa_ct_equal(const u8 *a, const u8 *b, size_t n)
{
    u8 d = 0;
    for (size_t i = 0; i < n; i++)
        d |= (u8)(a[i] ^ b[i]);
    return d == 0;
}

/* ══ HMAC, PBKDF2, PRF ═══════════════════════════════════════════════ */

void wpa_hmac_sha1(const u8 *key, size_t keylen, const u8 *msg, size_t len,
                   u8 out[20])
{
    br_hmac_key_context kc;
    br_hmac_context hc;

    br_hmac_key_init(&kc, &br_sha1_vtable, key, keylen);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, msg, len);
    br_hmac_out(&hc, out);

    wpa_wipe(&kc, sizeof kc);
    wpa_wipe(&hc, sizeof hc);
}

/* RFC 8018 section 5.2, with HMAC-SHA1 as the PRF.
 *
 * The HMAC key schedule - the two padded key blocks already run through
 * SHA-1 - is computed once and reused for all 8192 HMACs a WPA
 * passphrase costs. That is not a micro-optimisation on the Pi Zero W:
 * without it the inner and outer key blocks are hashed again every
 * round, which doubles the work on a CPU where the whole derivation is
 * already the slowest thing a connection does. */
void wpa_pbkdf2_sha1(const u8 *pass, size_t passlen,
                     const u8 *salt, size_t saltlen,
                     u32 rounds, u8 *out, size_t outlen)
{
    br_hmac_key_context kc;
    br_hmac_context hc;
    u8 u[20], t[20], idx[4];

    br_hmac_key_init(&kc, &br_sha1_vtable, pass, passlen);

    for (u32 block = 1; outlen > 0; block++) {
        idx[0] = (u8)(block >> 24);
        idx[1] = (u8)(block >> 16);
        idx[2] = (u8)(block >> 8);
        idx[3] = (u8)block;

        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, salt, saltlen);
        br_hmac_update(&hc, idx, 4);
        br_hmac_out(&hc, u);
        memcpy(t, u, 20);

        for (u32 r = 1; r < rounds; r++) {
            br_hmac_init(&hc, &kc, 0);
            br_hmac_update(&hc, u, 20);
            br_hmac_out(&hc, u);
            for (int i = 0; i < 20; i++)
                t[i] ^= u[i];
        }

        size_t take = outlen < 20 ? outlen : 20;
        memcpy(out, t, take);
        out    += take;
        outlen -= take;
    }

    wpa_wipe(&kc, sizeof kc);
    wpa_wipe(&hc, sizeof hc);
    wpa_wipe(u, sizeof u);
    wpa_wipe(t, sizeof t);
}

/* IEEE 802.11-2020 12.7.1.2:
 *
 *   for i in 0 .. (len + 159) / 160:
 *       R = R || HMAC-SHA1(K, A || Y || B || i)
 *
 * where A is the label as ASCII without its terminator, Y is one zero
 * byte, and i is a single byte counting from zero. The zero byte is the
 * detail that is easy to lose, and losing it produces a perfectly
 * plausible key that matches nobody else's. */
void wpa_prf_sha1(const u8 *key, size_t keylen, const char *label,
                  const u8 *data, size_t datalen, u8 *out, size_t outlen)
{
    br_hmac_key_context kc;
    br_hmac_context hc;
    u8 h[20];
    const u8 zero = 0;

    br_hmac_key_init(&kc, &br_sha1_vtable, key, keylen);

    for (u8 i = 0; outlen > 0; i++) {
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, label, strlen(label));
        br_hmac_update(&hc, &zero, 1);
        br_hmac_update(&hc, data, datalen);
        br_hmac_update(&hc, &i, 1);
        br_hmac_out(&hc, h);

        size_t take = outlen < 20 ? outlen : 20;
        memcpy(out, h, take);
        out    += take;
        outlen -= take;
    }

    wpa_wipe(&kc, sizeof kc);
    wpa_wipe(&hc, sizeof hc);
    wpa_wipe(h, sizeof h);
}

/* ══ AES key wrap (RFC 3394) ═════════════════════════════════════════
 *
 * BearSSL offers AES as CBC, CTR and friends, not as a bare block
 * function. CBC over exactly one block with an all-zero IV is the bare
 * block function - C = E(K, P xor 0) - and that is what these two use.
 * The IV is reset before every block because the CBC routines update it
 * in place, and forgetting that turns the second block into a chained
 * one that only looks right on the first. */

static void aes_ecb_enc(const br_aes_ct_cbcenc_keys *k, u8 blk[16])
{
    u8 iv[16];
    memset(iv, 0, sizeof iv);
    br_aes_ct_cbcenc_run(k, iv, blk, 16);
}

static void aes_ecb_dec(const br_aes_ct_cbcdec_keys *k, u8 blk[16])
{
    u8 iv[16];
    memset(iv, 0, sizeof iv);
    br_aes_ct_cbcdec_run(k, iv, blk, 16);
}

static const u8 KW_IV[8] = { 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6 };

static bool kek_len_ok(size_t n)
{
    return n == 16 || n == 24 || n == 32;
}

/* A xor t, with t as a 64-bit big-endian number. */
static void xor_counter(u8 a[8], u64 t)
{
    for (int b = 7; b >= 0; b--) {
        a[b] ^= (u8)t;
        t >>= 8;
    }
}

bool wpa_aes_wrap(const u8 *kek, size_t keklen, const u8 *plain, size_t n,
                  u8 *out)
{
    if (!kek_len_ok(keklen) || n < 16 || n % 8 != 0)
        return false;

    br_aes_ct_cbcenc_keys k;
    br_aes_ct_cbcenc_init(&k, kek, keklen);

    size_t blocks = n / 8;
    u8 a[8], b[16];
    memcpy(a, KW_IV, 8);
    memmove(out + 8, plain, n);         /* R[1..n] live in out[8..]   */

    for (u32 j = 0; j <= 5; j++) {
        for (size_t i = 1; i <= blocks; i++) {
            u8 *r = out + 8 * i;
            memcpy(b, a, 8);
            memcpy(b + 8, r, 8);
            aes_ecb_enc(&k, b);
            memcpy(a, b, 8);
            xor_counter(a, (u64)blocks * j + i);
            memcpy(r, b + 8, 8);
        }
    }
    memcpy(out, a, 8);

    wpa_wipe(&k, sizeof k);
    wpa_wipe(b, sizeof b);
    return true;
}

bool wpa_aes_unwrap(const u8 *kek, size_t keklen, const u8 *cipher, size_t n,
                    u8 *out)
{
    if (!kek_len_ok(keklen) || n < 24 || n % 8 != 0)
        return false;

    br_aes_ct_cbcdec_keys k;
    br_aes_ct_cbcdec_init(&k, kek, keklen);

    size_t blocks = n / 8 - 1;
    u8 a[8], b[16];
    memcpy(a, cipher, 8);
    memmove(out, cipher + 8, n - 8);    /* R[1..n] live in out[0..]   */

    for (int j = 5; j >= 0; j--) {
        for (size_t i = blocks; i >= 1; i--) {
            u8 *r = out + 8 * (i - 1);
            memcpy(b, a, 8);
            xor_counter(b, (u64)blocks * (u32)j + i);
            memcpy(b + 8, r, 8);
            aes_ecb_dec(&k, b);
            memcpy(a, b, 8);
            memcpy(r, b + 8, 8);
        }
    }

    wpa_wipe(&k, sizeof k);
    wpa_wipe(b, sizeof b);

    /* The integrity check is the whole point of a key wrap: a KEK that
     * is wrong by one bit produces a random-looking A, not a slightly
     * different key. Output that failed it is not handed back. */
    if (!wpa_ct_equal(a, KW_IV, 8)) {
        wpa_wipe(out, n - 8);
        return false;
    }
    return true;
}

/* ══ PSK -> PMK ══════════════════════════════════════════════════════ */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

wpa_psk_err_t wpa_pmk_from_psk(const char *psk, const u8 *ssid,
                               size_t ssid_len, u8 pmk[WPA_PMK_LEN])
{
    memset(pmk, 0, WPA_PMK_LEN);
    size_t n = strlen(psk);

    /* 64 characters is never a passphrase - the standard caps those at
     * 63 precisely so that 64 can mean "this is the key, in hex". */
    if (n == 64) {
        for (size_t i = 0; i < 32; i++) {
            int hi = hexval(psk[2 * i]), lo = hexval(psk[2 * i + 1]);
            if (hi < 0 || lo < 0) {
                memset(pmk, 0, WPA_PMK_LEN);
                return WPA_PSK_BAD_HEX;
            }
            pmk[i] = (u8)(hi << 4 | lo);
        }
        return WPA_PSK_OK;
    }
    if (n < 8)
        return WPA_PSK_SHORT;
    if (n > 63)
        return WPA_PSK_LONG;
    for (size_t i = 0; i < n; i++) {
        u8 c = (u8)psk[i];
        if (c < 32 || c > 126)
            return WPA_PSK_NOT_ASCII;
    }
    if (ssid_len == 0 || ssid_len > 32)
        return WPA_PSK_NO_SSID;

    wpa_pbkdf2_sha1((const u8 *)psk, n, ssid, ssid_len, 4096,
                    pmk, WPA_PMK_LEN);
    return WPA_PSK_OK;
}

const char *wpa_psk_error(wpa_psk_err_t e)
{
    switch (e) {
    case WPA_PSK_OK:        return "ok";
    case WPA_PSK_SHORT:     return "a WPA2 password is at least 8 characters";
    case WPA_PSK_LONG:      return "a WPA2 password is at most 63 characters"
                                   " (64 means a key written in hex)";
    case WPA_PSK_NOT_ASCII: return "a WPA2 password can only contain"
                                   " printable ASCII characters";
    case WPA_PSK_BAD_HEX:   return "64 characters is a key in hex, and this"
                                   " one has a character that is not hex";
    case WPA_PSK_NO_SSID:   return "the network name is empty or longer"
                                   " than 32 bytes";
    }
    return "an unknown password problem";
}

/* ══ PTK ═════════════════════════════════════════════════════════════ */

void wpa_ptk_derive(const u8 pmk[WPA_PMK_LEN], const u8 aa[6],
                    const u8 spa[6], const u8 anonce[WPA_NONCE_LEN],
                    const u8 snonce[WPA_NONCE_LEN], wpa_ptk_t *ptk)
{
    u8 data[6 + 6 + WPA_NONCE_LEN + WPA_NONCE_LEN];
    u8 key[WPA_KCK_LEN + WPA_KEK_LEN + WPA_TK_LEN];

    /* min and max by memcmp, which on bytes is the numeric order the
     * standard means when it says "the smaller address". */
    if (memcmp(aa, spa, 6) < 0) {
        memcpy(data, aa, 6);
        memcpy(data + 6, spa, 6);
    } else {
        memcpy(data, spa, 6);
        memcpy(data + 6, aa, 6);
    }
    if (memcmp(anonce, snonce, WPA_NONCE_LEN) < 0) {
        memcpy(data + 12, anonce, WPA_NONCE_LEN);
        memcpy(data + 12 + WPA_NONCE_LEN, snonce, WPA_NONCE_LEN);
    } else {
        memcpy(data + 12, snonce, WPA_NONCE_LEN);
        memcpy(data + 12 + WPA_NONCE_LEN, anonce, WPA_NONCE_LEN);
    }

    wpa_prf_sha1(pmk, WPA_PMK_LEN, "Pairwise key expansion",
                 data, sizeof data, key, sizeof key);

    memcpy(ptk->kck, key, WPA_KCK_LEN);
    memcpy(ptk->kek, key + WPA_KCK_LEN, WPA_KEK_LEN);
    memcpy(ptk->tk,  key + WPA_KCK_LEN + WPA_KEK_LEN, WPA_TK_LEN);
    wpa_wipe(key, sizeof key);
}

/* ══ Our RSN element ═════════════════════════════════════════════════ */

static void put_suite(u8 *p, u32 s)
{
    p[0] = (u8)(s >> 24);
    p[1] = (u8)(s >> 16);
    p[2] = (u8)(s >> 8);
    p[3] = (u8)s;
}

size_t wpa_rsn_ie_build(u8 *out, size_t cap, u32 group_cipher)
{
    /* 48 len | ver 1 | group | 1 pairwise: CCMP | 1 AKM: PSK | caps.
     * The counts are little-endian, unlike every other number in this
     * file - that is the RSN element's convention, not the EAPOL one. */
    if (cap < 22)
        return 0;
    out[0] = 48;
    out[1] = 20;
    out[2] = 1;  out[3] = 0;
    put_suite(out + 4, group_cipher);
    out[8] = 1;  out[9] = 0;
    put_suite(out + 10, WPA_SUITE_CCMP);
    out[14] = 1; out[15] = 0;
    put_suite(out + 16, WPA_SUITE_PSK);
    /* Capabilities 0: no pre-authentication, and MFPC clear - we do not
     * do 802.11w, and saying we could would let an AP turn it on. */
    out[20] = 0; out[21] = 0;
    return 22;
}

/* ══ Parsing ═════════════════════════════════════════════════════════ */

wpa_kp_err_t wpa_key_parse(const u8 *f, size_t len, wpa_key_t *k)
{
    memset(k, 0, sizeof *k);

    if (len < WPA_EAPOL_HDR)
        return WPA_KP_SHORT_HEADER;
    k->eapol_version = f[0];
    if (f[1] != WPA_EAPOL_KEY)
        return WPA_KP_NOT_KEY;
    k->body_len = be16(f + 2);
    if ((size_t)k->body_len > len - WPA_EAPOL_HDR)
        return WPA_KP_BODY_LEN;
    if ((size_t)k->body_len + WPA_EAPOL_HDR < WPA_KEY_MIN)
        return WPA_KP_SHORT_BODY;

    k->desc = f[WPA_OFF_DESC];
    if (k->desc == WPA_DESC_WPA1)
        return WPA_KP_DESC_WPA1;
    if (k->desc != WPA_DESC_RSN)
        return WPA_KP_DESC_UNKNOWN;

    k->info    = be16(f + WPA_OFF_INFO);
    k->key_len = be16(f + WPA_OFF_KEYLEN);
    memcpy(k->replay, f + WPA_OFF_REPLAY, WPA_REPLAY_LEN);
    memcpy(k->nonce,  f + WPA_OFF_NONCE,  WPA_NONCE_LEN);
    memcpy(k->rsc,    f + WPA_OFF_RSC,    8);
    memcpy(k->mic,    f + WPA_OFF_MIC,    WPA_MIC_LEN);
    k->data_len = be16(f + WPA_OFF_DATALEN);

    /* The body can be longer than the key data - the MIC still covers
     * all of it - but the key data can never be longer than the body. */
    if ((size_t)WPA_OFF_DATA + k->data_len > (size_t)k->body_len + WPA_EAPOL_HDR)
        return WPA_KP_DATA_LEN;

    k->data      = f + WPA_OFF_DATA;
    k->frame_len = (size_t)k->body_len + WPA_EAPOL_HDR;
    return WPA_KP_OK;
}

const char *wpa_kp_error(wpa_kp_err_t e)
{
    switch (e) {
    case WPA_KP_OK:           return "ok";
    case WPA_KP_SHORT_HEADER: return "shorter than an EAPOL header";
    case WPA_KP_NOT_KEY:      return "an EAPOL packet that is not an"
                                     " EAPOL-Key frame";
    case WPA_KP_BODY_LEN:     return "the EAPOL header claims more bytes"
                                     " than arrived";
    case WPA_KP_SHORT_BODY:   return "too short for an EAPOL-Key body";
    case WPA_KP_DESC_WPA1:    return "the old WPA1 key descriptor, which"
                                     " this supplicant does not do";
    case WPA_KP_DESC_UNKNOWN: return "a key descriptor type that is not RSN";
    case WPA_KP_DATA_LEN:     return "the key data length runs past the"
                                     " end of the frame";
    }
    return "malformed";
}

/* ══ MIC ═════════════════════════════════════════════════════════════
 *
 * Descriptor version 2: HMAC-SHA1 over the whole EAPOL frame with the
 * MIC field zeroed, cut to 16 bytes. Fed to the HMAC in three pieces
 * rather than copied into a buffer with a hole punched in it: nothing
 * to size, and the caller's frame is never written to. */
void wpa_mic_compute(const u8 kck[WPA_KCK_LEN], const u8 *frame, size_t len,
                     u8 mic[WPA_MIC_LEN])
{
    static const u8 zeros[WPA_MIC_LEN];
    br_hmac_key_context kc;
    br_hmac_context hc;
    u8 h[20];

    br_hmac_key_init(&kc, &br_sha1_vtable, kck, WPA_KCK_LEN);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, frame, WPA_OFF_MIC);
    br_hmac_update(&hc, zeros, WPA_MIC_LEN);
    br_hmac_update(&hc, frame + WPA_OFF_MIC + WPA_MIC_LEN,
                   len - (WPA_OFF_MIC + WPA_MIC_LEN));
    br_hmac_out(&hc, h);
    memcpy(mic, h, WPA_MIC_LEN);

    wpa_wipe(&kc, sizeof kc);
    wpa_wipe(&hc, sizeof hc);
    wpa_wipe(h, sizeof h);
}

bool wpa_mic_check(const u8 kck[WPA_KCK_LEN], const u8 *frame, size_t len)
{
    u8 want[WPA_MIC_LEN];
    if (len < WPA_KEY_MIN)
        return false;
    wpa_mic_compute(kck, frame, len, want);
    return wpa_ct_equal(want, frame + WPA_OFF_MIC, WPA_MIC_LEN);
}

/* ══ Key data ════════════════════════════════════════════════════════ */

#define KDE_GTK     1
#define KDE_PMKID   4
#define KDE_IGTK    9

bool wpa_kde_parse(const u8 *d, size_t len, wpa_kde_t *out)
{
    memset(out, 0, sizeof *out);

    size_t i = 0;
    while (i < len) {
        u8 id = d[i];

        /* The key wrap needs a multiple of 8 bytes, so an AP pads with
         * 0xDD followed by zeros. That looks like the start of a vendor
         * element with length 0, which no real element has. */
        if (id == 0xDD && (i + 1 == len || d[i + 1] == 0))
            break;

        if (len - i < 2)
            return false;
        size_t elen = d[i + 1];
        if (elen > len - i - 2)
            return false;
        const u8 *body = d + i + 2;

        if (id == 48 && !out->rsn_ie) {
            out->rsn_ie     = d + i;
            out->rsn_ie_len = elen + 2;
        } else if (id == 244 && !out->rsnxe) {
            out->rsnxe     = d + i;
            out->rsnxe_len = elen + 2;
        } else if (id == 0xDD && elen >= 4 &&
                   body[0] == 0x00 && body[1] == 0x0F && body[2] == 0xAC) {
            u8 type = body[3];
            if (type == KDE_GTK) {
                /* KeyID in bits 0-1, Tx in bit 2, one reserved byte,
                 * then the key. A GTK KDE shorter than that header is a
                 * broken frame, not an empty key. */
                if (elen < 6 + 1)
                    return false;
                out->gtk_idx  = body[4] & 0x03;
                out->gtk_tx   = (body[4] & 0x04) != 0;
                out->gtk      = body + 6;
                out->gtk_len  = elen - 6;
                out->have_gtk = true;
            } else if (type == KDE_PMKID) {
                out->have_pmkid = true;
            } else if (type == KDE_IGTK) {
                out->have_igtk = true;
            }
        }
        i += 2 + elen;
    }
    return true;
}

/* ══ The supplicant state machine ════════════════════════════════════ */

void wpa_sm_reset(wpa_sm_t *sm)
{
    sm->state      = WPA_SM_WAIT_1;
    sm->replay_set = false;
    wpa_wipe(sm->replay, sizeof sm->replay);
    wpa_wipe(sm->anonce, sizeof sm->anonce);
    wpa_wipe(sm->snonce, sizeof sm->snonce);
    sm->snonce_set = false;
    wpa_wipe(&sm->tptk, sizeof sm->tptk);
    wpa_wipe(&sm->ptk, sizeof sm->ptk);
    sm->ptk_set    = false;
    wpa_wipe(sm->gtk, sizeof sm->gtk);
    sm->gtk_len    = 0;
    sm->gtk_idx    = 0;
    sm->gtk_set    = false;
    sm->msg1_seen = sm->msg1_after_2 = sm->msg3_seen = 0;
    sm->group_rekeys = sm->ptk_rekeys = 0;
    sm->why[0] = '\0';
}

/* The replay counter must go up. It is compared against the last one
 * that came with a MIC we verified - never against a message 1, which
 * has no MIC and so proves nothing about who sent it. memcmp on the
 * eight big-endian bytes is the numeric comparison. */
static bool replay_newer(const wpa_sm_t *sm, const u8 r[WPA_REPLAY_LEN])
{
    return !sm->replay_set || memcmp(r, sm->replay, WPA_REPLAY_LEN) > 0;
}

/* Build one EAPOL-Key frame from us to the AP, MIC and all.
 *
 * EAPOL protocol version 1, not 2, for the reason wpa_supplicant gives
 * for the same default: there are access points that silently drop an
 * 802.1X-2004 version number they do not expect, and every AP accepts
 * version 1. A handshake that dies to that would look precisely like
 * the silence this stack was written to explain. */
static size_t build_key(u8 *tx, u16 info, const u8 replay[WPA_REPLAY_LEN],
                        const u8 *nonce_or_null, const u8 *data,
                        size_t data_len, const u8 kck[WPA_KCK_LEN])
{
    size_t total = WPA_OFF_DATA + data_len;
    if (total > WPA_FRAME_MAX)
        return 0;

    memset(tx, 0, total);
    tx[0] = 1;
    tx[1] = WPA_EAPOL_KEY;
    put_be16(tx + 2, (u16)(total - WPA_EAPOL_HDR));
    tx[WPA_OFF_DESC] = WPA_DESC_RSN;
    put_be16(tx + WPA_OFF_INFO, info);
    put_be16(tx + WPA_OFF_KEYLEN, 0);   /* 0 from a supplicant in RSN */
    memcpy(tx + WPA_OFF_REPLAY, replay, WPA_REPLAY_LEN);
    if (nonce_or_null)
        memcpy(tx + WPA_OFF_NONCE, nonce_or_null, WPA_NONCE_LEN);
    put_be16(tx + WPA_OFF_DATALEN, (u16)data_len);
    if (data_len)
        memcpy(tx + WPA_OFF_DATA, data, data_len);

    wpa_mic_compute(kck, tx, total, tx + WPA_OFF_MIC);
    return total;
}

/* The GTK as nl80211 wants it. For CCMP that is the key as it came. For
 * TKIP the two 8-byte Michael keys come in the authenticator's order -
 * its transmit key first - and a station receives with that key, so
 * they are swapped. wpa_supplicant does the same, for the same reason. */
static void gtk_for_kernel(u32 group, const u8 *gtk, size_t len, u8 *out)
{
    if (group == WPA_SUITE_TKIP && len == 32) {
        memcpy(out, gtk, 16);
        memcpy(out + 16, gtk + 24, 8);
        memcpy(out + 24, gtk + 16, 8);
    } else {
        memcpy(out, gtk, len);
    }
}

static size_t gtk_len_for(u32 group)
{
    return group == WPA_SUITE_TKIP ? 32 : 16;
}

static wpa_rx_t ignored(wpa_sm_t *sm, const char *why)
{
    snprintf(sm->why, sizeof sm->why, "%s", why);
    return WPA_RX_IGNORED;
}

static wpa_rx_t fatal(wpa_sm_t *sm, const char *why)
{
    snprintf(sm->why, sizeof sm->why, "%s", why);
    return WPA_RX_FATAL;
}

/* Unwrap and walk the key data of an authenticated frame. The buffer is
 * the caller's so that the GTK pointers in kde stay valid after this
 * returns. */
static bool open_key_data(wpa_sm_t *sm, const wpa_key_t *k,
                          const u8 kek[WPA_KEK_LEN], u8 *plain,
                          size_t plain_cap, size_t *plain_len,
                          wpa_kde_t *kde, const char *msgname)
{
    if (!(k->info & WPA_KI_ENCRYPTED)) {
        snprintf(sm->why, sizeof sm->why,
                 "%s carries its key data unencrypted - an RSN access"
                 " point always wraps it", msgname);
        return false;
    }
    if (k->data_len < 24 || k->data_len % 8 != 0 ||
        (size_t)k->data_len - 8 > plain_cap) {
        snprintf(sm->why, sizeof sm->why,
                 "%s has %u bytes of wrapped key data, which is not a"
                 " length AES key wrap produces", msgname,
                 (unsigned)k->data_len);
        return false;
    }
    if (!wpa_aes_unwrap(kek, WPA_KEK_LEN, k->data, k->data_len, plain)) {
        snprintf(sm->why, sizeof sm->why,
                 "the key data in %s failed the key wrap's integrity"
                 " check, so the KEK we derived is not the AP's", msgname);
        return false;
    }
    *plain_len = (size_t)k->data_len - 8;
    if (!wpa_kde_parse(plain, *plain_len, kde)) {
        snprintf(sm->why, sizeof sm->why,
                 "the key data in %s decrypted, but an element in it runs"
                 " past the end", msgname);
        return false;
    }
    return true;
}

static wpa_rx_t rx_msg1(wpa_sm_t *sm, const wpa_key_t *k,
                        const u8 fresh_nonce[WPA_NONCE_LEN], wpa_out_t *out)
{
    if (!replay_newer(sm, k->replay))
        return ignored(sm, "message 1 of 4 with a replay counter we have"
                           " already seen");
    if (sm->own_ie_len == 0)
        return fatal(sm, "there is no RSN element to put in message 2");

    sm->msg1_seen++;
    if (sm->state == WPA_SM_WAIT_3)
        sm->msg1_after_2++;

    /* A retransmitted message 1 in the same handshake gets the same
     * SNonce. A new handshake - the first one, or a rekey after the
     * keys are in - gets a fresh one. */
    if (!(sm->state == WPA_SM_WAIT_3 && sm->snonce_set)) {
        memcpy(sm->snonce, fresh_nonce, WPA_NONCE_LEN);
        sm->snonce_set = true;
    }
    memcpy(sm->anonce, k->nonce, WPA_NONCE_LEN);

    wpa_ptk_derive(sm->pmk, sm->aa, sm->spa, sm->anonce, sm->snonce,
                   &sm->tptk);

    out->tx_len = build_key(out->tx,
                            WPA_KEY_VERSION_AES | WPA_KI_PAIRWISE |
                            WPA_KI_MIC,
                            k->replay, sm->snonce, sm->own_ie,
                            sm->own_ie_len, sm->tptk.kck);
    if (!out->tx_len)
        return fatal(sm, "message 2 would not fit in a frame");

    sm->state  = WPA_SM_WAIT_3;
    out->msg   = 1;
    out->flags = WPA_DO_SEND;
    return WPA_RX_OK;
}

static wpa_rx_t rx_msg3(wpa_sm_t *sm, const wpa_key_t *k, const u8 *frame,
                        wpa_out_t *out)
{
    if (sm->state == WPA_SM_WAIT_1)
        return ignored(sm, "message 3 of 4 arrived before any message 1");
    if (!replay_newer(sm, k->replay))
        return ignored(sm, "message 3 of 4 with a replay counter we have"
                           " already seen");
    if (memcmp(k->nonce, sm->anonce, WPA_NONCE_LEN) != 0)
        return ignored(sm, "message 3 of 4 carries a different ANonce"
                           " from message 1, so it belongs to some other"
                           " handshake");
    if (!wpa_mic_check(sm->tptk.kck, frame, k->frame_len))
        return ignored(sm, "message 3 of 4 has a MIC that does not verify"
                           " with the key we derived at message 1");

    /* Authenticated from here on: this is the AP, and it agrees with us
     * about the PMK. Anything wrong below is the AP's doing, and none of
     * it is worth waiting for a retransmission to fix. */
    memcpy(sm->replay, k->replay, WPA_REPLAY_LEN);
    sm->replay_set = true;
    sm->msg3_seen++;

    if (k->key_len != WPA_TK_LEN) {
        snprintf(sm->why, sizeof sm->why,
                 "message 3 of 4 says the pairwise key is %u bytes; CCMP"
                 " is 16, so the AP chose a cipher we did not offer",
                 (unsigned)k->key_len);
        return WPA_RX_FATAL;
    }

    u8 plain[WPA_FRAME_MAX];
    size_t plain_len = 0;
    wpa_kde_t kde;
    if (!open_key_data(sm, k, sm->tptk.kek, plain, sizeof plain, &plain_len,
                       &kde, "message 3 of 4")) {
        wpa_wipe(plain, sizeof plain);
        return WPA_RX_FATAL;
    }

    if (!kde.rsn_ie) {
        wpa_wipe(plain, sizeof plain);
        return fatal(sm, "message 3 of 4 has no RSN element in it");
    }
    /* The AP repeats the RSN element from its beacon, under the MIC.
     * If it differs from what the beacon said, somebody rewrote the
     * beacon to talk us down to weaker settings - or the AP changed its
     * configuration between the scan and now. Either way the handshake
     * stops here, which is what the standard asks for (reason 17). */
    if (sm->ap_ie_len &&
        (kde.rsn_ie_len != sm->ap_ie_len ||
         memcmp(kde.rsn_ie, sm->ap_ie, sm->ap_ie_len) != 0)) {
        wpa_wipe(plain, sizeof plain);
        return fatal(sm, "the RSN element in message 3 of 4 is not the one"
                         " in the access point's beacon (802.11 reason 17)");
    }
    if (!kde.have_gtk) {
        wpa_wipe(plain, sizeof plain);
        return fatal(sm, "message 3 of 4 has no group key in it");
    }
    if (kde.gtk_len != gtk_len_for(sm->group_cipher)) {
        snprintf(sm->why, sizeof sm->why,
                 "the group key in message 3 of 4 is %u bytes; the %s group"
                 " cipher needs %u", (unsigned)kde.gtk_len,
                 sm->group_cipher == WPA_SUITE_TKIP ? "TKIP" : "CCMP",
                 (unsigned)gtk_len_for(sm->group_cipher));
        wpa_wipe(plain, sizeof plain);
        return WPA_RX_FATAL;
    }

    out->tx_len = build_key(out->tx,
                            WPA_KEY_VERSION_AES | WPA_KI_PAIRWISE |
                            WPA_KI_MIC | WPA_KI_SECURE,
                            k->replay, NULL, NULL, 0, sm->tptk.kck);
    out->msg   = 3;
    out->flags = WPA_DO_SEND;

    /* Install a key only when it is new. Reinstalling the same PTK or
     * GTK resets its packet number, and a retransmitted message 3 that
     * makes us do that is the whole of the KRACK attack. A message 3
     * that arrives again after we finished is still answered - our
     * message 4 may have been the frame that was lost - but it changes
     * nothing. */
    bool new_ptk = !sm->ptk_set ||
                   !wpa_ct_equal(sm->ptk.tk, sm->tptk.tk, WPA_TK_LEN);
    if (new_ptk) {
        if (sm->ptk_set)
            sm->ptk_rekeys++;
        sm->ptk = sm->tptk;
        sm->ptk_set = true;
        memcpy(out->tk, sm->ptk.tk, WPA_TK_LEN);
        out->flags |= WPA_DO_SET_PTK | WPA_DO_4WAY_DONE;
    }

    bool new_gtk = !sm->gtk_set || sm->gtk_idx != kde.gtk_idx ||
                   sm->gtk_len != kde.gtk_len ||
                   !wpa_ct_equal(sm->gtk, kde.gtk, kde.gtk_len);
    if (new_gtk) {
        memcpy(sm->gtk, kde.gtk, kde.gtk_len);
        sm->gtk_len = kde.gtk_len;
        sm->gtk_idx = kde.gtk_idx;
        sm->gtk_set = true;
        gtk_for_kernel(sm->group_cipher, kde.gtk, kde.gtk_len, out->gtk);
        out->gtk_len = kde.gtk_len;
        out->gtk_idx = kde.gtk_idx;
        memcpy(out->gtk_rsc, k->rsc, WPA_RSC_LEN);
        out->flags |= WPA_DO_SET_GTK;
    }

    sm->state = WPA_SM_DONE;
    sm->snonce_set = false;
    wpa_wipe(plain, sizeof plain);
    return WPA_RX_OK;
}

static wpa_rx_t rx_group1(wpa_sm_t *sm, const wpa_key_t *k, const u8 *frame,
                          wpa_out_t *out)
{
    if (!sm->ptk_set)
        return ignored(sm, "a group key message arrived before the"
                           " four-way handshake finished");
    if (!replay_newer(sm, k->replay))
        return ignored(sm, "a group key message with a replay counter we"
                           " have already seen");
    if (!wpa_mic_check(sm->ptk.kck, frame, k->frame_len))
        return ignored(sm, "a group key message whose MIC does not verify");

    memcpy(sm->replay, k->replay, WPA_REPLAY_LEN);
    sm->replay_set = true;

    u8 plain[WPA_FRAME_MAX];
    size_t plain_len = 0;
    wpa_kde_t kde;
    /* A broken rekey from an authenticated AP is refused but is not the
     * end of the association: the old group key still works, and the AP
     * will either retry or send us away with a reason code that says
     * more than we could. */
    if (!open_key_data(sm, k, sm->ptk.kek, plain, sizeof plain, &plain_len,
                       &kde, "the group key message")) {
        wpa_wipe(plain, sizeof plain);
        return WPA_RX_IGNORED;
    }
    if (!kde.have_gtk || kde.gtk_len != gtk_len_for(sm->group_cipher)) {
        wpa_wipe(plain, sizeof plain);
        return ignored(sm, "the group key message has no usable group key");
    }

    out->tx_len = build_key(out->tx,
                            WPA_KEY_VERSION_AES | WPA_KI_MIC | WPA_KI_SECURE,
                            k->replay, NULL, NULL, 0, sm->ptk.kck);
    out->msg   = 11;
    out->flags = WPA_DO_SEND | WPA_DO_GROUP_DONE;

    bool new_gtk = !sm->gtk_set || sm->gtk_idx != kde.gtk_idx ||
                   sm->gtk_len != kde.gtk_len ||
                   !wpa_ct_equal(sm->gtk, kde.gtk, kde.gtk_len);
    if (new_gtk) {
        memcpy(sm->gtk, kde.gtk, kde.gtk_len);
        sm->gtk_len = kde.gtk_len;
        sm->gtk_idx = kde.gtk_idx;
        sm->gtk_set = true;
        gtk_for_kernel(sm->group_cipher, kde.gtk, kde.gtk_len, out->gtk);
        out->gtk_len = kde.gtk_len;
        out->gtk_idx = kde.gtk_idx;
        memcpy(out->gtk_rsc, k->rsc, WPA_RSC_LEN);
        out->flags |= WPA_DO_SET_GTK;
        sm->group_rekeys++;
    }
    wpa_wipe(plain, sizeof plain);
    return WPA_RX_OK;
}

wpa_rx_t wpa_sm_rx(wpa_sm_t *sm, const u8 *frame, size_t len,
                   const u8 fresh_nonce[WPA_NONCE_LEN], wpa_out_t *out)
{
    memset(out, 0, sizeof *out);

    wpa_key_t k;
    wpa_kp_err_t e = wpa_key_parse(frame, len, &k);
    if (e != WPA_KP_OK) {
        snprintf(sm->why, sizeof sm->why, "an EAPOL frame we cannot use: %s",
                 wpa_kp_error(e));
        return WPA_RX_IGNORED;
    }

    if (k.info & WPA_KI_REQUEST)
        return ignored(sm, "a key request - those go from a station to an"
                           " access point, never to us");
    if (k.info & WPA_KI_ERROR)
        return ignored(sm, "a key frame with the Error bit set");
    if (!(k.info & WPA_KI_ACK))
        return ignored(sm, "a key frame without the Key Ack bit, so not"
                           " one an access point sends");

    unsigned ver = k.info & WPA_KI_VERSION_MASK;
    if (ver != WPA_KEY_VERSION_AES) {
        snprintf(sm->why, sizeof sm->why,
                 "the access point uses key descriptor version %u; we do"
                 " version 2 only (%s)", ver,
                 ver == 1 ? "1 is WPA1/TKIP" :
                 ver == 3 ? "3 is AES-CMAC, i.e. PSK-SHA256 or 802.11w" :
                            "0 is for AKMs we did not ask for");
        return WPA_RX_FATAL;
    }

    if (k.info & WPA_KI_PAIRWISE) {
        if (!(k.info & WPA_KI_MIC))
            return rx_msg1(sm, &k, fresh_nonce, out);
        if (k.info & WPA_KI_INSTALL)
            return rx_msg3(sm, &k, frame, out);
        return ignored(sm, "a pairwise key frame with a MIC and no Install"
                           " bit, which is no message of the handshake");
    }
    if ((k.info & WPA_KI_MIC) && (k.info & WPA_KI_SECURE))
        return rx_group1(sm, &k, frame, out);
    return ignored(sm, "a group key frame without the MIC and Secure bits");
}
