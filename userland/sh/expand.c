/* expand.c - the expansion engine. Part of sh.c; see sh.h.
 *
 * The parser (parse.c) resolved quoting once and left every word as a
 * list of parts: literal runs marked quoted or not, $parameters, command
 * substitutions (parsed into trees), arithmetic, and the (a b c) of an
 * array literal. This file turns one such word into the fields a command
 * actually receives, in the order POSIX XCU 2.6 lays down:
 *
 *   1. brace expansion      {a,b}  {1..5}     (bash, textual, done first)
 *   2. tilde expansion      ~  ~user  (and after : in an assignment)
 *   3. parameter expansion  $x  ${x...}
 *      command substitution $(...)  `...`
 *      arithmetic expansion $((...))
 *   4. field splitting      on the unquoted results of step 3, using IFS
 *   5. pathname expansion   * ? [ ]   (unless -f)
 *   6. quote removal        already done by the lexer for us
 *
 * The one thing that makes this more than string concatenation is that
 * steps 4 and 5 must act only on characters that came from an *unquoted*
 * expansion or an unquoted literal. So a field is built as text plus a
 * parallel mask (one byte per char: 1 = protected, came from something
 * quoted; 0 = open to splitting and globbing). Splitting reads the mask;
 * globbing turns the field into a pattern by escaping the protected
 * metacharacters, so `"*"` never lists the directory but `$x` holding a
 * star does. This is the design dash uses (its CTLESC bytes), written
 * out as a separate array because bytes with the top bit set are ordinary
 * in a UTF-8 world and cannot double as control codes here. */

/* Set whenever an expansion fails in a way that must abandon the command:
 * ${x?msg}, a bad substitution, a readonly assignment, division by zero.
 * The caller checks it after expand_words / expand_str. */
static bool expand_error;

/* ── the positional and special parameters ─────────────────────────── */

/* "$-": the single-letter options that are on, in the order bash prints
 * them. Kept in a static buffer; one reader at a time, which is always
 * the case during expansion. */
static const char *dash_opts(void)
{
    static char buf[NOPTS + 1];
    size_t n = 0;
    static const struct { int o; char c; } letters[] = {
        { O_errexit, 'e' }, { O_noglob, 'f' }, { O_hashall, 'h' },
        { O_monitor, 'm' }, { O_noexec, 'n' }, { O_interactive, 'i' },
        { O_nounset, 'u' }, { O_verbose, 'v' }, { O_xtrace, 'x' },
        { O_noclobber, 'C' },
    };
    for (unsigned i = 0; i < sizeof letters / sizeof *letters; i++)
        if (opt[letters[i].o])
            buf[n++] = letters[i].c;
    buf[n] = '\0';
    return buf;
}

/* The value of a one-character special parameter, or NULL if `name` is
 * not one. $@ and $* are handled by the field builder, not here. */
static const char *special_param(const char *name, char *scratch)
{
    if (name[1] != '\0')
        return NULL;
    switch (name[0]) {
    case '?': return itoa_s(exitstatus, scratch);
    case '#': return itoa_s(posc, scratch);
    case '$': return itoa_s(rootpid, scratch);
    case '!': return itoa_s(lastbgpid, scratch);
    case '-': return dash_opts();
    case '0': return arg0 ? arg0 : "sh";
    default:  return NULL;
    }
}

/* $1..$9 and, inside braces, $10 and up. Returns NULL when out of range,
 * which the caller treats as unset. */
static const char *positional(long n)
{
    if (n < 1 || n > posc)
        return NULL;
    return posv[n - 1];
}

/* Is `name` a run of digits? Then it is a positional parameter. */
static bool all_digits(const char *s)
{
    if (!*s)
        return false;
    for (; *s; s++)
        if (!is_digit((u8)*s))
            return false;
    return true;
}

/* ── arithmetic ($(( )) and array subscripts) ──────────────────────── *
 *
 * A full C-precedence integer evaluator over long long. It reads and
 * writes shell variables: a bare name is its value, `x = e` and the
 * compound forms assign, `++`/`--` step an lvalue. A variable whose value
 * is itself an expression is evaluated as one (bash does this: a=3+4;
 * echo $((a)) prints 7), with a small recursion limit so a=a does not
 * spin. Errors set expand_error and stop; nothing here can crash the
 * shell, which the old string-scanner could not promise. */

typedef struct {
    const char *p;
    bool        ok;
    int         depth;      /* variable-in-variable recursion guard */
} axs_t;

static long long ax_assign_expr(axs_t *a);   /* lowest precedence */

static void ax_skip(axs_t *a)
{
    while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n')
        a->p++;
}

static void ax_fail(axs_t *a, const char *msg)
{
    if (a->ok) {
        a->ok = false;
        sh_warn("%s: %s", "arithmetic", msg);
        expand_error = true;
    }
}

/* Evaluate a variable's value as arithmetic (bash's recursive rule). An
 * empty or unset variable is 0. */
static long long ax_var_value(axs_t *a, const char *name)
{
    const char *v = var_get(name);
    if (!v || !*v)
        return 0;
    char *end;
    long long n = strtoll(v, &end, 0);
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end == '\0')
        return n;               /* a plain number: the common case */
    if (a->depth > 32) {
        ax_fail(a, "expression recursion too deep");
        return 0;
    }
    axs_t inner = { v, true, a->depth + 1 };
    long long r = ax_assign_expr(&inner);
    if (!inner.ok)
        a->ok = false;
    return r;
}

/* Read an identifier at a->p into name (bounded); return its length or 0. */
static size_t ax_name(axs_t *a, char *name, size_t cap)
{
    size_t n = 0;
    if (!is_name_start((u8)*a->p))
        return 0;
    while (is_name_char((u8)*a->p) && n + 1 < cap)
        name[n++] = *a->p++;
    name[n] = '\0';
    return n;
}

static void ax_store(axs_t *a, const char *name, long long v)
{
    char buf[24];
    if (!var_set(name, itoa_s(v, buf), 0))
        a->ok = false;          /* readonly */
}

static long long ax_primary(axs_t *a)
{
    ax_skip(a);
    if (*a->p == '(') {
        a->p++;
        long long v = ax_assign_expr(a);
        ax_skip(a);
        if (*a->p == ')')
            a->p++;
        else
            ax_fail(a, "expecting ')'");
        return v;
    }
    /* pre-increment / pre-decrement */
    if ((a->p[0] == '+' && a->p[1] == '+') ||
        (a->p[0] == '-' && a->p[1] == '-')) {
        int add = a->p[0] == '+' ? 1 : -1;
        a->p += 2;
        ax_skip(a);
        char name[64];
        if (!ax_name(a, name, sizeof name)) {
            ax_fail(a, "expecting a name after ++ / --");
            return 0;
        }
        long long v = ax_var_value(a, name) + add;
        ax_store(a, name, v);
        return v;
    }
    if (*a->p == '!') { a->p++; return !ax_primary(a); }
    if (*a->p == '~') { a->p++; return ~ax_primary(a); }
    if (*a->p == '-') { a->p++; return -ax_primary(a); }
    if (*a->p == '+') { a->p++; return  ax_primary(a); }

    if (is_digit((u8)*a->p)) {
        char *end;
        long long v = strtoll(a->p, &end, 0);    /* 0x.., 0.., decimal */
        a->p = end;
        return v;
    }
    if (is_name_start((u8)*a->p)) {
        char name[64];
        ax_name(a, name, sizeof name);
        ax_skip(a);
        /* assignment forms are handled by ax_assign_expr; here we may
         * still see a post-increment. */
        if ((a->p[0] == '+' && a->p[1] == '+') ||
            (a->p[0] == '-' && a->p[1] == '-')) {
            int add = a->p[0] == '+' ? 1 : -1;
            a->p += 2;
            long long v = ax_var_value(a, name);
            ax_store(a, name, v + add);
            return v;
        }
        return ax_var_value(a, name);
    }
    ax_fail(a, "syntax error in expression");
    return 0;
}

static long long ax_mul(axs_t *a)
{
    long long v = ax_primary(a);
    for (;;) {
        ax_skip(a);
        char op = *a->p;
        if ((op == '*' && a->p[1] != '=') ||
            (op == '/' && a->p[1] != '=') ||
            (op == '%' && a->p[1] != '=')) {
            a->p++;
            long long r = ax_primary(a);
            if ((op == '/' || op == '%') && r == 0) {
                ax_fail(a, "division by 0");
                return 0;
            }
            v = op == '*' ? v * r : op == '/' ? v / r : v % r;
        } else {
            return v;
        }
    }
}

