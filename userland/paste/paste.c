/* paste - lines from several files, side by side.
 *
 *   paste a b            line 1 of a, a tab, line 1 of b
 *   paste -d, a b        with a comma instead
 *   paste -s a           one file's lines joined onto one line
 *
 * The other half of cut: cut takes columns apart, paste puts them back.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define MAX_FILES 64

static const char *delims = "\t";
static char  dbuf[256];
static int   ndelim = 1;
static bool  serial = false;
static char  eol = '\n';

/* -d takes escapes: \t, \n, \\ and \0 (no delimiter at all). */
static void set_delims(const char *s)
{
    int n = 0;
    for (const char *p = s; *p && n < (int)sizeof dbuf - 1; p++) {
        if (*p != '\\') { dbuf[n++] = *p; continue; }
        p++;
        switch (*p) {
        case 'n': dbuf[n++] = '\n'; break;
        case 't': dbuf[n++] = '\t'; break;
        case '0': dbuf[n++] = '\0'; break;
        case '\\': dbuf[n++] = '\\'; break;
        case '\0': dbuf[n++] = '\\'; p--; break;
        default: dbuf[n++] = *p; break;
        }
    }
    dbuf[n] = '\0';
    delims = dbuf;
    ndelim = n ? n : 1;
    if (!n) { dbuf[0] = '\t'; dbuf[1] = '\0'; ndelim = 1; }
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "delimiters", 1, 'd' }, { "serial", 0, 's' },
        { "zero-terminated", 0, 'z' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "d:sz", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'd': set_delims(g.arg); break;
        case 's': serial = true; break;
        case 'z': eol = '\0'; break;
        case 'H':
            printf("Usage: paste [OPTION]... [FILE]...\n"
                   "Write lines consisting of the sequentially corresponding lines from\n"
                   "each FILE, separated by TABs, to standard output.\n\n"
                   "  -d, --delimiters=LIST   reuse characters from LIST instead of TABs\n"
                   "  -s, --serial            paste one file at a time instead of in parallel\n"
                   "      --help     display this help and exit\n");
            return 0;
        default: lp_getopt_err("paste", &g); return 1;
        }
    }

    int nf = argc - g.ind;
    if (nf == 0) { nf = 1; }
    if (nf > MAX_FILES) nf = MAX_FILES;

    int fds[MAX_FILES];
    for (int i = 0; i < nf; i++) {
        const char *name = (argc - g.ind) ? argv[g.ind + i] : "-";
        if (strcmp(name, "-") == 0) { fds[i] = STDIN_FILENO; continue; }
        long f = lp_open(name, O_RDONLY, 0);
        if (f < 0) {
            lp_diag("paste", NULL, NULL, "cannot open", name, (int)-f);
            return 1;
        }
        fds[i] = (int)f;
    }

    static char line[65536];

    if (serial) {
        for (int i = 0; i < nf; i++) {
            bool first = true;
            int  d = 0;
            while (readrec(fds[i], line, sizeof line, '\n', NULL) >= 0) {
                if (!first) { lp_write(STDOUT_FILENO, &delims[d], 1);
                              d = (d + 1) % ndelim; }
                lp_write(STDOUT_FILENO, line, strlen(line));
                first = false;
            }
            if (!first) lp_write(STDOUT_FILENO, &eol, 1);
        }
    } else {
        bool alive[MAX_FILES];
        int  left = nf;
        for (int i = 0; i < nf; i++) alive[i] = true;

        while (left > 0) {
            int d = 0;
            bool any = false;
            char out[65536];
            size_t k = 0;
            for (int i = 0; i < nf; i++) {
                if (i) { out[k++] = delims[d]; d = (d + 1) % ndelim; }
                if (!alive[i]) continue;
                if (readrec(fds[i], line, sizeof line, '\n', NULL) < 0) {
                    alive[i] = false;
                    left--;
                    continue;
                }
                any = true;
                size_t n = strlen(line);
                if (k + n < sizeof out - 2) { memcpy(out + k, line, n); k += n; }
            }
            if (!any) break;
            out[k++] = eol;
            lp_write(STDOUT_FILENO, out, k);
        }
    }

    for (int i = 0; i < nf; i++)
        if (fds[i] != STDIN_FILENO) lp_close(fds[i]);
    return 0;
}
