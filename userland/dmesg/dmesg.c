/* dmesg - the kernel's own log.
 *
 *   dmesg [-n]      -n leaves the priority and timestamp on
 *
 * The console only shows warnings and worse - the command line carries
 * loglevel=4, so that a screen someone is typing at does not have kernel
 * chatter landing in the middle of it. Everything else still goes into
 * the ring buffer, and this is how to read it.
 *
 * /dev/kmsg hands over one record per read, formatted as
 *
 *   <priority>,<sequence>,<microseconds>,<flags>;<the message>
 *
 * Read normally it blocks at the end waiting for the next message, which
 * is what logd wants and not what we want - so we open it non-blocking
 * and stop when the reads run dry.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    bool raw = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) raw = true;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: dmesg [-n]\n");
            printf("  -n  keep the priority and timestamp on each line\n");
            printf("\nThe console shows warnings and worse; everything is\n");
            printf("here, and logd keeps a copy in /data/log/messages.\n");
            return 0;
        }
    }

    long fd = lp_open("/dev/kmsg", O_RDONLY | O_NONBLOCK, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "dmesg: cannot open /dev/kmsg (%ld)\n", -fd);
        return 1;
    }

    page_begin();

    char rec[8192];
    for (;;) {
        long n = lp_read((int)fd, rec, sizeof(rec) - 1);
        if (n <= 0)
            break;                  /* EAGAIN: we have caught up */
        rec[n] = '\0';

        char *text = rec;
        if (!raw) {
            /* Cut everything up to the first ; - that is the header. */
            char *semi = strchr(rec, ';');
            if (semi)
                text = semi + 1;
        }

        char *nl = strchr(text, '\n');
        if (nl) *nl = '\0';

        if (!page_line(text))
            break;
    }

    page_end();
    lp_close((int)fd);
    return 0;
}
