/* seq - print a run of numbers.
 *
 *   seq LAST
 *   seq FIRST LAST
 *   seq FIRST INCREMENT LAST
 *   seq -s , 1 10        something other than a newline between them
 *   seq -w 1 10          pad with zeros so the widths line up
 *
 * There is no floating point in this libc, and the old version therefore
 * refused anything with a dot in it. But `seq 0 0.1 1` is one of the
 * first things anyone tries, and a seq that cannot do it is a seq people
 * stop trusting. So the numbers are kept as an integer and a count of
 * decimal places - 0.1 is 1 at scale 1 - which is exact for everything
 * written with a fixed number of digits, and that is what a person types.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

typedef struct { s64 v; int scale; } num_t;

static void try_help(void)
{
    dprintf(STDERR_FILENO, "Try 'seq --help' for more information.\n");
}

static s64 pow10s(int n)
{
    s64 p = 1;
    while (n-- > 0) p *= 10;
    return p;
}

/* [+-]?digits[.digits]?([eE][+-]?digits)? -> value and decimal places */
static bool parse_num(const char *s, num_t *out)
{
    bool neg = false;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    if (!*s) return false;

    s64 v = 0;
    int scale = 0, digits = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); digits++; }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); scale++; digits++; }
    }
    if (!digits) return false;

    if (*s == 'e' || *s == 'E') {
        s++;
        bool eneg = false;
        if (*s == '+' || *s == '-') { eneg = (*s == '-'); s++; }
        if (!(*s >= '0' && *s <= '9')) return false;
        int e = 0;
        while (*s >= '0' && *s <= '9') e = e * 10 + (*s++ - '0');
        if (e > 18) return false;
        if (eneg) scale += e;
        else {
            int take = e < scale ? e : scale;
            scale -= take;
            v *= pow10s(e - take);
        }
    }
    if (*s) return false;

    out->v = neg ? -v : v;
    out->scale = scale;
    return true;
}

static void rescale(num_t *n, int to)
{
    if (to > n->scale) {
        n->v *= pow10s(to - n->scale);
        n->scale = to;
    }
}

/* A scaled integer as text, zero-padded to `width` after any sign. */
static int fmt_num(char *out, size_t cap, s64 v, int scale, int width)
{
    char body[64];
    bool neg = v < 0;
    u64  a   = neg ? (u64)(-v) : (u64)v;
    int  n   = 0;

    if (scale == 0) {
        n = snprintf(body, sizeof body, "%llu", (unsigned long long)a);
    } else {
        u64 p = (u64)pow10s(scale);
        n = snprintf(body, sizeof body, "%llu.%0*llu",
                     (unsigned long long)(a / p), scale,
                     (unsigned long long)(a % p));
    }
    if (n < 0) n = 0;

    int pad = width - n - (neg ? 1 : 0);
    if (pad < 0) pad = 0;

    size_t k = 0;
    if (neg && k < cap - 1) out[k++] = '-';
    while (pad-- > 0 && k < cap - 1) out[k++] = '0';
    for (int i = 0; i < n && k < cap - 1; i++) out[k++] = body[i];
    out[k] = '\0';
    return (int)k;
}

/* -f: enough of printf to cover what people write, which is a width and
 * a precision on one conversion. Anything else is refused rather than
 * quietly printed wrong. */
static bool user_format(const char *f, char *out, size_t cap, s64 v, int scale)
{
    const char *p = strchr(f, '%');
    if (!p) return false;
    const char *q = p + 1;
    char flag = ' ';
    if (*q == '0' || *q == '-' || *q == '+') flag = *q++;
    int width = 0;
    while (*q >= '0' && *q <= '9') width = width * 10 + (*q++ - '0');
    int prec = -1;
    if (*q == '.') { q++; prec = 0; while (*q >= '0' && *q <= '9') prec = prec * 10 + (*q++ - '0'); }
    char conv = *q;
    if (conv != 'f' && conv != 'd' && conv != 'i' && conv != 'g') return false;

    int want = (conv == 'f') ? (prec < 0 ? 6 : prec)
             : (conv == 'g') ? scale
             : 0;
    /* Move the value to the number of decimals the format asked for. */
    s64 nv = v;
    if (want > scale)      nv *= pow10s(want - scale);
    else if (want < scale) {
        s64 d = pow10s(scale - want);
        nv = (nv < 0) ? -((-nv + d / 2) / d) : (nv + d / 2) / d;
    }

    char body[80];
    fmt_num(body, sizeof body, nv, want, flag == '0' ? width : 0);
    int blen = (int)strlen(body);

    size_t k = 0;
    for (const char *r = f; r < p && k < cap - 1; r++) out[k++] = *r;
    if (flag != '0' && flag != '-')
        for (int i = blen; i < width && k < cap - 1; i++) out[k++] = ' ';
    for (int i = 0; i < blen && k < cap - 1; i++) out[k++] = body[i];
    if (flag == '-')
        for (int i = blen; i < width && k < cap - 1; i++) out[k++] = ' ';
    for (const char *r = q + 1; *r && k < cap - 1; r++) out[k++] = *r;
    out[k] = '\0';
    return true;
}

