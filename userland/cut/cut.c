/* cut - take columns out of each line.
 *
 *   cut -f 1,3 [-d :] [file]     fields, split on a delimiter (tab by default)
 *   cut -w -f 2 [file]           fields split on runs of whitespace
 *   cut -c 1-10 [file]           characters
 *
 * Field numbers start at 1, and a range is written 2-5. Missing fields
 * produce nothing rather than an error: lines in real files are ragged,
 * and stopping on the first short one is rarely what anyone wants.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define MAX_RANGES 16

typedef struct { int from, to; } range_t;
static range_t ranges[MAX_RANGES];
static int nranges;

static bool complement = false;      /* --complement */
static bool only_delimited = false;  /* -s */
static const char *out_delim = NULL; /* --output-delimiter */
static char rec_delim = '\n';        /* -z */

static bool wanted(int n)
{
    bool hit = false;
    for (int i = 0; i < nranges; i++)
        if (n >= ranges[i].from && (ranges[i].to == 0 || n <= ranges[i].to)) {
            hit = true;
            break;
        }
    return complement ? !hit : hit;
}

/* "1,3,5-7" or "2-" */
static bool parse_list(const char *spec)
{
    while (*spec && nranges < MAX_RANGES) {
        int from = (int)strtol(spec, (char **)&spec, 10);
        int to   = from;

        if (*spec == '-') {
            spec++;
            if (*spec >= '0' && *spec <= '9')
                to = (int)strtol(spec, (char **)&spec, 10);
            else
                to = 0;              /* open ended */
        }
        if (from < 1)
            return false;

        ranges[nranges].from = from;
        ranges[nranges].to   = to;
        nranges++;

        if (*spec == ',') spec++;
        else if (*spec)   return false;
    }
    return nranges > 0;
}

/* -w: fields are runs of whitespace, and leading whitespace does not
 * make an empty first field.
 *
 * This is not in POSIX cut, and it is here because almost everything
 * that prints a table on this machine pads it into columns - /proc,
 * free, df, ls. Cutting those on a single space picks up the padding
 * and hands back an empty string, which then looks like the file was
 * wrong rather than the command. */
static void cut_fields_ws(const char *line)
{
    int  field = 1;
    bool first = true;
    const char *p = line;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;

        if (wanted(field)) {
            if (!first) printf("%s", out_delim ? out_delim : " ");
            lp_write(STDOUT_FILENO, start, (size_t)(p - start));
            first = false;
        }
        field++;
    }
    lp_write(STDOUT_FILENO, &rec_delim, 1);
}

static void cut_fields(const char *line, char delim)
{
    int  field = 1;
    bool first = true;
    const char *p = line;

    /* -s: a line with no delimiter in it has no fields to take, and
     * printing it whole is how a header sneaks into cut's output. */
    if (!strchr(line, delim)) {
        if (only_delimited) return;
        printf("%s", line);
        lp_write(STDOUT_FILENO, &rec_delim, 1);
        return;
    }

    while (p) {
        const char *end = strchr(p, delim);
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (wanted(field)) {
            if (!first) {
                if (out_delim) printf("%s", out_delim);
                else           printf("%c", delim);
            }
            lp_write(STDOUT_FILENO, p, len);
            first = false;
        }

        field++;
        p = end ? end + 1 : NULL;
    }
    lp_write(STDOUT_FILENO, &rec_delim, 1);
}

static void cut_chars(const char *line)
{
    /* By character, not by byte: cutting a Hangul syllable in half
     * produces something that is not text at all. */
    int col = 1;
    size_t i = 0, len = strlen(line);

    while (i < len) {
        size_t next = utf8_next(line, len, i);
        if (wanted(col))
            lp_write(STDOUT_FILENO, line + i, next - i);
        i = next;
        col++;
    }
    lp_write(STDOUT_FILENO, &rec_delim, 1);
}

static void usage(int fd)
{
    dprintf(fd, "Usage: cut OPTION... [FILE]...\n"
                "Print selected parts of lines from each FILE to standard output.\n\n"
                "With no FILE, or when FILE is -, read standard input.\n\n"
                "  -b, --bytes=LIST        select only these bytes\n"
                "  -c, --characters=LIST   select only these characters\n"
                "  -d, --delimiter=DELIM   use DELIM instead of TAB for the field delimiter\n"
                "  -f, --fields=LIST       select only these fields\n"
                "  -s, --only-delimited    do not print lines not containing delimiters\n"
                "      --complement        complement the set of selected bytes or fields\n"
                "      --output-delimiter=STRING  use STRING as the output delimiter\n"
                "  -w                      fields are split on runs of whitespace, which is\n"
                "                            what the column-padded output of ps, df and ls\n"
                "                            actually is (this one is not GNU's)\n"
                "  -z, --zero-terminated   line delimiter is NUL, not newline\n"
                "      --help     display this help and exit\n\n"
                "Use one, and only one, of -b, -c or -f.  Each LIST is made up of one\n"
                "range, or many ranges separated by commas: N, N-M, N- or -M.\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "bytes", 1, 'b' }, { "characters", 1, 'c' },
        { "delimiter", 1, 'd' }, { "fields", 1, 'f' },
        { "only-delimited", 0, 's' }, { "complement", 0, 'C' },
        { "output-delimiter", 1, 'O' }, { "zero-terminated", 0, 'z' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    char delim = '\t';
    bool by_space = false, by_char = false, have_list = false;
    int  kinds = 0;                 /* how many of -b/-c/-f were given */

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "b:c:d:f:swzn", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'b': case 'c':
            by_char = true; kinds++;
            have_list = parse_list(g.arg);
            break;
        case 'f':
            kinds++;
            have_list = parse_list(g.arg);
            break;
        case 'd': delim = g.arg[0]; break;
        case 'w': by_space = true; break;
        case 's': only_delimited = true; break;
        case 'C': complement = true; break;
        case 'O': out_delim = g.arg; break;
        case 'z': rec_delim = '\0'; break;
        case 'n': break;            /* accepted and ignored, as GNU does */
        case 'H': usage(STDOUT_FILENO); return 0;
        default: lp_getopt_err("cut", &g); return 1;
        }
    }

    if (kinds > 1) {
        dprintf(STDERR_FILENO, "cut: only one list may be specified\n");
        dprintf(STDERR_FILENO, "Try 'cut --help' for more information.\n");
        return 1;
    }
    if (!have_list) {
        dprintf(STDERR_FILENO,
                "cut: you must specify a list of bytes, characters, or fields\n");
        dprintf(STDERR_FILENO, "Try 'cut --help' for more information.\n");
        return 1;
    }

    int rc = 0;
    int files = argc - g.ind;

    for (int i = 0; i < (files ? files : 1); i++) {
        int fd = STDIN_FILENO;
        if (files) {
            const char *name = argv[g.ind + i];
            if (strcmp(name, "-") != 0) {
                long f = lp_open(name, O_RDONLY, 0);
                if (f < 0) {
                    lp_diag("cut", NULL, NULL, "cannot open", name, (int)-f);
                    rc = 1;
                    continue;
                }
                fd = (int)f;
            }
        }

        char line[8192];
        while (readrec(fd, line, sizeof(line), rec_delim, NULL) >= 0) {
            if (by_char) cut_chars(line);
            else if (by_space) cut_fields_ws(line);
            else               cut_fields(line, delim);
        }
        if (fd != STDIN_FILENO) lp_close(fd);
    }
    return rc;
}
