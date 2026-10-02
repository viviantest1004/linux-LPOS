/* more - show a file a screenful at a time.
 *
 * On a screen attached to this board there is no scrollback at all. The
 * kernel used to keep one for the framebuffer console and that was
 * removed years ago, so what scrolls off the top is gone - Shift-PageUp
 * does nothing, there is nothing left to show. A pager is not a
 * convenience here; without one, any output longer than the screen
 * cannot be read at all.
 *
 *   more <file>...      page through files
 *   <command> | more    page through whatever is piped in
 *
 * Keys: space for the next screen, enter for one more line, q to stop.
 *
 * Piped onward or redirected to a file it copies straight through, so
 * "more x > y" behaves like cat and nothing waits for a keypress that is
 * never coming.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

/* Long lines still count as one line here. Wrapping them properly means
 * knowing the display width of every character - and we do know it, but
 * one wrapped line pushing the screen out by a row is a smaller problem
 * than the complexity of getting it exactly right. */
static bool page_fd(int fd)
{
    char line[4096];

    for (;;) {
        long n = readline(fd, line, sizeof(line));
        if (n < 0)
            return true;            /* end of this file */
        if (!page_line(line))
            return false;           /* the user pressed q */
    }
}

int main(int argc, char **argv)
{
    int rc = 0;
    bool go_on = true;

    page_begin();

    if (argc == 1) {
        go_on = page_fd(STDIN_FILENO);
    } else {
        for (int i = 1; i < argc && go_on; i++) {
            if (strcmp(argv[i], "-h") == 0) {
                printf("usage: more [file]...\n");
                printf("  space  next screen\n");
                printf("  enter  one more line\n");
                printf("  q      stop\n");
                page_end();
                return 0;
            }

            long fd = lp_open(argv[i], O_RDONLY, 0);
            if (fd < 0) {
                dprintf(STDERR_FILENO, "more: %s: cannot open (%ld)\n",
                        argv[i], -fd);
                rc = 1;
                continue;
            }

            /* With several files, say which one we are in. */
            if (argc > 2) {
                char hdr[280];
                snprintf(hdr, sizeof(hdr), "==> %s <==", argv[i]);
                if (!page_line(hdr)) { go_on = false; }
            }

            if (go_on)
                go_on = page_fd((int)fd);
            lp_close((int)fd);
        }
    }

    page_end();
    return rc;
}