static long long ax_add(axs_t *a)
{
    long long v = ax_mul(a);
    for (;;) {
        ax_skip(a);
        if (a->p[0] == '+' && a->p[1] != '=' && a->p[1] != '+') {
            a->p++; v += ax_mul(a);
        } else if (a->p[0] == '-' && a->p[1] != '=' && a->p[1] != '-') {
            a->p++; v -= ax_mul(a);
        } else {
            return v;
        }
    }
}

static long long ax_shift(axs_t *a)
{
    long long v = ax_add(a);
    for (;;) {
        ax_skip(a);
        if (a->p[0] == '<' && a->p[1] == '<' && a->p[2] != '=') {
            a->p += 2; v <<= ax_add(a);
        } else if (a->p[0] == '>' && a->p[1] == '>' && a->p[2] != '=') {
            a->p += 2; v >>= ax_add(a);
        } else {
            return v;
        }
    }
}

static long long ax_rel(axs_t *a)
{
    long long v = ax_shift(a);
    for (;;) {
        ax_skip(a);
        if (a->p[0] == '<' && a->p[1] == '=') { a->p += 2; v = v <= ax_shift(a); }
        else if (a->p[0] == '>' && a->p[1] == '=') { a->p += 2; v = v >= ax_shift(a); }
        else if (a->p[0] == '<' && a->p[1] != '<') { a->p += 1; v = v < ax_shift(a); }
        else if (a->p[0] == '>' && a->p[1] != '>') { a->p += 1; v = v > ax_shift(a); }
        else return v;
    }
}

static long long ax_eq(axs_t *a)
{
    long long v = ax_rel(a);
    for (;;) {
        ax_skip(a);
        if (a->p[0] == '=' && a->p[1] == '=') { a->p += 2; v = v == ax_rel(a); }
        else if (a->p[0] == '!' && a->p[1] == '=') { a->p += 2; v = v != ax_rel(a); }
        else return v;
    }
}

static long long ax_band(axs_t *a)
{
    long long v = ax_eq(a);
    while (ax_skip(a), a->p[0] == '&' && a->p[1] != '&' && a->p[1] != '=') {
        a->p++; v &= ax_eq(a);
    }
    return v;
}

static long long ax_bxor(axs_t *a)
{
    long long v = ax_band(a);
    while (ax_skip(a), a->p[0] == '^' && a->p[1] != '=') {
        a->p++; v ^= ax_band(a);
    }
    return v;
}

static long long ax_bor(axs_t *a)
{
    long long v = ax_bxor(a);
    while (ax_skip(a), a->p[0] == '|' && a->p[1] != '|' && a->p[1] != '=') {
        a->p++; v |= ax_bxor(a);
    }
    return v;
}

static long long ax_land(axs_t *a)
{
    long long v = ax_bor(a);
    while (ax_skip(a), a->p[0] == '&' && a->p[1] == '&') {
        a->p += 2;
        long long r = ax_bor(a);        /* no short circuit of side effects
                                         * matters here: still evaluate for
                                         * assignment consistency with bash */
        v = v && r;
    }
    return v;
}

static long long ax_lor(axs_t *a)
{
    long long v = ax_land(a);
    while (ax_skip(a), a->p[0] == '|' && a->p[1] == '|') {
        a->p += 2;
        long long r = ax_land(a);
        v = v || r;
    }
    return v;
}

static long long ax_cond(axs_t *a)
{
    long long c = ax_lor(a);
    ax_skip(a);
    if (*a->p != '?')
        return c;
    a->p++;
    long long t = ax_assign_expr(a);
    ax_skip(a);
    if (*a->p == ':')
        a->p++;
    else
        ax_fail(a, "expecting ':' in ?:");
    long long f = ax_cond(a);
    return c ? t : f;
}

/* Assignment is right-associative and needs the left side to be a bare
 * name, so it peeks: if what follows a name is one of the assignment
 * operators, it assigns; otherwise it hands back to the conditional
 * level with the name unconsumed. */
static long long ax_assign_expr(axs_t *a)
{
    ax_skip(a);
    const char *save = a->p;
    char name[64];
    if (is_name_start((u8)*a->p)) {
        ax_name(a, name, sizeof name);
        ax_skip(a);
        char op = a->p[0];
        const char *after = a->p;
        int kind = -1;          /* 0 plain, else the compound operator */
        if (op == '=' && a->p[1] != '=') { kind = 0; after = a->p + 1; }
        else if (a->p[1] == '=' && strchr("+-*/%&^|", op)) { kind = op; after = a->p + 2; }
        else if (op == '<' && a->p[1] == '<' && a->p[2] == '=') { kind = 'L'; after = a->p + 3; }
        else if (op == '>' && a->p[1] == '>' && a->p[2] == '=') { kind = 'R'; after = a->p + 3; }
        if (kind != -1) {
            a->p = after;
            long long rhs = ax_assign_expr(a);
            long long cur = kind == 0 ? 0 : ax_var_value(a, name);
            long long v;
            switch (kind) {
            case 0:   v = rhs; break;
            case '+': v = cur + rhs; break;
            case '-': v = cur - rhs; break;
            case '*': v = cur * rhs; break;
            case '/': if (!rhs) { ax_fail(a, "division by 0"); return 0; }
                      v = cur / rhs; break;
            case '%': if (!rhs) { ax_fail(a, "division by 0"); return 0; }
                      v = cur % rhs; break;
            case '&': v = cur & rhs; break;
            case '^': v = cur ^ rhs; break;
            case '|': v = cur | rhs; break;
            case 'L': v = cur << rhs; break;
            case 'R': v = cur >> rhs; break;
            default:  v = rhs; break;
            }
            ax_store(a, name, v);
            return v;
        }
        a->p = save;            /* not an assignment: rewind */
    }
    return ax_cond(a);
}

/* The comma operator, lowest of all. */
static long long ax_comma(axs_t *a)
{
    long long v = ax_assign_expr(a);
    for (;;) {
        ax_skip(a);
        if (*a->p != ',')
            return v;
        a->p++;
        v = ax_assign_expr(a);
    }
}

static bool arith_eval(const char *expr, long long *out)
{
    axs_t a = { expr, true, 0 };
    long long v = ax_comma(&a);
    ax_skip(&a);
    if (a.ok && *a.p != '\0') {
        ax_fail(&a, "syntax error in expression");
    }
    *out = v;
    return a.ok;
}

/* ── pattern matching (glob, case, ${x#pat}) ───────────────────────── *
 *
 * A pattern here already has quote removal done, with any character that
 * was quoted preceded by a backslash so it is matched literally. That is
 * how `case $x in \*) ` and `"${x#*}"` keep their metacharacter meaning
 * or lose it exactly as the quoting said. pmatch understands *, ?, the
 * bracket expression [..] with ranges, [!..]/[^..] negation and the
 * [:class:] names, and a leading ! or ^ after [ to negate. `pathname`
 * stops * and ? from matching a '/', which matters when a whole path is
 * matched at once. */

static bool cclass(const char *name, size_t n, int c)
{
    if (n == 5 && memcmp(name, "alpha", 5) == 0)
        return (c|32) >= 'a' && (c|32) <= 'z';
    if (n == 5 && memcmp(name, "digit", 5) == 0)
        return c >= '0' && c <= '9';
    if (n == 5 && memcmp(name, "alnum", 5) == 0)
        return ((c|32) >= 'a' && (c|32) <= 'z') || (c >= '0' && c <= '9');
    if (n == 5 && memcmp(name, "space", 5) == 0)
        return c == ' ' || (c >= '\t' && c <= '\r');
    if (n == 5 && memcmp(name, "upper", 5) == 0)
        return c >= 'A' && c <= 'Z';
    if (n == 5 && memcmp(name, "lower", 5) == 0)
        return c >= 'a' && c <= 'z';
    if (n == 5 && memcmp(name, "blank", 5) == 0)
        return c == ' ' || c == '\t';
    if (n == 5 && memcmp(name, "print", 5) == 0)
        return c >= 0x20 && c < 0x7f;
    if (n == 5 && memcmp(name, "graph", 5) == 0)
        return c > 0x20 && c < 0x7f;
    if (n == 5 && memcmp(name, "cntrl", 5) == 0)
        return c < 0x20 || c == 0x7f;
    if (n == 5 && memcmp(name, "punct", 5) == 0)
        return c >= 0x20 && c < 0x7f && !(((c|32) >= 'a' && (c|32) <= 'z') ||
               (c >= '0' && c <= '9') || c == ' ');
    if (n == 6 && memcmp(name, "xdigit", 6) == 0)
        return (c >= '0' && c <= '9') || ((c|32) >= 'a' && (c|32) <= 'f');
    return false;
}

