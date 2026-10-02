/* numfmt - numbers a person can read, and back again.
 *
 *   numfmt --to=iec 1048576        ->  1.0M
 *   numfmt --from=iec 1.5G         ->  1610612736
 *   df | numfmt --field=2 --to=iec --header
 *
 * There is no floating point in this libc, so none of this goes through
 * a double. A value is carried as an integer and a count of decimal
 * places - 1.5G is 15 at scale 1, times 2^30 - which is exact for every
 * number anybody writes down, and does not drift the way a float does
 * once the values get past a terabyte.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static const char *prog = "numfmt";

typedef enum { NONE = 0, AUTO, IEC, IEC_I, SI } unit_t;

static unit_t from_unit = NONE, to_unit = NONE;
static const char *suffix = NULL;
static long padding = 0;
static long field = 1;
static long header = 0;
static char delim = 0;                 /* 0 means "runs of blanks" */
static const char *round_mode = "from-zero";
static bool invalid_fail = true;
static bool auto_binary = false;      /* the last suffix read had an i */

static const char SUF[] = "KMGTPEZY";

static u64 base_of(unit_t u) { return (u == SI) ? 1000 : 1024; }

static int suffix_index(char c)
{
    if (c == 'k') c = 'K';
    const char *p = strchr(SUF, c);
    return p ? (int)(p - SUF) + 1 : 0;
}

/* Parse "1.5G" into value*10^scale and the power of the suffix. */
static bool parse_num(const char *s, u64 *mant, int *scale, int *power,
                      bool *neg, const char **endp)
{
    while (*s == ' ' || *s == '\t') s++;
    *neg = false;
    if (*s == '-') { *neg = true; s++; }
    else if (*s == '+') s++;

    if (*s < '0' || *s > '9') return false;

    u64 v = 0;
    int sc = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (u64)(*s++ - '0');
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (u64)(*s++ - '0'); sc++; }
    }
    *mant = v;
    *scale = sc;
    *power = 0;

    if (from_unit != NONE && *s) {
        int p = suffix_index(*s);
        if (p) {
            *power = p;
            s++;
            /* "2M" and "2Mi" are different numbers under --from=auto:
             * the bare suffix is a thousand, the i one is 1024. Getting
             * this backwards turns a 2 MB file into a 2 MiB one and
             * nothing complains. */
            if (*s == 'i') { s++; auto_binary = true; }
            else             auto_binary = false;
        }
    }
    *endp = s;
    return true;
}

static u64 pow_u64(u64 b, int n)
{
    u64 v = 1;
    while (n-- > 0) v *= b;
    return v;
}

static u64 pow10_u64(int n) { return pow_u64(10, n); }

/* value = mant / 10^scale * base^power, as an exact integer. Returns
 * false when it would not be a whole number. */
static bool to_integer(u64 mant, int scale, int power, unit_t u, u64 *out)
{
    u64 mul = pow_u64(base_of(u == NONE ? IEC : u), power);
    u64 div = pow10_u64(scale);
    /* Multiply first so 1.5G stays exact. */
    u64 v = mant * mul;
    if (div && v % div) {
        *out = (v + div / 2) / div;         /* round to nearest */
        return true;
    }
    *out = div ? v / div : v;
    return true;
}

/* An integer as "1.5G": one decimal below ten, none above, rounding the
 * way numfmt does - away from zero unless told otherwise. */
static void humanize(char *out, size_t cap, u64 v, unit_t u)
{
    u64 base = base_of(u);
    const char *tail = (u == IEC_I) ? "i" : "";

    if (v < base) {
        snprintf(out, cap, "%llu%s", (unsigned long long)v,
                 suffix ? suffix : "");
        return;
    }

    int i = 0;
    u64 n = v, rem = 0;
    while (n >= base && i < 8) { rem = n % base; n /= base; i++; }

    u64 tenths;
    if (strcmp(round_mode, "down") == 0 || strcmp(round_mode, "towards-zero") == 0)
        tenths = rem * 10 / base;
    else if (strcmp(round_mode, "nearest") == 0)
        tenths = (rem * 10 + base / 2) / base;
    else                                     /* up, from-zero: the default */
        tenths = (rem * 10 + base - 1) / base;

    if (tenths >= 10) { n++; tenths = 0; if (n >= base) { n /= base; i++; } }

    char sfx[4];
    snprintf(sfx, sizeof sfx, "%c%s", SUF[i - 1], tail);

    /* Below ten there is always a decimal, even a zero one - "1.0M",
     * not "1M". That is what keeps a column the same width. */
    if (n < 10)
        snprintf(out, cap, "%llu.%llu%s%s", (unsigned long long)n,
                 (unsigned long long)tenths, sfx, suffix ? suffix : "");
    else
        snprintf(out, cap, "%llu%s%s", (unsigned long long)(n + (tenths ? 1 : 0)),
                 sfx, suffix ? suffix : "");
}

static void pad_and_print(const char *s)
{
    long n = (long)strlen(s);
    if (padding > 0)      printf("%*s", (int)padding, s);
    else if (padding < 0) printf("%-*s", (int)-padding, s);
    else                  printf("%s", s);
    (void)n;
}

