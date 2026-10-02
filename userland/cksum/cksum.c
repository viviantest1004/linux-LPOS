/* cksum - the checksum that tells you a file arrived intact.
 *
 *   cksum file           the POSIX CRC-32 and the byte count
 *   cksum -a md5 file    or any of the other digests
 *
 * The default is the CRC from the POSIX spec, and it is not the CRC-32
 * that zip and gzip use: the bits are not reflected, the register starts
 * at zero, and - the part that catches everybody - the length of the file
 * is fed through the register after the data. Two files that differ only
 * in trailing NULs would otherwise get the same answer.
 *
 * The byte count is printed with it for the same reason it is fed in:
 * 32 bits is a weak thing to stake a file on, and "the same checksum and
 * the same length" is a good deal stronger than either alone. This is a
 * check against corruption, not against somebody malicious. For that,
 * use sha256sum.
 *
 * Not here, and the reason:
 *   -c            checking a list back is sha256sum's job on this system
 *   -l            only means anything for blake2b, which is not here
 *   sha224, sha384, blake2b, sm3   the libc has MD5, SHA-1, SHA-256 and
 *                 SHA-512 and no more; a truncated SHA-2 needs its own
 *                 initial values, so it is left out rather than guessed
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define PROG "cksum"

typedef enum { A_BSD = 0, A_SYSV, A_CRC, A_MD5, A_SHA1, A_SHA256, A_SHA512 } algo_t;

static const struct {
    const char *name;
    const char *tag;      /* NULL for the three that print their own shape */
    int         lp_algo;
} ALGO[] = {
    { "bsd",    NULL,     -1 },
    { "sysv",   NULL,     -1 },
    { "crc",    NULL,     -1 },
    { "md5",    "MD5",    LP_MD5 },
    { "sha1",   "SHA1",   LP_SHA1 },
    { "sha256", "SHA256", LP_SHA256 },
    { "sha512", "SHA512", LP_SHA512 },
};
#define N_ALGO ((int)(sizeof ALGO / sizeof ALGO[0]))

static algo_t algo = A_CRC;
static bool   prefix_tag = true;    /* --untagged turns it off */
static int    binary = 0;           /* --tag also means "binary", so the
                                     * untagged line then gets a '*' */
static bool   opt_base64 = false, opt_raw = false;
static char   eol = '\n';
static int    status = 0;

/* ── the POSIX CRC ──
 * Table built at run time; 1 KB of code beats 1 KB of constants. */
static u32 crctab[256];

static void crc_table(void)
{
    for (u32 i = 0; i < 256; i++) {
        u32 r = i << 24;
        for (int j = 0; j < 8; j++)
            r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : (r << 1);
        crctab[i] = r;
    }
}

static u32 crc_byte(u32 crc, u8 b)
{
    return (crc << 8) ^ crctab[((crc >> 24) ^ b) & 0xFF];
}

/* ── one file ── */
static u32 crc;         /* also holds the sysv/bsd running value */
static u64 total;
static lp_digest_t dig;
static char hex[160];

static bool checksum_fd(int fd, const char *whoami)
{
    static char buf[65536];

    crc = 0;
    total = 0;
    if (ALGO[algo].lp_algo >= 0)
        lp_digest_init(&dig, ALGO[algo].lp_algo);

    for (;;) {
        long n = lp_read(fd, buf, sizeof buf);
        if (n == 0)
            break;
        if (n < 0) {
            lp_diag(PROG, NULL, NULL, "read error", whoami, (int)-n);
            status = 1;
            return false;
        }
        total += (u64)n;
        switch (algo) {
        case A_CRC:
            for (long i = 0; i < n; i++)
                crc = crc_byte(crc, (u8)buf[i]);
            break;
        case A_SYSV:
            for (long i = 0; i < n; i++)
                crc += (u8)buf[i];
            break;
        case A_BSD:
            for (long i = 0; i < n; i++) {
                crc = (crc >> 1) + ((crc & 1) << 15);
                crc = (crc + (u8)buf[i]) & 0xFFFF;
            }
            break;
        default:
            lp_digest_update(&dig, buf, (size_t)n);
            break;
        }
    }

    switch (algo) {
    case A_CRC: {
        /* The length goes through the register too - this is what makes
         * it the POSIX CRC rather than the ordinary one. */
        u64 len = total;
        while (len) { crc = crc_byte(crc, (u8)(len & 0xFF)); len >>= 8; }
        crc = ~crc;
        break;
    }
    case A_SYSV: {
        u32 r = (crc & 0xFFFF) + ((crc & 0xFFFFFFFFu) >> 16);
        crc = (r & 0xFFFF) + (r >> 16);
        break;
    }
    case A_BSD:
        break;
    default:
        lp_digest_final(&dig, hex);
        break;
    }
    return true;
}

