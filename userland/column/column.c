/* column - put a list into columns, or a file into a table.
 *
 *   ls | column                 fill the screen with the names
 *   column -t /etc/fstab        line the fields up under each other
 *   column -t -s: /etc/passwd   when the fields are not separated by spaces
 *
 * Two different jobs share one name here, and they behave differently
 * on purpose:
 *
 * Without -t the input is a list of one-line items and the output is
 * that list poured down the columns of the screen - down first, then
 * across, unless -x says to fill rows first. The items are separated
 * with tab characters, not spaces, which is what the original column
 * did and what makes the output paste back into a terminal at the same
 * width. That is why the column stride is rounded up to a multiple of
 * eight: a tab stop is eight columns, and a stride that is not a
 * multiple of eight cannot be reached with tabs.
 *
 * With -t each line is a row and the output is padded with spaces so
 * every column starts at the same place. Every column but the last is
 * padded, including the empty cells of a short row, so a table with
 * ragged rows still lines up - and yes, that leaves trailing spaces on
 * those rows, exactly as util-linux does.
 *
 * Widths are counted in screen columns, not bytes: utf8_str_width knows
 * that 이름 is four columns wide and six bytes long. Ubuntu's column
 * gets this right only in a UTF-8 locale - in the C locale it escapes
 * every byte over 127 into \xNN and the table comes out mangled - so
 * this matches what a person actually sees there, not what the C locale
 * does to them.
 *
 * The options are the five that people type. util-linux has fifteen
 * more, for JSON output, tree drawing, named and hidden and wrapped
 * columns; none of them are here, and asking for one says so rather
 * than quietly ignoring it.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define TAB_LEN  8

static const char *prog = "column";

static bool  opt_table, opt_fillrows;
static const char *opt_sep = NULL;          /* -s: the delimiter set */
static const char *opt_outsep = "  ";       /* -o */
static long  opt_width = 0;                 /* -c, 0 means "ask the terminal" */

/* The whole input, every file end to end, exactly as column reads it. */
static char  *input;
static size_t inlen, incap;

/* The lines of it, then the fields of each line. */
static char **ents;
static int    nents, entcap;

typedef struct { char **f; int n; } row_t;
static row_t *rows;
static int    nrows, rowcap;

static int    ncols;
static size_t *colw;

static bool grow(void **p, int *cap, int need, size_t elem)
{
    if (need <= *cap)
        return true;
    int ncap = *cap ? *cap * 2 : 64;
    while (ncap < need) ncap *= 2;
    void *n = realloc(*p, (size_t)ncap * elem);
    if (!n)
        return false;
    *p = n;
    *cap = ncap;
    return true;
}

/* Read one whole file onto the end of the input buffer. Reading is done
 * here rather than line by line so that a read error - a directory, for
 * instance - is reported the way column reports it. */
static bool slurp(int fd)
{
    for (;;) {
        if (inlen + 65536 > incap) {
            size_t ncap = incap ? incap * 2 : 262144;
            while (inlen + 65536 > ncap) ncap *= 2;
            char *n = realloc(input, ncap);
            if (!n)
                return false;
            input = n;
            incap = ncap;
        }
        long n = lp_read(fd, input + inlen, 65536);
        if (n == 0)
            return true;
        if (n < 0) {
            dprintf(STDERR_FILENO, "%s: read failed: %s\n", prog,
                    lp_strerror((int)-n));
            return false;
        }
        inlen += (size_t)n;
    }
}

static bool add_ent(char *s)
{
    if (!grow((void **)&ents, &entcap, nents + 1, sizeof *ents))
        return false;
    ents[nents++] = s;
    return true;
}

/* Split the input into lines, in place. Empty lines are dropped, which
 * is what column does without --keep-empty-lines. */
static bool split_lines(void)
{
    char *p = input;
    char *end = input + inlen;
    while (p < end) {
        char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        *nl = '\0';
        if (*p && !add_ent(p))
            return false;
        p = nl + 1;
    }
    return true;
}

/* One line into its fields, in place. Without -s the separator is a run
 * of blanks and leading blanks do not make an empty first field; with
 * -s every single delimiter separates, so a::b really is three fields
 * and the middle one is empty. */
