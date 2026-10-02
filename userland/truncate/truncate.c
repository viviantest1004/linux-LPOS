/* truncate - make a file exactly this big.
 *
 *   truncate -s 0 log        empty it without deleting it
 *   truncate -s 1G sparse    a big file that occupies nothing yet
 *   truncate -s +1M f        a megabyte longer
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static const char *prog = "truncate";

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "size", 1, 's' }, { "no-create", 0, 'c' },
        { "reference", 1, 'r' }, { "io-blocks", 0, 'o' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    const char *size = NULL, *ref = NULL;
    bool no_create = false;

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "s:cr:o", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 's': size = g.arg; break;
        case 'c': no_create = true; break;
        case 'r': ref = g.arg; break;
        case 'o': break;
        case 'H':
            printf("Usage: truncate OPTION... FILE...\n"
                   "Shrink or extend the size of each FILE to the specified size.\n\n"
                   "  -c, --no-create        do not create any files\n"
                   "  -r, --reference=RFILE  base size on RFILE\n"
                   "  -s, --size=SIZE        set or adjust the file size by SIZE bytes\n"
                   "      --help     display this help and exit\n\n"
                   "SIZE may be prefixed by + or - to adjust, and suffixed by K, M, G.\n");
            return 0;
        default: lp_getopt_err(prog, &g); return 1;
        }
    }
    if (!size && !ref) {
        dprintf(STDERR_FILENO, "%s: you must specify either '--size' or '--reference'\n", prog);
        dprintf(STDERR_FILENO, "Try '%s --help' for more information.\n", prog);
        return 1;
    }
    if (g.ind >= argc) {
        dprintf(STDERR_FILENO, "%s: missing file operand\n", prog);
        dprintf(STDERR_FILENO, "Try '%s --help' for more information.\n", prog);
        return 1;
    }

    s64 base = 0;
    if (ref) {
        lp_stat_t st;
        if (lp_stat(ref, &st, true) < 0) {
            lp_diag(prog, "cannot stat", NULL, "cannot stat", ref, 2);
            return 1;
        }
        base = (s64)st.size;
    }

    int  adjust = 0;              /* 0 set, +1 grow, -1 shrink */
    s64  amount = base;
    if (size) {
        const char *p = size;
        if (*p == '+') { adjust = 1; p++; }
        else if (*p == '-') { adjust = -1; p++; }
        char *end;
        /* strtoll, so `truncate -s 5G` still means 5G on a 32-bit
         * machine rather than whatever is left of it. */
        amount = strtoll(p, &end, 10);
        switch (*end) {
        case 'K': case 'k': amount *= 1024; break;
        case 'M': case 'm': amount *= 1048576; break;
        case 'G': case 'g': amount *= 1073741824LL; break;
        case '\0': break;
        default:
            dprintf(STDERR_FILENO, "%s: invalid number: '%s'\n", prog, size);
            return 1;
        }
        if (ref && adjust == 0) amount = base;
        else if (ref) amount = base + adjust * amount;
    }

    int rc = 0;
    for (int i = g.ind; i < argc; i++) {
        int flags = O_WRONLY | (no_create ? 0 : O_CREAT);
        long fd = lp_open(argv[i], flags, 0644);
        if (fd < 0) {
            if (no_create && !lp_exists(argv[i])) continue;
            lp_diag(prog, "cannot open", "for writing", "cannot open",
                    argv[i], (int)-fd);
            rc = 1;
            continue;
        }
        s64 want = amount;
        if (!ref && adjust) {
            lp_stat_t st;
            s64 cur = (lp_stat(argv[i], &st, true) == 0) ? (s64)st.size : 0;
            want = cur + adjust * amount;
            if (want < 0) want = 0;
        }
        if (lp_ftruncate((int)fd, want) < 0) {
            dprintf(STDERR_FILENO, "%s: failed to truncate '%s'\n", prog, argv[i]);
            rc = 1;
        }
        lp_close((int)fd);
    }
    return rc;
}