/* One number, from text to text. false when it is not a number. */
static bool convert(const char *in, char *out, size_t cap)
{
    u64 mant; int scale, power; bool neg; const char *end;
    if (!parse_num(in, &mant, &scale, &power, &neg, &end)) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end) return false;

    unit_t inbase = from_unit;
    if (inbase == AUTO) inbase = auto_binary ? IEC : SI;
    if (inbase == NONE) inbase = IEC;

    u64 v;
    to_integer(mant, scale, power, inbase, &v);

    char body[64];
    if (to_unit == NONE)
        snprintf(body, sizeof body, "%llu%s", (unsigned long long)v,
                 suffix ? suffix : "");
    else
        humanize(body, sizeof body, v, to_unit);

    snprintf(out, cap, "%s%s", neg ? "-" : "", body);
    return true;
}

static unit_t unit_by_name(const char *s)
{
    if (strcmp(s, "auto") == 0)  return AUTO;
    if (strcmp(s, "iec") == 0)   return IEC;
    if (strcmp(s, "iec-i") == 0) return IEC_I;
    if (strcmp(s, "si") == 0)    return SI;
    if (strcmp(s, "none") == 0)  return NONE;
    dprintf(STDERR_FILENO, "%s: invalid unit: '%s'\n", prog, s);
    lp_exit(1);
    return NONE;
}

/* A whole line, converting only the field asked for. */
static void do_line(char *line)
{
    if (field <= 0) { printf("%s\n", line); return; }

    char *p = line;
    long n = 1;
    while (*p && n < field) {
        if (delim) { while (*p && *p != delim) p++; if (*p) { p++; n++; } else break; }
        else {
            while (*p == ' ' || *p == '\t') p++;
            while (*p && *p != ' ' && *p != '\t') p++;
            n++;
        }
    }
    if (n != field) { printf("%s\n", line); return; }

    if (!delim) while (*p == ' ' || *p == '\t') p++;
    char *start = p;
    if (delim) while (*p && *p != delim) p++;
    else       while (*p && *p != ' ' && *p != '\t') p++;

    char word[128];
    size_t wl = (size_t)(p - start);
    if (wl >= sizeof word) wl = sizeof word - 1;
    memcpy(word, start, wl);
    word[wl] = '\0';

    char conv[128];
    if (!convert(word, conv, sizeof conv)) {
        if (invalid_fail) {
            dprintf(STDERR_FILENO, "%s: invalid number: '%s'\n", prog, word);
            lp_exit(2);
        }
        strlcpy(conv, word, sizeof conv);
    }

    lp_write(STDOUT_FILENO, line, (size_t)(start - line));
    pad_and_print(conv);
    printf("%s\n", p);
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "from", 1, 'f' }, { "to", 1, 't' }, { "from-unit", 1, 'F' },
        { "to-unit", 1, 'T' }, { "suffix", 1, 's' }, { "padding", 1, 'p' },
        { "field", 1, 'd' }, { "header", 2, 'H' }, { "round", 1, 'r' },
        { "invalid", 1, 'i' }, { "delimiter", 1, 'D' },
        { "grouping", 0, 'g' }, { "format", 1, 'm' },
        { "help", 0, 'h' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "d:", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'f': from_unit = unit_by_name(g.arg); break;
        case 't': to_unit = unit_by_name(g.arg); break;
        case 'F': case 'T': case 'g': case 'm': break;
        case 's': suffix = g.arg; break;
        case 'p': padding = strtol(g.arg, NULL, 10); break;
        case 'd': field = strtol(g.arg, NULL, 10); break;
        case 'D': delim = g.arg[0]; break;
        case 'H': header = g.arg ? strtol(g.arg, NULL, 10) : 1; break;
        case 'r': round_mode = g.arg; break;
        case 'i': invalid_fail = (strcmp(g.arg, "abort") == 0 ||
                                  strcmp(g.arg, "fail") == 0); break;
        case 'h':
            printf("Usage: numfmt [OPTION]... [NUMBER]...\n"
                   "Reformat NUMBER(s), or the numbers from standard input if none are given.\n\n"
                   "      --from=UNIT      auto-scale input numbers to UNITs; default 'none'\n"
                   "      --to=UNIT        auto-scale output numbers to UNITs\n"
                   "  -d, --field=FIELDS   replace the numbers in these input fields\n"
                   "      --delimiter=X    use X instead of whitespace for field delimiter\n"
                   "      --header[=N]     print (without converting) the first N header lines\n"
                   "      --padding=N      pad the output to N characters\n"
                   "      --round=METHOD   up, down, from-zero (default), towards-zero, nearest\n"
                   "      --suffix=SUFFIX  add SUFFIX to output numbers\n"
                   "      --invalid=MODE   failure mode for invalid numbers: abort (default),\n"
                   "                         fail, warn, ignore\n"
                   "      --help     display this help and exit\n\n"
                   "UNIT is 'none', 'auto', 'si', 'iec' or 'iec-i'.\n");
            return 0;
        default: lp_getopt_err(prog, &g); return 1;
        }
    }

    if (g.ind < argc) {
        for (int i = g.ind; i < argc; i++) {
            char out[128];
            if (!convert(argv[i], out, sizeof out)) {
                dprintf(STDERR_FILENO, "%s: invalid number: '%s'\n", prog, argv[i]);
                if (invalid_fail) return 2;
                continue;
            }
            pad_and_print(out);
            printf("\n");
        }
        return 0;
    }

    char line[8192];
    long n = 0;
    while (readrec(STDIN_FILENO, line, sizeof line, '\n', NULL) >= 0) {
        if (n++ < header) { printf("%s\n", line); continue; }
        do_line(line);
    }
    return 0;
}
