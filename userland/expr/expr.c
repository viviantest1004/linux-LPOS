/* expr - arithmetic and string tests for scripts.
 *
 *   expr 3 + 4
 *   expr "$s" : '.*'          how many characters matched
 *   expr length "$s"
 *   expr substr "$s" 2 3
 *
 * The shell here has $(( )), so the arithmetic is a convenience. The
 * string operators are not, and every script written on another system
 * uses them.
 *
 * The exit status is backwards from everything else and deliberately
 * so: 0 when the result is neither empty nor zero, 1 when it is, 2 when
 * the expression itself is wrong. Scripts test it that way.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "regex.h"

static char **av;
static int    ac, pos;

static void die(const char *msg)
{
    dprintf(STDERR_FILENO, "expr: %s\n", msg);
    lp_exit(2);
}

static const char *peek(void)     { return pos < ac ? av[pos] : NULL; }
static const char *take(void)     { return pos < ac ? av[pos++] : NULL; }
static bool at(const char *s)     { const char *t = peek(); return t && strcmp(t, s) == 0; }

/* The value type is s64, not long: on a 32-bit machine a long is four
 * bytes, and `expr 2147483647 + 1` answered -2147483648. GNU expr uses
 * intmax_t for the same reason. A command that answers differently on
 * one machine is worse than one that is missing. */
typedef struct { bool is_num; s64 num; char str[4096]; } val_t;

static val_t make_num(s64 v)
{
    val_t r; r.is_num = true; r.num = v; r.str[0] = '\0'; return r;
}
static val_t make_str(const char *s)
{
    val_t r; r.is_num = false; r.num = 0; strlcpy(r.str, s, sizeof r.str); return r;
}

static const char *text(const val_t *v, char *tmp, size_t cap)
{
    if (!v->is_num) return v->str;
    snprintf(tmp, cap, "%lld", (long long)v->num);
    return tmp;
}

static bool as_number(const val_t *v, s64 *out)
{
    if (v->is_num) { *out = v->num; return true; }
    const char *s = v->str;
    while (*s == ' ') s++;
    if (!*s) return false;
    char *end;
    s64 n = strtoll(s, &end, 10);
    if (*end) return false;
    *out = n;
    return true;
}

static s64 need_number(const val_t *v)
{
    s64 n;
    if (!as_number(v, &n)) die("non-integer argument");
    return n;
}

static bool truthy(const val_t *v)
{
    s64 n;
    if (as_number(v, &n)) return n != 0;
    return v->str[0] != '\0';
}

static val_t parse_or(void);

static val_t parse_primary(void)
{
    const char *t = take();
    if (!t) die("syntax error: unexpected end of expression");

    if (strcmp(t, "(") == 0) {
        val_t v = parse_or();
        if (!at(")")) die("syntax error: expecting ')'");
        take();
        return v;
    }
    if (strcmp(t, "length") == 0) {
        val_t a = parse_primary();
        char tmp[32];
        return make_num((s64)strlen(text(&a, tmp, sizeof tmp)));
    }
    if (strcmp(t, "substr") == 0) {
        val_t s = parse_primary(), p = parse_primary(), l = parse_primary();
        char tmp[32];
        const char *str = text(&s, tmp, sizeof tmp);
        s64 from, len, slen = (s64)strlen(str);
        if (!as_number(&p, &from) || !as_number(&l, &len)) return make_str("");
        if (from < 1 || from > slen || len < 1) return make_str("");
        if (from - 1 + len > slen) len = slen - (from - 1);
        char out[4096];
        if (len > (s64)sizeof out - 1) len = (s64)sizeof out - 1;
        memcpy(out, str + from - 1, (size_t)len);
        out[len] = '\0';
        return make_str(out);
    }
    if (strcmp(t, "index") == 0) {
        val_t s = parse_primary(), c = parse_primary();
        char t1[32], t2[32];
        const char *str = text(&s, t1, sizeof t1);
        const char *set = text(&c, t2, sizeof t2);
        for (s64 i = 0; str[i]; i++)
            if (strchr(set, str[i])) return make_num(i + 1);
        return make_num(0);
    }
    if (strcmp(t, "+") == 0) {           /* + WORD: this is a value, not an operator */
        const char *w = take();
        if (!w) die("syntax error");
        return make_str(w);
    }

    char *end;
    s64 n = strtoll(t, &end, 10);
    if (*t && !*end) return make_num(n);
    return make_str(t);
}

