/* gzip - compress a file, and open the .gz half of the internet.
 *
 *   gzip [-cdfklnNqrtv] [-1..-9] [-S SUF] [FILE]...
 *   gunzip FILE...   is gzip -d
 *   zcat FILE...     is gzip -dc
 *
 * ── Why the two halves are not the same size ──
 * Decompression is the half that matters. Every tarball, every kernel
 * source tree, every package on the network arrives as a DEFLATE stream
 * somebody else produced, and a machine that cannot expand one cannot
 * be given anything. So inflate here is complete: stored, fixed-Huffman
 * and dynamic-Huffman blocks, the whole length and distance alphabet,
 * members concatenated end to end, and the CRC-32 and length check that
 * closes each one. If a stream GNU gunzip accepts fails here, that is a
 * bug and not a difference of opinion.
 *
 * Compression is the half where being merely good is enough. This one
 * builds fixed-Huffman blocks over a hash-chain LZ77 match finder and
 * never builds a Huffman tree of its own, so it gives up a few percent
 * against GNU on text. That was deliberate: a dynamic tree needs a
 * code-length optimiser and a second pass over every block, and all of
 * it is code that can be subtly wrong in a way nobody notices until a
 * file will not come back. What this writes is ordinary DEFLATE that
 * any gunzip on earth accepts, and it round-trips byte for byte, which
 * is the only promise a compressor can actually keep. Matching GNU's
 * output bytes was never on the table anyway - the format says what is
 * legal to emit, not what to emit.
 *
 * The one place a fixed tree would have been embarrassing is data that
 * does not compress: fixed Huffman spends 8.5 bits on an average random
 * byte and would grow a JPEG by 6%. So every block is costed before it
 * is written and goes out as a stored block when raw is cheaper, which
 * is what keeps gzip on a photo down to about +0.04%.
 *
 * ── Memory ──
 * This runs on a board with 512MB, so nothing here reads a file. Both
 * directions stream through fixed buffers with one 32KB sliding window,
 * and the largest thing in the program is the compressor's hash table.
 * A 4GB tarball costs exactly what a 4KB one costs.
 *
 * ── Not here ──
 * --version and --license are house rules. --rsyncable and
 * --synchronous are left out because both are promises about output
 * bytes and about durability that this implementation does not make,
 * and a flag that is accepted and ignored is worse than one that is
 * missing.
 *
 * Diagnostics say "gzip:" whichever of the three names was typed. That
 * is what a learner sees on Ubuntu, where gunzip and zcat are two lines
 * of shell that exec gzip, so gzip is the name in every message.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"

/* utimensat, which unistd.h has no wrapper for. Without it a file that
 * goes through gzip and back comes out dated today, and make rebuilds a
 * tree somebody has just unpacked. The numbers come from the same
 * kernel tables syscall-*.h carries, where x86-64 and asm-generic
 * disagree as usual. */
#if defined(__x86_64__)
#  define LP_SYS_UTIMENSAT 280
#else
#  define LP_SYS_UTIMENSAT 88
#endif
#define LP_UTIME_OMIT ((1L << 30) - 2)

#define WSIZE   32768u              /* the sliding window, both directions */
#define IOBUF   32768u

/* GNU's three exit codes. A warning never overwrites an error. */
#define RC_OK   0
#define RC_ERR  1
#define RC_WARN 2

#define ENOENT_ 2

/* ── options ────────────────────────────────────────────────────── */
static bool opt_decompress, opt_stdout, opt_keep, opt_force, opt_verbose;
static bool opt_test, opt_list, opt_quiet, opt_recursive;
static int  opt_level = 6;
/* -1 is "not said either way". GNU settles both to `decompress`, which
 * is why gunzip ignores the name and stamp stored in the file unless
 * you ask for them with -N. */
static int  no_name = -1, no_time = -1;
static const char *z_suffix = ".gz";
static int  exit_code = RC_OK;

static void warned(void) { if (exit_code == RC_OK) exit_code = RC_WARN; }

/* ── the file being worked on ───────────────────────────────────── */
#define PATH_LEN 1024
static char ifname[PATH_LEN];
static char ofname[PATH_LEN];

static lp_stat_t istat;
static bool      istat_ok;
static bool      istat_reg;

static u64  bytes_in;               /* consumed from the input */
static u64  bytes_out;              /* written to the output */
static u64  header_bytes;           /* gzip header + trailer, for the ratio */

/* ── buffered input ─────────────────────────────────────────────── */
static int  in_fd = -1;
static u8   inbuf[IOBUF];
static u32  in_len, in_pos;
static bool in_eof;
static u64  in_total;               /* bytes handed out of inbuf */

/* Two bytes of pushback. Sniffing the magic number to decide whether
 * `gzip -dfc` should decompress or copy has to happen before anything
 * else reads, and on a pipe those bytes cannot be seeked back. */
static u8   pb[2];
static int  pb_n, pb_i;

static void in_reset(int fd)
{
    in_fd = fd; in_len = in_pos = 0; in_eof = false; in_total = 0;
    pb_n = pb_i = 0;
}

static int in_byte(void)
{
    if (pb_i < pb_n) { in_total++; return pb[pb_i++]; }
    if (in_pos == in_len) {
        if (in_eof) return -1;
        long n = lp_read(in_fd, inbuf, sizeof inbuf);
        if (n <= 0) { in_eof = true; return -1; }
        in_len = (u32)n; in_pos = 0;
    }
    in_total++;
    return inbuf[in_pos++];
}

static void in_push(const u8 *p, int n)
{
    for (int i = 0; i < n; i++) pb[i] = p[i];
    pb_n = n; pb_i = 0;
    in_total -= (u64)n;
}

/* ── buffered output ────────────────────────────────────────────── */
/* out_fd below zero is the bit bucket. -t and the sizing pass of -l on
 * a pipe both run the whole decompressor and want none of its bytes. */
static int  out_fd = -1;
static u8   outbuf[IOBUF];
static u32  out_len;
static bool out_broken;

static void out_reset(int fd) { out_fd = fd; out_len = 0; out_broken = false; }

static void out_flush(void)
{
    if (out_fd < 0 || out_len == 0 || out_broken) { out_len = 0; return; }
    u32 done = 0;
    while (done < out_len) {
        long n = lp_write(out_fd, outbuf + done, out_len - done);
        if (n <= 0) {
            lp_diag("gzip", NULL, NULL, "write failed", ofname,
                    n < 0 ? (int)-n : 28);
            out_broken = true;
            exit_code = RC_ERR;
            break;
        }
        done += (u32)n;
    }
    out_len = 0;
}

static void out_bytes(const u8 *p, u32 n)
{
    bytes_out += n;
    while (n) {
        u32 room = IOBUF - out_len;
        u32 take = n < room ? n : room;
        memcpy(outbuf + out_len, p, take);
        out_len += take; p += take; n -= take;
        if (out_len == IOBUF) out_flush();
    }
}

static void out_byte(u8 c)
{
    bytes_out++;
    outbuf[out_len++] = c;
    if (out_len == IOBUF) out_flush();
}

/* ── CRC-32, the thing gzip checks at the end of every member ───── */
static u32 crc_tab[256];

static void crc_init(void)
{
    for (u32 n = 0; n < 256; n++) {
        u32 c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[n] = c;
    }
}

