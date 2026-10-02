/* strings - the readable text inside a binary.
 *
 *   strings /bin/ls | grep -i version
 *
 * Four printable characters in a row counts as a string, which is the
 * same rule binutils uses and the reason the output is mostly useful.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static long minlen = 4;
static char radix = 0;          /* 0 none, 'o' 'd' 'x' */

static void scan(int fd, const char *name, bool show_name)
{
    unsigned char buf[65536];
    char run[8192];
    size_t rn = 0;
    u64 off = 0, start = 0;

    for (;;) {
        long n = lp_read(fd, buf, sizeof buf);
        if (n <= 0) break;
        for (long i = 0; i < n; i++, off++) {
            unsigned char c = buf[i];
            bool ok = (c >= 32 && c < 127) || c == '\t';
            if (ok) {
                if (rn == 0) start = off;
                if (rn < sizeof run - 1) run[rn++] = (char)c;
                continue;
            }
            if ((long)rn >= minlen) {
                run[rn] = '\0';
                if (show_name) printf("%s: ", name);
                if (radix == 'o')      printf("%7llo ", (unsigned long long)start);
                else if (radix == 'd') printf("%7llu ", (unsigned long long)start);
                else if (radix == 'x') printf("%7llx ", (unsigned long long)start);
                printf("%s\n", run);
            }
            rn = 0;
        }
    }
    if ((long)rn >= minlen) {
        run[rn] = '\0';
        if (show_name) printf("%s: ", name);
        if (radix) printf("%7llu ", (unsigned long long)start);
        printf("%s\n", run);
    }
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "bytes", 1, 'n' }, { "radix", 1, 't' },
        { "print-file-name", 0, 'f' }, { "all", 0, 'a' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    bool show_name = false;
    lp_getopt_t g;
    lp_getopt_init_ex(&g, argc, argv, "n:t:fa", lo, LP_GETOPT_NEGNUM);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'n': minlen = strtol(g.arg, NULL, 10); break;
        case 't': radix = g.arg[0]; break;
        case 'f': show_name = true; break;
        case 'a': break;
        case 'H':
            printf("Usage: strings [OPTION]... [FILE]...\n"
                   "Print the sequences of printable characters in FILEs.\n\n"
                   "  -n, --bytes=MIN            at least MIN characters (default 4)\n"
                   "  -t, --radix=RADIX          print the offset: o, d or x\n"
                   "  -f, --print-file-name      print the name of the file before each string\n"
                   "      --help     display this help and exit\n");
            return 0;
        default: lp_getopt_err("strings", &g); return 1;
        }
    }
    if (minlen < 1) minlen = 1;

    /* strings -5 is the old spelling and still in scripts. */
    for (int i = 1; i < argc; i++)
        if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9')
            minlen = strtol(argv[i] + 1, NULL, 10);

    if (g.ind >= argc) { scan(STDIN_FILENO, "-", false); return 0; }

    int rc = 0;
    int files = argc - g.ind;
    for (int i = g.ind; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') continue;
        long f = lp_open(argv[i], O_RDONLY, 0);
        if (f < 0) {
            /* strings is binutils, not coreutils, and binutils says
             * "No such file" where coreutils says "No such file or
             * directory". Matching the command people actually have
             * means matching binutils here. */
            if (lp_voice() == LP_VOICE_GNU)
                dprintf(STDERR_FILENO, "strings: '%s': %s\n", argv[i],
                        (int)-f == 2 ? "No such file" : lp_strerror((int)-f));
            else
                lp_diag("strings", NULL, NULL, "cannot open", argv[i], (int)-f);
            rc = 1;
            continue;
        }
        scan((int)f, argv[i], show_name || files > 1);
        lp_close((int)f);
    }
    return rc;
}
