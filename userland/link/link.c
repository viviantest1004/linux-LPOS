/* link - make a second name for the same file, and nothing else.
 *
 * ln does this with options and checks; this is the bare system call,
 * which is what a script wants when it must not be second-guessed.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: link FILE1 FILE2\n"
               "Call the link function to create a link named FILE2"
               " to an existing FILE1.\n\n"
               "      --help     display this help and exit\n");
        return 0;
    }
    if (argc != 3) {
        dprintf(STDERR_FILENO, "link: missing operand\n");
        dprintf(STDERR_FILENO, "Try 'link --help' for more information.\n");
        return 1;
    }
    long r = lp_link(argv[1], argv[2]);
    if (r < 0) {
        dprintf(STDERR_FILENO, "link: cannot create link '%s' to '%s': %s\n",
                argv[2], argv[1], lp_strerror((int)-r));
        return 1;
    }
    return 0;
}
