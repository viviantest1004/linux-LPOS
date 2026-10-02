/* rev - reverse each line.
 *
 *   rev [file...]
 *
 * Characters, not bytes: reversing UTF-8 a byte at a time turns every
 * Hangul character into three broken ones, and this machine's console
 * speaks UTF-8.
 *
 * The line is read with readrec rather than readline, so a last line
 * with no newline stays that way in the output. readline cannot tell
 * the two apart, and `printf abc | rev` growing a newline out of
 * nowhere is the kind of difference that breaks a pipeline much later.
 *
 * rev is util-linux, not coreutils, and its diagnostics do not quote the
 * file name. That is not a slip here; it is what the installed one says.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

/* util-linux's shape: "rev: cannot open nosuch: No such file or
 * directory", with no quotes. lp_diag has GNU's three shapes and this
 * is not one of them, so the voice switch is applied here instead. */
static void oops(const char *what, const char *path, int err)
{
    const char *msg = lp_strerror(err);
    if (lp_voice() == LP_VOICE_GNU && msg)
        dprintf(STDERR_FILENO, "rev: %s %s: %s\n", what, path, msg);
    else
        dprintf(STDERR_FILENO, "rev: %s: %s (%d)\n", path, what, err);
}

static void rev_line(const char *line, size_t len, bool nl)
{
    char out[8192];
    size_t o = sizeof out;

    size_t i = 0;
    while (i < len) {
        int used = 0;
        utf8_decode(line + i, len - i, &used);
        if (used <= 0) used = 1;
        if ((size_t)used > len - i) used = (int)(len - i);
        if (o < (size_t)used) break;     /* line longer than the buffer */
        o -= (size_t)used;
        memcpy(out + o, line + i, (size_t)used);
        i += (size_t)used;
    }
    lp_write(STDOUT_FILENO, out + o, sizeof out - o);
    if (nl) lp_write(STDOUT_FILENO, "\n", 1);
}

static int do_fd(int fd)
{
    char line[8192];
    bool nl;
    for (long n; (n = readrec(fd, line, sizeof line, '\n', &nl)) >= 0; )
        rev_line(line, (size_t)n, nl);
    return 0;
}

static void usage(int fd)
{
    dprintf(fd, "Usage:\n"
                " rev [option] [file]...\n\n"
                "Reverse lines characterwise.\n\n"
                "Options:\n"
                " -h, --help     display this help\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = { { "help", 0, 'h' }, { 0, 0, 0 } };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "h", lo);
    for (int c; (c = lp_getopt(&g)) != -1; )
        switch (c) {
        case 'h': usage(STDOUT_FILENO); return 0;
        default:  lp_getopt_err("rev", &g); return 1;
        }

    if (g.ind >= argc) return do_fd(STDIN_FILENO);

    int rc = 0;
    for (int i = g.ind; i < argc; i++) {
        long fd = lp_open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            oops("cannot open", argv[i], (int)-fd);
            rc = 1;
            continue;
        }
        do_fd((int)fd);
        lp_close((int)fd);
    }
    return rc;
}