static u32 crc_add(u32 crc, const u8 *p, u32 n)
{
    while (n--) crc = crc_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

/* ── the ratio that gzip -v and gzip -l print ───────────────────────
 *
 * GNU spells this "%5.1f%%" and there is no floating point on this
 * machine, so it is done in integers. Rounding is half-to-even, which
 * is what glibc does for the ties a binary double can hold exactly -
 * .25 and .75, the ones that turn up. A tie at .x5 that is not exactly
 * representable is settled by whichever side the double landed on;
 * reaching one needs a ratio of exactly k/2000 and it has never been
 * seen in practice.
 */
static void print_ratio(int fd, s64 num, s64 den)
{
    bool neg = false;
    char buf[32];

    if (den <= 0) { num = 0; den = 1; }
    if (num < 0) { neg = true; num = -num; }

    s64 q   = (num * 1000) / den;
    s64 rem = (num * 1000) - q * den;
    if (rem * 2 > den || (rem * 2 == den && (q & 1)))
        q++;

    snprintf(buf, sizeof buf, "%s%ld.%ld", neg ? "-" : "",
             (long)(q / 10), (long)(q % 10));
    dprintf(fd, "%5s%%", buf);
}

/* ════════════════════════════════════════════════════════════════════
 *  INFLATE
 * ════════════════════════════════════════════════════════════════════
 *
 * Canonical Huffman decoded the way puff does it: walk the code lengths
 * one bit at a time and let the per-length symbol counts say when a
 * code is complete. A lookup table would be several times faster and
 * this is twenty lines that can be read straight against RFC 1951 - for
 * the half of the program that has to be right on everybody else's
 * files, that was the trade worth making.
 */
#define INF_OK      0
#define INF_FORMAT -1               /* not valid DEFLATE */
#define INF_TRUNC  -2               /* it ended in the middle */
#define INF_CRC    -3
#define INF_LEN    -4

static u32  bitbuf;
static int  bitcnt;
static bool inf_trunc;

static u8   win[WSIZE];
static u32  wpos;
static u64  member_out;             /* bytes this member has produced */
static u32  member_crc;
static u64  member_isize;

static void inf_reset(void)
{
    bitbuf = 0; bitcnt = 0; inf_trunc = false;
    wpos = 0; member_out = 0;
    member_crc = 0xFFFFFFFFu; member_isize = 0;
}

/* Input bytes really used up: what came out of inbuf, less the whole
 * bytes still sitting unread in the bit buffer. */
static u64 in_consumed(void) { return in_total - (u64)(bitcnt >> 3); }

static u32 get_bits(int need)
{
    while (bitcnt < need) {
        int c = in_byte();
        if (c < 0) { inf_trunc = true; c = 0; }
        bitbuf |= (u32)c << bitcnt;
        bitcnt += 8;
    }
    u32 v = bitbuf & ((1u << need) - 1);
    bitbuf >>= need;
    bitcnt -= need;
    return v;
}

/* One whole byte, out of the bit buffer first. Everything byte-shaped -
 * the header, a stored block, the trailer - comes through here, so the
 * bit view and the byte view never disagree about where the input is. */
static int raw_byte(void)
{
    if (bitcnt >= 8) {
        int c = (int)(bitbuf & 0xFF);
        bitbuf >>= 8; bitcnt -= 8;
        return c;
    }
    bitbuf = 0; bitcnt = 0;
    return in_byte();
}

static void align_to_byte(void)
{
    bitbuf >>= (bitcnt & 7);
    bitcnt  -= bitcnt & 7;
}

/* The window doubles as the output buffer. Bytes stay in it after they
 * have been written out, and that is what lets a copy reach 32KB back. */
static void win_flush(void)
{
    if (wpos == 0) return;
    member_crc = crc_add(member_crc, win, wpos);
    member_isize += wpos;
    out_bytes(win, wpos);
    wpos = 0;
}

static void win_put(u8 b)
{
    win[wpos++] = b;
    member_out++;
    if (wpos == WSIZE) win_flush();
}

typedef struct {
    s16 count[16];
    s16 symbol[288];
} huff_t;

/* 0 for a complete code, above zero for an incomplete one, below zero
 * when the lengths claim more codes than a tree that deep can hold. */
static int huff_build(huff_t *h, const u8 *len, int n)
{
    s16 offs[16];
    int left;

    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int s = 0; s < n; s++) h->count[len[s]]++;
    if (h->count[0] == n) return 0;

    left = 1;
    for (int i = 1; i < 16; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (s16)(offs[i] + h->count[i]);
    for (int s = 0; s < n; s++)
        if (len[s]) h->symbol[offs[len[s]]++] = (s16)s;
    return left;
}

static int huff_decode(const huff_t *h)
{
    int code = 0, first = 0, index = 0;

    for (int len = 1; len < 16; len++) {
        code |= (int)get_bits(1);
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code  <<= 1;
    }
    return -1;
}

static const u16 len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
    67,83,99,115,131,163,195,227,258
};
static const u8 len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const u16 dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const u8 dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static huff_t lit_huff, dist_huff;

static int inflate_codes(void)
{
    for (;;) {
        int sym = huff_decode(&lit_huff);
        if (inf_trunc) return INF_TRUNC;
        if (sym < 0) return INF_FORMAT;
        if (sym < 256) { win_put((u8)sym); continue; }
        if (sym == 256) return INF_OK;

        sym -= 257;
        if (sym >= 29) return INF_FORMAT;
        u32 len = len_base[sym] + get_bits(len_extra[sym]);

        int dsym = huff_decode(&dist_huff);
        if (inf_trunc) return INF_TRUNC;
        if (dsym < 0 || dsym >= 30) return INF_FORMAT;
        u32 dist = dist_base[dsym] + get_bits(dist_extra[dsym]);

        /* A distance reaching behind the start of this member is the
         * usual shape of a corrupt stream, and following it would copy
         * whatever the window still held from the last one. */
        if ((u64)dist > member_out) return INF_FORMAT;

        u32 src = (wpos - dist) & (WSIZE - 1);
        while (len--) {
            u8 b = win[src];
            src = (src + 1) & (WSIZE - 1);
            win_put(b);
        }
    }
}

static int inflate_stored(void)
{
    align_to_byte();
    int a = raw_byte(), b = raw_byte(), c = raw_byte(), d = raw_byte();
    if (a < 0 || b < 0 || c < 0 || d < 0) return INF_TRUNC;

    u32 len  = (u32)a | ((u32)b << 8);
    u32 nlen = (u32)c | ((u32)d << 8);
    if ((len ^ 0xFFFFu) != nlen) return INF_FORMAT;

    while (len--) {
        int v = raw_byte();
        if (v < 0) return INF_TRUNC;
        win_put((u8)v);
    }
    return INF_OK;
}

static void fixed_tables(void)
{
    u8 lengths[288];
    for (int i = 0;   i < 144; i++) lengths[i] = 8;
    for (int i = 144; i < 256; i++) lengths[i] = 9;
    for (int i = 256; i < 280; i++) lengths[i] = 7;
    for (int i = 280; i < 288; i++) lengths[i] = 8;
    huff_build(&lit_huff, lengths, 288);
    for (int i = 0; i < 30; i++) lengths[i] = 5;
    huff_build(&dist_huff, lengths, 30);
}

