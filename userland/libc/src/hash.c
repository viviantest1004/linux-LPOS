/* hash - MD5, SHA-1, SHA-256 and SHA-512 behind one interface.
 *
 * pkg checks every download against a hash, `update` verifies a
 * signature over one, and md5sum/sha1sum/sha256sum/sha512sum are four
 * names for the same program. They all want the same three calls, so
 * they share them; the only thing that differs is which compression
 * function runs and how many bytes come out.
 *
 * MD5 and SHA-1 are here because files on the internet are still
 * published with them, not because anything should be trusted to them.
 * Nothing in this system verifies a signature with either.
 */
#include "types.h"
#include "string.h"
#include "unistd.h"

static u32 ror32(u32 v, int n) { return (v >> n) | (v << (32 - n)); }
static u32 rol32(u32 v, int n) { return (v << n) | (v >> (32 - n)); }
static u64 ror64(u64 v, int n) { return (v >> n) | (v << (64 - n)); }

/* ── SHA-256 ───────────────────────────────────────────────────────── */
static const u32 K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_block(u32 *h, const u8 *p)
{
    u32 w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((u32)p[i*4] << 24) | ((u32)p[i*4+1] << 16) |
               ((u32)p[i*4+2] << 8) | (u32)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ror32(w[i-15], 7) ^ ror32(w[i-15], 18) ^ (w[i-15] >> 3);
        u32 s1 = ror32(w[i-2], 17) ^ ror32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3];
    u32 e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        u32 S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = hh + S1 + ch + K256[i] + w[i];
        u32 S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        u32 mj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

