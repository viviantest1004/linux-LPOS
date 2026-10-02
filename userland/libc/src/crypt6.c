/* crypt6.c - SHA-512 crypt ($6$), and checking a password against
 * /etc/shadow.
 *
 * This lived inside passwd, the only program that wanted it. Now sudo,
 * login and the two administrator daemons (lp-privd, lp-diskd) all have
 * to answer "is this the person's password", and four private copies of
 * a password hash is four chances for one of them to be subtly wrong.
 *
 * Only $6$ is understood. The image sets pam_unix to sha512 so Debian's
 * own passwd, chpasswd and useradd write $6$ as well; a yescrypt ($y$)
 * hash is answered "no" rather than guessed at, and the caller says why.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "syscall.h"
#include "crypt6.h"

#define PW_MAX       LP_CRYPT6_PW_MAX
#define SALT_LEN     16
#define ROUNDS_DEF   5000

/* ── SHA-512 crypt ────────────────────────────────────────────────────
 *
 * Ulrich Drepper's $6$, on top of lp_digest.
 *
 * lp_digest_final hands back hex, so each 64-byte digest is decoded once
 * on the way out. That costs 128 characters of parsing per round and
 * 5000 rounds is still a few milliseconds; the alternative was a second
 * SHA-512 in this file, which is a worse thing to have two of. */