static const u8 clen_order[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

static int dynamic_tables(void)
{
    static u8 lengths[288 + 32];
    huff_t clen;

    u32 nlen  = get_bits(5) + 257;
    u32 ndist = get_bits(5) + 1;
    u32 ncode = get_bits(4) + 4;
    if (nlen > 286 || ndist > 30) return INF_FORMAT;

    for (u32 i = 0; i < 19; i++) lengths[i] = 0;
    for (u32 i = 0; i < ncode; i++) lengths[clen_order[i]] = (u8)get_bits(3);
    if (huff_build(&clen, lengths, 19) != 0) return INF_FORMAT;

    u32 i = 0;
    while (i < nlen + ndist) {
        int sym = huff_decode(&clen);
        if (inf_trunc) return INF_TRUNC;
        if (sym < 0) return INF_FORMAT;
        if (sym < 16) { lengths[i++] = (u8)sym; continue; }

        u32 rep, val = 0;
        if (sym == 16) {
            if (i == 0) return INF_FORMAT;
            val = lengths[i - 1];
            rep = 3 + get_bits(2);
        } else if (sym == 17) {
            rep = 3 + get_bits(3);
        } else {
            rep = 11 + get_bits(7);
        }
        if (i + rep > nlen + ndist) return INF_FORMAT;
        while (rep--) lengths[i++] = (u8)val;
    }
    if (lengths[256] == 0) return INF_FORMAT;      /* no end-of-block code */

    /* An incomplete code is legal only when it holds a single symbol -
     * the distance table of a block that never copies anything. */
    int err = huff_build(&lit_huff, lengths, (int)nlen);
    if (err && (err < 0 ||
                nlen != (u32)(lit_huff.count[0] + lit_huff.count[1])))
        return INF_FORMAT;
    err = huff_build(&dist_huff, lengths + nlen, (int)ndist);
    if (err && (err < 0 ||
                ndist != (u32)(dist_huff.count[0] + dist_huff.count[1])))
        return INF_FORMAT;
    return INF_OK;
}

/* One DEFLATE stream, up to and including its final block. */
static int inflate_stream(void)
{
    int last;
    do {
        last = (int)get_bits(1);
        u32 type = get_bits(2);
        int rc;
        if (inf_trunc) return INF_TRUNC;

        if (type == 0)      rc = inflate_stored();
        else if (type == 1) { fixed_tables(); rc = inflate_codes(); }
        else if (type == 2) { rc = dynamic_tables();
                              if (rc == INF_OK) rc = inflate_codes(); }
        else                rc = INF_FORMAT;

        if (rc != INF_OK) return rc;
        if (inf_trunc) return INF_TRUNC;
    } while (!last);

    win_flush();
    return INF_OK;
}

/* ════════════════════════════════════════════════════════════════════
 *  DEFLATE
 * ════════════════════════════════════════════════════════════════════ */
#define MIN_MATCH     3u
#define MAX_MATCH     258u
#define MIN_LOOKAHEAD (MAX_MATCH + MIN_MATCH + 1)      /* 262 */
#define MAX_DIST      (WSIZE - MIN_LOOKAHEAD)          /* 32506 */
#define HASH_BITS     15
#define HASH_SIZE     (1u << HASH_BITS)
#define HASH_MASK     (HASH_SIZE - 1u)
#define H_SHIFT       5                     /* three bytes fill 15 bits */
#define LIT_BUFSIZE   16384u
/* A block is closed after this many input bytes whatever else happens,
 * so its bytes are certain to be still in the window if it turns out
 * that storing them raw is cheaper than coding them. */
#define BLOCK_MAX     30000u

static u8  dwin[2 * WSIZE];
static u16 dprev[WSIZE];
static u16 dhead[HASH_SIZE];

static u32 strstart, lookahead, block_start, ins_h;
static bool src_eof;
static u32 max_chain, nice_len;

static u8  sym_lit[LIT_BUFSIZE];    /* the byte, or the match length less 3 */
static u16 sym_dist[LIT_BUFSIZE];   /* 0 marks a literal */
static u32 sym_count;
static u64 block_bits;              /* what fixed Huffman would cost */

static u16 fixed_code[288];         /* bit-reversed, ready to send */
static u8  fixed_len[288];
static u16 fdist_code[30];
static u8  lcode_of[256];           /* match length - 3 -> length code */
static u8  dcode_of[512];

static u32 gz_crc;                  /* CRC of what is being compressed */
static u64 gz_isize;

static u32 bit_acc;
static int bit_n;

static void send_bits(u32 value, int len)
{
    bit_acc |= value << bit_n;
    bit_n += len;
    while (bit_n >= 8) {
        out_byte((u8)(bit_acc & 0xFF));
        bit_acc >>= 8;
        bit_n -= 8;
    }
}

static void send_align(void)
{
    if (bit_n) { out_byte((u8)(bit_acc & 0xFF)); bit_acc = 0; bit_n = 0; }
}

static u16 bit_reverse(u32 code, int len)
{
    u32 r = 0;
    while (len--) { r = (r << 1) | (code & 1); code >>= 1; }
    return (u16)r;
}

/* The fixed alphabet of RFC 1951 section 3.2.6, built from the code
 * lengths rather than tabulated, so the canonical-code rule is written
 * down once and the same rule is what the decoder above uses. */
static void deflate_tables(void)
{
    u8  lens[288];
    u16 bl_count[16], next[16];

    for (int i = 0;   i < 144; i++) lens[i] = 8;
    for (int i = 144; i < 256; i++) lens[i] = 9;
    for (int i = 256; i < 280; i++) lens[i] = 7;
    for (int i = 280; i < 288; i++) lens[i] = 8;

    for (int i = 0; i < 16; i++) bl_count[i] = 0;
    for (int i = 0; i < 288; i++) bl_count[lens[i]]++;
    u32 code = 0;
    next[0] = 0;
    for (int b = 1; b < 16; b++) {
        code = (code + bl_count[b - 1]) << 1;
        next[b] = (u16)code;
    }
    for (int i = 0; i < 288; i++) {
        fixed_len[i]  = lens[i];
        fixed_code[i] = bit_reverse(next[lens[i]]++, lens[i]);
    }
    for (int i = 0; i < 30; i++)
        fdist_code[i] = bit_reverse((u32)i, 5);

    int len = 0;
    for (int c = 0; c < 28; c++)
        for (int n = 0; n < (1 << len_extra[c]); n++)
            lcode_of[len++] = (u8)c;
    lcode_of[255] = 28;                 /* length 258 has a code to itself */

    int d = 0;
    for (int c = 0; c < 16; c++)
        for (int n = 0; n < (1 << dist_extra[c]); n++)
            dcode_of[d++] = (u8)c;
    /* Above 256 the table is indexed by the distance divided by 128, so
     * it restarts at 256/128 rather than at zero. Starting it at zero
     * shifts every long distance by two codes, which produces a stream
     * that inflates into something almost right - the worst kind of
     * wrong there is. */
    d = 2;
    for (int c = 16; c < 30; c++)
        for (int n = 0; n < (1 << (dist_extra[c] - 7)); n++)
            dcode_of[256 + d++] = (u8)c;
}

static u32 dist_code(u32 dist)
{
    u32 d0 = dist - 1;
    return d0 < 256 ? dcode_of[d0] : dcode_of[256 + (d0 >> 7)];
}

static void flush_block(bool last)
{
    u32 stored_len  = strstart - block_start;
    u64 coded_bits  = block_bits + fixed_len[256];
    u64 coded_bytes = (coded_bits + 3 + 7) / 8;

    if ((u64)stored_len + 4 <= coded_bytes) {
        send_bits(last ? 1u : 0u, 1);
        send_bits(0, 2);
        send_align();
        u8 hdr[4];
        hdr[0] = (u8)(stored_len & 0xFF);
        hdr[1] = (u8)((stored_len >> 8) & 0xFF);
        hdr[2] = (u8)(~stored_len & 0xFF);
        hdr[3] = (u8)((~stored_len >> 8) & 0xFF);
        out_bytes(hdr, 4);
        if (stored_len) out_bytes(dwin + block_start, stored_len);
    } else {
        send_bits(last ? 1u : 0u, 1);
        send_bits(1, 2);
        for (u32 i = 0; i < sym_count; i++) {
            u32 dist = sym_dist[i];
            if (dist == 0) {
                u32 c = sym_lit[i];
                send_bits(fixed_code[c], fixed_len[c]);
            } else {
                u32 lc  = lcode_of[sym_lit[i]];
                u32 sym = 257 + lc;
                send_bits(fixed_code[sym], fixed_len[sym]);
                if (len_extra[lc])
                    send_bits((u32)sym_lit[i] + MIN_MATCH - len_base[lc],
                              len_extra[lc]);
                u32 dc = dist_code(dist);
                send_bits(fdist_code[dc], 5);
                if (dist_extra[dc])
                    send_bits(dist - dist_base[dc], dist_extra[dc]);
            }
        }
        send_bits(fixed_code[256], fixed_len[256]);
    }
    if (last) send_align();

    block_start = strstart;
    sym_count   = 0;
    block_bits  = 0;
}

static bool tally(u32 dist, u32 lit_or_len)
{
    sym_dist[sym_count] = (u16)dist;
    sym_lit[sym_count]  = (u8)lit_or_len;
    sym_count++;

    if (dist == 0) {
        block_bits += fixed_len[lit_or_len];
    } else {
        u32 lc = lcode_of[lit_or_len];
        u32 dc = dist_code(dist);
        block_bits += fixed_len[257 + lc] + len_extra[lc] + 5 + dist_extra[dc];
    }
    return sym_count == LIT_BUFSIZE - 1 ||
           strstart - block_start >= BLOCK_MAX;
}

static void fill_window(void)
{
    u32 more = 2 * WSIZE - lookahead - strstart;

    if (strstart >= WSIZE + MAX_DIST) {
        memcpy(dwin, dwin + WSIZE, WSIZE);
        strstart    -= WSIZE;
        block_start -= WSIZE;
        for (u32 i = 0; i < HASH_SIZE; i++)
            dhead[i] = dhead[i] >= WSIZE ? (u16)(dhead[i] - WSIZE) : 0;
        for (u32 i = 0; i < WSIZE; i++)
            dprev[i] = dprev[i] >= WSIZE ? (u16)(dprev[i] - WSIZE) : 0;
        more += WSIZE;
    }
    while (!src_eof && more > 0) {
        long n = lp_read(in_fd, dwin + strstart + lookahead, more);
        if (n <= 0) { src_eof = true; break; }
        /* The input passes through here and nowhere else, so this is
         * where the CRC and the length that go in the trailer come
         * from. */
        gz_crc = crc_add(gz_crc, dwin + strstart + lookahead, (u32)n);
        gz_isize += (u64)n;
        bytes_in += (u64)n;
        lookahead += (u32)n;
        more      -= (u32)n;
    }
}

static u32 insert_string(u32 pos)
{
    ins_h = ((ins_h << H_SHIFT) ^ dwin[pos + MIN_MATCH - 1]) & HASH_MASK;
    u32 head = dhead[ins_h];
    dprev[pos & (WSIZE - 1)] = (u16)head;
    dhead[ins_h] = (u16)pos;
    return head;
}

/* Walk the chain of earlier positions with the same three-byte hash and
 * keep the longest match. The chain is most-recent-first, so the first
 * match of a given length is also the nearest one, and a near match
 * spends fewer bits on its distance. */
static u32 longest_match(u32 cur, u32 *out_start)
{
    u32 chain = max_chain;
    u32 limit = strstart > MAX_DIST ? strstart - MAX_DIST : 0;
    u32 best = MIN_MATCH - 1, best_start = 0;
    u32 maxlen = lookahead < MAX_MATCH ? lookahead : MAX_MATCH;
    const u8 *scan = dwin + strstart;

    if (maxlen < MIN_MATCH) return 0;

    do {
        const u8 *m = dwin + cur;
        /* Check the byte that would have to beat the best match first:
         * it fails for nearly every candidate and costs one compare. */
        if (m[best] == scan[best] && m[0] == scan[0] && m[1] == scan[1]) {
            u32 len = 0;
            while (len < maxlen && m[len] == scan[len]) len++;
            if (len > best) {
                best = len;
                best_start = cur;
                if (len >= nice_len || len >= maxlen) break;
            }
        }
        cur = dprev[cur & (WSIZE - 1)];
    } while (cur > limit && --chain != 0);

    if (best < MIN_MATCH) return 0;
    *out_start = best_start;
    return best;
}

static void deflate_stream(void)
{
    /* zlib's levels, minus the columns only lazy matching uses. */
    static const u16 cfg_nice[10]  = { 0, 8, 16, 32, 16, 32, 128, 128, 258, 258 };
    static const u16 cfg_chain[10] = { 0, 4,  8, 32, 16, 32, 128, 256,1024,4096 };

    nice_len  = cfg_nice[opt_level];
    max_chain = cfg_chain[opt_level];

    strstart = lookahead = block_start = ins_h = 0;
    src_eof = false; sym_count = 0; block_bits = 0;
    bit_acc = 0; bit_n = 0;
    for (u32 i = 0; i < HASH_SIZE; i++) dhead[i] = 0;

    fill_window();
    for (u32 i = 0; i + 1 < MIN_MATCH && i < lookahead; i++)
        ins_h = ((ins_h << H_SHIFT) ^ dwin[i]) & HASH_MASK;

    while (lookahead != 0) {
        u32 head = 0;
        if (lookahead >= MIN_MATCH) head = insert_string(strstart);

        u32 mstart = 0, mlen = 0;
        if (head != 0 && strstart - head <= MAX_DIST)
            mlen = longest_match(head, &mstart);

        bool full;
        if (mlen >= MIN_MATCH) {
            full = tally(strstart - mstart, mlen - MIN_MATCH);
            lookahead -= mlen;
            if (lookahead >= MIN_MATCH) {
                /* Every position inside the match still has to go into
                 * the chains, or the next match cannot begin there. */
                u32 n = mlen - 1;
                do { strstart++; insert_string(strstart); } while (--n);
                strstart++;
            } else {
                strstart += mlen;
                ins_h = 0;
                for (u32 i = 0; i + 1 < MIN_MATCH && i < lookahead; i++)
                    ins_h = ((ins_h << H_SHIFT) ^ dwin[strstart + i]) & HASH_MASK;
            }
        } else {
            full = tally(0, dwin[strstart]);
            lookahead--;
            strstart++;
        }

        if (full) flush_block(false);
        if (lookahead < MIN_LOOKAHEAD && !src_eof) fill_window();
    }
    flush_block(true);
}

/* ════════════════════════════════════════════════════════════════════
 *  the gzip member
 * ════════════════════════════════════════════════════════════════════ */
#define GZ_MAGIC1  0x1F
#define GZ_MAGIC2  0x8B
#define GZ_OLD2    0x9E             /* gzip 0.5 wrote this second byte */
#define F_HCRC     0x02
#define F_EXTRA    0x04
#define F_NAME     0x08
#define F_COMMENT  0x10
#define F_RESERVED 0xE0

static s64  hdr_mtime;              /* MTIME out of the header just read */
static char hdr_name[PATH_LEN];     /* ORIG_NAME, when there was one */

static void put_u32(u32 v)
{
    out_byte((u8)(v & 0xFF));
    out_byte((u8)((v >> 8) & 0xFF));
    out_byte((u8)((v >> 16) & 0xFF));
    out_byte((u8)((v >> 24) & 0xFF));
}

/* What follows the last slash: the part that goes in ORIG_NAME. */
static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void write_gzip_header(s64 mtime)
{
    u32 t = (mtime < 0 || mtime > 0xFFFFFFFFL) ? 0 : (u32)mtime;

    out_byte(GZ_MAGIC1);
    out_byte(GZ_MAGIC2);
    out_byte(8);                            /* deflate */
    out_byte(no_name ? 0 : F_NAME);
    put_u32(no_time ? 0 : t);
    out_byte(opt_level == 9 ? 2 : opt_level == 1 ? 4 : 0);
    out_byte(3);                            /* Unix */
    if (!no_name) {
        const char *n = base_name(ifname);
        while (*n) out_byte((u8)*n++);
        out_byte(0);
    }
}

/* 0 on a header, -1 on an error already reported, -2 on trailing
 * garbage, -3 at a clean end of file. `part` is 1 for the first member
 * of a file: GNU says different things about junk depending on whether
 * anything has been decompressed yet. */
static int read_gzip_header(int part)
{
    int m1 = raw_byte();
    int m2 = (m1 < 0) ? -1 : raw_byte();

    if (m1 < 0) return -3;
    if (m1 != GZ_MAGIC1 || (m2 != GZ_MAGIC2 && m2 != GZ_OLD2)) {
        if (part == 1) {
            dprintf(STDERR_FILENO, "\ngzip: %s: not in gzip format\n", ifname);
            exit_code = RC_ERR;
            return -1;
        }
        /* A tar padded with NULs and then gzipped, concatenated with
         * another, leaves zero bytes here. Those are not garbage. */
        if (m1 == 0) {
            int c = m2;
            while (c == 0) c = raw_byte();
            if (c < 0) {
                if (opt_verbose) {
                    dprintf(STDERR_FILENO, "\ngzip: %s: decompression OK, "
                            "trailing zero bytes ignored\n", ifname);
                    warned();
                }
                return -3;
            }
        }
        if (!opt_quiet)
            dprintf(STDERR_FILENO, "gzip: %s: decompression OK, trailing "
                    "garbage ignored\n", ifname);
        warned();
        return -2;
    }

    int method = raw_byte();
    int flags  = raw_byte();
    if (method < 0 || flags < 0) {
        dprintf(STDERR_FILENO, "\ngzip: %s: unexpected end of file\n", ifname);
        exit_code = RC_ERR;
        return -1;
    }
    if (method != 8) {
        dprintf(STDERR_FILENO, "gzip: %s: unknown method %d -- not supported\n",
                ifname, method);
        exit_code = RC_ERR;
        return -1;
    }
    if (flags & F_RESERVED) {
        dprintf(STDERR_FILENO, "gzip: %s: unknown flags 0x%x -- not supported\n",
                ifname, (unsigned)flags);
        exit_code = RC_ERR;
        return -1;
    }

    u32 stamp = 0;
    for (int i = 0; i < 4; i++) {
        int c = raw_byte();
        if (c < 0) c = 0;
        stamp |= (u32)c << (8 * i);
    }
    hdr_mtime = stamp ? (s64)stamp : -1;

    raw_byte();                             /* XFL */
    raw_byte();                             /* OS */

    if (flags & F_EXTRA) {
        int a = raw_byte(), b = raw_byte();
        if (a < 0 || b < 0) return -1;
        u32 n = (u32)a | ((u32)b << 8);
        while (n--) if (raw_byte() < 0) return -1;
    }
    hdr_name[0] = '\0';
    if (flags & F_NAME) {
        size_t i = 0;
        for (;;) {
            int c = raw_byte();
            if (c <= 0) break;
            if (i + 1 < sizeof hdr_name) hdr_name[i++] = (char)c;
        }
        hdr_name[i] = '\0';
    }
    if (flags & F_COMMENT) {
        int c;
        do { c = raw_byte(); } while (c > 0);
    }
    if (flags & F_HCRC) { raw_byte(); raw_byte(); }

    /* The eight trailer bytes are counted in with the header, because
     * that is how GNU's ratio is defined and the number is only ever
     * used for that. */
    if (part == 1) header_bytes = in_consumed() + 8;
    return 0;
}

/* Everything after the deflate stream: the CRC of the plain text and
 * its length modulo 2^32. Both are checked - a stream that inflates
 * cleanly but fails here has had a byte changed inside it. */
static int read_trailer(void)
{
    u32 crc = 0, isize = 0;

    for (int i = 0; i < 4; i++) {
        int c = raw_byte();
        if (c < 0) return INF_TRUNC;
        crc |= (u32)c << (8 * i);
    }
    for (int i = 0; i < 4; i++) {
        int c = raw_byte();
        if (c < 0) return INF_TRUNC;
        isize |= (u32)c << (8 * i);
    }
    if (crc != (member_crc ^ 0xFFFFFFFFu)) return INF_CRC;
    if (isize != (u32)(member_isize & 0xFFFFFFFFu)) return INF_LEN;
    return INF_OK;
}

/* Every member of one gzip file. 0 on success, 1 with a message already
 * printed on failure. */
static int gunzip_file(void)
{
    int part = 0;

    for (;;) {
        inf_reset();
        part++;
        int h = read_gzip_header(part);
        if (h == -3 || h == -2) break;
        if (h < 0) return 1;

        int rc = inflate_stream();
        if (rc == INF_OK) rc = read_trailer();
        if (rc == INF_OK) continue;

        win_flush();
        const char *why =
            rc == INF_TRUNC ? "unexpected end of file" :
            rc == INF_CRC   ? "invalid compressed data--crc error" :
            rc == INF_LEN   ? "invalid compressed data--length error" :
                              "invalid compressed data--format violated";
        dprintf(STDERR_FILENO, "\ngzip: %s: %s\n", ifname, why);
        exit_code = RC_ERR;
        return 1;
    }
    bytes_in = in_consumed();
    return 0;
}

/* Copy the input through untouched. `gzip -dfc` on something that is
 * not compressed does this, and it is the reason `zcat -f` can be
 * pointed at a directory holding both kinds of file. */
static void copy_through(void)
{
    while (pb_i < pb_n) out_byte((u8)in_byte());
    for (;;) {
        if (in_pos == in_len) {
            if (in_eof) break;
            long n = lp_read(in_fd, inbuf, sizeof inbuf);
            if (n <= 0) { in_eof = true; break; }
            in_len = (u32)n; in_pos = 0;
        }
        u32 have = in_len - in_pos;
        out_bytes(inbuf + in_pos, have);
        in_total += have;
        in_pos = in_len;
    }
    bytes_in = in_total;
}

/* ════════════════════════════════════════════════════════════════════
 *  names
 * ════════════════════════════════════════════════════════════════════ */
static bool tail_is(const char *name, const char *suf)
{
    size_t n = strlen(name), s = strlen(suf);
    if (s == 0 || n <= s) return false;
    const char *p = name + n - s;
    for (size_t i = 0; i < s; i++) {
        char a = p[i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return false;
    }
    return true;
}

/* The suffix that marks `name` as compressed, or NULL. The list is
 * GNU's: .z and _z are from before .gz existed and .tgz is the one
 * people still type. */
static char *get_suffix(char *name)
{
    static const char *known[] = { ".gz", ".z", ".taz", ".tgz", "-gz", "-z",
                                   "_z", NULL };
    if (tail_is(name, z_suffix))
        return name + strlen(name) - strlen(z_suffix);
    for (int i = 0; known[i]; i++)
        if (tail_is(name, known[i]))
            return name + strlen(name) - strlen(known[i]);
    return NULL;
}

/* ofname from ifname. false when this file should be skipped. */
static bool make_ofname(void)
{
    strlcpy(ofname, ifname, sizeof ofname);
    char *suff = get_suffix(ofname);

    if (opt_decompress) {
        if (suff == NULL) {
            /* -t and -l will try anything; -d has to know where to put
             * the answer, and guessing would overwrite the input. */
            if (opt_list || opt_test) return true;
            if (opt_verbose || (!opt_recursive && !opt_quiet)) {
                dprintf(STDERR_FILENO, "gzip: %s: unknown suffix -- ignored\n",
                        ifname);
                warned();
            }
            return false;
        }
        if (tail_is(ofname, ".tgz") || tail_is(ofname, ".taz"))
            strcpy(suff, ".tar");
        else
            *suff = '\0';
        return true;
    }

    if (suff != NULL) {
        /* GNU does not treat this as a warning for the exit status
         * either: doing nothing to a file that is already compressed is
         * not a failure of the run. */
        if (opt_verbose || (!opt_recursive && !opt_quiet))
            dprintf(STDERR_FILENO,
                    "gzip: %s already has %s suffix -- unchanged\n",
                    ifname, suff);
        return false;
    }
    strlcat(ofname, z_suffix, sizeof ofname);
    return true;
}

/* ════════════════════════════════════════════════════════════════════
 *  metadata
 * ════════════════════════════════════════════════════════════════════ */
static void copy_stat(s64 mtime)
{
    if (!istat_ok) return;
    /* atime is left alone (UTIME_OMIT): the one field a backup program
     * notices being touched. */
    s64 ts[4] = { 0, LP_UTIME_OMIT, mtime, 0 };
    sys_call4(LP_SYS_UTIMENSAT, AT_FDCWD, (long)ofname, (long)ts, 0);
    lp_chmod(ofname, istat.mode & 07777);
    lp_chown(ofname, istat.uid, istat.gid);
}

/* ── the y/n question ──
 *
 * Only asked when somebody is there to answer it. In a script the
 * answer is no and the file is left alone, which is why a Makefile that
 * runs gzip twice does not quietly lose the second copy. */
static bool yesno(void)
{
    char c, first = 0;
    while (lp_read(STDIN_FILENO, &c, 1) == 1 && c != '\n')
        if (!first) first = c;
    return first == 'y' || first == 'Y';
}

static bool check_ofname(void)
{
    lp_stat_t st;
    if (lp_stat(ofname, &st, false) != 0) return true;
    if (!opt_force) {
        dprintf(STDERR_FILENO, "gzip: %s already exists;", ofname);
        bool ok = false;
        if (lp_isatty(STDIN_FILENO)) {
            dprintf(STDERR_FILENO, " do you wish to overwrite (y or n)? ");
            ok = yesno();
        }
        if (!ok) {
            dprintf(STDERR_FILENO, "\tnot overwritten\n");
            warned();
            return false;
        }
    }
    lp_unlink(ofname);
    return true;
}

/* ════════════════════════════════════════════════════════════════════
 *  -l
 * ════════════════════════════════════════════════════════════════════ */
static int  list_count;             /* operands, so totals know to print */
static bool list_first = true;
static u64  total_in, total_out;

/* GNU sizes these columns from OFF_T_MAX, which has 19 digits. */
#define LIST_W 19

static void list_header(void)
{
    if (!list_first) return;
    list_first = false;
    if (opt_verbose) printf("method  crc     date  time  ");
    if (!opt_quiet)
        printf("%*s %*s  ratio uncompressed_name\n",
               LIST_W, "compressed", LIST_W, "uncompressed");
}

static void list_totals(void)
{
    if (opt_quiet || list_count <= 1) return;
    if (total_in == 0 || total_out == 0) return;
    if (opt_verbose) printf("                            ");
    printf("%*lu %*lu ", LIST_W, (unsigned long)total_in,
           LIST_W, (unsigned long)total_out);
    print_ratio(STDOUT_FILENO,
                (s64)total_out - ((s64)total_in - (s64)header_bytes),
                (s64)total_out);
    printf(" (totals)\n");
}

static void list_line(u32 crc, u64 comp, u64 uncomp, s64 stamp)
{
    static const char *month[12] = { "Jan","Feb","Mar","Apr","May","Jun",
                                     "Jul","Aug","Sep","Oct","Nov","Dec" };
    list_header();
    if (opt_verbose) {
        lp_tm_t tm;
        lp_localtime(stamp, &tm);
        printf("defla %08lx ", (unsigned long)crc);
        if (tm.mon >= 1 && tm.mon <= 12 && tm.day >= 1 && tm.day <= 31)
            printf("%s %2d %02d:%02d ", month[tm.mon - 1], tm.day,
                   tm.hour, tm.min);
        else
            printf("??? ?? ??:?? ");
    }
    printf("%*lu %*lu ", LIST_W, (unsigned long)comp,
           LIST_W, (unsigned long)uncomp);
    print_ratio(STDOUT_FILENO,
                (s64)uncomp - ((s64)comp - (s64)header_bytes), (s64)uncomp);
    printf(" %s\n", ofname);

    total_in  += comp;
    total_out += uncomp;
}

/* The crc and length live in the last eight bytes, so on a real file
 * this is a seek rather than a decompression. On a pipe there is no
 * choice but to expand the whole thing and count. */
static void do_list(int ifd)
{
    in_reset(ifd);
    out_reset(-1);
    bytes_in = bytes_out = 0;
    header_bytes = 0;
    inf_reset();
    if (read_gzip_header(1) < 0) return;

    u32 crc = 0;
    u64 uncomp = 0, comp = 0;

    if (istat_reg && istat.size >= 8 &&
        lp_lseek(ifd, -8, SEEK_END) >= 0) {
        u8 t[8];
        u32 got = 0;
        while (got < 8) {
            long n = lp_read(ifd, t + got, 8 - got);
            if (n <= 0) break;
            got += (u32)n;
        }
        if (got == 8) {
            crc = (u32)t[0] | ((u32)t[1] << 8) | ((u32)t[2] << 16) |
                  ((u32)t[3] << 24);
            uncomp = (u32)t[4] | ((u32)t[5] << 8) | ((u32)t[6] << 16) |
                     ((u32)t[7] << 24);
        }
        comp = istat.size;
    } else {
        if (inflate_stream() == INF_OK) {
            crc = member_crc ^ 0xFFFFFFFFu;
            uncomp = member_isize;
        }
        comp = in_consumed() + 8;
    }

    s64 stamp = istat_ok ? istat.mtime : lp_time();
    if (!no_time && hdr_mtime > 0) stamp = hdr_mtime;
    list_line(crc, comp, uncomp, stamp);
}

/* ════════════════════════════════════════════════════════════════════
 *  one file
 * ════════════════════════════════════════════════════════════════════ */
static void treat_file(const char *path);

static void treat_dir(const char *path)
{
    char buf[8192];
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        lp_diag("gzip", NULL, NULL, "cannot open", path, (int)-fd);
        exit_code = RC_ERR;
        return;
    }
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof buf);
        if (n <= 0) break;
        for (long off = 0; off < n; ) {
            char *rec = buf + off;
            u16 reclen = *(u16 *)(rec + 16);
            const char *name = rec + 19;
            off += reclen;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
            char child[PATH_LEN];
            snprintf(child, sizeof child, "%s%s%s", path,
                     strcmp(path, "/") == 0 ? "" : "/", name);
            treat_file(child);
        }
    }
    lp_close((int)fd);
}