static bool split_fields(char *line)
{
    char **f = NULL;
    int    n = 0, cap = 0;

    if (!opt_sep) {
        char *p = line;
        for (;;) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            if (!grow((void **)&f, &cap, n + 1, sizeof *f)) return false;
            f[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
    } else {
        char *p = line;
        for (;;) {
            if (!grow((void **)&f, &cap, n + 1, sizeof *f)) return false;
            f[n++] = p;
            while (*p && !strchr(opt_sep, *p)) p++;
            if (!*p) break;
            *p++ = '\0';
        }
    }

    if (!grow((void **)&rows, &rowcap, nrows + 1, sizeof *rows))
        return false;
    rows[nrows].f = f;
    rows[nrows].n = n;
    nrows++;
    if (n > ncols) ncols = n;
    return true;
}

static void table(void)
{
    colw = calloc((size_t)ncols, sizeof *colw);
    if (!colw)
        return;
    for (int r = 0; r < nrows; r++)
        for (int c = 0; c < rows[r].n; c++) {
            size_t w = utf8_str_width(rows[r].f[c], strlen(rows[r].f[c]));
            if (w > colw[c]) colw[c] = w;
        }

    for (int r = 0; r < nrows; r++) {
        for (int c = 0; c < ncols; c++) {
            const char *cell = c < rows[r].n ? rows[r].f[c] : "";
            fputs(cell, STDOUT_FILENO);
            if (c == ncols - 1)
                break;
            size_t w = utf8_str_width(cell, strlen(cell));
            if (colw[c] > w)
                printf("%*s", (int)(colw[c] - w), "");
            fputs(opt_outsep, STDOUT_FILENO);
        }
        printf("\n");
    }
}

/* How wide the output may be. -c wins; then the terminal itself, if
 * this is going to one; then $COLUMNS, which is how a pipe is told; and
 * 80 when nothing knows. */
static long term_width(void)
{
    if (opt_width > 0)
        return opt_width;
    if (opt_width < 0)
        return 0;

    int rows_unused, cols;
    if (lp_term_size(STDOUT_FILENO, &rows_unused, &cols) == 0 && cols > 0)
        return cols;
    const char *env = getenv("COLUMNS");
    if (env && *env) {
        long v = strtol(env, NULL, 10);
        if (v > 0)
            return v;
    }
    return 80;
}

/* The list, poured into columns. Padding is tabs, so the stride has to
 * be a multiple of the tab stop; that rounding is where the extra
 * space between columns comes from. */
static void columnate(void)
{
    size_t maxlen = 0;
    for (int i = 0; i < nents; i++) {
        size_t w = utf8_str_width(ents[i], strlen(ents[i]));
        if (w > maxlen) maxlen = w;
    }
    size_t stride = (maxlen + TAB_LEN) & ~(size_t)(TAB_LEN - 1);
    long   width  = term_width();
    int    numcols = (int)((size_t)width / stride);
    if (numcols < 1) numcols = 1;

    if (opt_fillrows) {
        size_t chcnt = 0, endcol = stride;
        for (int i = 0; i < nents; i++) {
            fputs(ents[i], STDOUT_FILENO);
            chcnt += utf8_str_width(ents[i], strlen(ents[i]));
            /* The last item ends the output; padding after it would be
             * trailing whitespace on the line and nothing to align. */
            if (i == nents - 1) {
                printf("\n");
                chcnt = 0;
                break;
            }
            if ((i + 1) % numcols == 0) {
                printf("\n");
                chcnt = 0;
                endcol = stride;
            } else {
                size_t col;
                while ((col = (chcnt + TAB_LEN) & ~(size_t)(TAB_LEN - 1)) <= endcol) {
                    putchar('\t');
                    chcnt = col;
                }
                endcol += stride;
            }
        }
        if (chcnt)
            printf("\n");
        return;
    }

    int numrows = nents / numcols + (nents % numcols ? 1 : 0);
    for (int r = 0; r < numrows; r++) {
        size_t chcnt = 0, endcol = stride;
        int    base = r;
        for (int c = 0; c < numcols; c++) {
            fputs(ents[base], STDOUT_FILENO);
            chcnt += utf8_str_width(ents[base], strlen(ents[base]));
            base += numrows;
            if (base >= nents)
                break;
            size_t col;
            while ((col = (chcnt + TAB_LEN) & ~(size_t)(TAB_LEN - 1)) <= endcol) {
                putchar('\t');
                chcnt = col;
            }
            endcol += stride;
        }
        printf("\n");
    }
}

static void usage(int fd)
{
    dprintf(fd, "Usage: column [options] [<file>...]\n"
                "Columnate lists.\n\n"
                "  -t, --table                      create a table\n"
                "  -s, --separator <string>         possible table delimiters\n"
                "  -o, --output-separator <string>  columns separator for table\n"
                "                                     output (default is two spaces)\n"
                "  -c, --output-width <width>       width of output in number of characters\n"
                "  -x, --fillrows                   fill rows before columns\n"
                "  -h, --help                       display this help\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "table", 0, 't' }, { "separator", 1, 's' },
        { "output-separator", 1, 'o' }, { "output-width", 1, 'c' },
        { "fillrows", 0, 'x' }, { "help", 0, 'h' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "ts:o:c:xh", lo);

    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 't': opt_table = true; break;
        case 'x': opt_fillrows = true; break;
        case 's': opt_sep = g.arg; break;
        case 'o': opt_outsep = g.arg; break;
        case 'c': {
            char *end;
            long v = strtol(g.arg, &end, 10);
            if (end == g.arg || *end) {
                dprintf(STDERR_FILENO, "%s: invalid columns argument: '%s'\n",
                        prog, g.arg);
                return 1;
            }
            if (v < 0) {
                dprintf(STDERR_FILENO, "%s: invalid columns argument: '%s': "
                        "Numerical result out of range\n", prog, g.arg);
                return 1;
            }
            /* -c 0 is not "the default"; it is a width of nothing, and
             * one item per line is what that comes to. */
            opt_width = v ? v : -1;
            break;
        }
        case 'h': usage(STDOUT_FILENO); return 0;
        default:
            lp_getopt_err(prog, &g);
            return 1;
        }
    }

    if (opt_table && opt_fillrows) {
        dprintf(STDERR_FILENO,
                "%s: mutually exclusive arguments: --table --fillrows\n", prog);
        return 1;
    }

    int rc = 0;
    if (g.ind >= argc) {
        if (!slurp(STDIN_FILENO))
            return 1;
    } else {
        for (int i = g.ind; i < argc; i++) {
            /* "-" is not standard input here. column has never read it
             * that way, and a file really called - is what it opens. */
            long fd = lp_open(argv[i], O_RDONLY, 0);
            if (fd < 0) {
                lp_diag(prog, NULL, NULL, "cannot open", argv[i], (int)-fd);
                rc = 1;
                continue;
            }
            if (!slurp((int)fd))
                rc = 1;
            lp_close((int)fd);
        }
    }

    if (!split_lines())
        return 1;
    if (nents == 0)
        return rc;

    if (opt_table) {
        for (int i = 0; i < nents; i++)
            if (!split_fields(ents[i]))
                return 1;
        table();
    } else {
        columnate();
    }
    return rc;
}
