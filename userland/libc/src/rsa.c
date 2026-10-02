/* rsa.c - RSA-2048 signature verification, and nothing else.
 *
 * `update` replaces the file this board boots from. Until now it checked
 * that the download was the length the server claimed and that the first
 * 64 bytes looked like a kernel, which says the transfer worked and
 * nothing at all about who wrote it. Anyone who could answer for the
 * update URL - a hostile DNS reply, a machine on the same network, the
 * server itself - could hand this board a kernel of their choosing and
 * it would install it and reboot into it.
 *
 * So: the release is signed on the machine that builds it, and this
 * verifies the signature before anything is installed. Only the public
 * key is on the board, so nothing here can produce a signature; a
 * stolen image is inert without the private key, which never leaves the
 * build machine.
 *
 * RSA-2048 with PKCS#1 v1.5 padding over a SHA-256 digest, e = 65537.
 * Not because it is modern - it is not - but because verification is
 * one modular exponentiation and a padding check, which is a few
 * hundred lines that can be read in one sitting and tested exhaustively
 * against a reference. Ed25519 would be a better choice and four times
 * the code, most of it field arithmetic that is easy to get subtly
 * wrong. A signature check nobody can audit is not obviously better
 * than none.
 *
 * Verification only. There is no private-key code here and there never
 * should be.
 */
#include "types.h"
#include "string.h"
#include "rsa.h"

#define LIMBS 64            /* 64 x 32 bits = 2048 bits */

/* ── Big numbers ──────────────────────────────────────────────────────
 *
 * Little-endian arrays of 32-bit limbs, fixed length. No allocation, no
 * length tracking: every value here is exactly 2048 bits, and the one
 * place a wider intermediate appears (a product) is handled inside
 * mont_mul, which never materialises it.
 */

/* out = in, reading big-endian bytes (which is how RSA numbers travel). */
static void be_to_limbs(const u8 *be, u32 *out)
{
    for (int i = 0; i < LIMBS; i++) {
        const u8 *p = be + (LIMBS - 1 - i) * 4;
        out[i] = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                 ((u32)p[2] << 8)  |  (u32)p[3];
    }
}

static void limbs_to_be(const u32 *in, u8 *be)
{
    for (int i = 0; i < LIMBS; i++) {
        u8 *p = be + (LIMBS - 1 - i) * 4;
        p[0] = (u8)(in[i] >> 24); p[1] = (u8)(in[i] >> 16);
        p[2] = (u8)(in[i] >> 8);  p[3] = (u8)in[i];
    }
}

/* a >= b ? Compared from the top down, in constant time as far as the
 * loop shape goes - there is nothing secret here, but a data-dependent
 * early exit is a habit worth not forming. */
static int geq(const u32 *a, const u32 *b)
{
    int ge = 1;
    for (int i = 0; i < LIMBS; i++) {
        int lt = (a[i] < b[i]);
        int gt = (a[i] > b[i]);
        ge = gt ? 1 : (lt ? 0 : ge);
    }
    return ge;
}

/* a -= b, assuming a >= b. Returns the final borrow, which is 0. */
static u32 sub_in_place(u32 *a, const u32 *b)
{
    u64 borrow = 0;
    for (int i = 0; i < LIMBS; i++) {
        u64 d = (u64)a[i] - b[i] - borrow;
        a[i]  = (u32)d;
        borrow = (d >> 32) & 1;
    }
    return (u32)borrow;
}

/* ── Montgomery multiplication ────────────────────────────────────────
 *
 * The only way to do modular arithmetic here without implementing
 * division. Values are kept in Montgomery form (x * R mod n, with
 * R = 2^2048), where a modular multiply is a multiply plus a shift.
 *
 * n0inv is -n^-1 mod 2^32, computed once by Newton iteration: for odd n
 * the inverse doubles its correct bits each round, so five rounds cover
 * 32 bits.
 */
static u32 mont_n0inv(u32 n0)
{
    u32 x = n0;                 /* correct to 3 bits for odd n0 */
    for (int i = 0; i < 5; i++)
        x *= 2 - n0 * x;
    return (u32)(0u - x);       /* -n^-1 mod 2^32 */
}

