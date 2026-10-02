/* sync - write everything still sitting in memory out to the disk.
 *
 * On a machine that can lose power at any moment this is the difference
 * between a file that exists and one that nearly did.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: sync [OPTION] [FILE]...\n"
                   "Synchronize cached writes to persistent storage.\n\n"
                   "      --help     display this help and exit\n");
            return 0;
        }
    lp_sync();
    return 0;
}