/* ── SHA-1 ─────────────────────────────────────────────────────────── */
static void sha1_block(u32 *h, const u8 *p)
{
    u32 w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((u32)p[i*4] << 24) | ((u32)p[i*4+1] << 16) |
               ((u32)p[i*4+2] << 8) | (u32)p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if      (i < 20) { f = (b & c) | (~b & d);          k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else             { f = b ^ c ^ d;                   k = 0xca62c1d6; }
        u32 t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
}

/* ── MD5 ───────────────────────────────────────────────────────────── */
static const u32 KMD5[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
    0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
    0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
    0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
    0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
    0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
    0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
    0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
    0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const u8 RMD5[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

static void md5_block(u32 *h, const u8 *p)
{
    u32 m[16];
    for (int i = 0; i < 16; i++)                      /* little endian */
        m[i] = (u32)p[i*4] | ((u32)p[i*4+1] << 8) |
               ((u32)p[i*4+2] << 16) | ((u32)p[i*4+3] << 24);

    u32 a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        u32 f; int g;
        if      (i < 16) { f = (b & c) | (~b & d);       g = i; }
        else if (i < 32) { f = (d & b) | (~d & c);       g = (5*i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;                g = (3*i + 5) & 15; }
        else             { f = c ^ (b | ~d);             g = (7*i) & 15; }
        u32 t = d;
        d = c; c = b;
        b = b + rol32(a + f + KMD5[i] + m[g], RMD5[i]);
        a = t;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
}

/* ── SHA-512 ───────────────────────────────────────────────────────── */
static const u64 K512[80] = {
0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};

static void sha512_block(u64 *h, const u8 *p)
{
    u64 w[80];
    for (int i = 0; i < 16; i++) {
        u64 v = 0;
        for (int b = 0; b < 8; b++) v = (v << 8) | p[i*8 + b];
        w[i] = v;
    }
    for (int i = 16; i < 80; i++) {
        u64 s0 = ror64(w[i-15],1) ^ ror64(w[i-15],8) ^ (w[i-15] >> 7);
        u64 s1 = ror64(w[i-2],19) ^ ror64(w[i-2],61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    u64 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 80; i++) {
        u64 S1 = ror64(e,14) ^ ror64(e,18) ^ ror64(e,41);
        u64 ch = (e & f) ^ (~e & g);
        u64 t1 = hh + S1 + ch + K512[i] + w[i];
        u64 S0 = ror64(a,28) ^ ror64(a,34) ^ ror64(a,39);
        u64 mj = (a & b) ^ (a & c) ^ (b & c);
        u64 t2 = S0 + mj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

/* ── The shared front end ──────────────────────────────────────────── */
static void run_block(lp_digest_t *d, const u8 *p)
{
    switch (d->algo) {
    case LP_MD5:    md5_block(d->h32, p);    break;
    case LP_SHA1:   sha1_block(d->h32, p);   break;
    case LP_SHA256: sha256_block(d->h32, p); break;
    default:        sha512_block(d->h64, p); break;
    }
}

int lp_digest_bits(int algo)
{
    switch (algo) {
    case LP_MD5:    return 128;
    case LP_SHA1:   return 160;
    case LP_SHA256: return 256;
    default:        return 512;
    }
}

void lp_digest_init(lp_digest_t *d, int algo)
{
    memset(d, 0, sizeof *d);
    d->algo  = algo;
    d->block = (algo == LP_SHA512) ? 128 : 64;

    switch (algo) {
    case LP_MD5:
        d->h32[0]=0x67452301; d->h32[1]=0xefcdab89;
        d->h32[2]=0x98badcfe; d->h32[3]=0x10325476;
        break;
    case LP_SHA1:
        d->h32[0]=0x67452301; d->h32[1]=0xefcdab89; d->h32[2]=0x98badcfe;
        d->h32[3]=0x10325476; d->h32[4]=0xc3d2e1f0;
        break;
    case LP_SHA256:
        d->h32[0]=0x6a09e667; d->h32[1]=0xbb67ae85; d->h32[2]=0x3c6ef372;
        d->h32[3]=0xa54ff53a; d->h32[4]=0x510e527f; d->h32[5]=0x9b05688c;
        d->h32[6]=0x1f83d9ab; d->h32[7]=0x5be0cd19;
        break;
    default:
        d->h64[0]=0x6a09e667f3bcc908ULL; d->h64[1]=0xbb67ae8584caa73bULL;
        d->h64[2]=0x3c6ef372fe94f82bULL; d->h64[3]=0xa54ff53a5f1d36f1ULL;
        d->h64[4]=0x510e527fade682d1ULL; d->h64[5]=0x9b05688c2b3e6c1fULL;
        d->h64[6]=0x1f83d9abfb41bd6bULL; d->h64[7]=0x5be0cd19137e2179ULL;
        break;
    }
}

void lp_digest_update(lp_digest_t *d, const void *data, size_t n)
{
    const u8 *p = data;
    d->len += n;
    while (n) {
        size_t take = (size_t)d->block - (size_t)d->used;
        if (take > n) take = n;
        memcpy(d->buf + d->used, p, take);
        d->used += (int)take;
        p += take;
        n -= take;
        if (d->used == d->block) {
            run_block(d, d->buf);
            d->used = 0;
        }
    }
}

void lp_digest_final(lp_digest_t *d, char *hex)
{
    u64 bits = d->len * 8;
    int lenfield = (d->algo == LP_SHA512) ? 16 : 8;
    bool little = (d->algo == LP_MD5);

    u8 pad = 0x80;
    lp_digest_update(d, &pad, 1);
    u8 zero = 0;
    while (d->used != d->block - lenfield)
        lp_digest_update(d, &zero, 1);

    /* Straight into the block: update() would count these bytes too. */
    u8 *tail = d->buf + d->block - lenfield;
    memset(tail, 0, (size_t)lenfield);
    if (little)
        for (int i = 0; i < 8; i++) tail[i] = (u8)(bits >> (i * 8));
    else
        for (int i = 0; i < 8; i++) tail[lenfield - 1 - i] = (u8)(bits >> (i * 8));
    run_block(d, d->buf);
    d->used = 0;

    static const char digits[] = "0123456789abcdef";
    int words = lp_digest_bits(d->algo) / (d->algo == LP_SHA512 ? 64 : 32);
    for (int i = 0; i < words; i++) {
        int wide = (d->algo == LP_SHA512) ? 8 : 4;
        for (int b = 0; b < wide; b++) {
            u8 byte;
            if (little)
                byte = (u8)(d->h32[i] >> (b * 8));
            else if (d->algo == LP_SHA512)
                byte = (u8)(d->h64[i] >> (56 - b * 8));
            else
                byte = (u8)(d->h32[i] >> (24 - b * 8));
            *hex++ = digits[byte >> 4];
            *hex++ = digits[byte & 15];
        }
    }
    *hex = '\0';
}

bool lp_digest_fd(int fd, int algo, char *hex)
{
    lp_digest_t d;
    lp_digest_init(&d, algo);

    static u8 buf[16384];
    for (;;) {
        long n = lp_read(fd, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0)  return false;
        lp_digest_update(&d, buf, (size_t)n);
    }
    lp_digest_final(&d, hex);
    return true;
}

bool lp_digest_file(const char *path, int algo, char *hex)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return false;
    bool ok = lp_digest_fd((int)fd, algo, hex);
    lp_close((int)fd);
    return ok;
}

/* The hash of a file, as 64 hex characters. false if it cannot be read. */
bool lp_sha256_file(const char *path, char *hex)
{
    return lp_digest_file(path, LP_SHA256, hex);
}
