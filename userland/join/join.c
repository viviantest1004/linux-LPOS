/* join - two sorted files, matched on a shared field.
 *
 *   join a b                 match on field 1 of both
 *   join -1 2 -2 1 a b       field 2 of the first, field 1 of the second
 *   join -t: /etc/passwd x   split on colons instead of blanks
 *   join -a1 a b             also keep the lines of a that matched nothing
 *
 * This is the shell's version of a database join, and like a database
 * it needs both inputs in order. When they are not, the answer is
 * quietly incomplete rather than wrong-looking, which is why the
 * out-of-order warning below is on by default.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define MAXF   256
#define LINE   8192
#define MAXDUP 1024

static char  tab = 0;              /* 0 means "runs of blanks" */
static long  key1 = 1, key2 = 1;
static bool  show_unpaired1 = false, show_unpaired2 = false;
static bool  only_unpaired = false;
static bool  fold = false, check_order = true;
static const char *empty = NULL;   /* -e */

/* -o: which fields to print, as (file, field) pairs. file 0 means the
 * join field itself, which is what "0" means in the spec. */
typedef struct { int file, field; } outspec_t;
static outspec_t outspec[MAXF];
static int nout = 0;

typedef struct {
    char  text[LINE];
    char *f[MAXF];
    int   nf;
} row_t;

static bool blank(char c) { return c == ' ' || c == '\t'; }

static int split(row_t *r)
{
    r->nf = 0;
    char *p = r->text;
    if (tab) {
        r->f[r->nf++] = p;
        while (*p && r->nf < MAXF) {
            if (*p == tab) { *p = '\0'; r->f[r->nf++] = p + 1; }
            p++;
        }
    } else {
        while (*p && r->nf < MAXF) {
            while (blank(*p)) p++;
            if (!*p) break;
            r->f[r->nf++] = p;
            while (*p && !blank(*p)) p++;
            if (*p) { *p = '\0'; p++; }
        }
    }
    return r->nf;
}

/* Copying a row copies the text; the field pointers still point into
 * the original, which is about to be overwritten by the next line. This
 * is the bug that made every duplicate key show the last value read.
 * Move the pointers to the copy's own text. */
static void copy_row(row_t *dst, const row_t *src)
{
    *dst = *src;
    for (int i = 0; i < src->nf; i++)
        dst->f[i] = dst->text + (src->f[i] - src->text);
}

