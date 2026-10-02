/* sort - put lines in order.
 *
 *   sort [-b] [-d] [-f] [-i] [-h] [-M] [-n] [-r] [-u] [-V]
 *        [-k KEYDEF] [-t SEP] [-o FILE] [-s] [-z] [-c|-C] [file]...
 *
 * The old version had -r, -n and -u and nothing else, which is fine
 * until the first `sort -k2 -n` - and that is the second thing anybody
 * types. Keys are the reason sort exists: without them it can only order
 * whole lines, and a table is not a list of whole lines.
 *
 * There is no floating point in this libc, so -n does not go through a
 * double. It compares sign, then integer digits by length and then by
 * value, then the fraction digit by digit. That is exact for numbers of
 * any size - `sort -n` on 40-digit values is right here and rounds in a
 * sort that parses to double.
 *
 * Comparison is byte order, which is what the C and C.UTF-8 locales do.
 * There is no locale table in this system to do anything else with.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

/* ── the lines ─────────────────────────────────────────────────────── */
static char **lines;
static long   nlines, linecap;

/* ── flags ─────────────────────────────────────────────────────────── */
typedef struct {
    int  sword, schar;      /* start: field (0-based), char (1-based) */
    int  eword, echar;      /* end: -1 for "to end of line" */
    bool numeric, human, month, version, general;
    bool fold, ignore_nonprint, dictionary, skip_blanks, reverse;
    bool set;               /* any modifier was given on this key */
} key_t;

#define MAX_KEYS 16
static key_t keys[MAX_KEYS];
static int   nkeys;
static key_t gflags;               /* options given outside any -k */
static bool  opt_unique, opt_reverse, opt_stable, opt_check, opt_check_quiet;
static char  tab = '\0';           /* '\0' means "runs of blanks" */
static char  delim = '\n';
static const char *outfile = NULL;
static const char *prog = "sort";

