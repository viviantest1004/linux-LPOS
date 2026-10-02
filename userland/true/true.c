/* true - succeed, and do nothing else.
 *
 * The shell has it built in. This is the one in /bin, which is what
 * `xargs true`, a crontab line and `env true` get.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0)
        printf("Usage: true [ignored command line arguments]\n"
               "Exit with a status code indicating success.\n");
    return 0;
}
