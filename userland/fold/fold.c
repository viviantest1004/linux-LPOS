/* fold - break long lines so they fit.
 *
 *   fold -w 72 FILE      break at 72 columns
 *   fold -s FILE         break at a space rather than mid-word
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static long width = 80;
static bool at_space = false, count_bytes = false;

static void fold_fd(int fd)
{
    char line[65536];
    while (readrec(fd, line, sizeof line, '\n', NULL) >= 0) {
        size_t len = strlen(line), start = 0;
        while (len - start > (size_t)width) {
            size_t take = (size_t)width;
            if (at_space) {
                size_t back = take;
                while (back > 0 && line[start + back - 1] != ' ' &&
                                   line[start + back - 1] != '\t') back--;
                if (back > 0) take = back;
            }
            lp_write(STDOUT_FILENO, line + start, take);
            lp_write(STDOUT_FILENO, "\n", 1);
            start += take;
        }
        lp_write(STDOUT_FILENO, line + start, len - start);
        lp_write(STDOUT_FILENO, "\n", 1);
    }
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "bytes", 0, 'b' }, { "spaces", 0, 's' }, { "width", 1, 'w' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init_ex(&g, argc, argv, "bsw:", lo, LP_GETOPT_NEGNUM);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'b': count_bytes = true; break;
        case 's': at_space = true; break;
        case 'w': width = strtol(g.arg, NULL, 10); break;
        case 'H':
            printf("Usage: fold [OPTION]... [FILE]...\n"
                   "Wrap input lines in each FILE, writing to standard output.\n\n"
                   "  -b, --bytes           count bytes rather than columns\n"
                   "  -s, --spaces          break at spaces\n"
                   "  -w, --width=WIDTH     use WIDTH columns instead of 80\n"
                   "      --help     display this help and exit\n");
            return 0;
        default: lp_getopt_err("fold", &g); return 1;
        }
    }
    (void)count_bytes;
    if (width < 1) {
        dprintf(STDERR_FILENO, "fold: invalid number of columns\n");
        return 1;
    }

    if (g.ind >= argc) { fold_fd(STDIN_FILENO); return 0; }
    int rc = 0;
    for (int i = g.ind; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) { fold_fd(STDIN_FILENO); continue; }
        long f = lp_open(argv[i], O_RDONLY, 0);
        if (f < 0) {
            lp_diag("fold", NULL, NULL, "cannot open", argv[i], (int)-f);
            rc = 1; continue;
        }
        fold_fd((int)f);
        lp_close((int)f);
    }
    return rc;
}