/* Open the input, trying the compressed spellings of the name when it
 * was given without one - `gunzip foo` has always meant foo.gz, and the
 * error message has to name foo.gz too or it makes no sense. */
static long open_input(const char *path)
{
    static const char *extra[] = { ".gz", ".z", "-z", ".Z" };
    int flags = O_RDONLY;

    /* O_NOFOLLOW when the file is about to be replaced. Following the
     * link there would delete the link and leave the target sitting
     * where it was, compressed under a name nobody asked for. */
    if (!opt_force && !opt_stdout && !opt_list && !opt_test && !opt_keep)
        flags |= O_NOFOLLOW;

    strlcpy(ifname, path, sizeof ifname);
    long fd = lp_open(ifname, flags, 0);
    if (fd >= 0) return fd;
    if (-fd != ENOENT_ || !opt_decompress || get_suffix(ifname) != NULL) {
        lp_diag("gzip", NULL, NULL, "cannot open", ifname, (int)-fd);
        exit_code = RC_ERR;
        return -1;
    }

    for (int i = -1; i < 4; i++) {
        const char *s = (i < 0) ? z_suffix : extra[i];
        if (i >= 0 && strcmp(s, z_suffix) == 0) continue;
        snprintf(ifname, sizeof ifname, "%s%s", path, s);
        fd = lp_open(ifname, flags, 0);
        if (fd >= 0) return fd;
        if (-fd != ENOENT_) {
            lp_diag("gzip", NULL, NULL, "cannot open", ifname, (int)-fd);
            exit_code = RC_ERR;
            return -1;
        }
    }
    snprintf(ifname, sizeof ifname, "%s%s", path, z_suffix);
    lp_diag("gzip", NULL, NULL, "cannot open", ifname, ENOENT_);
    exit_code = RC_ERR;
    return -1;
}

