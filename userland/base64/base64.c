/* base64 - text that survives anything.
 *
 *   base64 FILE          encode
 *   base64 -d FILE       decode
 *   base64 -w0           one long line instead of 76-character ones
 *
 * base32 is the same program under the other name, because the only
 * difference is the alphabet and how many bits go in a character.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static const char *prog = "base64";
static const char *alpha = B64;
static int  bits_per = 6;          /* 6 for base64, 5 for base32 */
static long wrap = 76;
static bool ignore_garbage = false;

static char outbuf[4096];
static size_t outn = 0;
static long   col = 0;

static void flush_out(void)
{
    if (outn) { lp_write(STDOUT_FILENO, outbuf, outn); outn = 0; }
}

static void emit(char c)
{
    if (outn == sizeof outbuf) flush_out();
    outbuf[outn++] = c;
}

static void emit_wrapped(char c)
{
    emit(c);
    if (wrap > 0 && ++col == wrap) { emit('\n'); col = 0; }
}

static int value_of(char c)
{
    const char *p = strchr(alpha, c);
    if (!p || !c) return -1;
    return (int)(p - alpha);
}

static void encode(int fd)
{
    unsigned char in[3072];
    u32  acc = 0;
    int  nbits = 0;
    long total = 0;

    for (;;) {
        long n = lp_read(fd, in, sizeof in);
        if (n <= 0) break;
        for (long i = 0; i < n; i++) {
            acc = (acc << 8) | in[i];
            nbits += 8;
            total++;
            while (nbits >= bits_per) {
                nbits -= bits_per;
                emit_wrapped(alpha[(acc >> nbits) & ((1u << bits_per) - 1)]);
            }
        }
    }
    if (nbits) {
        emit_wrapped(alpha[(acc << (bits_per - nbits)) & ((1u << bits_per) - 1)]);
        nbits = 0;
    }
    /* Pad to a whole group: 4 characters for base64, 8 for base32. */
    int group = (bits_per == 6) ? 4 : 8;
    long chars = (total * 8 + bits_per - 1) / bits_per;
    while (chars % group) { emit_wrapped('='); chars++; }
    if (col != 0) emit('\n');
    else if (total == 0 && wrap > 0) { /* nothing at all: no newline */ }
    flush_out();
}

static bool decode(int fd)
{
    char in[4096];
    u32  acc = 0;
    int  nbits = 0;

    for (;;) {
        long n = lp_read(fd, in, sizeof in);
        if (n <= 0) break;
        for (long i = 0; i < n; i++) {
            char c = in[i];
            if (c == '\n' || c == '\r' || c == '=') continue;
            int v = value_of(c);
            if (v < 0) {
                if (ignore_garbage) continue;
                flush_out();
                dprintf(STDERR_FILENO, "%s: invalid input\n", prog);
                return false;
            }
            acc = (acc << bits_per) | (u32)v;
            nbits += bits_per;
            if (nbits >= 8) {
                nbits -= 8;
                emit((char)((acc >> nbits) & 0xff));
            }
        }
    }
    flush_out();
    return true;
}

static void usage(int fd)
{
    dprintf(fd, "Usage: %s [OPTION]... [FILE]\n"
                "Base%d encode or decode FILE, or standard input, to standard output.\n\n"
                "  -d, --decode          decode data\n"
                "  -i, --ignore-garbage  when decoding, ignore non-alphabet characters\n"
                "  -w, --wrap=COLS       wrap encoded lines after COLS character (default 76).\n"
                "                          Use 0 to disable line wrapping\n"
                "      --help     display this help and exit\n",
                prog, bits_per == 6 ? 64 : 32);
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "decode", 0, 'd' }, { "ignore-garbage", 0, 'i' },
        { "wrap", 1, 'w' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    prog = base;
    if (strcmp(base, "base32") == 0) { alpha = B32; bits_per = 5; }

    bool dec = false;
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "diw:", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'd': dec = true; break;
        case 'i': ignore_garbage = true; break;
        case 'w': wrap = strtol(g.arg, NULL, 10); break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default: lp_getopt_err(prog, &g); return 1;
        }
    }

    int fd = STDIN_FILENO;
    if (g.ind < argc && strcmp(argv[g.ind], "-") != 0) {
        long f = lp_open(argv[g.ind], O_RDONLY, 0);
        if (f < 0) {
            lp_diag(prog, NULL, NULL, "cannot open", argv[g.ind], (int)-f);
            return 1;
        }
        fd = (int)f;
    }

    bool ok = true;
    if (dec) ok = decode(fd);
    else     encode(fd);
    if (fd != STDIN_FILENO) lp_close(fd);
    return ok ? 0 : 1;
}
