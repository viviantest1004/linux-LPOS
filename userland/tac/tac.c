/* tac - cat backwards, last line first.
 *
 * Reading a log is the reason it exists: the newest lines are at the
 * bottom and the question is almost always about the newest lines.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static char  *buf;
static size_t len, cap;
static char   sep = '\n';
static bool   before = false;      /* -b: the separator leads the line */

static bool slurp(int fd)
{
    for (;;) {
        if (len + 65536 > cap) {
            size_t n = cap ? cap * 2 : 131072;
            while (len + 65536 > n) n *= 2;
            char *nb = realloc(buf, n);
            if (!nb) return false;
            buf = nb; cap = n;
        }
        long n = lp_read(fd, buf + len, 65536);
        if (n <= 0) break;
        len += (size_t)n;
    }
    return true;
}

static void out(const char *p, size_t n)
{
    while (n) {
        long w = lp_write(STDOUT_FILENO, p, n);
        if (w <= 0) return;
        p += w; n -= (size_t)w;
    }
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "before", 0, 'b' }, { "separator", 1, 's' },
        { "regex", 0, 'r' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "bs:r", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'b': before = true; break;
        case 's': sep = g.arg[0]; break;
        case 'r': break;
        case 'H':
            printf("Usage: tac [OPTION]... [FILE]...\n"
                   "Write each FILE to standard output, last line first.\n\n"
                   "  -b, --before             attach the separator before instead of after\n"
                   "  -s, --separator=STRING   use STRING as the separator instead of newline\n"
                   "      --help     display this help and exit\n");
            return 0;
        default: lp_getopt_err("tac", &g); return 1;
        }
    }

    int files = argc - g.ind;
    int rc = 0;
    for (int i = 0; i < (files ? files : 1); i++) {
        int fd = STDIN_FILENO;
        const char *name = "-";
        if (files) {
            name = argv[g.ind + i];
            if (strcmp(name, "-") != 0) {
                long f = lp_open(name, O_RDONLY, 0);
                if (f < 0) {
                    lp_diag("tac", NULL, NULL, "cannot open", name, (int)-f);
                    rc = 1;
                    continue;
                }
                fd = (int)f;
            }
        }
        len = 0;
        if (!slurp(fd)) { rc = 1; }
        if (fd != STDIN_FILENO) lp_close(fd);
        if (len == 0) continue;

        /* Where each record starts. Without -b a record ends after its
         * separator; with -b it begins with one. Both are one pass. */
        size_t scap = 1024, nst = 0;
        size_t *st = malloc(sizeof(size_t) * scap);
        if (!st) { rc = 1; continue; }
        st[nst++] = 0;
        for (size_t k = 0; k < len; k++) {
            if (buf[k] != sep) continue;
            size_t begin = before ? k : k + 1;
            if (begin == 0 || begin >= len + 1) continue;
            if (nst == scap) {
                size_t *n2 = realloc(st, sizeof(size_t) * scap * 2);
                if (!n2) break;
                st = n2; scap *= 2;
            }
            if (begin != st[nst - 1]) st[nst++] = begin;
        }

        for (size_t k = nst; k-- > 0; ) {
            size_t a = st[k];
            size_t b = (k + 1 < nst) ? st[k + 1] : len;
            if (b > a) out(buf + a, b - a);
        }
        free(st);
    }
    free(buf);
    return rc;
}