/* The work itself, once the input is open and the output name settled.
 * Returns false when it failed and the output should be thrown away. */
static bool run_one(int ifd, int ofd, s64 in_mtime)
{
    in_reset(ifd);
    out_reset(ofd);
    bytes_in = bytes_out = 0;
    header_bytes = 0;

    if (opt_decompress || opt_test) {
        /* -f -c on something that is not compressed hands it through
         * rather than refusing it. */
        if (opt_force && opt_stdout) {
            u8 magic[2];
            int got = 0;
            int c = in_byte();
            if (c >= 0) {
                magic[got++] = (u8)c;
                c = in_byte();
                if (c >= 0) magic[got++] = (u8)c;
            }
            if (got == 0) return true;
            bool is_gz = got == 2 && magic[0] == GZ_MAGIC1 &&
                         (magic[1] == GZ_MAGIC2 || magic[1] == GZ_OLD2);
            in_push(magic, got);
            if (!is_gz) {
                inf_reset();
                copy_through();
                out_flush();
                return true;
            }
        }
        bool ok = gunzip_file() == 0;
        out_flush();
        return ok && !out_broken;
    }

    gz_crc = 0xFFFFFFFFu;
    gz_isize = 0;
    write_gzip_header(in_mtime);
    /* GNU counts the trailer in with the header, because the only thing
     * this number is used for is the ratio it prints. */
    header_bytes = bytes_out + 8;
    deflate_stream();
    put_u32(gz_crc ^ 0xFFFFFFFFu);
    put_u32((u32)(gz_isize & 0xFFFFFFFFu));
    out_flush();
    return !out_broken;
}

