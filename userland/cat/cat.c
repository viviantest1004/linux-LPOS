/* cat - copy files, or standard input, to standard output.
 *
 *   cat [-AbeEnstTuv] [file]...
 *
 * The common case is a straight byte copy and it stays that way: with no
 * display option set, cat never looks at the bytes it moves, so it is as
 * fast as the pipe underneath it and it cannot corrupt anything it does
 * not understand. The per-byte path only runs when -n, -b, -s, -E, -T or
 * -v asked for it.
 *
 * A directory is not rejected at open time, because open(2) accepts one.
 * GNU finds out on the first read and so do we, which is why the read
 * error carries the file name: "cat: adir: Is a directory" is the line
 * people actually see, and it comes from read, not from open.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define BUF_SIZE 65536

static bool number_all = false;     /* -n */
static bool number_nonblank = false;/* -b, overrides -n */
static bool show_ends = false;      /* -E */
static bool show_tabs = false;      /* -T */
static bool show_nonprint = false;  /* -v */
static bool squeeze = false;        /* -s */

static char  obuf[BUF_SIZE];
static size_t olen = 0;
static int   write_err = 0;

static void flush(void)
{
    size_t off = 0;
    while (off < olen) {
        long w = lp_write(STDOUT_FILENO, obuf + off, olen - off);
        if (w <= 0) { write_err = w < 0 ? (int)-w : 5; olen = 0; return; }
        off += (size_t)w;
    }
    olen = 0;
}

static void put(char c)
{
    if (olen == sizeof obuf) flush();
    obuf[olen++] = c;
}

/* Line numbers are "%6d\t", the width GNU has always used. */
static void put_num(long n)
{
    char tmp[24];
    int  i = 0;
    if (n == 0) tmp[i++] = '0';
    while (n > 0) { tmp[i++] = (char)('0' + n % 10); n /= 10; }
    for (int pad = 6 - i; pad > 0; pad--) put(' ');
    while (i > 0) put(tmp[--i]);
    put('\t');
}

/* -v: everything outside printable ASCII gets a name you can type. */
static void put_visible(unsigned char c)
{
    if (c >= 128) { put('M'); put('-'); c -= 128; }
    if (c < 32)   { put('^'); put((char)(c + 64)); }
    else if (c == 127) { put('^'); put('?'); }
    else put((char)c);
}

static bool plain(void)
{
    return !number_all && !number_nonblank && !show_ends &&
           !show_tabs && !show_nonprint && !squeeze;
}

/* Straight copy: the bytes are never inspected. */
static int copy_raw(int fd, const char *label)
{
    static char buf[BUF_SIZE];
    for (;;) {
        long n = lp_read(fd, buf, sizeof buf);
        if (n == 0) return 0;
        if (n < 0) { lp_diag("cat", NULL, NULL, "cannot read", label, (int)-n); return 1; }
        long off = 0;
        while (off < n) {
            long w = lp_write(STDOUT_FILENO, buf + off, (size_t)(n - off));
            if (w <= 0) { write_err = w < 0 ? (int)-w : 5; return 1; }
            off += w;
        }
    }
}

/* State that has to survive between files: GNU numbers the whole stream,
 * not each file, and a blank line at the end of one file squeezes
 * against a blank line at the start of the next. */
static long lineno = 1;
static bool bol = true;
static long blank_run = 0;

static int copy_slow(int fd, const char *label)
{
    static char buf[BUF_SIZE];
    for (;;) {
        long n = lp_read(fd, buf, sizeof buf);
        if (n == 0) { flush(); return 0; }
        if (n < 0) {
            flush();
            lp_diag("cat", NULL, NULL, "cannot read", label, (int)-n);
            return 1;
        }
        for (long i = 0; i < n; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (bol) {
                if (c == '\n') {
                    if (squeeze && ++blank_run > 1) continue;
                    if (number_all) put_num(lineno++);
                    if (show_ends) put('$');
                    put('\n');
                    continue;
                }
                blank_run = 0;
                if (number_all || number_nonblank) put_num(lineno++);
                bol = false;
            }
            if (c == '\n') { if (show_ends) put('$'); put('\n'); bol = true; continue; }
            if (c == '\t') {
                if (show_tabs) { put('^'); put('I'); }
                else put('\t');
                continue;
            }
            if (show_nonprint) put_visible(c);
            else put((char)c);
        }
        if (write_err) { return 1; }
    }
}

static int copy_fd(int fd, const char *label)
{
    return plain() ? copy_raw(fd, label) : copy_slow(fd, label);
}

static void usage(int fd)
{
    dprintf(fd, "Usage: cat [OPTION]... [FILE]...\n"
                "Concatenate FILE(s) to standard output.\n\n"
                "With no FILE, or when FILE is -, read standard input.\n\n"
                "  -A, --show-all           equivalent to -vET\n"
                "  -b, --number-nonblank    number nonempty output lines, overrides -n\n"
                "  -e                       equivalent to -vE\n"
                "  -E, --show-ends          display $ at end of each line\n"
                "  -n, --number             number all output lines\n"
                "  -s, --squeeze-blank      suppress repeated empty output lines\n"
                "  -t                       equivalent to -vT\n"
                "  -T, --show-tabs          display TAB characters as ^I\n"
                "  -u                       (ignored)\n"
                "  -v, --show-nonprinting   use ^ and M- notation, except for LFD and TAB\n"
                "      --help        display this help and exit\n\n"
                "Examples:\n"
                "  cat f - g  Output f's contents, then standard input, then g's contents.\n"
                "  cat        Copy standard input to standard output.\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "show-all", 0, 'A' }, { "number-nonblank", 0, 'b' },
        { "show-ends", 0, 'E' }, { "number", 0, 'n' },
        { "squeeze-blank", 0, 's' }, { "show-tabs", 0, 'T' },
        { "show-nonprinting", 0, 'v' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "AbeEnstTuv", lo);
    for (int c; (c = lp_getopt(&g)) != -1; )
        switch (c) {
        case 'A': show_nonprint = show_ends = show_tabs = true; break;
        case 'b': number_nonblank = true; number_all = false; break;
        case 'e': show_nonprint = show_ends = true; break;
        case 'E': show_ends = true; break;
        case 'n': if (!number_nonblank) number_all = true; break;
        case 's': squeeze = true; break;
        case 't': show_nonprint = show_tabs = true; break;
        case 'T': show_tabs = true; break;
        case 'u': break;                         /* GNU ignores it too */
        case 'v': show_nonprint = true; break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default:  lp_getopt_err("cat", &g); return 1;
        }

    int rc = 0;
    if (g.ind >= argc) {
        /* `cat` alone reads what you type, which at a prompt looks
         * exactly like a machine that has stopped answering. GNU says
         * nothing, so under its voice neither do we. */
        if (lp_voice() == LP_VOICE_LP && lp_isatty(STDIN_FILENO))
            dprintf(STDERR_FILENO,
                    "cat: reading what you type. Ctrl-D ends it,"
                    " Ctrl-C cancels.\n");
        rc = copy_fd(STDIN_FILENO, "-");
    }
    for (int i = g.ind; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            rc |= copy_fd(STDIN_FILENO, "-");
            continue;
        }
        long fd = lp_open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            lp_diag("cat", NULL, NULL, "cannot open", argv[i], (int)-fd);
            rc = 1;
            continue;
        }
        rc |= copy_fd((int)fd, argv[i]);
        lp_close((int)fd);
    }
    flush();
    if (write_err) {
        lp_diag("cat", NULL, NULL, "cannot write", "write error", write_err);
        rc = 1;
    }
    return rc;
}
