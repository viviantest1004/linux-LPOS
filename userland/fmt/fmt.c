/* fmt - reflow paragraphs to a width.
 *
 *   fmt -w 72 notes.txt
 *   fmt -s log.txt          only break long lines; never join short ones
 *
 * fold cuts at a column, wherever that lands. fmt moves whole words and
 * keeps paragraphs apart, which is what you want for prose and what
 * makes a commit message or a comment block readable.
 *
 * Indentation is taken from the first line of each paragraph and kept
 * on every line of it, so a bullet list survives being reflowed.
 *
 * One difference from GNU worth knowing about: GNU fmt does not fill
 * greedily. It runs a small optimiser over the whole paragraph that
 * weighs how ragged the right edge is, whether a break leaves one word
 * of a sentence alone on a line, and half a dozen other things, and it
 * sometimes ends a line early to make the next two look better. This
 * fills greedily - a word goes on the current line if it fits. The
 * words and the width are the same; the exact break points are not
 * always. `fmt -s`, which only splits and never joins, is identical.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define LINE 8192

static long width = 75;            /* GNU's default */
static bool split_only = false;    /* -s */
static bool uniform = false;       /* -u */
static const char *prefix = NULL;  /* -p */

static char out[LINE * 4];
static size_t outn = 0;
static long   col = 0;
static char   indent[LINE];
static bool   line_open = false;

static void flush_line(void)
{
    if (!line_open) return;
    out[outn++] = '\n';
    lp_write(STDOUT_FILENO, out, outn);
    outn = 0;
    col = 0;
    line_open = false;
}

static void put_word(const char *w, size_t n, bool sentence_end)
{
    if (!line_open) {
        size_t ilen = strlen(indent);
        memcpy(out + outn, indent, ilen);
        outn += ilen;
        col = (long)ilen;
        line_open = true;
    } else {
        /* One space, and two after a full stop only when -u asks for
         * it. That is the way round GNU has it. */
        long gap = (sentence_end && uniform) ? 2 : 1;
        if (col + gap + (long)n > width) {
            flush_line();
            size_t ilen = strlen(indent);
            memcpy(out + outn, indent, ilen);
            outn += ilen;
            col = (long)ilen;
            line_open = true;
        } else {
            for (long i = 0; i < gap; i++) out[outn++] = ' ';
            col += gap;
        }
    }
    memcpy(out + outn, w, n);
    outn += n;
    col += (long)n;
}

static bool ends_sentence(const char *w, size_t n)
{
    while (n && (w[n-1] == '"' || w[n-1] == '\'' || w[n-1] == ')' ||
                 w[n-1] == ']')) n--;
    return n && (w[n-1] == '.' || w[n-1] == '!' || w[n-1] == '?');
}

static void fmt_fd(int fd)
{
    char line[LINE];
    bool have_para = false;
    bool after_sentence = false;

    while (readrec(fd, line, sizeof line, '\n', NULL) >= 0) {
        /* A blank line ends a paragraph and is printed as itself. */
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) {
            flush_line();
            printf("\n");
            have_para = false;
            after_sentence = false;
            continue;
        }

        if (!have_para) {
            size_t ilen = (size_t)(p - line);
            if (prefix) ilen = 0;
            if (ilen >= sizeof indent) ilen = sizeof indent - 1;
            memcpy(indent, line, ilen);
            indent[ilen] = '\0';
            have_para = true;
        } else if (split_only) {
            /* -s never joins, so each input line starts a new output
             * line however short it is. */
            flush_line();
        }

        const char *q = p;
        while (*q) {
            while (*q == ' ' || *q == '\t') q++;
            if (!*q) break;
            const char *w = q;
            while (*q && *q != ' ' && *q != '\t') q++;
            size_t n = (size_t)(q - w);
            if (outn + n + 64 > sizeof out) flush_line();
            /* The gap before a word is decided by the word before it,
             * so that is what is carried across. */
            put_word(w, n, after_sentence);
            after_sentence = ends_sentence(w, n);
        }
    }
    flush_line();
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "width", 1, 'w' }, { "split-only", 0, 's' },
        { "uniform-spacing", 0, 'u' }, { "prefix", 1, 'p' },
        { "crown-margin", 0, 'c' }, { "tagged-paragraph", 0, 't' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init_ex(&g, argc, argv, "w:sup:ct", lo, LP_GETOPT_NEGNUM);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'w': width = strtol(g.arg, NULL, 10); break;
        case 's': split_only = true; break;
        case 'u': uniform = true; break;
        case 'p': prefix = g.arg; break;
        case 'c': case 't': break;
        case 'H':
            printf("Usage: fmt [-WIDTH] [OPTION]... [FILE]...\n"
                   "Reformat each paragraph in the FILE(s), writing to standard output.\n\n"
                   "  -s, --split-only        split long lines, but do not refill\n"
                   "  -u, --uniform-spacing   one space between words, two after sentences\n"
                   "  -w, --width=WIDTH       maximum line width (default of 75 columns)\n"
                   "  -p, --prefix=STRING     reformat only lines beginning with STRING\n"
                   "      --help     display this help and exit\n");
            return 0;
        default: lp_getopt_err("fmt", &g); return 1;
        }
    }
    /* fmt -72 is the old spelling and still in every set of notes. */
    for (int i = 1; i < argc; i++)
        if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9')
            width = strtol(argv[i] + 1, NULL, 10);
    if (width < 1) width = 75;

    if (g.ind >= argc) { fmt_fd(STDIN_FILENO); return 0; }
    int rc = 0;
    for (int i = g.ind; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') continue;
        if (strcmp(argv[i], "-") == 0) { fmt_fd(STDIN_FILENO); continue; }
        long f = lp_open(argv[i], O_RDONLY, 0);
        if (f < 0) {
            lp_diag("fmt", "cannot open", "for reading",
                    "cannot open", argv[i], (int)-f);
            rc = 1;
            continue;
        }
        fmt_fd((int)f);
        lp_close((int)f);
    }
    return rc;
}