/* Match one bracket expression against character c. On entry *pp points
 * just past '['. On success advances *pp past ']' and returns 1/0 for
 * match; returns -1 if the [ never closes (then it is a literal '['). */
static int bracket(const char **pp, int c)
{
    const char *p = *pp;
    bool neg = false;
    if (*p == '!' || *p == '^') { neg = true; p++; }
    const char *start = p;
    bool matched = false;
    for (;;) {
        if (*p == '\0')
            return -1;                          /* unterminated */
        if (*p == ']' && p != start)
            break;
        if (p[0] == '[' && p[1] == ':') {
            const char *e = strstr(p + 2, ":]");
            if (e) {
                if (cclass(p + 2, (size_t)(e - (p + 2)), c))
                    matched = true;
                p = e + 2;
                continue;
            }
        }
        int lo = (u8)*p;
        if (*p == '\\' && p[1]) { lo = (u8)p[1]; p++; }
        if (p[1] == '-' && p[2] != ']' && p[2] != '\0') {
            int hi = (u8)p[2];
            const char *q = p + 2;
            if (*q == '\\' && q[1]) { hi = (u8)q[1]; q++; }
            if (c >= lo && c <= hi)
                matched = true;
            p = q + 1;
        } else {
            if (c == lo)
                matched = true;
            p++;
        }
    }
    *pp = p + 1;                                /* past ']' */
    return matched != neg;
}

static bool pmatch(const char *pat, const char *s, bool pathname)
{
    while (*pat) {
        if (*pat == '\\' && pat[1]) {           /* escaped: literal */
            if (*s != pat[1])
                return false;
            pat += 2; s++;
            continue;
        }
        if (*pat == '*') {
            while (*pat == '*')
                pat++;
            if (!*pat)
                return !pathname || !strchr(s, '/');
            for (const char *t = s; ; t++) {
                if (pmatch(pat, t, pathname))
                    return true;
                if (!*t)
                    return false;
                if (pathname && *t == '/')
                    return false;
            }
        }
        if (*pat == '?') {
            if (!*s || (pathname && *s == '/'))
                return false;
            pat++; s++;
            continue;
        }
        if (*pat == '[') {
            const char *pp = pat + 1;
            int r = bracket(&pp, (u8)*s);
            if (r >= 0) {
                if (!*s || (pathname && *s == '/') || r == 0)
                    return false;
                pat = pp; s++;
                continue;
            }
            /* unterminated [ is a literal [ */
        }
        if (*pat != *s)
            return false;
        pat++; s++;
    }
    return *s == '\0';
}

static bool has_glob_chars(const char *pat)
{
    for (const char *p = pat; *p; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '*' || *p == '?')
            return true;
        if (*p == '[' && strchr(p + 1, ']'))
            return true;
    }
    return false;
}

/* Drop the backslash escapes a pattern carries, leaving the literal
 * string it stands for. Used when a glob matches nothing: the word is
 * then the pattern with its quoting removed. */
static char *pat_unescape(const char *pat)
{
    strbuf_t b = {0};
    for (const char *p = pat; *p; p++) {
        if (*p == '\\' && p[1])
            p++;
        sb_putc(&b, *p);
    }
    return sb_take(&b);
}

/* ── tilde ─────────────────────────────────────────────────────────── */

static char *tilde_home(const char *user)
{
    if (!*user) {
        const char *h = var_get("HOME");
        return h ? xstrdup(h) : NULL;
    }
    lp_user_t u;
    if (lp_user_by_name(user, &u) && u.home[0])
        return xstrdup(u.home);
    return NULL;
}