static void usage(int fd)
{
    dprintf(fd, "Usage: seq [OPTION]... LAST\n"
                "  or:  seq [OPTION]... FIRST LAST\n"
                "  or:  seq [OPTION]... FIRST INCREMENT LAST\n"
                "Print numbers from FIRST to LAST, in steps of INCREMENT.\n\n"
                "  -f, --format=FORMAT      use printf style floating-point FORMAT\n"
                "  -s, --separator=STRING   use STRING to separate numbers (default: \\n)\n"
                "  -w, --equal-width        equalize width by padding with leading zeroes\n"
                "      --help     display this help and exit\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "format", 1, 'f' }, { "separator", 1, 's' },
        { "equal-width", 0, 'w' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    const char *sep = "\n", *format = NULL;
    bool pad = false;

    lp_getopt_t g;
    lp_getopt_init_ex(&g, argc, argv, "f:s:w", lo, LP_GETOPT_NEGNUM);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 's': sep = g.arg; break;
        case 'f': format = g.arg; break;
        case 'w': pad = true; break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default: lp_getopt_err("seq", &g); return 1;
        }
    }

    int n = argc - g.ind;
    if (n == 0)  { dprintf(STDERR_FILENO, "seq: missing operand\n"); try_help(); return 1; }
    if (n > 3)   { dprintf(STDERR_FILENO, "seq: extra operand '%s'\n", argv[g.ind + 3]);
                   try_help(); return 1; }

    num_t first = { 1, 0 }, step = { 1, 0 }, last = { 0, 0 };
    const char *sfirst = "1", *sstep = "1", *slast;
    if (n == 1)      { slast = argv[g.ind]; }
    else if (n == 2) { sfirst = argv[g.ind]; slast = argv[g.ind + 1]; }
    else             { sfirst = argv[g.ind]; sstep = argv[g.ind + 1]; slast = argv[g.ind + 2]; }

    const char *bad = NULL;
    if (!parse_num(sfirst, &first)) bad = sfirst;
    else if (!parse_num(sstep, &step)) bad = sstep;
    else if (!parse_num(slast, &last)) bad = slast;
    if (bad) {
        dprintf(STDERR_FILENO, "seq: invalid floating point argument: '%s'\n", bad);
        try_help();
        return 1;
    }
    if (step.v == 0) {
        dprintf(STDERR_FILENO, "seq: invalid Zero increment value: '%s'\n", sstep);
        try_help();
        return 1;
    }

    int scale = first.scale;
    if (step.scale > scale) scale = step.scale;
    if (last.scale > scale) scale = last.scale;
    rescale(&first, scale); rescale(&step, scale); rescale(&last, scale);

    int width = 0;
    if (pad) {
        char a[80], b[80];
        int la = fmt_num(a, sizeof a, first.v, scale, 0);
        int lb = fmt_num(b, sizeof b, last.v, scale, 0);
        width = la > lb ? la : lb;
    }

    char buf[128];
    bool any = false;
    for (s64 v = first.v; step.v > 0 ? v <= last.v : v >= last.v; v += step.v) {
        if (any) fputs(sep, STDOUT_FILENO);
        if (format) {
            if (!user_format(format, buf, sizeof buf, v, scale)) {
                dprintf(STDERR_FILENO, "seq: format '%s' is not supported here\n", format);
                return 1;
            }
        } else {
            fmt_num(buf, sizeof buf, v, scale, width);
        }
        fputs(buf, STDOUT_FILENO);
        any = true;
    }
    if (any) fputs("\n", STDOUT_FILENO);
    return 0;
}