/* ── printing ── */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

static void put_base64(const char *h)
{
    static const char *T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t nbytes = strlen(h) / 2;
    u8 b[64];
    for (size_t i = 0; i < nbytes && i < sizeof b; i++)
        b[i] = (u8)((hexval(h[2 * i]) << 4) | hexval(h[2 * i + 1]));

    char out[128];
    size_t o = 0;
    for (size_t i = 0; i < nbytes; i += 3) {
        u32 v = (u32)b[i] << 16;
        if (i + 1 < nbytes) v |= (u32)b[i + 1] << 8;
        if (i + 2 < nbytes) v |= b[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < nbytes) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < nbytes) ? T[v & 63]        : '=';
    }
    out[o] = '\0';
    fputs(out, STDOUT_FILENO);
}

/* GNU escapes a name holding a backslash or a newline and marks the line
 * with a leading backslash, so a checker can read the list back. With -z
 * the names are NUL separated and none of that is needed. */
static bool needs_escape(const char *name)
{
    if (eol != '\n' || !name)
        return false;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '\n')
            return true;
    return false;
}

static void put_name(const char *name, bool escaped)
{
    if (!escaped) { fputs(name, STDOUT_FILENO); return; }
    for (const char *p = name; *p; p++) {
        if (*p == '\\')      fputs("\\\\", STDOUT_FILENO);
        else if (*p == '\n') fputs("\\n", STDOUT_FILENO);
        else                 putchar(*p);
    }
}

static void print_result(const char *name)
{
    if (opt_raw) {
        u8 b[64];
        size_t n;
        if (algo == A_CRC) {
            n = 4;
            for (size_t i = 0; i < 4; i++) b[i] = (u8)(crc >> (8 * (3 - i)));
        } else if (algo == A_SYSV || algo == A_BSD) {
            n = 2;
            b[0] = (u8)(crc >> 8); b[1] = (u8)crc;
        } else {
            n = strlen(hex) / 2;
            for (size_t i = 0; i < n; i++)
                b[i] = (u8)((hexval(hex[2 * i]) << 4) | hexval(hex[2 * i + 1]));
        }
        lp_write(STDOUT_FILENO, b, n);
        return;
    }

    switch (algo) {
    case A_CRC:
        printf("%u %lu", crc, (unsigned long)total);
        if (name) printf(" %s", name);
        putchar(eol);
        return;
    case A_SYSV:
        printf("%u %lu", crc, (unsigned long)((total + 511) / 512));
        if (name) printf(" %s", name);
        putchar(eol);
        return;
    case A_BSD:
        printf("%05u %5lu", crc, (unsigned long)((total + 1023) / 1024));
        if (name) printf(" %s", name);
        putchar(eol);
        return;
    default:
        break;
    }

    if (!name) name = "-";
    bool esc = needs_escape(name);
    if (esc) putchar('\\');

    if (prefix_tag) {
        printf("%s (", ALGO[algo].tag);
        put_name(name, esc);
        fputs(") = ", STDOUT_FILENO);
        if (opt_base64) put_base64(hex); else fputs(hex, STDOUT_FILENO);
    } else {
        if (opt_base64) put_base64(hex); else fputs(hex, STDOUT_FILENO);
        printf(" %c", binary > 0 ? '*' : ' ');
        put_name(name, esc);
    }
    putchar(eol);
}