/* ── pathname expansion ────────────────────────────────────────────── *
 *
 * The field is a pattern (protected characters escaped). If it holds an
 * active metacharacter, match it against the filesystem; a match of
 * nothing leaves the word as the literal (POSIX default, no nullglob).
 * Patterns with slashes are walked one component at a time so `/etc/*.d`
 * and `src/*/x.c` both work, which the old shell's last-component-only
 * glob could not do. */

static int glob_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void glob_sort(char **v, int n)
{
    for (int i = 1; i < n; i++) {
        char *t = v[i];
        int j = i - 1;
        while (j >= 0 && glob_cmp(&v[j], &t) > 0) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = t;
    }
}

/* linux_dirent64: d_ino(8) d_off(8) d_reclen(2) d_type(1) d_name[]. */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

/* Match `pat` (one path component, escaped) against the names in `dir`
 * (empty means the current directory) and push "<prefix><name>" for each
 * hit into out. `dot` says whether a leading-dot name may match (only
 * when the pattern's first char is a literal dot). */
static void glob_dir(const char *dir, const char *pat, const char *prefix,
                     strvec_t *out, bool want_dir)
{
    const char *opendir = dir[0] ? dir : ".";
    long fd = lp_open(opendir, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;
    bool dotpat = pat[0] == '.' || (pat[0] == '\\' && pat[1] == '.');
    strvec_t hits = {0};
    char buf[8192];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (long off = 0; off < n; ) {
            char *rec = buf + off;
            u16 reclen;
            memcpy(&reclen, rec + DIRENT_RECLEN, sizeof reclen);
            const char *name = rec + DIRENT_NAME;
            off += reclen;
            if (name[0] == '.' && !dotpat)
                continue;
            if (!pmatch(pat, name, false))
                continue;
            if (want_dir) {
                /* a trailing component that must be a directory: check */
                strbuf_t full = {0};
                if (prefix[0]) { sb_puts(&full, prefix); }
                sb_puts(&full, name);
                lp_stat_t st;
                bool isdir = lp_stat(sb_str(&full), &st, true) == 0 &&
                             (st.mode & 0170000) == 0040000;
                sb_free(&full);
                if (!isdir)
                    continue;
            }
            strbuf_t full = {0};
            if (prefix[0])
                sb_puts(&full, prefix);
            sb_puts(&full, name);
            sv_push(&hits, sb_take(&full));
        }
    }
    lp_close((int)fd);
    glob_sort(hits.v, hits.n);
    for (int i = 0; i < hits.n; i++)
        sv_push(out, hits.v[i]);
    xfree(hits.v);          /* the strings were handed to out */
}

/* Recursive walk. `rest` is the remaining pattern (escaped); `prefix` is
 * the concrete path built so far (unescaped). */
static void glob_walk(const char *prefix, const char *rest, strvec_t *out)
{
    /* Split off the first component of rest at an unescaped '/'. */
    const char *slash = NULL;
    for (const char *p = rest; *p; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '/') { slash = p; break; }
    }
    size_t clen = slash ? (size_t)(slash - rest) : strlen(rest);
    char comp[1024];
    if (clen >= sizeof comp)
        return;
    memcpy(comp, rest, clen);
    comp[clen] = '\0';

    if (!has_glob_chars(comp)) {
        /* a literal component: append and go on without touching disk */
        char *lit = pat_unescape(comp);
        strbuf_t np = {0};
        sb_puts(&np, prefix);
        sb_puts(&np, lit);
        xfree(lit);
        if (slash) {
            sb_putc(&np, '/');
            /* skip runs of slashes */
            const char *next = slash + 1;
            while (*next == '/') next++;
            if (*next == '\0') {
                /* trailing slash: only if the directory exists */
                lp_stat_t st;
                if (lp_stat(sb_str(&np), &st, true) == 0)
                    sv_push(out, sb_take(&np));
                else
                    sb_free(&np);
            } else {
                char *pfx = sb_take(&np);
                glob_walk(pfx, next, out);
                xfree(pfx);
            }
        } else {
            /* last component, literal: only emit if it exists */
            lp_stat_t st;
            if (lp_stat(sb_str(&np), &st, false) == 0)
                sv_push(out, sb_take(&np));
            else
                sb_free(&np);
        }
        return;
    }

    if (slash) {
        const char *next = slash + 1;
        while (*next == '/') next++;
        strvec_t dirs = {0};
        char pfx[1024];
        strlcpy(pfx, prefix, sizeof pfx);
        glob_dir(prefix, comp, pfx, &dirs, true);
        for (int i = 0; i < dirs.n; i++) {
            strbuf_t np = {0};
            sb_puts(&np, dirs.v[i]);
            sb_putc(&np, '/');
            if (*next == '\0') {
                sv_push(out, sb_take(&np));
            } else {
                char *p = sb_take(&np);
                glob_walk(p, next, out);
                xfree(p);
            }
        }
        sv_free(&dirs);
    } else {
        char pfx[1024];
        strlcpy(pfx, prefix, sizeof pfx);
        glob_dir(prefix, comp, pfx, out, false);
    }
}

static bool glob_expand(const char *pat, strvec_t *out)
{
    if (opt[O_noglob] || !has_glob_chars(pat))
        return false;
    int before = out->n;
    if (pat[0] == '/') {
        const char *p = pat + 1;
        while (*p == '/') p++;
        glob_walk("/", p, out);
    } else {
        glob_walk("", pat, out);
    }
    return out->n > before;
}

/* ── the field builder ─────────────────────────────────────────────── */

typedef struct {
    strvec_t  fields;       /* finished field texts */
    strvec_t  masks;        /* parallel: one byte per char, 1 = protected */
    strbuf_t  cur;
    strbuf_t  curmask;
    bool      started;      /* the current field has been opened */
    const char *ifs;
    int       flags;
} fb_t;

static void fb_init(fb_t *f, int flags)
{
    memset(f, 0, sizeof *f);
    f->ifs = ifs_value();
    f->flags = flags;
}

static bool ifs_ws(fb_t *f, int c)
{
    return (c == ' ' || c == '\t' || c == '\n') && strchr(f->ifs, c);
}

static bool ifs_has(fb_t *f, int c)
{
    return c && strchr(f->ifs, c) != NULL;
}

static void fb_pushchar(fb_t *f, char c, int protect)
{
    sb_putc(&f->cur, c);
    sb_putc(&f->curmask, protect ? 1 : 0);
    f->started = true;
}

static void fb_endfield(fb_t *f)
{
    if (!f->started)
        return;
    sv_push(&f->fields, sb_take(&f->cur));
    sv_push(&f->masks, sb_take(&f->curmask));
    memset(&f->cur, 0, sizeof f->cur);
    memset(&f->curmask, 0, sizeof f->curmask);
    f->started = false;
}

/* An explicitly empty field, for the gap between two non-whitespace IFS
 * delimiters (a::b with IFS=: is three fields). */
static void fb_empty_field(fb_t *f)
{
    sv_push(&f->fields, xstrdup(""));
    sv_push(&f->masks, xstrdup(""));
}

/* Append literal text that is not subject to splitting. */
static void fb_add_protected(fb_t *f, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        fb_pushchar(f, s[i], 1);
}

/* Append text that came from an unquoted literal - not split (a literal
 * space is a word boundary the parser already acted on), but its glob
 * metacharacters stay active (mask 0). */
static void fb_add_open_literal(fb_t *f, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        fb_pushchar(f, s[i], 0);
}

/* Append the result of an unquoted expansion, doing IFS field splitting.
 * Whitespace IFS runs collapse and are trimmed at the ends of the run of
 * text; a non-whitespace IFS character always makes a boundary. */
static void fb_add_split(fb_t *f, const char *s)
{
    if (!*f->ifs) {                     /* IFS empty: no splitting */
        for (const char *p = s; *p; p++)
            fb_pushchar(f, *p, 0);
        return;
    }
    const char *p = s;
    bool nonws_pending = false;         /* a non-ws sep just closed a field;
                                         * a second one makes an empty field */
    while (*p) {
        u8 c = (u8)*p;
        if (ifs_ws(f, c)) {
            /* whitespace IFS: end the field, skip the whole run, and if a
             * single non-ws IFS follows, it is part of this one delimiter */
            if (f->started)
                fb_endfield(f);
            while (*p && ifs_ws(f, (u8)*p))
                p++;
            if (*p && !ifs_ws(f, (u8)*p) && ifs_has(f, (u8)*p))
                p++;
            nonws_pending = false;
            continue;
        }
        if (ifs_has(f, c)) {            /* non-whitespace IFS: a boundary */
            if (f->started)
                fb_endfield(f);
            else if (nonws_pending)
                fb_empty_field(f);
            nonws_pending = true;
            p++;
            continue;
        }
        fb_pushchar(f, (char)c, 0);
        nonws_pending = false;
        p++;
    }
}

/* ── parameter expansion ───────────────────────────────────────────── */

static char *expand_word_str(word_t *w, int flags);    /* forward */

/* Look up the base value of a parameter part into *val (a borrowed or
 * scratch pointer) and say whether it was set. Arrays and $@/$* are
 * handled separately by the caller when they can produce many fields;
 * this returns their joined-with-space form for the scalar contexts
 * (${#x}, ${x:-..}). */
static bool param_base(wpart_t *p, char *scratch, const char **val,
                       strvec_t *multi, bool *is_multi)
{
    *is_multi = false;
    const char *name = p->text;

    /* $@ / $* and array[@] / array[*]: many values. */
    bool star = strcmp(name, "*") == 0, at = strcmp(name, "@") == 0;
    if (star || at) {
        *is_multi = true;
        for (int i = 0; i < posc; i++)
            sv_push(multi, xstrdup(posv[i]));
        return posc > 0;
    }
    if (p->sub && (p->sub->lit) &&
        (strcmp(p->sub->lit, "@") == 0 || strcmp(p->sub->lit, "*") == 0)) {
        var_t *v = var_lookup(name);
        *is_multi = true;
        if (v && (v->flags & V_ARRAY)) {
            for (int i = 0; i < v->arrn; i++)
                if (v->arr[i])
                    sv_push(multi, xstrdup(v->arr[i]));
            return v->arrn > 0;
        }
        const char *s = var_get(name);
        if (s) { sv_push(multi, xstrdup(s)); return true; }
        return false;
    }

    /* array element name[expr] */
    if (p->sub) {
        char *idxs = expand_word_str(p->sub, 0);
        long long idx = 0;
        arith_eval(idxs ? idxs : "0", &idx);
        xfree(idxs);
        var_t *v = var_lookup(name);
        const char *e = var_elem(v, idx);
        *val = e;
        return e != NULL;
    }

    const char *sp = special_param(name, scratch);
    if (sp) { *val = sp; return true; }
    if (all_digits(name)) {
        const char *pv = positional(strtoll(name, NULL, 10));
        *val = pv;
        return pv != NULL;
    }
    const char *v = var_get(name);
    *val = v;
    return v != NULL;
}

/* Count for ${#x}: characters for a scalar, elements for @ / * / array. */
static long param_count(wpart_t *p)
{
    const char *name = p->text;
    if (strcmp(name, "*") == 0 || strcmp(name, "@") == 0)
        return posc;
    if (p->sub && p->sub->lit &&
        (strcmp(p->sub->lit, "@") == 0 || strcmp(p->sub->lit, "*") == 0)) {
        var_t *v = var_lookup(name);
        if (v && (v->flags & V_ARRAY)) {
            long n = 0;
            for (int i = 0; i < v->arrn; i++)
                if (v->arr[i]) n++;
            return n;
        }
        return var_get(name) ? 1 : 0;
    }
    char scratch[24];
    const char *val = NULL;
    bool multi, ok = param_base(p, scratch, &val, NULL, &multi);
    (void)ok;
    return val ? (long)strlen(val) : 0;
}

/* Trim a prefix/suffix pattern from s (PO_RMPRE.. PO_RMSUF..). */
static char *trim_pattern(const char *s, const char *pat, int op)
{
    size_t len = strlen(s);
    if (op == PO_RMPRE || op == PO_RMPREL) {
        /* shortest / longest prefix that matches */
        int longest = op == PO_RMPREL;
        size_t best = 0;
        bool found = false;
        for (size_t i = 0; i <= len; i++) {
            char save = ((char *)s)[i];
            ((char *)s)[i] = '\0';
            bool m = pmatch(pat, s, false);
            ((char *)s)[i] = save;
            if (m) {
                best = i;
                found = true;
                if (!longest) break;
            }
        }
        return found ? xstrdup(s + best) : xstrdup(s);
    }
    /* suffix */
    int longest = op == PO_RMSUFL;
    size_t best = len;
    bool found = false;
    for (size_t i = len + 1; i-- > 0; ) {
        if (pmatch(pat, s + i, false)) {
            best = i;
            found = true;
            if (!longest) break;
        }
        if (longest && i == 0) break;
    }
    if (!found)
        return xstrdup(s);
    return xstrndup(s, best);
}

/* Replace matches of pat with rep in s (PO_REPL.. PO_REPLSUF..). */
static char *replace_pattern(const char *s, const char *pat, const char *rep,
                             int op)
{
    strbuf_t out = {0};
    size_t len = strlen(s);
    bool anchored_pre = op == PO_REPLPRE;
    bool anchored_suf = op == PO_REPLSUF;
    bool all = op == PO_REPLALL;
    bool done = false;
    for (size_t i = 0; i <= len; ) {
        if (!done && (!anchored_suf || i == 0) &&
            (!anchored_pre || i == 0)) {
            /* try the longest match starting at i */
            size_t mlen = 0;
            bool matched = false;
            for (size_t j = len; j + 1 > i; j--) {
                char save = ((char *)s)[j];
                ((char *)s)[j] = '\0';
                bool m = pmatch(pat, s + i, false);
                ((char *)s)[j] = save;
                if (m) {
                    if (anchored_suf && j != len) { m = false; }
                }
                if (m) { mlen = j - i; matched = true; break; }
                if (j == i) break;
            }
            if (matched) {
                sb_puts(&out, rep);
                if (mlen == 0) {
                    if (i < len) sb_putc(&out, s[i]);
                    i++;
                } else {
                    i += mlen;
                }
                if (!all) done = true;
                if (anchored_pre) done = true;
                continue;
            }
        }
        if (i < len) sb_putc(&out, s[i]);
        i++;
    }
    return sb_take(&out);
}

/* Substring ${x:off:len}. */
static char *substr(const char *s, long long off, bool have_len, long long len)
{
    long long slen = (long long)strlen(s);
    if (off < 0) { off += slen; if (off < 0) off = 0; }
    if (off > slen) off = slen;
    long long end;
    if (!have_len) end = slen;
    else if (len < 0) { end = slen + len; if (end < off) end = off; }
    else end = off + len;
    if (end > slen) end = slen;
    if (end < off) end = off;
    return xstrndup(s + off, (size_t)(end - off));
}

/* Upper/lower case modification. `first` limits it to the first char. */
static char *case_mod(const char *s, int op)
{
    strbuf_t b = {0};
    bool first = op == PO_UPPER1 || op == PO_LOWER1;
    bool up = op == PO_UPPER || op == PO_UPPER1;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if ((!first || p == s)) {
            if (up && c >= 'a' && c <= 'z') c = (char)(c - 32);
            else if (!up && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        }
        sb_putc(&b, c);
    }
    return sb_take(&b);
}

/* Expand one WP_PARAM part. Appends to the field builder. The heavy
 * lifting of the ${op} forms lands here. */
static void expand_param(fb_t *f, wpart_t *p, bool word_dq)
{
    char scratch[24];

    /* ${#x} - a length, always a scalar number. */
    if (p->flags & PF_LEN) {
        char buf[24];
        const char *n = itoa_s(param_count(p), buf);
        if (word_dq || p->quoted) fb_add_protected(f, n, strlen(n));
        else fb_add_split(f, n);
        return;
    }

    /* ${!x} indirect, ${!a[@]} keys. */
    if (p->flags & PF_KEYS) {
        var_t *v = var_lookup(p->text);
        strbuf_t b = {0};
        if (v && (v->flags & V_ARRAY)) {
            bool first = true;
            for (int i = 0; i < v->arrn; i++)
                if (v->arr[i]) {
                    if (!first) sb_putc(&b, ' ');
                    sb_printf(&b, "%d", i);
                    first = false;
                }
        } else if (var_get(p->text)) {
            sb_putc(&b, '0');
        }
        if (word_dq || p->quoted) fb_add_protected(f, sb_str(&b), b.len);
        else fb_add_split(f, sb_str(&b));
        sb_free(&b);
        return;
    }

    wpart_t local = *p;
    if (p->flags & PF_INDIRECT) {
        const char *nm = var_get(p->text);
        if (!nm || !is_valid_name(nm)) {
            /* indirection to nothing: empty */
            return;
        }
        local.text = (char *)nm;
        local.flags &= ~PF_INDIRECT;
        p = &local;
    }

    strvec_t multi = {0};
    const char *val = NULL;
    bool is_multi = false;
    bool set = param_base(p, scratch, &val, &multi, &is_multi);
    bool null_or_unset = !set || (!is_multi && (!val || !*val));

    /* the operators */
    int op = p->op;
    bool colon = (p->flags & PF_COLON) != 0;

    if (op == PO_DEFAULT || op == PO_ASSIGN || op == PO_ERROR || op == PO_ALT) {
        bool use_alt = op == PO_ALT;
        bool trigger = colon ? null_or_unset : !set;
        if (op == PO_ALT) {
            /* ${x:+word}: word if set(/non-null), else nothing */
            if (colon ? !null_or_unset : set) {
                char *w = expand_word_str(p->arg, 0);
                /* the alternate is itself subject to splitting when the
                 * whole thing is unquoted */
                if (word_dq || p->quoted) fb_add_protected(f, w ? w : "", w ? strlen(w) : 0);
                else if (w) fb_add_split(f, w);
                xfree(w);
            }
            sv_free(&multi);
            (void)use_alt;
            return;
        }
        if (trigger) {
            char *w = p->arg ? expand_word_str(p->arg, 0) : xstrdup("");
            if (op == PO_ERROR) {
                sh_warn("%s: %s", p->text,
                        (w && *w) ? w : "parameter null or not set");
                expand_error = true;
                xfree(w);
                sv_free(&multi);
                return;
            }
            if (op == PO_ASSIGN) {
                if (all_digits(p->text) || strchr("@*?#$!-0", p->text[0])) {
                    sh_warn("$%s: cannot assign in this way", p->text);
                    expand_error = true;
                    xfree(w);
                    sv_free(&multi);
                    return;
                }
                var_set(p->text, w ? w : "", 0);
                val = var_get(p->text);
            } else {
                /* PO_DEFAULT: use the word but do not assign */
                if (word_dq || p->quoted) fb_add_protected(f, w ? w : "", w ? strlen(w) : 0);
                else if (w) fb_add_split(f, w);
                xfree(w);
                sv_free(&multi);
                return;
            }
            xfree(w);
            /* fall through to emit val (for := ) */
            is_multi = false;
        }
    } else if (op && !null_or_unset && !is_multi && val) {
        /* pattern / substring / case operators on a scalar value */
        char *result = NULL;
        if (op == PO_SUBSTR) {
            char *offs = expand_word_str(p->arg, 0);
            long long off = 0, len = 0;
            arith_eval(offs ? offs : "0", &off);
            xfree(offs);
            bool have_len = p->arg2 != NULL;
            if (have_len) {
                char *lens = expand_word_str(p->arg2, 0);
                arith_eval(lens ? lens : "0", &len);
                xfree(lens);
            }
            result = substr(val, off, have_len, len);
        } else if (op == PO_RMPRE || op == PO_RMPREL || op == PO_RMSUF ||
                   op == PO_RMSUFL) {
            char *pat = expand_word_str(p->arg, X_PATTERN);
            char *v = xstrdup(val);
            result = trim_pattern(v, pat ? pat : "", op);
            xfree(v);
            xfree(pat);
        } else if (op == PO_REPL || op == PO_REPLALL || op == PO_REPLPRE ||
                   op == PO_REPLSUF) {
            char *pat = expand_word_str(p->arg, X_PATTERN);
            char *rep = p->arg2 ? expand_word_str(p->arg2, 0) : xstrdup("");
            char *v = xstrdup(val);
            result = replace_pattern(v, pat ? pat : "", rep ? rep : "", op);
            xfree(v); xfree(pat); xfree(rep);
        } else if (op == PO_UPPER || op == PO_UPPER1 || op == PO_LOWER ||
                   op == PO_LOWER1) {
            result = case_mod(val, op);
        }
        if (result) {
            if (word_dq || p->quoted) fb_add_protected(f, result, strlen(result));
            else fb_add_split(f, result);
            xfree(result);
            sv_free(&multi);
            return;
        }
    }

    /* No operator, or the operator asked for the plain value. Emit it. */
    if (is_multi) {
        bool q = word_dq || p->quoted;
        bool star = strcmp(p->text, "*") == 0 ||
                    (p->sub && p->sub->lit && strcmp(p->sub->lit, "*") == 0);
        if (q && star) {
            /* "$*": join with the first IFS char (space by default) */
            const char *ifs = ifs_value();
            char sep = ifs[0] ? ifs[0] : 0;
            strbuf_t b = {0};
            for (int i = 0; i < multi.n; i++) {
                if (i && sep) sb_putc(&b, sep);
                sb_puts(&b, multi.v[i]);
            }
            fb_add_protected(f, sb_str(&b), b.len);
            sb_free(&b);
        } else if (q) {
            /* "$@": each element is its own field, protected */
            for (int i = 0; i < multi.n; i++) {
                if (i) fb_endfield(f);
                fb_add_protected(f, multi.v[i], strlen(multi.v[i]));
                if (multi.n > 1 && i == 0) f->started = true;
            }
        } else {
            /* unquoted $@ / $*: each element split, boundaries forced */
            for (int i = 0; i < multi.n; i++) {
                if (i) fb_endfield(f);
                fb_add_split(f, multi.v[i]);
            }
        }
    } else if (val) {
        if (word_dq || p->quoted) fb_add_protected(f, val, strlen(val));
        else fb_add_split(f, val);
    }
    sv_free(&multi);
}

/* ── command substitution ──────────────────────────────────────────── */

static void expand_cmdsub(fb_t *f, wpart_t *p, bool word_dq)
{
    int status;
    char *outp = cmdsub_capture(p->tree, &status);
    exitstatus = status;
    if (!outp)
        return;
    /* strip trailing newlines */
    size_t n = strlen(outp);
    while (n > 0 && outp[n - 1] == '\n')
        outp[--n] = '\0';
    if (word_dq || p->quoted)
        fb_add_protected(f, outp, n);
    else
        fb_add_split(f, outp);
    xfree(outp);
}

static void expand_arith(fb_t *f, wpart_t *p, bool word_dq)
{
    char *expr = expand_word_str(p->arg, 0);
    long long v = 0;
    arith_eval(expr ? expr : "", &v);
    xfree(expr);
    char buf[24];
    const char *s = itoa_s(v, buf);
    if (word_dq || p->quoted) fb_add_protected(f, s, strlen(s));
    else fb_add_split(f, s);
}

/* ── tilde at the head of a word ───────────────────────────────────── */

/* If the first part is an unquoted literal starting with ~, replace the
 * tilde prefix in place (returns a new part text) - done before other
 * expansion. Handles ~ and ~user; ~+ and ~- map to PWD/OLDPWD. */
static char *maybe_tilde(const char *lit, size_t len, size_t *consumed)
{
    if (len == 0 || lit[0] != '~')
        return NULL;
    size_t i = 1;
    while (i < len && lit[i] != '/' && lit[i] != ':')
        i++;
    char user[128];
    size_t ulen = i - 1;
    if (ulen >= sizeof user)
        return NULL;
    memcpy(user, lit + 1, ulen);
    user[ulen] = '\0';
    char *home = NULL;
    if (strcmp(user, "+") == 0) {
        const char *p = var_get("PWD");
        home = p ? xstrdup(p) : NULL;
    } else if (strcmp(user, "-") == 0) {
        const char *p = var_get("OLDPWD");
        home = p ? xstrdup(p) : NULL;
    } else {
        home = tilde_home(user);
    }
    if (!home)
        return NULL;
    *consumed = i;
    return home;
}

/* ── expanding a whole word into fields ────────────────────────────── */

static void expand_parts(fb_t *f, word_t *w, int flags)
{
    bool first_part = true;
    bool assign_ctx = (flags & X_ASSIGN) != 0;
    bool after_colon = false;
    for (wpart_t *p = w->parts; p; p = p->next) {
        bool tilde_here = (flags & X_TILDE) &&
                          (first_part || (assign_ctx && after_colon));
        if (p->type == WP_LIT) {
            const char *s = p->text;
            size_t len = p->len;
            size_t used = 0;
            if (tilde_here && !p->quoted) {
                size_t consumed = 0;
                char *home = maybe_tilde(s, len, &consumed);
                if (home) {
                    fb_add_protected(f, home, strlen(home));
                    xfree(home);
                    used = consumed;
                }
            }
            /* the rest of this literal part */
            for (size_t i = used; i < len; i++) {
                char c = s[i];
                if (assign_ctx && !p->quoted && c == ':')
                    after_colon = true;
                else
                    after_colon = false;
                if (p->quoted)
                    fb_pushchar(f, c, 1);
                else
                    fb_pushchar(f, c, 0);
            }
            first_part = false;
            continue;
        }
        after_colon = false;
        first_part = false;
        switch (p->type) {
        case WP_PARAM:  expand_param(f, p, p->quoted); break;
        case WP_CMDSUB: expand_cmdsub(f, p, p->quoted); break;
        case WP_ARITH:  expand_arith(f, p, p->quoted); break;
        default: break;
        }
    }
}

/* Turn a built field (text + mask) into a glob pattern: escape the
 * protected metacharacters so they match literally. */
static char *field_pattern(const char *text, const char *mask, size_t n)
{
    strbuf_t b = {0};
    for (size_t i = 0; i < n; i++) {
        char c = text[i];
        if (mask[i] && (c == '*' || c == '?' || c == '[' || c == '\\'))
            sb_putc(&b, '\\');
        else if (!mask[i] && c == '\\')
            sb_putc(&b, '\\');      /* keep an unquoted backslash as itself
                                     * so pmatch treats the next char literal */
        sb_putc(&b, c);
    }
    return sb_take(&b);
}

/* mask length equals text length; but sb_take gives a NUL-terminated
 * string, and the mask bytes may include a 0 that looks like NUL. So we
 * carry the field length via strlen of text (text has no embedded NUL in
 * practice - the shell cannot hold one in a word). */
static bool expand_one_word(word_t *w, strvec_t *out, int flags)
{
    fb_t f;
    fb_init(&f, flags);
    expand_parts(&f, w, flags);
    fb_endfield(&f);
    if (expand_error) {
        sv_free(&f.fields);
        sv_free(&f.masks);
        return false;
    }
    for (int i = 0; i < f.fields.n; i++) {
        const char *text = f.fields.v[i];
        const char *mask = f.masks.v[i];
        size_t n = strlen(text);
        if (flags & X_GLOB) {
            char *pat = field_pattern(text, mask, n);
            strvec_t g = {0};
            if (glob_expand(pat, &g)) {
                for (int j = 0; j < g.n; j++)
                    sv_push(out, g.v[j]);
                xfree(g.v);
                xfree(pat);
                continue;
            }
            /* no match: the field is the pattern with quoting removed */
            char *lit = pat_unescape(pat);
            sv_push(out, lit);
            xfree(pat);
        } else {
            sv_push(out, xstrdup(text));
        }
    }
    sv_free(&f.fields);
    sv_free(&f.masks);
    return true;
}

/* ── brace expansion ───────────────────────────────────────────────── *
 *
 * Textual and first, on the unquoted braces of a word. The word is turned
 * into a flat list of atoms - one per literal character (carrying its
 * quotedness and whether it is brace-eligible) or one per opaque part
 * (a $expansion, kept whole). Braces are expanded over the eligible
 * characters, cloning atom ranges, and each result is turned back into a
 * word_t built from the current arena. */

typedef struct {
    u8       kind;      /* 0 = literal char, 1 = opaque part */
    char     c;
    u8       quoted;
    u8       eligible;  /* an unquoted literal brace character */
    wpart_t *part;
} atom_t;

typedef struct { atom_t *v; int n, cap; } atomvec_t;

static void av_push(atomvec_t *a, atom_t at)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 16;
        a->v = xrealloc(a->v, (size_t)a->cap * sizeof *a->v);
    }
    a->v[a->n++] = at;
}

static void word_to_atoms(word_t *w, atomvec_t *out)
{
    for (wpart_t *p = w->parts; p; p = p->next) {
        if (p->type == WP_LIT && p->sub == NULL) {
            for (size_t i = 0; i < p->len; i++) {
                atom_t a = { 0, p->text[i], p->quoted, (u8)!p->quoted, NULL };
                av_push(out, a);
            }
        } else {
            atom_t a = { 1, 0, p->quoted, 0, p };
            av_push(out, a);
        }
    }
}

/* Build a word_t from a run of atoms [lo,hi). Literal runs of the same
 * quotedness coalesce into one WP_LIT part. */
static word_t *atoms_to_word(atom_t *a, int lo, int hi)
{
    word_t *w = pa_alloc(sizeof *w);
    wpart_t **tail = &w->parts;
    u8 flags = 0;
    int i = lo;
    while (i < hi) {
        if (a[i].kind == 1) {
            wpart_t *np = pa_alloc(sizeof *np);
            *np = *a[i].part;
            np->next = NULL;
            *tail = np;
            tail = &np->next;
            if (np->quoted) flags |= W_QUOTED;
            flags |= W_HASPARTS;
            i++;
        } else {
            int j = i;
            u8 q = a[i].quoted;
            strbuf_t b = {0};
            while (j < hi && a[j].kind == 0 && a[j].quoted == q) {
                sb_putc(&b, a[j].c);
                j++;
            }
            wpart_t *np = pa_alloc(sizeof *np);
            np->type = WP_LIT;
            np->quoted = q;
            np->text = pa_strndup(b.s ? b.s : "", b.len);
            np->len = b.len;
            sb_free(&b);
            *tail = np;
            tail = &np->next;
            if (q) flags |= W_QUOTED;
            i = j;
        }
    }
    w->flags = flags;
    if (w->parts && !w->parts->next && w->parts->type == WP_LIT &&
        !w->parts->quoted)
        w->lit = w->parts->text;
    else if (!w->parts)
        w->lit = pa_strndup("", 0);
    return w;
}

typedef struct { word_t **v; int n, cap; } wordvec_t;

static void wv_push(wordvec_t *v, word_t *w)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->v = xrealloc(v->v, (size_t)v->cap * sizeof *v->v);
    }
    v->v[v->n++] = w;
}

/* Find, from index `from`, the first eligible '{' that begins a valid
 * brace group (contains a top-level eligible ',' or a valid a..b range),
 * and return its index or -1. Fills *close with the matching '}' index
 * and, for a comma group, records nothing extra. */
static int find_brace(atom_t *a, int n, int from, int *close)
{
    for (int i = from; i < n; i++) {
        if (!(a[i].kind == 0 && a[i].eligible && a[i].c == '{'))
            continue;
        int depth = 0;
        bool comma = false;
        int j;
        for (j = i; j < n; j++) {
            if (a[j].kind != 0 || !a[j].eligible) continue;
            if (a[j].c == '{') depth++;
            else if (a[j].c == '}') {
                depth--;
                if (depth == 0) break;
            } else if (a[j].c == ',' && depth == 1) {
                comma = true;
            }
        }
        if (j >= n || depth != 0)
            continue;                   /* no matching close */
        if (comma) { *close = j; return i; }
        /* a sequence {x..y}? checked by the caller via expand */
        *close = j;
        /* mark that this is only valid if it is a sequence: signal with a
         * negative-style by still returning i; the caller re-checks. */
        return i;
    }
    return -1;
}

static bool atoms_int(atom_t *a, int lo, int hi, long long *out)
{
    strbuf_t b = {0};
    for (int i = lo; i < hi; i++) {
        if (a[i].kind != 0) { sb_free(&b); return false; }
        sb_putc(&b, a[i].c);
    }
    char *end;
    const char *s = sb_str(&b);
    if (!*s) { sb_free(&b); return false; }
    long long v = strtoll(s, &end, 10);
    bool ok = *end == '\0';
    sb_free(&b);
    if (ok) *out = v;
    return ok;
}

static void brace_recurse(atom_t *a, int n, wordvec_t *out);

/* Emit clones of pre + option + post for a comma group. */
static void brace_comma(atom_t *a, int n, int open, int close, wordvec_t *out)
{
    /* split options at top-level commas between open+1 and close */
    int depth = 0;
    int start = open + 1;
    atomvec_t opt_ranges = {0};   /* store starts; we walk again */
    /* We build each combined atom list: [0,open) + option + (close,n). */
    for (int j = open + 1; j <= close; j++) {
        bool is_sep = (j == close) ||
                      (a[j].kind == 0 && a[j].eligible && a[j].c == ',' && depth == 0);
        if (a[j].kind == 0 && a[j].eligible) {
            if (a[j].c == '{') depth++;
            else if (a[j].c == '}') { if (j != close) depth--; }
        }
        if (is_sep) {
            atomvec_t combo = {0};
            for (int k = 0; k < open; k++) av_push(&combo, a[k]);
            for (int k = start; k < j; k++) av_push(&combo, a[k]);
            for (int k = close + 1; k < n; k++) av_push(&combo, a[k]);
            brace_recurse(combo.v, combo.n, out);
            xfree(combo.v);
            start = j + 1;
        }
    }
    xfree(opt_ranges.v);
}

/* Emit a numeric or single-char sequence {a..b} or {a..b..step}. */
static bool brace_sequence(atom_t *a, int n, int open, int close, wordvec_t *out)
{
    /* find the two ".." separators at top level */
    int dots1 = -1, dots2 = -1;
    for (int j = open + 1; j < close - 1; j++) {
        if (a[j].kind == 0 && a[j].eligible && a[j].c == '.' &&
            a[j+1].kind == 0 && a[j+1].eligible && a[j+1].c == '.') {
            if (dots1 < 0) dots1 = j;
            else if (dots2 < 0) { dots2 = j; }
            j++;
        }
    }
    if (dots1 < 0)
        return false;
    long long lo, hi, step = 1;
    int lo_hi_end = dots2 >= 0 ? dots2 : close;
    bool numeric = atoms_int(a, open + 1, dots1, &lo) &&
                   atoms_int(a, dots1 + 2, lo_hi_end, &hi);
    bool charseq = false;
    char clo = 0, chi = 0;
    if (!numeric) {
        if (dots1 == open + 2 && lo_hi_end == dots1 + 3 &&
            a[open+1].kind == 0 && a[dots1+2].kind == 0) {
            clo = a[open+1].c;
            chi = a[dots1+2].c;
            charseq = true;
        }
    }
    if (!numeric && !charseq)
        return false;
    if (dots2 >= 0) {
        long long s;
        if (!atoms_int(a, dots2 + 2, close, &s))
            return false;
        step = s < 0 ? -s : s;
        if (step == 0) step = 1;
    }
    /* build each value and recurse with pre + value + post */
    if (numeric) {
        long long dir = hi >= lo ? 1 : -1;
        for (long long v = lo; dir > 0 ? v <= hi : v >= hi; v += dir * step) {
            char buf[24];
            itoa_s(v, buf);
            atomvec_t combo = {0};
            for (int k = 0; k < open; k++) av_push(&combo, a[k]);
            for (char *c = buf; *c; c++) {
                atom_t at = { 0, *c, 0, 0, NULL };
                av_push(&combo, at);
            }
            for (int k = close + 1; k < n; k++) av_push(&combo, a[k]);
            brace_recurse(combo.v, combo.n, out);
            xfree(combo.v);
            if (step == 0) break;
        }
    } else {
        int dir = chi >= clo ? 1 : -1;
        for (int v = clo; dir > 0 ? v <= chi : v >= chi; v += dir * (int)step) {
            atomvec_t combo = {0};
            for (int k = 0; k < open; k++) av_push(&combo, a[k]);
            atom_t at = { 0, (char)v, 0, 0, NULL };
            av_push(&combo, at);
            for (int k = close + 1; k < n; k++) av_push(&combo, a[k]);
            brace_recurse(combo.v, combo.n, out);
            xfree(combo.v);
        }
    }
    return true;
}

static void brace_recurse(atom_t *a, int n, wordvec_t *out)
{
    int from = 0;
    for (;;) {
        int close = -1;
        int open = find_brace(a, n, from, &close);
        if (open < 0) {
            /* no more braces: this atom list is a finished word */
            wv_push(out, atoms_to_word(a, 0, n));
            return;
        }
        /* comma group takes priority; else try a sequence; else skip
         * this brace and look for the next. */
        int depth = 0; bool comma = false;
        for (int j = open; j <= close; j++) {
            if (a[j].kind != 0 || !a[j].eligible) continue;
            if (a[j].c == '{') depth++;
            else if (a[j].c == '}') { depth--; if (depth==0) break; }
            else if (a[j].c == ',' && depth == 1) comma = true;
        }
        if (comma) { brace_comma(a, n, open, close, out); return; }
        if (brace_sequence(a, n, open, close, out)) return;
        from = open + 1;        /* not a real group: move past this '{' */
    }
}

/* ── the public entries ────────────────────────────────────────────── */

static bool expand_words(word_t *w, strvec_t *out, int flags)
{
    expand_error = false;
    for (; w; w = w->next) {
        /* brace expansion first, unless suppressed */
        if (!(flags & X_NOBRACE)) {
            atomvec_t atoms = {0};
            word_to_atoms(w, &atoms);
            /* quick check: any eligible brace at all? */
            bool has_brace = false;
            for (int i = 0; i < atoms.n; i++)
                if (atoms.v[i].kind == 0 && atoms.v[i].eligible &&
                    atoms.v[i].c == '{') { has_brace = true; break; }
            if (has_brace) {
                wordvec_t words = {0};
                brace_recurse(atoms.v, atoms.n, &words);
                for (int i = 0; i < words.n; i++) {
                    if (!expand_one_word(words.v[i], out, flags)) {
                        xfree(words.v);
                        xfree(atoms.v);
                        return false;
                    }
                }
                xfree(words.v);
                xfree(atoms.v);
                continue;
            }
            xfree(atoms.v);
        }
        if (!expand_one_word(w, out, flags))
            return false;
    }
    return true;
}

/* Expand a word to a single string (no splitting, no globbing): used for
 * assignments, redirection targets, case subjects, here-strings and the
 * operands of ${..} operators. The result is protected throughout. */
static char *expand_word_str(word_t *w, int flags)
{
    fb_t f;
    fb_init(&f, 0);
    /* Force everything into one protected field: expand_parts already
     * routes quoted/scalar values to fb_add_protected; the risk is an
     * unquoted $x that would split. So mark the builder as "no split" by
     * lying about IFS. */
    f.ifs = "";
    expand_parts(&f, w, (flags & X_TILDE) | X_ASSIGN);
    fb_endfield(&f);
    char *result;
    if (f.fields.n == 0) {
        result = xstrdup("");
    } else if (f.fields.n == 1) {
        /* the common case; PATTERN wants the escaped form so quoted
         * metacharacters stay literal downstream (${x#pat}, case) */
        const char *text = f.fields.v[0];
        const char *mask = f.masks.v[0];
        size_t n = strlen(text);
        result = (flags & X_PATTERN) ? field_pattern(text, mask, n)
                                     : xstrdup(text);
    } else {
        /* an unquoted $@ with several parameters reached a scalar context
         * (an assignment, a case subject): join with a space, as $* does */
        strbuf_t b = {0};
        for (int i = 0; i < f.fields.n; i++) {
            if (i) sb_putc(&b, ' ');
            if (flags & X_PATTERN) {
                char *pp = field_pattern(f.fields.v[i], f.masks.v[i],
                                         strlen(f.fields.v[i]));
                sb_puts(&b, pp);
                xfree(pp);
            } else {
                sb_puts(&b, f.fields.v[i]);
            }
        }
        result = sb_take(&b);
    }
    sv_free(&f.fields);
    sv_free(&f.masks);
    return expand_error ? (xfree(result), (char *)NULL) : result;
}

static char *expand_str(word_t *w, int flags)
{
    expand_error = false;
    return expand_word_str(w, flags);
}

/* A heredoc body was collected as text; parse it as a heredoc-mode word
 * (only $ ` \ act) and expand it, protected throughout. */
