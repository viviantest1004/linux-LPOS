/* od - the bytes, in a base you can read.
 *
 *   od -c file        characters, escapes for the ones that have none
 *   od -An -tx1 file  plain hex, no offsets - for a diff of two binaries
 *   od -tx4z file     words, with the text alongside
 *
 * When a file "looks fine" and still does not work, this is what shows
 * the CR, the BOM or the NUL that is actually there.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

typedef struct { char kind; int size; } fmt_t;   /* kind: x o u d c a */

#define MAX_FMT 8
static fmt_t fmts[MAX_FMT];
static int   nfmt;
static char  addr_base = 'o';       /* o d x n */
static u64   skip_bytes = 0, limit = 0;
static bool  have_limit = false;

static const char *NAMED[] = {
    "nul","soh","stx","etx","eot","enq","ack","bel",
    " bs"," ht"," nl"," vt"," ff"," cr","\x73o","\x73i",
    "dle","dc1","dc2","dc3","dc4","nak","syn","etb",
    "can"," em","sub","esc"," fs"," gs"," rs"," us"
};

static void print_addr(u64 off, bool label_only)
{
    if (addr_base == 'n') return;
    if (label_only) { /* the closing line */ }
    if (addr_base == 'x')      printf("%07llx", (unsigned long long)off);
    else if (addr_base == 'd') printf("%07llu", (unsigned long long)off);
    else                       printf("%07llo", (unsigned long long)off);
}

/* -t c prints C escapes; -t a prints the ASCII names. They are two
 * different questions - "what would this look like in a string literal"
 * and "which control character is this" - and od answers both. */
static void print_char_cell(unsigned char c)
{
    switch (c) {
    case 0:    printf("  \\0"); return;
    case 7:    printf("  \\a"); return;
    case 8:    printf("  \\b"); return;
    case 9:    printf("  \\t"); return;
    case 10:   printf("  \\n"); return;
    case 11:   printf("  \\v"); return;
    case 12:   printf("  \\f"); return;
    case 13:   printf("  \\r"); return;
    case '\\': printf("   \\"); return;
    default: break;
    }
    if (c < 32 || c >= 127) { printf(" %03o", c); return; }
    printf("   %c", c);
}

static void print_named_cell(unsigned char c)
{
    if (c < 32)   { printf(" %3s", NAMED[c]); return; }
    if (c == 32)  { printf("  sp"); return; }
    if (c == 127) { printf(" del"); return; }
    if (c < 127)  { printf("   %c", c); return; }
    printf(" %03o", c);
}

static u64 word_at(const unsigned char *p, int size, size_t avail)
{
    u64 v = 0;
    for (int i = size - 1; i >= 0; i--) {
        unsigned char b = ((size_t)i < avail) ? p[i] : 0;
        v = (v << 8) | b;
    }
    return v;
}

/* A run of identical lines becomes one "*". A page of zeroes is the
 * normal case in a binary and printing all of it hides the rest. -v
 * turns it off. */
static bool show_dups = false;
static unsigned char prev_line[256];
static size_t prev_n = 0;
static bool   have_prev = false, star_open = false;

static void print_line(const unsigned char *p, size_t n, u64 off)
{
    if (!show_dups && have_prev && n == prev_n &&
        memcmp(p, prev_line, n) == 0) {
        if (!star_open) { printf("*\n"); star_open = true; }
        return;
    }
    star_open = false;
    memcpy(prev_line, p, n);
    prev_n = n;
    have_prev = true;

    for (int f = 0; f < nfmt; f++) {
        if (f == 0) print_addr(off, false);
        else if (addr_base != 'n') printf("       ");

        const fmt_t *ft = &fmts[f];
        if (ft->kind == 'c') {
            for (size_t i = 0; i < n; i++) print_char_cell(p[i]);
        } else if (ft->kind == 'a') {
            for (size_t i = 0; i < n; i++) print_named_cell(p[i]);
        } else {
            for (size_t i = 0; i < n; i += (size_t)ft->size) {
                u64 v = word_at(p + i, ft->size, n - i);
                int width = ft->size * (ft->kind == 'x' ? 2 :
                                        ft->kind == 'o' ? 3 : 3);
                if (ft->kind == 'x') printf(" %0*llx", width, (unsigned long long)v);
                else if (ft->kind == 'o') printf(" %0*llo", width, (unsigned long long)v);
                else if (ft->kind == 'u') printf(" %*llu", width, (unsigned long long)v);
                else {
                    s64 sv = (s64)v;
                    if (ft->size == 1) sv = (s8)v;
                    else if (ft->size == 2) sv = (s16)v;
                    else if (ft->size == 4) sv = (s32)v;
                    printf(" %*lld", width, (long long)sv);
                }
            }
        }
        printf("\n");
    }
}

static void add_fmt(char kind, int size)
{
    if (nfmt < MAX_FMT) { fmts[nfmt].kind = kind; fmts[nfmt].size = size; nfmt++; }
}