/* out = a * b * R^-1 mod n. out may alias neither a nor b. */
static void mont_mul(u32 *out, const u32 *a, const u32 *b,
                     const u32 *n, u32 n0inv)
{
    u32 t[LIMBS + 1];
    memset(t, 0, sizeof t);

    for (int i = 0; i < LIMBS; i++) {
        /* t += a[i] * b */
        u64 carry = 0;
        for (int j = 0; j < LIMBS; j++) {
            u64 v = (u64)t[j] + (u64)a[i] * b[j] + carry;
            t[j]  = (u32)v;
            carry = v >> 32;
        }
        u64 top = (u64)t[LIMBS] + carry;
        t[LIMBS] = (u32)top;
        u32 overflow = (u32)(top >> 32);

        /* t += (t[0] * n0inv) * n, which makes the low limb zero */
        u32 m = t[0] * n0inv;
        carry = 0;
        for (int j = 0; j < LIMBS; j++) {
            u64 v = (u64)t[j] + (u64)m * n[j] + carry;
            t[j]  = (u32)v;
            carry = v >> 32;
        }
        top = (u64)t[LIMBS] + carry;
        t[LIMBS] = (u32)top;
        overflow += (u32)(top >> 32);

        /* shift down one limb */
        for (int j = 0; j < LIMBS; j++)
            t[j] = t[j + 1];
        t[LIMBS] = overflow;
    }

    /* t is now < 2n; bring it under n. t[LIMBS] is 0 or 1. */
    if (t[LIMBS] || geq(t, n))
        sub_in_place(t, n);

    memcpy(out, t, LIMBS * sizeof(u32));
}

/* ── The public operation ─────────────────────────────────────────────
 *
 * m = s^65537 mod n. 65537 is 2^16 + 1, so this is sixteen squarings
 * and one multiply - no exponent bits to walk, nothing secret to leak.
 */
static bool modexp_65537(const u8 *sig_be, const u8 *mod_be, u8 *out_be)
{
    u32 n[LIMBS], s[LIMBS], a[LIMBS], b[LIMBS], rr[LIMBS];

    be_to_limbs(mod_be, n);
    be_to_limbs(sig_be, s);

    if (!(n[0] & 1))            /* an even modulus is not an RSA key */
        return false;
    if (geq(s, n))              /* the signature must be less than n */
        return false;

    u32 n0inv = mont_n0inv(n[0]);

    /* R^2 mod n, by starting at R mod n and doubling 2048 times.
     * R mod n = 2^2048 mod n = (2^2048 - n) + ... - computed by taking
     * the value 1 shifted up limb by limb, reducing as we go. Doubling
     * is cheap and there are only 2048 of them. */
    memset(rr, 0, sizeof rr);
    rr[0] = 1;
    for (int i = 0; i < 2 * LIMBS * 32; i++) {
        u32 carry = 0;
        for (int j = 0; j < LIMBS; j++) {
            u32 hi = rr[j] >> 31;
            rr[j] = (rr[j] << 1) | carry;
            carry = hi;
        }
        if (carry || geq(rr, n))
            sub_in_place(rr, n);
    }

    /* a = s in Montgomery form */
    mont_mul(a, s, rr, n, n0inv);

    /* sixteen squarings: a = s^(2^16) * R mod n */
    for (int i = 0; i < 16; i++) {
        mont_mul(b, a, a, n, n0inv);
        memcpy(a, b, sizeof a);
    }

    /* one more multiply by s, for the +1 in 65537 */
    u32 sm[LIMBS];
    mont_mul(sm, s, rr, n, n0inv);
    mont_mul(b, a, sm, n, n0inv);

    /* out of Montgomery form: multiply by 1 */
    u32 one[LIMBS];
    memset(one, 0, sizeof one);
    one[0] = 1;
    mont_mul(a, b, one, n, n0inv);

    limbs_to_be(a, out_be);
    return true;
}

/* The DigestInfo prefix PKCS#1 v1.5 puts in front of a SHA-256 hash:
 * the DER encoding of the algorithm identifier. Fixed bytes. */
static const u8 SHA256_DER[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
    0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
    0x00, 0x04, 0x20
};

bool lp_rsa2048_verify(const u8 *modulus_be, const u8 *sig_be,
                       const u8 *sha256)
{
    u8 m[RSA2048_BYTES];

    if (!modexp_65537(sig_be, modulus_be, m))
        return false;

    /* PKCS#1 v1.5 signature block:
     *   00 01 FF FF ... FF 00 <DigestInfo> <32-byte hash>
     * Everything about it is fixed, so check it exactly rather than
     * scanning for the separator - a parser that hunts for 0x00 is how
     * padding-check bugs happen. */
    size_t dlen = sizeof SHA256_DER + 32;
    size_t pad  = RSA2048_BYTES - dlen - 3;   /* the FF run */

    int bad = 0;
    bad |= (m[0] != 0x00);
    bad |= (m[1] != 0x01);
    for (size_t i = 0; i < pad; i++)
        bad |= (m[2 + i] != 0xFF);
    bad |= (m[2 + pad] != 0x00);

    for (size_t i = 0; i < sizeof SHA256_DER; i++)
        bad |= (m[3 + pad + i] != SHA256_DER[i]);

    for (int i = 0; i < 32; i++)
        bad |= (m[3 + pad + sizeof SHA256_DER + i] != sha256[i]);

    return bad == 0;
}
