/* calc - integer calculator.
 *
 *   calc "1 + 2 * 3"        evaluate once, from the argument
 *   calc                    interactive (blank line or Ctrl-D to leave)
 *
 * Supported:
 *   arithmetic  + - * / % **      (** is power)
 *   bitwise     & | ^ ~ << >>
 *   grouping    ( )
 *   input       decimal, 0x hex, 0b binary
 *
 * Every result is shown in decimal, hex and binary at once. This project
 * deals with addresses and bit masks constantly, and converting by hand
 *
 * every time is worse. 64-bit integers only. No floating point - our
 * printf does not do it, and adding it would multiply the code size.
 *
 * A recursive descent parser, from lowest precedence down to highest:
 *   or -> xor -> and -> shift -> add -> mul -> unary -> power -> atom
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

typedef struct {
    const char *p;
    bool        error;
    const char *msg;
} parser_t;

static s64 parse_or(parser_t *ps);

static void fail(parser_t *ps, const char *msg)
{
    if (!ps->error) {          /* keep only the first error */
        ps->error = true;
        ps->msg = msg;
    }
}

static void skip_space(parser_t *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t') ps->p++;
}

/* Consume op and return true if it is next. Two-character operators must
 * be tested first, or ** reads as * and << reads as <. */
static bool eat(parser_t *ps, const char *op)
{
    skip_space(ps);
    size_t n = strlen(op);
    if (strncmp(ps->p, op, n) != 0)
        return false;
    ps->p += n;
    return true;
}

static s64 parse_atom(parser_t *ps)
{
    skip_space(ps);

    if (eat(ps, "(")) {
        s64 v = parse_or(ps);
        if (!eat(ps, ")"))
            fail(ps, "missing closing parenthesis");
        return v;
    }

    /* 0x / 0b prefixes */
    int base = 10;
    if (ps->p[0] == '0' && (ps->p[1] == 'x' || ps->p[1] == 'X')) {
        base = 16; ps->p += 2;
    } else if (ps->p[0] == '0' && (ps->p[1] == 'b' || ps->p[1] == 'B')) {
        base = 2;  ps->p += 2;
    }

    const char *start = ps->p;
    s64 v = 0;

    for (;;) {
        int d;
        char c = *ps->p;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c == '_')             { ps->p++; continue; }  /* 1_000_000 */
        else break;

        if (d >= base) break;
        v = v * base + d;
        ps->p++;
    }

    if (ps->p == start) {
        fail(ps, "expected a number");
        return 0;
    }
    return v;
}

/* Power is right associative: 2**3**2 = 2**(3**2) */
static s64 parse_power(parser_t *ps)
{
    s64 base = parse_atom(ps);
    if (!eat(ps, "**"))
        return base;

    s64 exp = parse_power(ps);
    if (exp < 0) {
        fail(ps, "a negative exponent has no integer result");
        return 0;
    }

    s64 r = 1;
    while (exp-- > 0) {
        r *= base;
        if (exp > 62) { fail(ps, "too large"); return 0; }
    }
    return r;
}

static s64 parse_unary(parser_t *ps)
{
    skip_space(ps);
    if (eat(ps, "-")) return -parse_unary(ps);
    if (eat(ps, "+")) return  parse_unary(ps);
    if (eat(ps, "~")) return ~parse_unary(ps);
    return parse_power(ps);
}

static s64 parse_mul(parser_t *ps)
{
    s64 v = parse_unary(ps);
    for (;;) {
        skip_space(ps);
        /* ** is power, so it must not be read as * */
        if (ps->p[0] == '*' && ps->p[1] == '*') return v;

        if (eat(ps, "*")) { v *= parse_unary(ps); continue; }
        if (eat(ps, "/")) {
            s64 d = parse_unary(ps);
            if (d == 0) { fail(ps, "division by zero"); return 0; }
            v /= d; continue;
        }
        if (eat(ps, "%")) {
            s64 d = parse_unary(ps);
            if (d == 0) { fail(ps, "division by zero"); return 0; }
            v %= d; continue;
        }
        return v;
    }
}

static s64 parse_add(parser_t *ps)
{
    s64 v = parse_mul(ps);
    for (;;) {
        skip_space(ps);
        if (eat(ps, "+")) { v += parse_mul(ps); continue; }
        if (eat(ps, "-")) { v -= parse_mul(ps); continue; }
        return v;
    }
}

static s64 parse_shift(parser_t *ps)
{
    s64 v = parse_add(ps);
    for (;;) {
        if (eat(ps, "<<")) { v = (s64)((u64)v << (parse_add(ps) & 63)); continue; }
        if (eat(ps, ">>")) { v = (s64)((u64)v >> (parse_add(ps) & 63)); continue; }
        return v;
    }
}

/* & | ^ bind less tightly than << >>, as in C */
static s64 parse_and(parser_t *ps)
{
    s64 v = parse_shift(ps);
    while (eat(ps, "&")) v &= parse_shift(ps);
    return v;
}

static s64 parse_xor(parser_t *ps)
{
    s64 v = parse_and(ps);
    for (;;) {
        skip_space(ps);
        if (ps->p[0] == '^' && ps->p[1] != '^') { ps->p++; v ^= parse_and(ps); continue; }
        return v;
    }
}

static s64 parse_or(parser_t *ps)
{
    s64 v = parse_xor(ps);
    while (eat(ps, "|")) v |= parse_xor(ps);
    return v;
}

/* Binary: drop leading zeros and group in fours */
static void print_binary(u64 v)
{
    if (v == 0) { printf("0"); return; }

    int top = 63;
    while (top > 0 && !((v >> top) & 1)) top--;

    for (int i = top; i >= 0; i--) {
        printf("%c", ((v >> i) & 1) ? '1' : '0');
        if (i && i % 4 == 0) printf("_");
    }
}

static void show(s64 v)
{
    printf("  %ld\n", (long)v);
    printf("  0x%lx\n", (unsigned long)v);
    printf("  0b"); print_binary((u64)v); printf("\n");
}

static int evaluate(const char *expr, bool verbose)
{
    parser_t ps = { .p = expr, .error = false, .msg = NULL };
    s64 v = parse_or(&ps);

    skip_space(&ps);
    if (!ps.error && *ps.p)
        fail(&ps, "unexpected characters after the expression");

    if (ps.error) {
        dprintf(STDERR_FILENO, "calc: %s\n", ps.msg);
        if (*ps.p)
            dprintf(STDERR_FILENO, "      -> %s\n", ps.p);
        return 1;
    }

    if (verbose) show(v);
    else         printf("%ld\n", (long)v);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        /* Join all arguments: the shell splits them on spaces. */
        char expr[512];
        expr[0] = '\0';
        for (int i = 1; i < argc; i++) {
            strlcpy(expr + strlen(expr), argv[i], sizeof(expr) - strlen(expr));
            if (i + 1 < argc)
                strlcpy(expr + strlen(expr), " ", sizeof(expr) - strlen(expr));
        }
        return evaluate(expr, true);
    }

    printf("calc - integer calculator.  Blank line or Ctrl-D to leave.\n");
    printf("  + - * / %% **   & | ^ ~ << >>   ( )   0x.. 0b..\n");
    printf("  ^ is xor, as in C. Powers are **, so 2**10 is 1024.\n\n");

    char line[512];
    for (;;) {
        printf("calc> ");
        long n = readline(STDIN_FILENO, line, sizeof(line));
        if (n < 0) { printf("\n"); break; }
        if (n == 0) break;
        evaluate(line, true);
    }
    return 0;
}
