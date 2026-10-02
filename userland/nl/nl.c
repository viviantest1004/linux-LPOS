/* nl - number the lines.
 *
 *   nl [-b a] [-w n] [-s sep] [file...]
 *
 *   -b a  number every line, blank ones included (default: only
 *         non-blank, which is what nl has always done)
 *   -w    how wide the number column is (default 6)
 *   -s    what goes between the number and the line (default a tab)
 *
 * `cat -n` does not exist here, so this is how you find out which line
 * an error message meant.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static bool all_lines = false;
static int  width = 6;
static const char *sep = "\t";
static long counter = 1;

static void do_fd(int fd)
{
    char line[8192];
    while (readline(fd, line, sizeof line) >= 0) {
        if (!all_lines && !line[0]) { printf("\n"); continue; }
        printf("%*ld%s%s\n", width, counter++, sep, line);
    }
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-ba") == 0) all_lines = true;
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
            all_lines = (argv[++i][0] == 'a');
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
            if (width < 1 || width > 20) width = 6;
        }
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) sep = argv[++i];
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: nl [-b a] [-w n] [-s sep] [file...]\n");
            printf("  -b a  number blank lines too\n");
            return 0;
        }
        else break;
    }

    if (i >= argc) { do_fd(STDIN_FILENO); return 0; }

    int rc = 0;
    for (; i < argc; i++) {
        long fd = lp_open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "nl: %s: cannot open\n", argv[i]);
            rc = 1;
            continue;
        }
        do_fd((int)fd);
        lp_close((int)fd);
    }
    return rc;
}
