/* test, and its other name [ - the conditions a shell script asks.
 *
 *   test -f FILE          [ -f FILE ]
 *   test "$a" = "$b"      [ "$a" = "$b" ]
 *   test 3 -lt 4 -a -d /  and, or, negation, parentheses
 *
 * The shell has this built in, and every script still runs `[` often
 * enough that /bin/[ must exist: a `find -exec [ ... ]` and a crontab
 * line both reach for the file.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static char **av;
static int    ac, pos;
static bool   bad;

static const char *peek(void) { return pos < ac ? av[pos] : NULL; }
static const char *take(void) { return pos < ac ? av[pos++] : NULL; }
static bool at(const char *s) { const char *t = peek(); return t && strcmp(t, s) == 0; }

static bool parse_or(void);

static bool file_is(const char *path, u32 want, bool follow)
{
    lp_stat_t st;
    if (lp_stat(path, &st, follow) < 0) return false;
    return (st.mode & LP_S_IFMT) == want;
}

static bool unary(char op, const char *arg)
{
    lp_stat_t st;
    switch (op) {
    case 'e': return lp_exists(arg);
    case 'f': return file_is(arg, LP_S_IFREG, true);
    case 'd': return file_is(arg, LP_S_IFDIR, true);
    case 'L': case 'h': return file_is(arg, LP_S_IFLNK, false);
    case 'b': return file_is(arg, LP_S_IFBLK, true);
    case 'c': return file_is(arg, LP_S_IFCHR, true);
    case 'p': return file_is(arg, LP_S_IFIFO, true);
    case 'S': return file_is(arg, LP_S_IFSOCK, true);
    case 'r': return lp_access(arg, R_OK) == 0;
    case 'w': return lp_access(arg, W_OK) == 0;
    case 'x': return lp_access(arg, X_OK) == 0;
    case 's': return lp_stat(arg, &st, true) == 0 && st.size > 0;
    case 'z': return arg[0] == '\0';
    case 'n': return arg[0] != '\0';
    case 't': return lp_isatty(atoi(arg));
    case 'u': return lp_stat(arg, &st, true) == 0 && (st.mode & 04000);
    case 'g': return lp_stat(arg, &st, true) == 0 && (st.mode & 02000);
    case 'k': return lp_stat(arg, &st, true) == 0 && (st.mode & 01000);
    case 'O': return lp_stat(arg, &st, true) == 0 && (int)st.uid == lp_getuid();
    case 'G': return lp_stat(arg, &st, true) == 0 && (int)st.gid == lp_getgid();
    default:  bad = true; return false;
    }
}

static bool is_unary_op(const char *s)
{
    return s && s[0] == '-' && s[1] && !s[2] &&
           strchr("efdLhbcpSrwxszntugkOG", s[1]) != NULL;
}

static bool is_binary_op(const char *s)
{
    if (!s) return false;
    if (strcmp(s, "=") == 0 || strcmp(s, "==") == 0 || strcmp(s, "!=") == 0 ||
        strcmp(s, "<") == 0 || strcmp(s, ">") == 0) return true;
    static const char *ops[] = { "-eq","-ne","-lt","-le","-gt","-ge",
                                 "-nt","-ot","-ef", NULL };
    for (int i = 0; ops[i]; i++) if (strcmp(s, ops[i]) == 0) return true;
    return false;
}

static long num(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end) bad = true;
    return v;
}

static bool binary(const char *a, const char *op, const char *b)
{
    if (strcmp(op, "=")  == 0 || strcmp(op, "==") == 0) return strcmp(a, b) == 0;
    if (strcmp(op, "!=") == 0) return strcmp(a, b) != 0;
    if (strcmp(op, "<")  == 0) return strcmp(a, b) <  0;
    if (strcmp(op, ">")  == 0) return strcmp(a, b) >  0;

    if (strcmp(op, "-nt") == 0 || strcmp(op, "-ot") == 0 || strcmp(op, "-ef") == 0) {
        lp_stat_t sa, sb;
        if (lp_stat(a, &sa, true) < 0 || lp_stat(b, &sb, true) < 0) return false;
        if (strcmp(op, "-nt") == 0) return sa.mtime > sb.mtime;
        if (strcmp(op, "-ot") == 0) return sa.mtime < sb.mtime;
        return sa.dev == sb.dev && sa.ino == sb.ino;
    }

    long x = num(a), y = num(b);
    if (strcmp(op, "-eq") == 0) return x == y;
    if (strcmp(op, "-ne") == 0) return x != y;
    if (strcmp(op, "-lt") == 0) return x <  y;
    if (strcmp(op, "-le") == 0) return x <= y;
    if (strcmp(op, "-gt") == 0) return x >  y;
    if (strcmp(op, "-ge") == 0) return x >= y;
    bad = true;
    return false;
}

static bool parse_primary(void)
{
    if (at("!")) { take(); return !parse_primary(); }
    if (at("(")) {
        take();
        bool v = parse_or();
        if (at(")")) take(); else bad = true;
        return v;
    }

    const char *t = peek();
    if (!t) { bad = true; return false; }

    /* "-f" alone is a string, not a broken -f test. The operator form
     * only counts when there is something after it to test. */
    if (is_unary_op(t) && pos + 1 < ac && !is_binary_op(av[pos + 1])) {
        take();
        return unary(t[1], take());
    }
    if (pos + 2 < ac + 1 && is_binary_op(pos + 1 < ac ? av[pos + 1] : NULL)) {
        const char *a = take();
        const char *op = take();
        const char *b = take();
        if (!b) { bad = true; return false; }
        return binary(a, op, b);
    }
    take();
    return t[0] != '\0';
}

static bool parse_and(void)
{
    bool v = parse_primary();
    while (at("-a")) { take(); bool r = parse_primary(); v = v && r; }
    return v;
}

static bool parse_or(void)
{
    bool v = parse_and();
    while (at("-o")) { take(); bool r = parse_and(); v = v || r; }
    return v;
}

int main(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];

    if (strcmp(base, "[") == 0) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            dprintf(STDERR_FILENO, "[: missing ']'\n");
            return 2;
        }
        argc--;                       /* drop the ] */
    } else if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: test EXPRESSION\n"
               "  or:  [ EXPRESSION ]\n"
               "Exit with the status determined by EXPRESSION.\n\n"
               "  -e FILE   exists          -f FILE   is a regular file\n"
               "  -d FILE   is a directory  -L FILE   is a symbolic link\n"
               "  -r/-w/-x FILE  readable, writable, executable\n"
               "  -s FILE   is not empty    -t FD     FD is a terminal\n"
               "  -z STR    is empty        -n STR    is not empty\n"
               "  S1 = S2   equal strings   S1 != S2  different strings\n"
               "  N1 -eq N2  also -ne -lt -le -gt -ge\n"
               "  F1 -nt F2  newer, -ot older, -ef the same file\n"
               "  ! EXPR    negation, EXPR -a EXPR and, EXPR -o EXPR or\n");
        return 0;
    }

    if (argc == 1) return 1;

    av = argv + 1;
    ac = argc - 1;
    pos = 0;
    bad = false;

    bool v = parse_or();
    if (pos != ac) bad = true;
    if (bad) {
        dprintf(STDERR_FILENO, "%s: %s\n", base,
                pos < ac ? "too many arguments" : "missing argument");
        return 2;
    }
    return v ? 0 : 1;
}