static void treat_file(const char *path)
{
    long ifd = open_input(path);
    if (ifd < 0) return;

    istat_ok  = lp_stat(ifname, &istat, true) == 0;
    istat_reg = istat_ok && (istat.mode & LP_S_IFMT) == LP_S_IFREG;

    if (istat_ok && (istat.mode & LP_S_IFMT) == LP_S_IFDIR) {
        lp_close((int)ifd);
        if (opt_recursive) { treat_dir(ifname); return; }
        if (!opt_quiet)
            dprintf(STDERR_FILENO, "gzip: %s is a directory -- ignored\n",
                    ifname);
        warned();
        return;
    }
    if (istat_ok && !istat_reg && !opt_stdout && !opt_list && !opt_test) {
        lp_close((int)ifd);
        if (!opt_quiet)
            dprintf(STDERR_FILENO, "gzip: %s is not a directory or a regular "
                    "file - ignored\n", ifname);
        warned();
        return;
    }
    if (istat_reg && istat.nlink > 1 && !opt_force && !opt_stdout &&
        !opt_list && !opt_test) {
        lp_close((int)ifd);
        if (!opt_quiet)
            dprintf(STDERR_FILENO, "gzip: %s has %lu other link%s -- file "
                    "ignored\n", ifname, (unsigned long)(istat.nlink - 1),
                    istat.nlink > 2 ? "s" : "");
        warned();
        return;
    }

    if (opt_stdout && !opt_list && !opt_test) {
        strlcpy(ofname, "stdout", sizeof ofname);
    } else if (!make_ofname()) {
        lp_close((int)ifd);
        return;
    }

    if (opt_list) {
        do_list((int)ifd);
        lp_close((int)ifd);
        return;
    }

    int ofd;
    bool made_file = false;
    if (opt_test) {
        ofd = -1;
    } else if (opt_stdout) {
        ofd = STDOUT_FILENO;
    } else {
        if (!check_ofname()) { lp_close((int)ifd); return; }
        long f = lp_open(ofname, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (f < 0) {
            lp_diag("gzip", NULL, NULL, "cannot create", ofname, (int)-f);
            exit_code = RC_ERR;
            lp_close((int)ifd);
            return;
        }
        ofd = (int)f;
        made_file = true;
    }

    s64 in_mtime = istat_reg ? istat.mtime : lp_time();
    hdr_mtime = -1;
    bool ok = run_one((int)ifd, ofd, in_mtime);
    lp_close((int)ifd);

    if (made_file) {
        lp_close(ofd);
        if (!ok) { lp_unlink(ofname); return; }
        s64 stamp = in_mtime;
        if (opt_decompress && !no_time && hdr_mtime > 0) stamp = hdr_mtime;
        copy_stat(stamp);
    }
    if (!ok) return;

    if (opt_verbose) {
        dprintf(STDERR_FILENO, "%s:\t", ifname);
        if (opt_test)
            dprintf(STDERR_FILENO, " OK");
        else if (opt_decompress)
            print_ratio(STDERR_FILENO,
                        (s64)bytes_out - ((s64)bytes_in - (s64)header_bytes),
                        (s64)bytes_out);
        else
            print_ratio(STDERR_FILENO,
                        (s64)bytes_in - ((s64)bytes_out - (s64)header_bytes),
                        (s64)bytes_in);
        if (!opt_test)
            dprintf(STDERR_FILENO, " -- %s %s",
                    opt_keep ? "created" : "replaced with", ofname);
        dprintf(STDERR_FILENO, "\n");
    }

    if (made_file && !opt_keep)
        lp_unlink(ifname);
}

static void treat_stdin(void)
{
    if (!opt_force && !opt_list &&
        lp_isatty(opt_decompress ? STDIN_FILENO : STDOUT_FILENO)) {
        dprintf(STDERR_FILENO,
                "gzip: compressed data not %s a terminal. Use -f to force "
                "%scompression.\nFor help, type: gzip -h\n",
                opt_decompress ? "read from" : "written to",
                opt_decompress ? "de" : "");
        exit_code = RC_ERR;
        return;
    }

    strlcpy(ifname, "stdin", sizeof ifname);
    strlcpy(ofname, "stdout", sizeof ofname);

    istat_ok = lp_stat("/proc/self/fd/0", &istat, true) == 0;
    istat_reg = istat_ok && (istat.mode & LP_S_IFMT) == LP_S_IFREG;

    if (opt_list) {
        do_list(STDIN_FILENO);
        return;
    }

    hdr_mtime = -1;
    s64 mt = istat_reg ? istat.mtime : lp_time();
    bool ok = run_one(STDIN_FILENO, opt_test ? -1 : STDOUT_FILENO, mt);

    if (ok && opt_verbose) {
        dprintf(STDERR_FILENO, "%s:\t", ifname);
        if (opt_test)
            dprintf(STDERR_FILENO, " OK");
        else if (opt_decompress)
            print_ratio(STDERR_FILENO,
                        (s64)bytes_out - ((s64)bytes_in - (s64)header_bytes),
                        (s64)bytes_out);
        else
            print_ratio(STDERR_FILENO,
                        (s64)bytes_in - ((s64)bytes_out - (s64)header_bytes),
                        (s64)bytes_in);
        if (!opt_test)
            dprintf(STDERR_FILENO, " -- replaced with stdout");
        dprintf(STDERR_FILENO, "\n");
    }
}

/* ════════════════════════════════════════════════════════════════════
 *  main
 * ════════════════════════════════════════════════════════════════════ */
static void usage(const char *invoked, int mode)
{
    if (mode == 2) {                        /* zcat */
        printf("Usage: %s [OPTION]... [FILE]...\n"
               "Uncompress FILEs to standard output.\n\n"
               "  -f, --force       force; read compressed data even from a terminal\n"
               "  -l, --list        list compressed file contents\n"
               "  -q, --quiet       suppress all warnings\n"
               "  -r, --recursive   operate recursively on directories\n"
               "  -S, --suffix=SUF  use suffix SUF on compressed files\n"
               "  -t, --test        test compressed file integrity\n"
               "  -v, --verbose     verbose mode\n"
               "      --help        display this help and exit\n\n"
               "With no FILE, or when FILE is -, read standard input.\n",
               invoked);
        return;
    }
    if (mode == 1) {                        /* gunzip */
        printf("Usage: %s [OPTION]... [FILE]...\n"
               "Uncompress FILEs (by default, in-place).\n\n"
               "Mandatory arguments to long options are mandatory for short options too.\n\n"
               "  -c, --stdout      write on standard output, keep original files unchanged\n"
               "  -f, --force       force overwrite of output file and compress links\n"
               "  -k, --keep        keep (don't delete) input files\n"
               "  -l, --list        list compressed file contents\n"
               "  -n, --no-name     do not save or restore the original name and timestamp\n"
               "  -N, --name        save or restore the original name and timestamp\n"
               "  -q, --quiet       suppress all warnings\n"
               "  -r, --recursive   operate recursively on directories\n"
               "  -S, --suffix=SUF  use suffix SUF on compressed files\n"
               "  -t, --test        test compressed file integrity\n"
               "  -v, --verbose     verbose mode\n"
               "      --help        display this help and exit\n\n"
               "With no FILE, or when FILE is -, read standard input.\n",
               invoked);
        return;
    }
    printf("Usage: gzip [OPTION]... [FILE]...\n"
           "Compress or uncompress FILEs (by default, compress FILES in-place).\n\n"
           "Mandatory arguments to long options are mandatory for short options too.\n\n"
           "  -c, --stdout      write on standard output, keep original files unchanged\n"
           "  -d, --decompress  decompress\n"
           "  -f, --force       force overwrite of output file and compress links\n"
           "  -h, --help        give this help\n"
           "  -k, --keep        keep (don't delete) input files\n"
           "  -l, --list        list compressed file contents\n"
           "  -n, --no-name     do not save or restore the original name and timestamp\n"
           "  -N, --name        save or restore the original name and timestamp\n"
           "  -q, --quiet       suppress all warnings\n"
           "  -r, --recursive   operate recursively on directories\n"
           "  -S, --suffix=SUF  use suffix SUF on compressed files\n"
           "  -t, --test        test compressed file integrity\n"
           "  -v, --verbose     verbose mode\n"
           "  -1, --fast        compress faster\n"
           "  -9, --best        compress better\n\n"
           "With no FILE, or when FILE is -, read standard input.\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "stdout", 0, 'c' }, { "to-stdout", 0, 'c' },
        { "decompress", 0, 'd' }, { "uncompress", 0, 'd' },
        { "force", 0, 'f' }, { "help", 0, 'h' }, { "keep", 0, 'k' },
        { "list", 0, 'l' }, { "no-name", 0, 'n' }, { "name", 0, 'N' },
        { "quiet", 0, 'q' }, { "silent", 0, 'q' }, { "recursive", 0, 'r' },
        { "suffix", 1, 'S' }, { "test", 0, 't' }, { "verbose", 0, 'v' },
        { "fast", 0, '1' }, { "best", 0, '9' },
        { 0, 0, 0 }
    };

    /* Which of the three names was typed. gunzip and zcat are the same
     * program with two options already set. */
    const char *me = base_name(argv[0]);
    int mode = 0;
    if (strncmp(me, "un", 2) == 0 || strncmp(me, "gun", 3) == 0) {
        opt_decompress = true; mode = 1;
    } else if (strncmp(me, "zcat", 4) == 0 || strncmp(me, "gzcat", 5) == 0) {
        opt_decompress = true; opt_stdout = true; mode = 2;
    }

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "cdfhklnNqrS:tv123456789", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'c': opt_stdout = true; break;
        case 'd': opt_decompress = true; break;
        case 'f': opt_force = true; break;
        case 'k': opt_keep = true; break;
        case 'l': opt_list = true; opt_decompress = true; break;
        case 'n': no_name = 1; no_time = 1; break;
        case 'N': no_name = 0; no_time = 0; break;
        case 'q': opt_quiet = true; opt_verbose = false; break;
        case 'r': opt_recursive = true; break;
        case 'S':
            if (!g.arg) {
                dprintf(STDERR_FILENO, "gzip: option '--suffix' requires an "
                        "argument\nTry `gzip --help' for more information.\n");
                return 1;
            }
            z_suffix = g.arg;
            break;
        case 't': opt_test = true; opt_decompress = true; break;
        case 'v': opt_verbose = true; opt_quiet = false; break;
        case 'h': usage(argv[0], mode); return 0;
        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9':
            opt_level = c - '0';
            break;
        case '?':
            if (g.badlong)
                dprintf(STDERR_FILENO, "gzip: unrecognized option '%s'\n",
                        g.badlong);
            else
                dprintf(STDERR_FILENO, "gzip: -%c not supported in this "
                        "version\n", g.badchar);
            dprintf(STDERR_FILENO, "Try `gzip --help' for more information.\n");
            return 1;
        }
    }

    /* GNU's rule, and the reason gunzip does not restore the stored
     * name by default: on the way out both default to "do not". */
    if (no_time < 0) no_time = opt_decompress;
    if (no_name < 0) no_name = opt_decompress;

    crc_init();
    deflate_tables();

    list_count = argc - g.ind;
    if (list_count <= 0) {
        treat_stdin();
    } else {
        for (int i = g.ind; i < argc; i++) {
            if (strcmp(argv[i], "-") == 0) treat_stdin();
            else treat_file(argv[i]);
        }
    }
    if (opt_list) list_totals();
    out_flush();
    return exit_code;
}