static const char *field(const row_t *r, long n)
{
    if (n < 1 || n > r->nf) return "";
    return r->f[n - 1];
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int keycmp(const char *a, const char *b)
{
    if (!fold) return strcmp(a, b);
    while (*a && *b) {
        int x = lower((unsigned char)*a), y = lower((unsigned char)*b);
        if (x != y) return x < y ? -1 : 1;
        a++; b++;
    }
    return *a ? 1 : (*b ? -1 : 0);
}

static char outsep(void) { return tab ? tab : ' '; }

/* One joined line. GNU's default is the join field, then the rest of
 * the first file, then the rest of the second. */
static void emit(const row_t *a, const row_t *b)
{
    char sep = outsep();
    bool first = true;

    if (nout) {
        for (int i = 0; i < nout; i++) {
            const row_t *r = outspec[i].file == 2 ? b : a;
            const char *v;
            if (outspec[i].file == 0)
                v = a ? field(a, key1) : field(b, key2);
            else if (!r)
                v = empty ? empty : "";
            else
                v = field(r, outspec[i].file == 1 ? outspec[i].field
                                                  : outspec[i].field);
            if (!first) printf("%c", sep);
            printf("%s", v);
            first = false;
        }
        printf("\n");
        return;
    }

    printf("%s", a ? field(a, key1) : field(b, key2));
    if (a) for (long i = 1; i <= a->nf; i++)
        if (i != key1) printf("%c%s", sep, a->f[i - 1]);
    if (b) for (long i = 1; i <= b->nf; i++)
        if (i != key2) printf("%c%s", sep, b->f[i - 1]);
    printf("\n");
}

static void parse_outspec(const char *s)
{
    while (*s && nout < MAXF) {
        while (*s == ' ' || *s == ',') s++;
        if (!*s) break;
        if (*s == '0') { outspec[nout].file = 0; outspec[nout].field = 0; nout++; s++; continue; }
        int file = *s - '0';
        s++;
        if (*s == '.') s++;
        int fld = 0;
        while (*s >= '0' && *s <= '9') fld = fld * 10 + (*s++ - '0');
        outspec[nout].file = file;
        outspec[nout].field = fld;
        nout++;
    }
}

static char parse_tab(const char *s)
{
    if (s[0] == '\\' && s[1] == 't' && !s[2]) return '\t';
    if (s[0] == '\\' && s[1] == '0' && !s[2]) return '\0';
    return s[0];
}

static void usage(int fd)
{
    dprintf(fd, "Usage: join [OPTION]... FILE1 FILE2\n"
                "For each pair of input lines with identical join fields, write a line to\n"
                "standard output.  The default join field is the first, delimited by blanks.\n\n"
                "  -a FILENUM        also print unpairable lines from file FILENUM\n"
                "  -e STRING         replace missing input fields with STRING\n"
                "  -i, --ignore-case ignore differences in case when comparing fields\n"
                "  -j FIELD          equivalent to '-1 FIELD -2 FIELD'\n"
                "  -o FORMAT         obey FORMAT while constructing output line\n"
                "  -t CHAR           use CHAR as input and output field separator\n"
                "  -v FILENUM        like -a FILENUM, but suppress joined output lines\n"
                "  -1 FIELD          join on this FIELD of file 1\n"
                "  -2 FIELD          join on this FIELD of file 2\n"
                "      --nocheck-order  do not check that the input is correctly sorted\n"
                "      --help     display this help and exit\n\n"
                "Both inputs must be sorted on the join field.\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "ignore-case", 0, 'i' }, { "check-order", 0, 'C' },
        { "nocheck-order", 0, 'N' }, { "header", 0, 'H' },
        { "help", 0, 'h' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "a:e:ij:o:t:v:1:2:", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'a': if (g.arg[0] == '1') show_unpaired1 = true;
                  else                 show_unpaired2 = true; break;
        case 'v': only_unpaired = true;
                  if (g.arg[0] == '1') show_unpaired1 = true;
                  else                 show_unpaired2 = true; break;
        case 'e': empty = g.arg; break;
        case 'i': fold = true; break;
        case 'j': key1 = key2 = strtol(g.arg, NULL, 10); break;
        case '1': key1 = strtol(g.arg, NULL, 10); break;
        case '2': key2 = strtol(g.arg, NULL, 10); break;
        case 'o': parse_outspec(g.arg); break;
        case 't': tab = parse_tab(g.arg); break;
        case 'C': check_order = true; break;
        case 'N': check_order = false; break;
        case 'H': break;
        case 'h': usage(STDOUT_FILENO); return 0;
        default: lp_getopt_err("join", &g); return 1;
        }
    }

    if (argc - g.ind != 2) {
        dprintf(STDERR_FILENO, "join: missing operand\n");
        dprintf(STDERR_FILENO, "Try 'join --help' for more information.\n");
        return 1;
    }

    int fd[2];
    for (int i = 0; i < 2; i++) {
        const char *name = argv[g.ind + i];
        if (strcmp(name, "-") == 0) { fd[i] = STDIN_FILENO; continue; }
        long f = lp_open(name, O_RDONLY, 0);
        if (f < 0) {
            lp_diag("join", NULL, NULL, "cannot open", name, (int)-f);
            return 1;
        }
        fd[i] = (int)f;
    }

    static row_t a, b, prev_a, prev_b;
    static row_t group[MAXDUP];          /* the run of equal keys in file 2 */
    bool have_a = readrec(fd[0], a.text, LINE, '\n', NULL) >= 0;
    bool have_b = readrec(fd[1], b.text, LINE, '\n', NULL) >= 0;
    if (have_a) split(&a);
    if (have_b) split(&b);
    bool first_a = true, first_b = true, warned = false;

    while (have_a || have_b) {
        if (!have_b) {
            if (show_unpaired1) emit(&a, NULL);
            have_a = readrec(fd[0], a.text, LINE, '\n', NULL) >= 0;
            if (have_a) split(&a);
            continue;
        }
        if (!have_a) {
            if (show_unpaired2) emit(NULL, &b);
            have_b = readrec(fd[1], b.text, LINE, '\n', NULL) >= 0;
            if (have_b) split(&b);
            continue;
        }

        int c = keycmp(field(&a, key1), field(&b, key2));

        if (check_order && !warned) {
            if (!first_a && keycmp(field(&prev_a, key1), field(&a, key1)) > 0) {
                dprintf(STDERR_FILENO, "join: %s:%d: is not sorted: %s\n",
                        argv[g.ind], 0, a.text);
                warned = true;
            } else if (!first_b && keycmp(field(&prev_b, key2), field(&b, key2)) > 0) {
                dprintf(STDERR_FILENO, "join: %s:%d: is not sorted: %s\n",
                        argv[g.ind + 1], 0, b.text);
                warned = true;
            }
        }

        if (c < 0) {
            if (show_unpaired1) emit(&a, NULL);
            copy_row(&prev_a, &a); first_a = false;
            have_a = readrec(fd[0], a.text, LINE, '\n', NULL) >= 0;
            if (have_a) split(&a);
        } else if (c > 0) {
            if (show_unpaired2) emit(NULL, &b);
            copy_row(&prev_b, &b); first_b = false;
            have_b = readrec(fd[1], b.text, LINE, '\n', NULL) >= 0;
            if (have_b) split(&b);
        } else {
            /* Every line of file 2 with this key has to pair with every
             * line of file 1 that has it, so the run is collected once
             * and replayed for each. */
            int n = 0;
            char key[LINE];
            strlcpy(key, field(&b, key2), sizeof key);
            while (have_b && keycmp(field(&b, key2), key) == 0 && n < MAXDUP) {
                copy_row(&group[n++], &b);
                copy_row(&prev_b, &b); first_b = false;
                have_b = readrec(fd[1], b.text, LINE, '\n', NULL) >= 0;
                if (have_b) split(&b);
            }
            while (have_a && keycmp(field(&a, key1), key) == 0) {
                if (!only_unpaired)
                    for (int i = 0; i < n; i++) emit(&a, &group[i]);
                copy_row(&prev_a, &a); first_a = false;
                have_a = readrec(fd[0], a.text, LINE, '\n', NULL) >= 0;
                if (have_a) split(&a);
            }
        }
    }

    for (int i = 0; i < 2; i++) if (fd[i] != STDIN_FILENO) lp_close(fd[i]);
    return 0;
}
