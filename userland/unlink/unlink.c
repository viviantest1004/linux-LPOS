/* unlink - remove one name, and nothing else.
 *
 * rm asks questions and walks directories. This does exactly one
 * unlink() and says what the kernel said.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: unlink FILE\n"
               "Call the unlink function to remove the specified FILE.\n\n"
               "      --help     display this help and exit\n");
        return 0;
    }
    if (argc != 2) {
        dprintf(STDERR_FILENO, "unlink: missing operand\n");
        dprintf(STDERR_FILENO, "Try 'unlink --help' for more information.\n");
        return 1;
    }
    long r = lp_unlink(argv[1]);
    if (r < 0) {
        dprintf(STDERR_FILENO, "unlink: cannot unlink '%s': %s\n",
                argv[1], lp_strerror((int)-r));
        return 1;
    }
    return 0;
}