static const char B64[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static int hexnib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

static void sha_final(lp_digest_t *d, u8 out[64])
{
    char hex[2 * 64 + 1];
    lp_digest_final(d, hex);
    for (int i = 0; i < 64; i++)
        out[i] = (u8)((hexnib(hex[2 * i]) << 4) | hexnib(hex[2 * i + 1]));
}

/* The byte order the $6$ encoding interleaves the digest in. Twenty-one
 * groups of three bytes, then one lone byte. It is not a rotation you
 * can compute, so it is written out. */
static const u8 PERM[63] = {
     0,21,42, 22,43, 1, 44, 2,23,  3,24,45, 25,46, 4, 47, 5,26,
     6,27,48, 28,49, 7, 50, 8,29,  9,30,51, 31,52,10, 53,11,32,
    12,33,54, 34,55,13, 56,14,35, 15,36,57, 37,58,16, 59,17,38,
    18,39,60, 40,61,19, 62,20,41
};

static void b64_group(u32 w, int n, char **out)
{
    for (int i = 0; i < n; i++) {
        *(*out)++ = B64[w & 0x3F];
        w >>= 6;
    }
}

/* rounds is what the salt asked for, or ROUNDS_DEF. `salt` is the salt
 * text only, without the $6$ and without any rounds= part. */
void lp_crypt6(const char *pw, const char *salt, unsigned rounds,
               char *out, size_t outn)
{
    size_t pwlen = strlen(pw), slen = strlen(salt);
    if (pwlen > PW_MAX)
        pwlen = PW_MAX;
    if (slen > SALT_LEN)
        slen = SALT_LEN;
    if (rounds < 1000)
        rounds = 1000;              /* the floor the format defines */
    if (rounds > 999999999)
        rounds = 999999999;
    u8 A[64], B[64], DP[64], DS[64];
    u8 P[PW_MAX], S[64];
    lp_digest_t c;

    lp_digest_init(&c, LP_SHA512);
    lp_digest_update(&c, pw, pwlen);
    lp_digest_update(&c, salt, slen);
    lp_digest_update(&c, pw, pwlen);
    sha_final(&c, B);

    lp_digest_init(&c, LP_SHA512);
    lp_digest_update(&c, pw, pwlen);
    lp_digest_update(&c, salt, slen);
    size_t cnt = pwlen;
    for (; cnt > 64; cnt -= 64)
        lp_digest_update(&c, B, 64);
    lp_digest_update(&c, B, cnt);
    for (size_t n = pwlen; n > 0; n >>= 1) {
        if (n & 1) lp_digest_update(&c, B, 64);
        else       lp_digest_update(&c, pw, pwlen);
    }
    sha_final(&c, A);

    lp_digest_init(&c, LP_SHA512);
    for (size_t i = 0; i < pwlen; i++)
        lp_digest_update(&c, pw, pwlen);
    sha_final(&c, DP);
    for (size_t i = 0; i < pwlen; i++)
        P[i] = DP[i % 64];

    lp_digest_init(&c, LP_SHA512);
    for (unsigned i = 0; i < 16u + A[0]; i++)
        lp_digest_update(&c, salt, slen);
    sha_final(&c, DS);
    for (size_t i = 0; i < slen; i++)
        S[i] = DS[i % 64];

    for (unsigned r = 0; r < rounds; r++) {
        lp_digest_init(&c, LP_SHA512);
        if (r & 1) lp_digest_update(&c, P, pwlen);
        else       lp_digest_update(&c, A, 64);
        if (r % 3) lp_digest_update(&c, S, slen);
        if (r % 7) lp_digest_update(&c, P, pwlen);
        if (r & 1) lp_digest_update(&c, A, 64);
        else       lp_digest_update(&c, P, pwlen);
        sha_final(&c, A);
    }

    char body[90], *p = body;
    for (int i = 0; i < 63; i += 3)
        b64_group(((u32)A[PERM[i]] << 16) | ((u32)A[PERM[i + 1]] << 8) |
                  (u32)A[PERM[i + 2]], 4, &p);
    b64_group((u32)A[63], 2, &p);
    *p = '\0';

    if (rounds == ROUNDS_DEF)
        snprintf(out, outn, "$6$%s$%s", salt, body);
    else
        snprintf(out, outn, "$6$rounds=%u$%s$%s", rounds, salt, body);
}

/* Does this password produce this stored hash? Only $6$ is understood;
 * anything else is answered "no" rather than guessed at. */
bool lp_crypt6_verify(const char *pw, const char *stored)
{
    if (strncmp(stored, "$6$", 3) != 0)
        return false;
    const char *s = stored + 3;
    unsigned rounds = ROUNDS_DEF;
    if (strncmp(s, "rounds=", 7) == 0) {
        s += 7;
        rounds = 0;
        while (*s >= '0' && *s <= '9')
            rounds = rounds * 10 + (unsigned)(*s++ - '0');
        if (*s != '$' || rounds == 0)
            return false;
        s++;
    }
    const char *dollar = strchr(s, '$');
    if (!dollar || dollar - s > SALT_LEN)
        return false;

    char salt[SALT_LEN + 1];
    size_t n = (size_t)(dollar - s);
    memcpy(salt, s, n);
    salt[n] = '\0';

    char again[256];
    lp_crypt6(pw, salt, rounds, again, sizeof again);
    /* Every byte compared, whatever the first difference: how long a
     * wrong guess takes must not say how much of it was right. */
    size_t la = strlen(again), ls = strlen(stored);
    unsigned diff = (unsigned)(la ^ ls);
    for (size_t i = 0; i < la && i < ls; i++)
        diff |= (unsigned)(u8)again[i] ^ (u8)stored[i];
    return diff == 0;
}

static bool make_salt(char *out)
{
    u8 raw[SALT_LEN];
    if (lp_getrandom(raw, sizeof raw, 0) != (long)sizeof raw)
        return false;
    for (int i = 0; i < SALT_LEN; i++)
        out[i] = B64[raw[i] & 0x3F];
    out[SALT_LEN] = '\0';
    return true;
}

bool lp_crypt6_new(const char *pw, char *out, size_t outn)
{
    char salt[SALT_LEN + 1];
    if (!make_salt(salt))
        return false;
    lp_crypt6(pw, salt, ROUNDS_DEF, out, outn);
    return true;
}

/* The account's password field from /etc/shadow (or `shadow` if given,
 * for tests), and whether this password matches it. A locked ("!" or
 * "*") or empty field never matches: an administrator check that let an
 * empty password through would be no check at all. */
int lp_shadow_check(const char *shadow, const char *user, const char *pw)
{
    static char buf[65536];
    long fd = lp_open(shadow ? shadow : "/etc/shadow", O_RDONLY, 0);
    if (fd < 0)
        return LP_SHADOW_NOFILE;
    long n = 0, r;
    while (n < (long)sizeof buf - 1 &&
           (r = lp_read((int)fd, buf + n, sizeof buf - 1 - (size_t)n)) > 0)
        n += r;
    lp_close((int)fd);
    buf[n] = '\0';

    size_t ul = strlen(user);
    for (char *line = buf; line && *line; ) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        if (strncmp(line, user, ul) == 0 && line[ul] == ':') {
            char *hash = line + ul + 1;
            char *end = strchr(hash, ':');
            if (end)
                *end = '\0';
            int res;
            if (!*hash)
                res = LP_SHADOW_EMPTY;
            else if (*hash == '!' || *hash == '*')
                res = LP_SHADOW_LOCKED;
            else if (strncmp(hash, "$6$", 3) != 0)
                res = LP_SHADOW_UNSUPPORTED;
            else
                res = lp_crypt6_verify(pw, hash) ? LP_SHADOW_OK : LP_SHADOW_WRONG;
            memset(buf, 0, (size_t)n);
            return res;
        }
        line = next;
    }
    memset(buf, 0, (size_t)n);
    return LP_SHADOW_NOUSER;
}
