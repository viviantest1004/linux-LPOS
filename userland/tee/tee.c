/* tee - write to a file and pass it along.
 *
 *   <command> | tee [-a] <file>...
 *
 * For when the output is wanted on the screen and kept at the same time.
 * Redirecting with > gives one or the other; this gives both.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define MAX_FILES 8

int main(int argc, char **argv)
{
    bool append = false;
    int  fds[MAX_FILES];
    int  nfds = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            append = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: <command> | tee [-a] <file>...\n");
            printf("  -a  add to the file instead of replacing it\n");
            return 0;
        } else if (nfds < MAX_FILES) {
            int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
            long fd = lp_open(argv[i], flags, 0644);
            if (fd < 0) {
                dprintf(STDERR_FILENO, "tee: %s: cannot write\n", argv[i]);
                continue;
            }
            fds[nfds++] = (int)fd;
        }
    }

    char buf[8192];
    for (;;) {
        long n = lp_read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0)
            break;
        lp_write(STDOUT_FILENO, buf, (size_t)n);
        for (int i = 0; i < nfds; i++)
            lp_write(fds[i], buf, (size_t)n);
    }

    for (int i = 0; i < nfds; i++)
        lp_close(fds[i]);
    return 0;
}
