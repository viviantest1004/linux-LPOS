/* pwd - where we are.
 *
 * The shell has this built in; this is the one in /bin, which is what
 * `env pwd` and a script with an explicit path get.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    bool logical = true;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-P") == 0 || strcmp(argv[i], "--physical") == 0)
            logical = false;
        else if (strcmp(argv[i], "-L") == 0 || strcmp(argv[i], "--logical") == 0)
            logical = true;
        else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: pwd [OPTION]...\n"
                   "Print the full filename of the current working directory.\n\n"
                   "  -L, --logical    use PWD from environment, even if it contains symlinks\n"
                   "  -P, --physical   avoid all symlinks\n"
                   "      --help     display this help and exit\n");
            return 0;
        }
    }

    if (logical) {
        const char *pwd = getenv("PWD");
        if (pwd && pwd[0] == '/') {
            lp_stat_t a, b;
            if (lp_stat(pwd, &a, true) == 0 && lp_stat(".", &b, true) == 0 &&
                a.ino == b.ino && a.dev == b.dev) {
                printf("%s\n", pwd);
                return 0;
            }
        }
    }

    char buf[4096];
    if (lp_getcwd(buf, sizeof buf) < 0) {
        dprintf(STDERR_FILENO, "pwd: error retrieving current directory\n");
        return 1;
    }
    printf("%s\n", buf);
    return 0;
}