/* S : REGEXP - the length of the match, or what the first \(...\) caught. */
static val_t do_match(const val_t *left, const val_t *right)
{
    char t1[32], t2[32];
    const char *s = text(left, t1, sizeof t1);
    const char *p = text(right, t2, sizeof t2);

    /* expr anchors the pattern at the start whether it says so or not. */
    char anchored[4096];
    snprintf(anchored, sizeof anchored, "%s%s", p[0] == '^' ? "" : "^", p);

    const char *err = NULL;
    lpre *re = re_compile(anchored, false, false, &err);
    if (!re) die("invalid regular expression");

    bool grouped = strstr(p, "\\(") != NULL;
    int  caps[RE_MAX_CAPS];
    if (!re_search(re, s, 0, false, caps))
        return grouped ? make_str("") : make_num(0);

    if (grouped && caps[2] >= 0) {
        char out[4096];
        size_t k = (size_t)(caps[3] - caps[2]);
        if (k > sizeof out - 1) k = sizeof out - 1;
        memcpy(out, s + caps[2], k);
        out[k] = '\0';
        return make_str(out);
    }
    return make_num(caps[1] - caps[0]);
}

static val_t parse_match(void)
{
    val_t l = parse_primary();
    while (at(":")) {
        take();
        val_t r = parse_primary();
        l = do_match(&l, &r);
    }
    return l;
}

static val_t parse_mul(void)
{
    val_t l = parse_match();
    while (at("*") || at("/") || at("%")) {
        char op = take()[0];
        val_t r = parse_match();
        s64 a = need_number(&l), b = need_number(&r);
        if ((op == '/' || op == '%') && b == 0) die("division by zero");
        l = make_num(op == '*' ? a * b : op == '/' ? a / b : a % b);
    }
    return l;
}

static val_t parse_add(void)
{
    val_t l = parse_mul();
    while (at("+") || at("-")) {
        char op = take()[0];
        val_t r = parse_mul();
        s64 a = need_number(&l), b = need_number(&r);
        l = make_num(op == '+' ? a + b : a - b);
    }
    return l;
}

static val_t parse_cmp(void)
{
    val_t l = parse_add();
    for (;;) {
        const char *op = peek();
        if (!op) break;
        if (strcmp(op, "=") && strcmp(op, "==") && strcmp(op, "!=") &&
            strcmp(op, "<") && strcmp(op, "<=") &&
            strcmp(op, ">") && strcmp(op, ">=")) break;
        take();
        val_t r = parse_add();

        s64 a, b;
        int c;
        if (as_number(&l, &a) && as_number(&r, &b))
            c = (a < b) ? -1 : (a > b) ? 1 : 0;
        else {
            char t1[32], t2[32];
            c = strcmp(text(&l, t1, sizeof t1), text(&r, t2, sizeof t2));
            if (c < 0) c = -1; else if (c > 0) c = 1;
        }

        bool res =
            (strcmp(op, "=")  == 0 || strcmp(op, "==") == 0) ? (c == 0) :
            (strcmp(op, "!=") == 0) ? (c != 0) :
            (strcmp(op, "<")  == 0) ? (c <  0) :
            (strcmp(op, "<=") == 0) ? (c <= 0) :
            (strcmp(op, ">")  == 0) ? (c >  0) : (c >= 0);
        l = make_num(res ? 1 : 0);
    }
    return l;
}

static val_t parse_and(void)
{
    val_t l = parse_cmp();
    while (at("&")) {
        take();
        val_t r = parse_cmp();
        l = (truthy(&l) && truthy(&r)) ? l : make_num(0);
    }
    return l;
}

static val_t parse_or(void)
{
    val_t l = parse_and();
    while (at("|")) {
        take();
        val_t r = parse_and();
        if (!truthy(&l)) l = truthy(&r) ? r : make_num(0);
    }
    return l;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: expr EXPRESSION\n"
               "Print the value of EXPRESSION to standard output.\n\n"
               "  ARG1 | ARG2       ARG1 if it is neither null nor 0, otherwise ARG2\n"
               "  ARG1 & ARG2       ARG1 if neither argument is null or 0, otherwise 0\n"
               "  ARG1 < ARG2       comparison, also <= = != >= >\n"
               "  ARG1 + ARG2       arithmetic, also - * / %%\n"
               "  STRING : REGEXP   anchored pattern match of REGEXP in STRING\n"
               "  length STRING     length of STRING\n"
               "  substr STRING POS LENGTH   substring, POS counting from 1\n"
               "  index STRING CHARS         index in STRING where any CHARS is found\n\n"
               "Exit status is 0 if EXPRESSION is neither null nor 0, 1 if it is,\n"
               "2 if EXPRESSION is syntactically invalid.\n");
        return 0;
    }
    if (argc < 2) {
        dprintf(STDERR_FILENO, "expr: missing operand\n");
        dprintf(STDERR_FILENO, "Try 'expr --help' for more information.\n");
        return 2;
    }

    av = argv + 1;
    ac = argc - 1;
    pos = 0;

    val_t v = parse_or();
    if (pos != ac) die("syntax error: unexpected argument");

    char tmp[32];
    printf("%s\n", text(&v, tmp, sizeof tmp));
    return truthy(&v) ? 0 : 1;
}
