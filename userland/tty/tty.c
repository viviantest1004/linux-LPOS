/* tty - the name of this terminal.
 *
 * Scripts ask "am I on a terminal" and the exit code answers even when
 * the name cannot be found.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    bool silent = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--silent") == 0 ||
            strcmp(argv[i], "--quiet") == 0) silent = true;
        else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: tty [OPTION]...\n"
                   "Print the file name of the terminal connected to standard input.\n\n"
                   "  -s, --silent, --quiet   print nothing, only return an exit status\n"
                   "      --help     display this help and exit\n");
            return 0;
        }
    }

    if (!lp_isatty(STDIN_FILENO)) {
        if (!silent) printf("not a tty\n");
        return 1;
    }
    if (silent) return 0;

    /* /proc/self/fd/0 is a symlink to the device. */
    char name[256];
    long n = lp_readlink("/proc/self/fd/0", name, sizeof name - 1);
    if (n > 0) { name[n] = '\0'; printf("%s\n", name); return 0; }
    printf("/dev/console\n");
    return 0;
}