static bool blank(char c) { return c == ' ' || c == '\t'; }
static int  lower(int c)  { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static bool printable(unsigned char c) { return c >= 0x20 && c != 0x7f; }
static bool dictchar(unsigned char c)
{
    return c == ' ' || c == '\t' ||
           (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/* ── where a key starts and ends ───────────────────────────────────── */
static const char *begfield(const char *p, const char *lim, const key_t *k)
{
    int sword = k->sword;
    if (tab) {
        while (p < lim && sword--) {
            while (p < lim && *p != tab) p++;
            if (p < lim) p++;
        }
    } else {
        while (p < lim && sword--) {
            while (p < lim && blank(*p)) p++;
            while (p < lim && !blank(*p)) p++;
        }
    }
    if (k->skip_blanks)
        while (p < lim && blank(*p)) p++;
    if (k->schar > 1) {
        int skip = k->schar - 1;
        while (p < lim && skip--) p++;
    }
    return p;
}

static const char *limfield(const char *line, const char *lim, const key_t *k)
{
    if (k->eword < 0)
        return lim;

    const char *p = line;
    int eword = k->eword;
    if (k->echar == 0)
        eword++;               /* no .C means "through the end of that field" */

    if (tab) {
        while (p < lim && eword--) {
            while (p < lim && *p != tab) p++;
            if (p < lim && (eword > 0 || k->echar)) p++;
        }
    } else {
        while (p < lim && eword--) {
            while (p < lim && blank(*p)) p++;
            while (p < lim && !blank(*p)) p++;
        }
    }

    if (k->echar) {
        /* .C on the end field counts from the start of that field. */
        const char *q = p;
        if (tab) {
            /* p sits at the start of the end field */
        } else if (k->skip_blanks) {
            while (q < lim && blank(*q)) q++;
            p = q;
        }
        int take = k->echar;
        while (p < lim && take--) p++;
    }
    return p;
}

/* ── the comparisons ───────────────────────────────────────────────── */

/* Sign, integer digits, then fraction - no floating point anywhere. */
static int cmp_numeric(const char *a, size_t na, const char *b, size_t nb)
{
    const char *ae = a + na, *be = b + nb;
    while (a < ae && blank(*a)) a++;
    while (b < be && blank(*b)) b++;

    int sa = 1, sb = 1;
    if (a < ae && (*a == '-' || *a == '+')) { if (*a == '-') sa = -1; a++; }
    if (b < be && (*b == '-' || *b == '+')) { if (*b == '-') sb = -1; b++; }

    const char *ai = a, *bi = b;
    while (a < ae && *a >= '0' && *a <= '9') a++;
    const char *aie = a;
    while (b < be && *b >= '0' && *b <= '9') b++;
    const char *bie = b;

    const char *af = a, *afe = a, *bf = b, *bfe = b;
    if (a < ae && *a == '.') { af = ++a; while (a < ae && *a >= '0' && *a <= '9') a++; afe = a; }
    if (b < be && *b == '.') { bf = ++b; while (b < be && *b >= '0' && *b <= '9') b++; bfe = b; }

    bool a_num = (aie > ai) || (afe > af);
    bool b_num = (bie > bi) || (bfe > bf);
    if (!a_num && !b_num) return 0;
    if (!a_num) return -1;              /* a non-number sorts before */
    if (!b_num) return 1;

    /* Zero has no sign, so -0 and 0 must not order differently. */
    bool a_zero = true, b_zero = true;
    for (const char *p = ai; p < aie; p++) if (*p != '0') a_zero = false;
    for (const char *p = af; p < afe; p++) if (*p != '0') a_zero = false;
    for (const char *p = bi; p < bie; p++) if (*p != '0') b_zero = false;
    for (const char *p = bf; p < bfe; p++) if (*p != '0') b_zero = false;
    if (a_zero) sa = 0;
    if (b_zero) sb = 0;
    if (sa != sb) return sa < sb ? -1 : 1;

    while (ai < aie && *ai == '0') ai++;
    while (bi < bie && *bi == '0') bi++;
    int c = 0;
    if (aie - ai != bie - bi)
        c = (aie - ai < bie - bi) ? -1 : 1;
    else
        for (const char *p = ai, *q = bi; p < aie; p++, q++)
            if (*p != *q) { c = (*p < *q) ? -1 : 1; break; }

    if (c == 0) {
        const char *p = af, *q = bf;
        while (p < afe || q < bfe) {
            char x = (p < afe) ? *p++ : '0';
            char y = (q < bfe) ? *q++ : '0';
            if (x != y) { c = (x < y) ? -1 : 1; break; }
        }
    }
    return (sa < 0) ? -c : c;
}

static int suffix_rank(char c)
{
    static const char *s = "kKMGTPEZY";
    const char *p = strchr(s, c);
    if (!p) return 0;
    int r = (int)(p - s);
    return r == 0 ? 1 : r;             /* k and K are the same rank */
}

static int cmp_human(const char *a, size_t na, const char *b, size_t nb)
{
    /* Rank by suffix first: 1G beats 900M whatever the digits say. */
    size_t ia = 0, ib = 0;
    while (ia < na && (blank(a[ia]) || a[ia] == '-' || a[ia] == '+' ||
                       (a[ia] >= '0' && a[ia] <= '9') || a[ia] == '.')) ia++;
    while (ib < nb && (blank(b[ib]) || b[ib] == '-' || b[ib] == '+' ||
                       (b[ib] >= '0' && b[ib] <= '9') || b[ib] == '.')) ib++;
    int ra = (ia < na) ? suffix_rank(a[ia]) : 0;
    int rb = (ib < nb) ? suffix_rank(b[ib]) : 0;
    bool nega = (na && (a[0] == '-')), negb = (nb && (b[0] == '-'));
    if (nega) ra = -ra;
    if (negb) rb = -rb;
    if (ra != rb) return ra < rb ? -1 : 1;
    return cmp_numeric(a, na, b, nb);
}

static int month_of(const char *s, size_t n)
{
    static const char *m[12] = { "JAN","FEB","MAR","APR","MAY","JUN",
                                 "JUL","AUG","SEP","OCT","NOV","DEC" };
    size_t i = 0;
    while (i < n && blank(s[i])) i++;
    if (n - i < 3) return 0;
    for (int k = 0; k < 12; k++)
        if (lower((unsigned char)s[i])   == lower((unsigned char)m[k][0]) &&
            lower((unsigned char)s[i+1]) == lower((unsigned char)m[k][1]) &&
            lower((unsigned char)s[i+2]) == lower((unsigned char)m[k][2]))
            return k + 1;
    return 0;
}

/* Natural order: run of digits compares as a number, everything else as
 * bytes. `v1.10` after `v1.9`, which is the whole point. */
static int cmp_version(const char *a, size_t na, const char *b, size_t nb)
{
    size_t i = 0, j = 0;
    while (i < na && j < nb) {
        bool da = (a[i] >= '0' && a[i] <= '9');
        bool db = (b[j] >= '0' && b[j] <= '9');
        if (da && db) {
            size_t si = i, sj = j;
            while (i < na && a[i] == '0') i++;
            while (j < nb && b[j] == '0') j++;
            size_t ai = i, bj = j;
            while (i < na && a[i] >= '0' && a[i] <= '9') i++;
            while (j < nb && b[j] >= '0' && b[j] <= '9') j++;
            if (i - ai != j - bj) return (i - ai < j - bj) ? -1 : 1;
            for (size_t k = 0; k < i - ai; k++)
                if (a[ai + k] != b[bj + k])
                    return (a[ai + k] < b[bj + k]) ? -1 : 1;
            if (si != sj) { /* differing leading zeros, keep going */ }
            continue;
        }
        if (a[i] != b[j])
            return ((unsigned char)a[i] < (unsigned char)b[j]) ? -1 : 1;
        i++; j++;
    }
    if (i < na) return 1;
    if (j < nb) return -1;
    return 0;
}

static int cmp_text(const char *a, size_t na, const char *b, size_t nb,
                    const key_t *k)
{
    size_t i = 0, j = 0;
    for (;;) {
        while (i < na) {
            unsigned char c = (unsigned char)a[i];
            if (k->ignore_nonprint && !printable(c)) { i++; continue; }
            if (k->dictionary && !dictchar(c))       { i++; continue; }
            break;
        }
        while (j < nb) {
            unsigned char c = (unsigned char)b[j];
            if (k->ignore_nonprint && !printable(c)) { j++; continue; }
            if (k->dictionary && !dictchar(c))       { j++; continue; }
            break;
        }
        if (i >= na || j >= nb) break;
        int x = (unsigned char)a[i], y = (unsigned char)b[j];
        if (k->fold) { x = lower(x); y = lower(y); }
        if (x != y) return x < y ? -1 : 1;
        i++; j++;
    }
    bool ar = (i < na), br = (j < nb);
    if (ar == br) return 0;
    return ar ? 1 : -1;
}

static int cmp_key(const char *a, size_t na, const char *b, size_t nb,
                   const key_t *k)
{
    if (k->skip_blanks) {
        while (na && blank(*a)) { a++; na--; }
        while (nb && blank(*b)) { b++; nb--; }
    }
    if (k->numeric || k->general) return cmp_numeric(a, na, b, nb);
    if (k->human)   return cmp_human(a, na, b, nb);
    if (k->version) return cmp_version(a, na, b, nb);
    if (k->month) {
        int ma = month_of(a, na), mb = month_of(b, nb);
        if (ma != mb) return ma < mb ? -1 : 1;
        return 0;
    }
    return cmp_text(a, na, b, nb, k);
}

static int compare(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);

    if (nkeys == 0) {
        int c = cmp_key(a, la, b, lb, &gflags);
        if (c == 0 && !opt_stable && (gflags.numeric || gflags.human ||
                                      gflags.month || gflags.version ||
                                      gflags.fold || gflags.dictionary ||
                                      gflags.ignore_nonprint || gflags.skip_blanks))
            c = cmp_text(a, la, b, lb, &(key_t){ 0 });
        return opt_reverse ? -c : c;
    }

    for (int i = 0; i < nkeys; i++) {
        const key_t *k = &keys[i];
        const char *as = begfield(a, a + la, k), *ae = limfield(a, a + la, k);
        const char *bs = begfield(b, b + lb, k), *be = limfield(b, b + lb, k);
        if (ae < as) ae = as;
        if (be < bs) be = bs;
        int c = cmp_key(as, (size_t)(ae - as), bs, (size_t)(be - bs), k);
        if (c) return (k->reverse ^ opt_reverse) ? -c : c;
    }
    if (opt_stable || opt_unique)
        return 0;
    int c = cmp_text(a, la, b, lb, &(key_t){ 0 });
    return opt_reverse ? -c : c;
}

/* ── reading ───────────────────────────────────────────────────────── */
static bool add_line(const char *s)
{
    if (nlines == linecap) {
        long cap = linecap ? linecap * 2 : 1024;
        char **n = realloc(lines, sizeof(char *) * (size_t)cap);
        if (!n) return false;
        lines = n;
        linecap = cap;
    }
    char *copy = strdup(s);
    if (!copy) return false;
    lines[nlines++] = copy;
    return true;
}

static bool read_fd(int fd)
{
    static char line[65536];
    for (;;) {
        long n = readrec(fd, line, sizeof line, delim, NULL);
        if (n < 0) return true;
        if (!add_line(line)) {
            dprintf(STDERR_FILENO, "%s: out of memory\n", prog);
            return false;
        }
    }
}

/* Merge sort: n log n without the worst case a quicksort can hit on
 * input that is already in order, which is exactly the input this gets
 * given most often. It is also stable, which -s asks for and which the
 * last-resort comparison relies on. */
static void merge_sort(char **a, long n, char **tmp)
{
    if (n < 2) return;
    long mid = n / 2;
    merge_sort(a, mid, tmp);
    merge_sort(a + mid, n - mid, tmp);

    long i = 0, j = mid, k = 0;
    while (i < mid && j < n)
        tmp[k++] = (compare(a[i], a[j]) <= 0) ? a[i++] : a[j++];
    while (i < mid) tmp[k++] = a[i++];
    while (j < n)   tmp[k++] = a[j++];
    for (long m = 0; m < n; m++) a[m] = tmp[m];
}

/* ── options ───────────────────────────────────────────────────────── */
static void apply_mod(key_t *k, char c)
{
    switch (c) {
    case 'n': k->numeric = true; break;
    case 'g': k->general = true; break;
    case 'h': k->human = true; break;
    case 'M': k->month = true; break;
    case 'V': k->version = true; break;
    case 'f': k->fold = true; break;
    case 'i': k->ignore_nonprint = true; break;
    case 'd': k->dictionary = true; break;
    case 'b': k->skip_blanks = true; break;
    case 'r': k->reverse = true; break;
    default:
        dprintf(STDERR_FILENO, "%s: invalid sort key\n", prog);
        lp_exit(2);
    }
    k->set = true;
}

/* F[.C][OPTS][,F[.C][OPTS]] */
static bool parse_key(const char *s, key_t *k)
{
    memset(k, 0, sizeof *k);
    k->eword = -1;

    char *end;
    long f = strtol(s, &end, 10);
    if (end == s || f < 1) return false;
    k->sword = (int)f - 1;
    s = end;
    if (*s == '.') {
        k->schar = (int)strtol(s + 1, &end, 10);
        if (end == s + 1) return false;
        s = end;
    }
    while (*s && *s != ',') apply_mod(k, *s++);

    if (*s == ',') {
        s++;
        f = strtol(s, &end, 10);
        if (end == s || f < 1) return false;
        k->eword = (int)f - 1;
        s = end;
        if (*s == '.') {
            k->echar = (int)strtol(s + 1, &end, 10);
            if (end == s + 1) return false;
            s = end;
        }
        while (*s) apply_mod(k, *s++);
    }
    return true;
}

static char parse_tab(const char *s)
{
    if (s[0] == '\\' && s[1] == 't' && !s[2]) return '\t';
    if (s[0] == '\\' && s[1] == '0' && !s[2]) return '\0';
    if (s[0] && !s[1]) return s[0];
    dprintf(STDERR_FILENO, "%s: multi-character tab '%s'\n", prog, s);
    lp_exit(2);
    return 0;
}

static void usage(int fd)
{
    dprintf(fd, "Usage: sort [OPTION]... [FILE]...\n"
                "Write sorted concatenation of all FILE(s) to standard output.\n\n"
                "  -b, --ignore-leading-blanks  ignore leading blanks\n"
                "  -d, --dictionary-order       consider only blanks and alphanumerics\n"
                "  -f, --ignore-case            fold lower case to upper case characters\n"
                "  -h, --human-numeric-sort     compare human readable numbers (e.g., 2K 1G)\n"
                "  -i, --ignore-nonprinting     consider only printable characters\n"
                "  -M, --month-sort             compare (unknown) < 'JAN' < ... < 'DEC'\n"
                "  -n, --numeric-sort           compare according to string numerical value\n"
                "  -r, --reverse                reverse the result of comparisons\n"
                "  -V, --version-sort           natural sort of (version) numbers within text\n"
                "  -k, --key=KEYDEF             sort via a key; KEYDEF gives location and type\n"
                "  -o, --output=FILE            write result to FILE instead of standard output\n"
                "  -s, --stable                 stabilize sort by disabling last-resort comparison\n"
                "  -t, --field-separator=SEP    use SEP instead of non-blank to blank transition\n"
                "  -u, --unique                 output only the first of an equal run\n"
                "  -z, --zero-terminated        line delimiter is NUL, not newline\n"
                "  -c, --check                  check for sorted input; do not sort\n"
                "  -C, --check=quiet            like -c, but do not report the first bad line\n"
                "      --help     display this help and exit\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "ignore-leading-blanks", 0, 'b' }, { "dictionary-order", 0, 'd' },
        { "ignore-case", 0, 'f' }, { "general-numeric-sort", 0, 'g' },
        { "human-numeric-sort", 0, 'h' }, { "ignore-nonprinting", 0, 'i' },
        { "month-sort", 0, 'M' }, { "numeric-sort", 0, 'n' },
        { "reverse", 0, 'r' }, { "version-sort", 0, 'V' },
        { "key", 1, 'k' }, { "output", 1, 'o' }, { "stable", 0, 's' },
        { "field-separator", 1, 't' }, { "unique", 0, 'u' },
        { "zero-terminated", 0, 'z' }, { "check", 2, 'c' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    memset(&gflags, 0, sizeof gflags);
    gflags.eword = -1;

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "bdfghiMnrVk:o:st:uzcC", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'b': case 'd': case 'f': case 'g': case 'h':
        case 'i': case 'M': case 'n': case 'V':
            apply_mod(&gflags, (char)c); break;
        case 'r': opt_reverse = true; break;
        case 's': opt_stable = true; break;
        case 'u': opt_unique = true; break;
        case 'z': delim = '\0'; break;
        case 'o': outfile = g.arg; break;
        case 't': tab = parse_tab(g.arg); break;
        case 'c': opt_check = true;
                  if (g.arg && strcmp(g.arg, "quiet") == 0) opt_check_quiet = true;
                  break;
        case 'C': opt_check = true; opt_check_quiet = true; break;
        case 'k':
            if (nkeys == MAX_KEYS) {
                dprintf(STDERR_FILENO, "%s: too many sort keys\n", prog);
                return 2;
            }
            if (!parse_key(g.arg, &keys[nkeys])) {
                dprintf(STDERR_FILENO, "%s: invalid number at field start: "
                                       "invalid count at start of '%s'\n", prog, g.arg);
                return 2;
            }
            nkeys++;
            break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default: lp_getopt_err(prog, &g); return 2;
        }
    }

    /* A key that named no type of its own uses whatever -n/-r/-f were
     * given outside the key, wherever on the line they appeared. */
    for (int i = 0; i < nkeys; i++) {
        if (keys[i].set) continue;
        key_t saved = keys[i], merged = gflags;
        merged.sword = saved.sword; merged.schar = saved.schar;
        merged.eword = saved.eword; merged.echar = saved.echar;
        merged.reverse = false;
        keys[i] = merged;
    }

    int files = argc - g.ind;
    if (files == 0) {
        if (!read_fd(STDIN_FILENO)) return 2;
    } else {
        for (int i = g.ind; i < argc; i++) {
            if (strcmp(argv[i], "-") == 0) {
                if (!read_fd(STDIN_FILENO)) return 2;
                continue;
            }
            long fd = lp_open(argv[i], O_RDONLY, 0);
            if (fd < 0) {
                if (lp_voice() == LP_VOICE_GNU)
                    dprintf(STDERR_FILENO, "%s: cannot read: %s: %s\n", prog,
                            argv[i], lp_strerror((int)-fd));
                else
                    lp_diag(prog, NULL, NULL, "cannot open", argv[i], (int)-fd);
                return 2;
            }
            bool ok = read_fd((int)fd);
            lp_close((int)fd);
            if (!ok) return 2;
        }
    }

    if (opt_check) {
        for (long i = 1; i < nlines; i++) {
            int c = compare(lines[i - 1], lines[i]);
            if (c > 0 || (opt_unique && c == 0)) {
                if (!opt_check_quiet)
                    dprintf(STDERR_FILENO, "%s: %s:%ld: %s: %s\n", prog,
                            files ? argv[g.ind] : "-", i + 1,
                            c > 0 ? "disorder" : "disorder", lines[i]);
                return 1;
            }
        }
        return 0;
    }

    if (nlines > 1) {
        char **tmp = malloc(sizeof(char *) * (size_t)nlines);
        if (!tmp) {
            dprintf(STDERR_FILENO, "%s: out of memory (%ld lines)\n", prog, nlines);
            return 2;
        }
        merge_sort(lines, nlines, tmp);
        free(tmp);
    }

    int out = STDOUT_FILENO;
    if (outfile) {
        long f = lp_open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f < 0) {
            lp_diag(prog, "open failed", NULL, "cannot open", outfile, (int)-f);
            return 2;
        }
        out = (int)f;
    }

    for (long i = 0; i < nlines; i++) {
        if (opt_unique && i > 0 && compare(lines[i - 1], lines[i]) == 0)
            continue;
        lp_write(out, lines[i], strlen(lines[i]));
        lp_write(out, &delim, 1);
    }
    if (out != STDOUT_FILENO) lp_close(out);
    return 0;
}
