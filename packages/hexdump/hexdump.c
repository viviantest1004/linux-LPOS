/* hexdump - look at the bytes.
 *
 *   hexdump [-n bytes] [-s skip] [file]
 *
 * Sixteen bytes a line, hex on the left and printable characters on the
 * right, with identical lines collapsed to a * the way every hexdump
 * does - a file that is mostly zeroes should not take a thousand lines
 * to say so.
 *
 * Built with the SDK against this system's own libc. It is a package
 * rather than part of the image because a board that has never needed
 * it should not carry it in RAM for the life of the machine.
 */
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "unistd.h"

static bool printable(u8 c) { return c >= 0x20 && c < 0x7f; }

static void line(u64 offset, const u8 *b, int n)
{
    printf("%08lx  ", (unsigned long)offset);
    for (int i = 0; i < 16; i++) {
        if (i < n) printf("%02x ", b[i]);
        else       printf("   ");
        if (i == 7) printf(" ");
    }
    printf(" |");
    for (int i = 0; i < n; i++)
        printf("%c", printable(b[i]) ? b[i] : '.');
    printf("|\n");
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    u64 limit = 0, skip = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            limit = (u64)strtol(argv[++i], 0, 0);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            skip = (u64)strtol(argv[++i], 0, 0);
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: hexdump [-n bytes] [-s skip] [file]\n");
            printf("  with no file, standard input\n");
            return 0;
        } else path = argv[i];
    }

    int fd = STDIN_FILENO;
    if (path) {
        long f = lp_open(path, O_RDONLY, 0);
        if (f < 0) {
            dprintf(STDERR_FILENO, "hexdump: %s: cannot open\n", path);
            return 1;
        }
        fd = (int)f;
        if (skip) lp_lseek(fd, (off_t)skip, 0);
    }

    u8   buf[16], prev[16];
    u64  offset = skip, done = 0;
    bool repeating = false;
    int  prev_n = -1;

    for (;;) {
        if (limit && done >= limit)
            break;

        int want = 16;
        if (limit && limit - done < 16)
            want = (int)(limit - done);

        long n = lp_read(fd, buf, (size_t)want);
        if (n <= 0)
            break;

        /* A run of identical lines collapses to one *, which is what
         * makes a mostly-empty file readable. */
        if (n == 16 && prev_n == 16 && memcmp(buf, prev, 16) == 0) {
            if (!repeating) { printf("*\n"); repeating = true; }
        } else {
            line(offset, buf, (int)n);
            repeating = false;
        }

        memcpy(prev, buf, (size_t)n);
        prev_n = (int)n;
        offset += (u64)n;
        done   += (u64)n;
    }

    printf("%08lx\n", (unsigned long)offset);
    if (fd != STDIN_FILENO) lp_close(fd);
    return 0;
}