static char *expand_heredoc(const char *body)
{
    expand_error = false;
    bool err = false;
    word_t *w = parse_string_word(body, true, &err);
    if (err || !w)
        return xstrdup(body);
    return expand_word_str(w, 0);
}

/* ── prompt expansion (PS1/PS2 escapes) ────────────────────────────── *
 *
 * Turns \u \h \H \w \W \$ \t \T \@ \d \n \\ \[ \] and the \nnn octals
 * into the text a prompt shows, and reports the visible width so the line
 * editor can place the cursor. \[ and \] bracket non-printing runs (colour
 * escapes) which do not count toward the width. */

static char *prompt_expand(const char *ps, int *visible_width)
{
    strbuf_t b = {0};
    int width = 0;
    bool nonprint = false;
    lp_tm_t tm;
    memset(&tm, 0, sizeof tm);
    bool have_tm = false;
    for (const char *p = ps; *p; p++) {
        if (*p != '\\') {
            sb_putc(&b, *p);
            if (!nonprint && (u8)*p >= 0x20 && (u8)*p != 0x7f)
                width++;
            continue;
        }
        p++;
        switch (*p) {
        case 'u': {
            lp_user_t u;
            const char *nm = "user";
            if (lp_user_by_uid(sys_getuid_(), &u) == 0 && u.name[0])
                nm = u.name;
            sb_puts(&b, nm);
            if (!nonprint) width += (int)strlen(nm);
            break;
        }
        case 'h': case 'H': {
            char host[65] = "linux-lp";
            /* struct utsname is six fixed 65-byte fields; nodename is the
             * second, so it starts 65 bytes in. */
            char uts[6 * 65];
            if (lp_uname(uts) >= 0 && uts[65])
                strlcpy(host, uts + 65, sizeof host);
            if (*p == 'h') {
                char *dot = strchr(host, '.');
                if (dot) *dot = '\0';
            }
            sb_puts(&b, host);
            if (!nonprint) width += (int)strlen(host);
            break;
        }
        case 'w': case 'W': {
            const char *cwd = var_get("PWD");
            char buf[512];
            if (!cwd) { if (lp_getcwd(buf, sizeof buf) >= 0) cwd = buf; }
            if (!cwd) cwd = "?";
            const char *home = var_get("HOME");
            char shown[512];
            if (home && *home && strncmp(cwd, home, strlen(home)) == 0 &&
                (cwd[strlen(home)] == '/' || cwd[strlen(home)] == '\0')) {
                snprintf(shown, sizeof shown, "~%s", cwd + strlen(home));
            } else {
                strlcpy(shown, cwd, sizeof shown);
            }
            const char *out = shown;
            if (*p == 'W') {
                char *slash = strrchr(shown, '/');
                if (slash && slash[1]) out = slash + 1;
                else if (slash && slash != shown) out = slash;
            }
            sb_puts(&b, out);
            if (!nonprint) width += (int)strlen(out);
            break;
        }
        case '$': {
            char c = sys_geteuid() == 0 ? '#' : '$';
            sb_putc(&b, c);
            if (!nonprint) width++;
            break;
        }
        case 't': case 'T': case '@': case 'A': {
            if (!have_tm) { lp_localtime(lp_time(), &tm); have_tm = true; }
            char ts[16];
            if (*p == 'A')
                snprintf(ts, sizeof ts, "%02d:%02d", tm.hour, tm.min);
            else
                snprintf(ts, sizeof ts, "%02d:%02d:%02d", tm.hour, tm.min, tm.sec);
            sb_puts(&b, ts);
            if (!nonprint) width += (int)strlen(ts);
            break;
        }
        case 'd': {
            if (!have_tm) { lp_localtime(lp_time(), &tm); have_tm = true; }
            static const char *const wd[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
            static const char *const mo[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                             "Jul","Aug","Sep","Oct","Nov","Dec"};
            char ds[32];
            int wi = tm.wday % 7, mi = (tm.mon - 1) % 12;
            snprintf(ds, sizeof ds, "%s %s %d", wd[wi < 0 ? 0 : wi],
                     mo[mi < 0 ? 0 : mi], tm.day);
            sb_puts(&b, ds);
            if (!nonprint) width += (int)strlen(ds);
            break;
        }
        case 'n': sb_putc(&b, '\n'); width = 0; break;
        case 'r': sb_putc(&b, '\r'); break;
        case '\\': sb_putc(&b, '\\'); if (!nonprint) width++; break;
        case '[': nonprint = true; break;
        case ']': nonprint = false; break;
        case 'e': sb_putc(&b, 27); break;
        case 'a': sb_putc(&b, '\a'); break;
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            int v = 0, k = 0;
            while (k < 3 && *p >= '0' && *p <= '7') { v = v*8 + (*p - '0'); p++; k++; }
            p--;
            sb_putc(&b, (char)v);
            if (!nonprint && v >= 0x20) width++;
            break;
        }
        case '\0': sb_putc(&b, '\\'); p--; break;
        default: sb_putc(&b, *p); if (!nonprint) width++; break;
        }
    }
    if (visible_width)
        *visible_width = width;
    return sb_take(&b);
}