static void bad_algo(const char *given)
{
    dprintf(STDERR_FILENO,
            PROG ": invalid argument '%s' for '--algorithm'\n"
            "Valid arguments are:\n", given);
    for (int i = 0; i < N_ALGO; i++)
        dprintf(STDERR_FILENO, "  - '%s'\n", ALGO[i].name);
    dprintf(STDERR_FILENO, "Try '" PROG " --help' for more information.\n");
    lp_exit(1);
}

static void usage(void)
{
    printf("Usage: cksum [OPTION]... [FILE]...\n"
           "Print checksums.\n"
           "By default use the 32 bit CRC algorithm.\n\n"
           "With no FILE, or when FILE is -, read standard input.\n\n"
           "  -a, --algorithm=TYPE  select the digest type to use.  See DIGEST below.\n"
           "      --base64          emit base64-encoded digests, not hexadecimal\n"
           "      --raw             emit a raw binary digest, not hexadecimal\n"
           "      --tag             create a BSD-style checksum (the default)\n"
           "      --untagged        create a reversed style checksum, without digest type\n"
           "  -z, --zero            end each output line with NUL, not newline,\n"
           "                          and disable file name escaping\n"
           "      --help        display this help and exit\n\n"
           "DIGEST determines the digest algorithm and default output format:\n"
           "  sysv      (equivalent to sum -s)\n"
           "  bsd       (equivalent to sum -r)\n"
           "  crc       (equivalent to cksum)\n"
           "  md5       (equivalent to md5sum)\n"
           "  sha1      (equivalent to sha1sum)\n"
           "  sha256    (equivalent to sha256sum)\n"
           "  sha512    (equivalent to sha512sum)\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "algorithm", 1, 'a' }, { "base64", 0, 'B' }, { "raw", 0, 'R' },
        { "tag", 0, 'T' }, { "untagged", 0, 'U' }, { "zero", 0, 'z' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "a:z", lo);

    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'a': {
            if (!g.arg) {
                dprintf(STDERR_FILENO,
                        PROG ": option requires an argument -- 'a'\n"
                        "Try '" PROG " --help' for more information.\n");
                return 1;
            }
            int i = 0;
            while (i < N_ALGO && strcmp(ALGO[i].name, g.arg) != 0) i++;
            if (i == N_ALGO) bad_algo(g.arg);
            algo = (algo_t)i;
            break;
        }
        case 'B': opt_base64 = true; break;
        case 'R': opt_raw = true; break;
        case 'T': prefix_tag = true; binary = 1; break;
        case 'U': prefix_tag = false; break;
        case 'z': eol = '\0'; break;
        case 'H': usage(); return 0;
        default: lp_getopt_err(PROG, &g); return 1;
        }
    }

    int nfiles = argc - g.ind;
    if (opt_raw && nfiles > 1) {
        dprintf(STDERR_FILENO,
                PROG ": the --raw option is not supported with multiple files\n");
        return 1;
    }

    crc_table();

    if (nfiles == 0) {
        if (checksum_fd(STDIN_FILENO, "-"))
            print_result(NULL);
        return status;
    }

    for (int i = g.ind; i < argc; i++) {
        const char *name = argv[i];
        if (strcmp(name, "-") == 0) {
            if (checksum_fd(STDIN_FILENO, name))
                print_result(name);
            continue;
        }
        long fd = lp_open(name, O_RDONLY, 0);
        if (fd < 0) {
            lp_diag(PROG, NULL, NULL, "cannot open", name, (int)-fd);
            status = 1;
            continue;
        }
        if (checksum_fd((int)fd, name))
            print_result(name);
        lp_close((int)fd);
    }
    return status;
}
