/* nproc - how many processors are available.
 *
 * make -j$(nproc) is the reason it exists.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: nproc [OPTION]...\n"
                   "Print the number of processing units available.\n\n"
                   "      --all      print the number of installed processors\n"
                   "      --help     display this help and exit\n");
            return 0;
        }

    char buf[65536];
    long n = proc_read("/proc/cpuinfo", buf, sizeof buf);
    int count = 0;
    if (n > 0) {
        for (char *p = buf; (p = strstr(p, "processor")); p++)
            if (p == buf || p[-1] == '\n') count++;
    }
    if (count == 0) count = 1;
    printf("%d\n", count);
    return 0;
}
