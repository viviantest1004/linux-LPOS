/* yes - print the same thing until stopped.
 *
 *   yes [text...]
 *
 * Its real use is feeding a program that insists on a confirmation:
 *
 *   yes | fsck /dev/sda2
 *
 * It writes in blocks rather than a line at a time, because one write
 * syscall per "y" is enough to keep a Zero 2 W's CPU busy on its own.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-h") == 0) {
        printf("usage: yes [text...]\n");
        printf("  repeats text (or \"y\") until something stops it\n");
        return 0;
    }

    char line[512];
    line[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1) strlcat(line, " ", sizeof line);
        strlcat(line, argv[i], sizeof line);
    }
    if (!line[0]) strlcpy(line, "y", sizeof line);
    strlcat(line, "\n", sizeof line);

    char block[4096];
    size_t len = strlen(line), n = 0;
    while (n + len <= sizeof block) { memcpy(block + n, line, len); n += len; }
    if (n == 0) { n = len; memcpy(block, line, len); }

    for (;;)
        if (lp_write(STDOUT_FILENO, block, n) < 0)
            return 1;                 /* the reader went away */
}
