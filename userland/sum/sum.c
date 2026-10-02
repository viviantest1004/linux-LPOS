/* sum - the two checksums that came before cksum.
 *
 *   sum file        BSD: a 16-bit sum with the accumulator rotated right
 *   sum -s file     System V: a 32-bit sum folded down to 16 bits
 *
 * Neither is a hash. Both are old enough that swapping two bytes in a
 * file, or changing one byte and compensating in another, leaves the
 * number alone. They are here because tapes, floppies and the manifests
 * of thirty-year-old packages carry these numbers and nothing else can
 * read them; for deciding whether a file is the file you meant, use
 * sha256sum.
 *
 * The two halves of the line are as easy to get wrong as the checksums:
 * BSD prints the number in five columns padded with zeroes and the block
 * count in five columns padded with spaces, System V prints both with no
 * padding at all, and the block is 1024 bytes for BSD and 512 for System
 * V. Get any of that wrong and the number is right but no old script
 * that greps for it will match.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

static bool sysv = false;

static char buf[65536];

/* Both algorithms in one pass. Returns false when the read failed;
 * *err then holds the errno so the caller can name the file. */
static bool sum_fd(int fd, u32 *out_sum, u64 *out_blocks, int *err)
{
    u64 total = 0;
    u32 bsd = 0;           /* 16 bits, rotated right before each byte */
    u64 sysv_acc = 0;      /* the plain sum, folded only at the end */

    for (;;) {
        long n = lp_read(fd, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0) { *err = (int)-n; return false; }
        total += (u64)n;
        if (sysv) {
            for (long i = 0; i < n; i++)
                sysv_acc += (u8)buf[i];
        } else {
            for (long i = 0; i < n; i++) {
                bsd = (bsd >> 1) | ((bsd & 1) << 15);
                bsd = (bsd + (u8)buf[i]) & 0xffff;
            }
        }
    }

    if (sysv) {
        u32 r = (u32)((sysv_acc & 0xffff) + ((sysv_acc & 0xffffffff) >> 16));
        *out_sum = (r & 0xffff) + (r >> 16);
        *out_blocks = (total + 511) / 512;
    } else {
        *out_sum = bsd;
        *out_blocks = (total + 1023) / 1024;
    }
    return true;
}

/* name == NULL is plain standard input, which prints no name at all -
 * `sum < f` and `sum - < f` give different lines and both are right. */
static bool report(int fd, const char *name, const char *diagname)
{
    u32 s = 0;
    u64 blocks = 0;
    int err = 0;

    if (!sum_fd(fd, &s, &blocks, &err)) {
        lp_diag("sum", NULL, NULL, "cannot read", diagname, err);
        return false;
    }
    if (sysv)
        printf("%u %llu", s, (unsigned long long)blocks);
    else
        printf("%05u %5llu", s, (unsigned long long)blocks);
    if (name) printf(" %s", name);
    printf("\n");
    return true;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "sysv", 0, 's' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "rs", lo);

    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        /* Last one wins: `sum -s -r` is BSD, `sum -r -s` is System V. */
        case 'r': sysv = false; break;
        case 's': sysv = true;  break;
        case 'H':
            printf("Usage: sum [OPTION]... [FILE]...\n"
                   "Print or check BSD (16-bit) checksums.\n\n"
                   "With no FILE, or when FILE is -, read standard input.\n\n"
                   "  -r              use BSD sum algorithm (the default), use 1K blocks\n"
                   "  -s, --sysv      use System V sum algorithm, use 512 bytes blocks\n"
                   "      --help        display this help and exit\n");
            return 0;
        default: lp_getopt_err("sum", &g); return 1;
        }
    }

    if (g.ind >= argc)
        return report(STDIN_FILENO, NULL, "-") ? 0 : 1;

    int rc = 0;
    for (int i = g.ind; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            if (!report(STDIN_FILENO, "-", "-")) rc = 1;
            continue;
        }
        long fd = lp_open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            lp_diag("sum", NULL, NULL, "cannot open", argv[i], (int)-fd);
            rc = 1;
            continue;
        }
        if (!report((int)fd, argv[i], argv[i])) rc = 1;
        lp_close((int)fd);
    }
    return rc;
}