/* -t takes things like x1, x4, d2, c, u8. */
static void parse_type(const char *s)
{
    while (*s) {
        char kind = *s++;
        int size = (kind == 'c' || kind == 'a') ? 1 : 4;
        if (*s >= '1' && *s <= '9') { size = *s - '0'; s++; }
        else if (*s == 'C') { size = 1; s++; }
        else if (*s == 'S') { size = 2; s++; }
        else if (*s == 'I' || *s == 'L') { size = 4; s++; }
        while (*s == 'z') s++;              /* accepted, not shown */
        add_fmt(kind, size);
    }
}

static u64 parse_num(const char *s)
{
    char *end;
    u64 v = (u64)strtoll(s, &end, (s[0] == '0' && s[1] == 'x') ? 16 : 10);
    switch (*end) {
    case 'b': v *= 512; break;
    case 'k': case 'K': v *= 1024; break;
    case 'm': case 'M': v *= 1048576; break;
    default: break;
    }
    return v;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "address-radix", 1, 'A' }, { "skip-bytes", 1, 'j' },
        { "read-bytes", 1, 'N' }, { "format", 1, 't' },
        { "output-duplicates", 0, 'v' }, { "width", 1, 'w' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    long width = 16;
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "A:j:N:t:vw:bcdoxsi", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'A': addr_base = g.arg[0]; break;
        case 'j': skip_bytes = parse_num(g.arg); break;
        case 'N': limit = parse_num(g.arg); have_limit = true; break;
        case 't': parse_type(g.arg); break;
        case 'w': width = strtol(g.arg, NULL, 10); break;
        case 'v': show_dups = true; break;
        case 'b': add_fmt('o', 1); break;
        case 'c': add_fmt('c', 1); break;
        case 'd': add_fmt('u', 2); break;
        case 'o': add_fmt('o', 2); break;
        case 'x': add_fmt('x', 2); break;
        case 's': add_fmt('d', 2); break;
        case 'i': add_fmt('d', 4); break;
        case 'H':
            printf("Usage: od [OPTION]... [FILE]...\n"
                   "Write an unambiguous representation of FILE to standard output.\n\n"
                   "  -A, --address-radix=RADIX   output format for file offsets: d o x n\n"
                   "  -j, --skip-bytes=BYTES      skip BYTES input bytes first\n"
                   "  -N, --read-bytes=BYTES      limit dump to BYTES input bytes\n"
                   "  -t, --format=TYPE           select output format or formats\n"
                   "  -w, --width=BYTES           output BYTES bytes per output line\n"
                   "  -c   same as -t c      -x   same as -t x2\n"
                   "  -b   same as -t o1     -d   same as -t u2\n"
                   "      --help     display this help and exit\n");
            return 0;
        default: lp_getopt_err("od", &g); return 1;
        }
    }
    if (nfmt == 0) add_fmt('o', 2);
    if (width < 1) width = 16;

    int fd = STDIN_FILENO;
    if (g.ind < argc && strcmp(argv[g.ind], "-") != 0) {
        long f = lp_open(argv[g.ind], O_RDONLY, 0);
        if (f < 0) {
            lp_diag("od", NULL, NULL, "cannot open", argv[g.ind], (int)-f);
            return 1;
        }
        fd = (int)f;
    }

    unsigned char line[256];
    if (width > (long)sizeof line) width = (long)sizeof line;
    u64 off = 0;
    size_t have = 0;

    /* Seek first, and only read through the skipped bytes when seeking
     * is not possible - a pipe. Reading was the whole implementation
     * once, which was correct and meant `od -j 3G` on a card read three
     * gigabytes through a 1GHz core before printing its first line. */
    if (skip_bytes) {
        s64 landed = lp_lseek((int)fd, (off_t)skip_bytes, SEEK_SET);
        if (landed >= 0 && (u64)landed == skip_bytes) {
            off = skip_bytes;
            skip_bytes = 0;
        }
    }
    while (skip_bytes) {
        unsigned char junk[4096];
        size_t want = skip_bytes > sizeof junk ? sizeof junk : (size_t)skip_bytes;
        long n = lp_read(fd, junk, want);
        if (n <= 0) break;
        skip_bytes -= (u64)n;
        off += (u64)n;
    }

    u64 read_total = 0;
    for (;;) {
        size_t want = (size_t)width - have;
        if (have_limit) {
            if (read_total >= limit) break;
            if (limit - read_total < want) want = (size_t)(limit - read_total);
        }
        long n = lp_read(fd, line + have, want);
        if (n <= 0) break;
        have += (size_t)n;
        read_total += (u64)n;
        if (have == (size_t)width) {
            print_line(line, have, off);
            off += have;
            have = 0;
        }
    }
    if (have) { print_line(line, have, off); off += have; }
    if (addr_base != 'n') { print_addr(off, true); printf("\n"); }

    if (fd != STDIN_FILENO) lp_close(fd);
    return 0;
}
