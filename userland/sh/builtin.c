/* builtin.c - the commands the shell runs itself. Part of sh.c; see
 * sh.h.
 *
 * Everything POSIX requires of sh is here, because a builtin that is
 * missing is not a smaller shell but a broken one: the old shell had no
 * `eval`, and that one gap is why apt-key died and `apt update` failed
 * its signature check. Beyond POSIX are the bash builtins people type
 * without thinking - local, declare, source, pushd/popd, shopt, help,
 * mapfile - so that the shell feels like a normal Linux shell.
 *
 * Output goes through out1/out2 (core.c), which exec.c flushes after
 * every builtin while its redirections are still in place, and checks
 * for a failed write. Messages follow bash's wording; exit statuses
 * follow dash, which is what Debian's scripts are tested against.
 * Where the two print different formats for something a script might
 * parse (export -p, trap, getopts, ulimit) the POSIX/dash form is used. */

/* ── options ───────────────────────────────────────────────────────── */

static const optname_t optnames[NOPTS] = {
    { "allexport", 'a' }, { "notify", 'b' }, { "noclobber", 'C' },
    { "errexit", 'e' }, { "noglob", 'f' }, { "hashall", 'h' },
    { "interactive", 'i' }, { "monitor", 'm' }, { "noexec", 'n' },
    { "nounset", 'u' }, { "verbose", 'v' }, { "xtrace", 'x' },
    { "pipefail", 0 }, { "ignoreeof", 0 }, { "nolog", 0 },
    { "posix", 0 }, { "emacs", 0 }, { "vi", 0 },
};

static int opt_index(const char *name)
{
    for (int i = 0; i < NOPTS; i++)
        if (strcmp(optnames[i].name, name) == 0)
            return i;
    return -1;
}

static int opt_by_letter(char c)
{
    for (int i = 0; i < NOPTS; i++)
        if (optnames[i].letter == c)
            return i;
    return -1;
}

/* Turning some options on or off has consequences beyond the flag. */
static void opt_changed(int o)
{
    if (o == O_monitor) {
        if (opt[O_monitor] && !job_control && !in_subshell)
            jobs_init();
        else if (!opt[O_monitor])
            job_control = false;
    } else if (o == O_emacs && opt[O_emacs]) {
        opt[O_vi] = false;
    } else if (o == O_vi && opt[O_vi]) {
        opt[O_emacs] = false;
    }
}

/* shopt, bash's second set of options. Only the ones that do something
 * here are accepted; claiming to have set one that has no effect would
 * be worse than saying no. */
static bool shopt_nullglob, shopt_dotglob, shopt_autocd, shopt_failglob;
static bool shopt_noop_true = true;

static const struct { const char *name; bool *flag; bool fixed; } shopts[] = {
    { "autocd", &shopt_autocd, false },
    { "checkwinsize", &shopt_noop_true, true },
    { "dotglob", &shopt_dotglob, false },
    { "expand_aliases", &shopt_noop_true, true },
    { "failglob", &shopt_failglob, false },
    { "histappend", &shopt_noop_true, true },
    { "nullglob", &shopt_nullglob, false },
    { "promptvars", &shopt_noop_true, true },
    { "sourcepath", &shopt_noop_true, true },
};

/* ── small helpers ─────────────────────────────────────────────────── */

static void bi_usage(const char *name, const char *usage)
{
    sh_warn("%s: usage: %s", name, usage);
}

/* The common shape of option parsing for builtins: leading -xyz groups
 * until "--" or the first non-option. *idx is left at the first operand.
 * Returns the offending letter on an unknown option, 0 otherwise. */
static char bi_opts(int argc, char **argv, const char *allowed, bool *set,
                    int *idx)
{
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1])
            break;
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        for (const char *p = a + 1; *p; p++) {
            const char *k = strchr(allowed, *p);
            if (!k) {
                *idx = i;
                return *p;
            }
            set[k - allowed] = true;
        }
    }
    *idx = i;
    return 0;
}

static bool str_to_int(const char *s, long long *v)
{
    return parse_int(s, v);
}

/* ── : true false ──────────────────────────────────────────────────── */

static int bi_true(int argc, char **argv)  { (void)argc; (void)argv; return 0; }
static int bi_false(int argc, char **argv) { (void)argc; (void)argv; return 1; }

/* ── echo ──────────────────────────────────────────────────────────── *
 *
 * Backslash escapes are interpreted by default, as dash and XSI do -
 * Debian's /bin/sh scripts are written against that. -n, and bash's -e
 * and -E, are accepted as options when they are all an argument holds. */

static int esc_octal(const char **pp, int max)
{
    int v = 0, n = 0;
    while (n < max && **pp >= '0' && **pp <= '7') {
        v = v * 8 + (**pp - '0');
        (*pp)++;
        n++;
    }
    return v;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Append s to b with backslash escapes turned into bytes. echo_mode is
 * echo's and %b's rule (\0nnn octal); otherwise printf's format rule
 * (\nnn). Returns false when \c said to stop all output. */
static bool unescape(strbuf_t *b, const char *s, bool echo_mode)
{
    for (const char *p = s; *p; ) {
        if (*p != '\\') {
            sb_putc(b, *p++);
            continue;
        }
        p++;
        char c = *p;
        if (!c) {
            sb_putc(b, '\\');
            break;
        }
        p++;
        switch (c) {
        case 'a': sb_putc(b, '\a'); break;
        case 'b': sb_putc(b, '\b'); break;
        case 'e': case 'E': sb_putc(b, 27); break;
        case 'f': sb_putc(b, '\f'); break;
        case 'n': sb_putc(b, '\n'); break;
        case 'r': sb_putc(b, '\r'); break;
        case 't': sb_putc(b, '\t'); break;
        case 'v': sb_putc(b, '\v'); break;
        case '\\': sb_putc(b, '\\'); break;
        case 'c': return false;
        case 'x': {
            int v = 0, n = 0, h;
            while (n < 2 && (h = hexval((u8)*p)) >= 0) {
                v = v * 16 + h;
                p++;
                n++;
            }
            if (n)
                sb_putc(b, (char)v);
            else
                sb_puts(b, "\\x");
            break;
        }
        case '0':
            if (echo_mode) {
                sb_putc(b, (char)esc_octal(&p, 3));
                break;
            }
            /* fall through */
        case '1': case '2': case '3': case '4': case '5': case '6': case '7':
            if (echo_mode && c != '0') {
                /* echo: \1 is not an escape, but dash takes it anyway */
                p--;
                sb_putc(b, (char)esc_octal(&p, 3));
                break;
            }
            p--;
            sb_putc(b, (char)esc_octal(&p, 3));
            break;
        case '"': case '\'':
            if (!echo_mode) {
                sb_putc(b, c);
                break;
            }
            /* fall through */
        default:
            sb_putc(b, '\\');
            sb_putc(b, c);
            break;
        }
    }
    return true;
}

static int bi_echo(int argc, char **argv)
{
    bool nl = true, esc = true;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1])
            break;
        bool all = true;
        for (const char *p = a + 1; *p; p++)
            if (*p != 'n' && *p != 'e' && *p != 'E')
                all = false;
        if (!all)
            break;
        for (const char *p = a + 1; *p; p++) {
            if (*p == 'n') nl = false;
            else if (*p == 'e') esc = true;
            else esc = false;
        }
    }
    strbuf_t b = {0};
    bool go = true;
    for (int k = i; k < argc && go; k++) {
        if (k > i)
            sb_putc(&b, ' ');
        if (esc)
            go = unescape(&b, argv[k], true);
        else
            sb_puts(&b, argv[k]);
    }
    if (go && nl)
        sb_putc(&b, '\n');
    outn(out1, b.s ? b.s : "", b.len);
    sb_free(&b);
    return 0;
}

/* ── printf ────────────────────────────────────────────────────────── */

static bool printf_err;

/* A numeric operand: decimal, 0x hex, 0 octal, or 'c for the value of
 * the character c. On garbage, the value of the leading number and a
 * message, with printf's status turned to 1 - which is what both dash and
 * bash do, so a script sees output and a failure together. */
static long long printf_int(const char *s, bool is_unsigned)
{
    if (*s == '\'' || *s == '"') {
        int used = 0;
        u32 cp = s[1] ? utf8_decode(s + 1, strlen(s + 1), &used) : 0;
        return (long long)cp;
    }
    const char *p = s;
    while (is_blank(*p)) p++;
    if (!*p)
        return 0;
    char *end;
    long long v;
    if (is_unsigned && *p != '-') {
        v = strtoll(p, &end, 0);
        /* values above LLONG_MAX: parse by hand */
        if (*end == '\0' && v < 0)
            ;
    } else {
        v = strtoll(p, &end, 0);
    }
    while (is_blank(*end) || *end == '\n') end++;
    if (*end || end == p) {
        sh_warn("printf: %s: invalid number", s);
        printf_err = true;
    }
    return v;
}

static double printf_double(const char *s)
{
    if (*s == '\'' || *s == '"')
        return (double)(u8)s[1];
    const char *p = s;
    while (is_blank(*p)) p++;
    bool neg = false;
    if (*p == '+' || *p == '-')
        neg = *p++ == '-';
    double v = 0;
    bool any = false;
    if ((p[0] == 'i' || p[0] == 'I') && (p[1] == 'n' || p[1] == 'N')) {
        v = 1e308 * 10;
        p += 3;
        if (strncmp(p, "inity", 5) == 0 || strncmp(p, "INITY", 5) == 0)
            p += 5;
        return neg ? -v : v;
    }
    while (is_digit((u8)*p)) {
        v = v * 10 + (*p++ - '0');
        any = true;
    }
    if (*p == '.') {
        p++;
        double scale = 0.1;
        while (is_digit((u8)*p)) {
            v += (*p++ - '0') * scale;
            scale /= 10;
            any = true;
        }
    }
    if (any && (*p == 'e' || *p == 'E')) {
        p++;
        bool eneg = false;
        if (*p == '+' || *p == '-')
            eneg = *p++ == '-';
        int e = 0;
        while (is_digit((u8)*p))
            e = e * 10 + (*p++ - '0');
        while (e--)
            v = eneg ? v / 10 : v * 10;
    }
    while (is_blank(*p) || *p == '\n') p++;
    if (*p || !any) {
        sh_warn("printf: %s: invalid number", s);
        printf_err = true;
    }
    return neg ? -v : v;
}

/* Fixed-point text for v with prec digits after the point. Good to the
 * precision of a double, which is what %f in a shell script asks of it. */
static void fmt_fixed(strbuf_t *b, double v, int prec)
{
    if (v != v) { sb_puts(b, "nan"); return; }
    if (v < 0) { sb_putc(b, '-'); v = -v; }
    if (v > 1.7e308) { sb_puts(b, "inf"); return; }
    double r = 0.5;
    for (int i = 0; i < prec; i++)
        r /= 10;
    v += r;
    /* integer part */
    char ip[400];
    int n = 0;
    double iv = v;
    double p10 = 1;
    while (p10 * 10 <= iv && n < 330) {
        p10 *= 10;
        n++;
    }
    int k = 0;
    for (double d = p10; d >= 1; d /= 10) {
        int dig = (int)(iv / d);
        if (dig > 9) dig = 9;
        if (dig < 0) dig = 0;
        ip[k++] = (char)('0' + dig);
        iv -= dig * d;
        if (k >= (int)sizeof ip - 1) break;
    }
    if (k == 0) ip[k++] = '0';
    sb_putn(b, ip, (size_t)k);
    if (prec > 0) {
        sb_putc(b, '.');
        double frac = iv;
        if (frac < 0) frac = 0;
        for (int i = 0; i < prec; i++) {
            frac *= 10;
            int dig = (int)frac;
            if (dig > 9) dig = 9;
            sb_putc(b, (char)('0' + dig));
            frac -= dig;
        }
    }
}

static void fmt_exp(strbuf_t *b, double v, int prec, char e)
{
    if (v != v) { sb_puts(b, "nan"); return; }
    if (v < 0) { sb_putc(b, '-'); v = -v; }
    if (v > 1.7e308) { sb_puts(b, "inf"); return; }
    int ex = 0;
    if (v != 0) {
        while (v >= 10) { v /= 10; ex++; }
        while (v < 1) { v *= 10; ex--; }
    }
    strbuf_t m = {0};
    fmt_fixed(&m, v, prec);
    /* rounding can carry to 10.0 */
    if (m.s[0] == '1' && m.s[1] == '0') {
        v /= 10;
        ex++;
        m.len = 0;
        fmt_fixed(&m, v, prec);
    }
    sb_putn(b, m.s, m.len);
    sb_free(&m);
    sb_printf(b, "%c%c%02d", e, ex < 0 ? '-' : '+', ex < 0 ? -ex : ex);
}

static void fmt_general(strbuf_t *b, double v, int prec, bool alt, char e)
{
    if (prec == 0)
        prec = 1;
    double a = v < 0 ? -v : v;
    int ex = 0;
    if (a != 0) {
        double t = a;
        while (t >= 10) { t /= 10; ex++; }
        while (t < 1) { t *= 10; ex--; }
    }
    strbuf_t t = {0};
    if (ex < -4 || ex >= prec)
        fmt_exp(&t, v, prec - 1, e);
    else
        fmt_fixed(&t, v, prec - 1 - ex);
    char *s = sb_str(&t);
    if (!alt && strchr(s, '.')) {
        /* drop trailing zeros of the fraction */
        char *ep = strchr(s, e);
        size_t end = ep ? (size_t)(ep - s) : strlen(s);
        size_t z = end;
        while (z > 0 && s[z - 1] == '0') z--;
        if (z > 0 && s[z - 1] == '.') z--;
        sb_putn(b, s, z);
        if (ep)
            sb_puts(b, ep);
    } else {
        sb_puts(b, s);
    }
    sb_free(&t);
}

static void pad_out(strbuf_t *out, const char *s, size_t n, int width,
                    bool left, bool zero)
{
    size_t w = utf8_str_width(s, n);
    size_t pad = width > 0 && (size_t)width > w ? (size_t)width - w : 0;
    if (!left && zero) {
        /* zero padding goes after the sign or 0x */
        size_t pre = 0;
        if (n && (s[0] == '-' || s[0] == '+' || s[0] == ' ')) pre = 1;
        if (n > pre + 1 && s[pre] == '0' && (s[pre + 1] == 'x' || s[pre + 1] == 'X'))
            pre += 2;
        sb_putn(out, s, pre);
        for (size_t i = 0; i < pad; i++) sb_putc(out, '0');
        sb_putn(out, s + pre, n - pre);
        return;
    }
    if (!left)
        for (size_t i = 0; i < pad; i++) sb_putc(out, ' ');
    sb_putn(out, s, n);
    if (left)
        for (size_t i = 0; i < pad; i++) sb_putc(out, ' ');
}

static void sb_shell_quote(strbuf_t *b, const char *s)
{
    if (!*s) {
        sb_puts(b, "''");
        return;
    }
    bool plain = true;
    for (const char *p = s; *p; p++)
        if (!(is_name_char((u8)*p) || strchr("@%+=:,./-_", *p) || (u8)*p >= 0x80))
            plain = false;
    if (plain) {
        sb_puts(b, s);
        return;
    }
    for (const char *p = s; *p; p++) {
        if (is_name_char((u8)*p) || strchr("@%+=:,./-_", *p) || (u8)*p >= 0x80)
            sb_putc(b, *p);
        else if (*p == '\n')
            sb_puts(b, "$'\\n'");
        else {
            sb_putc(b, '\\');
            sb_putc(b, *p);
        }
    }
}

/* One pass over the format. Returns how many arguments it used, or -1
 * when \c or %b's \c said to stop. */
static int printf_once(strbuf_t *out, const char *fmt, char **args, int nargs)
{
    int used = 0;
    for (const char *p = fmt; *p; ) {
        if (*p == '\\') {
            /* one escape at a time */
            const char *q = p + 1;
            char tmp[8];
            size_t tl = 0;
            tmp[tl++] = '\\';
            if (*q == 'x') {
                tmp[tl++] = *q++;
                for (int i = 0; i < 2 && hexval((u8)*q) >= 0; i++)
                    tmp[tl++] = *q++;
            } else if (*q >= '0' && *q <= '7') {
                for (int i = 0; i < 3 && *q >= '0' && *q <= '7'; i++)
                    tmp[tl++] = *q++;
            } else if (*q) {
                tmp[tl++] = *q++;
            }
            tmp[tl] = '\0';
            if (!unescape(out, tmp, false))
                return -1;
            p = q;
            continue;
        }
        if (*p != '%') {
            sb_putc(out, *p++);
            continue;
        }
        p++;
        if (*p == '%') {
            sb_putc(out, '%');
            p++;
            continue;
        }
        bool left = false, plus = false, space = false, alt = false, zero = false;
        for (;; p++) {
            if (*p == '-') left = true;
            else if (*p == '+') plus = true;
            else if (*p == ' ') space = true;
            else if (*p == '#') alt = true;
            else if (*p == '0') zero = true;
            else break;
        }
        int width = 0;
        if (*p == '*') {
            p++;
            width = used < nargs ? (int)printf_int(args[used], false) : 0;
            used++;
            if (width < 0) { left = true; width = -width; }
        } else {
            while (is_digit((u8)*p))
                width = width * 10 + (*p++ - '0');
        }
        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                p++;
                prec = used < nargs ? (int)printf_int(args[used], false) : 0;
                used++;
            } else {
                while (is_digit((u8)*p))
                    prec = prec * 10 + (*p++ - '0');
            }
        }
        while (*p && strchr("hlLqjzt", *p))
            p++;
        char conv = *p;
        if (!conv) {
            sh_warn("printf: %%: missing format character");
            printf_err = true;
            return -1;
        }
        p++;
        const char *arg = used < nargs ? args[used] : NULL;
        if (strchr("diouxXcsbqeEfFgGaA", conv))
            used++;
        strbuf_t f = {0};
        switch (conv) {
        case 'd': case 'i': {
            long long v = arg ? printf_int(arg, false) : 0;
            char num[32];
            bool neg = v < 0;
            unsigned long long u = neg ? 0 - (unsigned long long)v
                                       : (unsigned long long)v;
            int k = 0;
            do { num[k++] = (char)('0' + u % 10); u /= 10; } while (u);
            if (neg) sb_putc(&f, '-');
            else if (plus) sb_putc(&f, '+');
            else if (space) sb_putc(&f, ' ');
            for (int z = k; z < prec; z++) sb_putc(&f, '0');
            if (!(prec == 0 && v == 0))
                while (k) sb_putc(&f, num[--k]);
            pad_out(out, f.s ? f.s : "", f.len, width, left, zero && prec < 0);
            break;
        }
        case 'o': case 'u': case 'x': case 'X': {
            unsigned long long v = arg ? (unsigned long long)printf_int(arg, true) : 0;
            unsigned base = conv == 'o' ? 8 : conv == 'u' ? 10 : 16;
            const char *dg = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char num[32];
            int k = 0;
            unsigned long long u = v;
            do { num[k++] = dg[u % base]; u /= base; } while (u);
            if (alt && v && conv == 'o') num[k++] = '0';
            if (alt && v && (conv == 'x' || conv == 'X')) {
                sb_putc(&f, '0');
                sb_putc(&f, conv);
            }
            for (int z = k; z < prec; z++) sb_putc(&f, '0');
            if (!(prec == 0 && v == 0))
                while (k) sb_putc(&f, num[--k]);
            pad_out(out, f.s ? f.s : "", f.len, width, left, zero && prec < 0);
            break;
        }
        case 'c': {
            if (arg && *arg) {
                int n = utf8_seq_len((u8)*arg);
                if (n < 1 || (size_t)n > strlen(arg)) n = 1;
                pad_out(out, arg, (size_t)n, width, left, false);
            } else {
                pad_out(out, "", arg ? 0 : 0, width, left, false);
                if (arg && !*arg) { /* %c of "" prints nothing */ }
            }
            break;
        }
        case 's': {
            const char *s = arg ? arg : "";
            size_t n = strlen(s);
            if (prec >= 0 && (size_t)prec < n) {
                /* precision counts bytes; do not cut a character */
                n = (size_t)prec;
                while (n > 0 && ((u8)s[n] & 0xC0) == 0x80) n--;
            }
            pad_out(out, s, n, width, left, false);
            break;
        }
        case 'b': {
            strbuf_t t = {0};
            bool go = unescape(&t, arg ? arg : "", true);
            size_t n = t.len;
            if (prec >= 0 && (size_t)prec < n) n = (size_t)prec;
            pad_out(out, t.s ? t.s : "", n, width, left, false);
            sb_free(&t);
            if (!go) {
                sb_free(&f);
                return -1;
            }
            break;
        }
        case 'q': {
            strbuf_t t = {0};
            sb_shell_quote(&t, arg ? arg : "");
            pad_out(out, t.s ? t.s : "", t.len, width, left, false);
            sb_free(&t);
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
        case 'a': case 'A': {
            double v = arg ? printf_double(arg) : 0;
            if (prec < 0) prec = 6;
            if (v >= 0 && plus) sb_putc(&f, '+');
            else if (v >= 0 && space) sb_putc(&f, ' ');
            if (conv == 'f' || conv == 'F')
                fmt_fixed(&f, v, prec);
            else if (conv == 'e' || conv == 'E' || conv == 'a' || conv == 'A')
                fmt_exp(&f, v, prec, conv == 'E' || conv == 'A' ? 'E' : 'e');
            else
                fmt_general(&f, v, prec, alt, conv == 'G' ? 'E' : 'e');
            pad_out(out, f.s ? f.s : "", f.len, width, left, zero);
            break;
        }
        default:
            sh_warn("printf: %%%c: invalid format character", conv);
            printf_err = true;
            sb_free(&f);
            return -1;
        }
        sb_free(&f);
    }
    return used;
}

static int bi_printf(int argc, char **argv)
{
    int i = 1;
    const char *var = NULL;
    if (i < argc && strcmp(argv[i], "-v") == 0) {
        if (i + 1 >= argc) {
            bi_usage("printf", "printf [-v var] format [arguments]");
            return 2;
        }
        var = argv[i + 1];
        i += 2;
    }
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (i >= argc) {
        bi_usage("printf", "printf [-v var] format [arguments]");
        return 2;
    }
    const char *fmt = argv[i++];
    char **args = argv + i;
    int nargs = argc - i;
    printf_err = false;
    strbuf_t out = {0};
    int done = 0;
    for (;;) {
        int used = printf_once(&out, fmt, args + done, nargs - done);
        if (used < 0)
            break;
        done += used;
        if (used == 0 || done >= nargs)
            break;
    }
    if (var) {
        if (!var_set(var, out.s ? out.s : "", 0))
            printf_err = true;
    } else {
        outn(out1, out.s ? out.s : "", out.len);
    }
    sb_free(&out);
    return printf_err ? 1 : 0;
}

/* ── test / [ ─────────────────────────────────────────────────────── */

static bool test_is_unary(const char *s)
{
    static const char *const u[] = {
        "-b", "-c", "-d", "-e", "-f", "-g", "-G", "-h", "-k", "-L", "-n",
        "-N", "-O", "-p", "-r", "-s", "-S", "-t", "-u", "-w", "-x", "-z",
        "-a", "-v", "-o", NULL
    };
    for (int i = 0; u[i]; i++)
        if (strcmp(u[i], s) == 0)
            return true;
    return false;
}

static bool test_is_binary(const char *s)
{
    static const char *const b[] = {
        "=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge",
        "-nt", "-ot", "-ef", NULL
    };
    for (int i = 0; b[i]; i++)
        if (strcmp(b[i], s) == 0)
            return true;
    return false;
}

static int test_unary_op(const char *op, const char *arg)
{
    lp_stat_t st;
    char c = op[1];
    switch (c) {
    case 'n': return *arg ? 0 : 1;
    case 'z': return *arg ? 1 : 0;
    case 't': {
        long long fd;
        if (!parse_int(arg, &fd))
            return 1;
        return fd >= 0 && fd < 1024 && lp_isatty((int)fd) ? 0 : 1;
    }
    case 'h': case 'L':
        return lp_stat(arg, &st, false) == 0 &&
               (st.mode & LP_S_IFMT) == LP_S_IFLNK ? 0 : 1;
    case 'v':
        return var_get(arg) ? 0 : 1;
    case 'o': {
        int o = opt_index(arg);
        return o >= 0 && opt[o] ? 0 : 1;
    }
    }
    if (!*arg || lp_stat(arg, &st, true) != 0)
        return 1;
    u32 t = st.mode & LP_S_IFMT;
    switch (c) {
    case 'a': case 'e': return 0;
    case 'f': return t == LP_S_IFREG ? 0 : 1;
    case 'd': return t == LP_S_IFDIR ? 0 : 1;
    case 'b': return t == LP_S_IFBLK ? 0 : 1;
    case 'c': return t == LP_S_IFCHR ? 0 : 1;
    case 'p': return t == LP_S_IFIFO ? 0 : 1;
    case 'S': return t == LP_S_IFSOCK ? 0 : 1;
    case 's': return st.size > 0 ? 0 : 1;
    case 'g': return (st.mode & 02000) ? 0 : 1;
    case 'u': return (st.mode & 04000) ? 0 : 1;
    case 'k': return (st.mode & 01000) ? 0 : 1;
    case 'r': return lp_access(arg, R_OK) == 0 ? 0 : 1;
    case 'w': return lp_access(arg, W_OK) == 0 ? 0 : 1;
    case 'x': return lp_access(arg, X_OK) == 0 ? 0 : 1;
    case 'O': return (int)st.uid == sys_geteuid() ? 0 : 1;
    case 'G': return (int)st.gid == sys_getegid() ? 0 : 1;
    case 'N': return st.mtime > st.atime ||
                     (st.mtime == st.atime && st.mtime_ns > st.atime_ns) ? 0 : 1;
    }
    return 2;
}

static bool test_int(const char *s, long long *v, const char *cmd)
{
    if (parse_int(s, v))
        return true;
    sh_warn("%s: %s: integer expression expected", cmd, s);
    return false;
}

static const char *test_cmd_name = "test";

static int test_binary_op(const char *l, const char *op, const char *r)
{
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0)
        return strcmp(l, r) == 0 ? 0 : 1;
    if (strcmp(op, "!=") == 0)
        return strcmp(l, r) != 0 ? 0 : 1;
    if (strcmp(op, "<") == 0)
        return strcmp(l, r) < 0 ? 0 : 1;
    if (strcmp(op, ">") == 0)
        return strcmp(l, r) > 0 ? 0 : 1;
    if (strcmp(op, "-nt") == 0 || strcmp(op, "-ot") == 0) {
        lp_stat_t a, b;
        bool ha = lp_stat(l, &a, true) == 0, hb = lp_stat(r, &b, true) == 0;
        bool nt;
        if (!ha || !hb)
            nt = op[1] == 'n' ? (ha && !hb) : (!ha && hb);
        else if (op[1] == 'n')
            nt = a.mtime > b.mtime || (a.mtime == b.mtime && a.mtime_ns > b.mtime_ns);
        else
            nt = a.mtime < b.mtime || (a.mtime == b.mtime && a.mtime_ns < b.mtime_ns);
        return nt ? 0 : 1;
    }
    if (strcmp(op, "-ef") == 0) {
        lp_stat_t a, b;
        if (lp_stat(l, &a, true) != 0 || lp_stat(r, &b, true) != 0)
            return 1;
        return a.dev == b.dev && a.ino == b.ino ? 0 : 1;
    }
    long long a, b;
    if (!test_int(l, &a, test_cmd_name) || !test_int(r, &b, test_cmd_name))
        return 2;
    if (strcmp(op, "-eq") == 0) return a == b ? 0 : 1;
    if (strcmp(op, "-ne") == 0) return a != b ? 0 : 1;
    if (strcmp(op, "-lt") == 0) return a < b ? 0 : 1;
    if (strcmp(op, "-le") == 0) return a <= b ? 0 : 1;
    if (strcmp(op, "-gt") == 0) return a > b ? 0 : 1;
    if (strcmp(op, "-ge") == 0) return a >= b ? 0 : 1;
    return 2;
}

/* The general case: -a, -o, !, ( ) with the usual precedence. */
typedef struct {
    char **v;
    int    n, i;
    bool   err;
} tparse_t;

static int t_or(tparse_t *t);

static void t_error(tparse_t *t, const char *what)
{
    if (!t->err)
        sh_warn("%s: %s", test_cmd_name, what);
    t->err = true;
}

static int t_primary(tparse_t *t)
{
    if (t->i >= t->n) {
        t_error(t, "argument expected");
        return 2;
    }
    const char *a = t->v[t->i];
    if (strcmp(a, "(") == 0) {
        t->i++;
        int r = t_or(t);
        if (t->i >= t->n || strcmp(t->v[t->i], ")") != 0) {
            t_error(t, "`)' expected");
            return 2;
        }
        t->i++;
        return r;
    }
    /* binary: a op b */
    if (t->i + 2 < t->n + 0 || t->i + 2 == t->n) {
        if (t->i + 1 < t->n && test_is_binary(t->v[t->i + 1]) &&
            t->i + 2 < t->n) {
            int r = test_binary_op(a, t->v[t->i + 1], t->v[t->i + 2]);
            t->i += 3;
            if (r == 2) t->err = true;
            return r;
        }
    }
    if (test_is_unary(a) && strcmp(a, "-a") && strcmp(a, "-o") &&
        t->i + 1 < t->n) {
        int r = test_unary_op(a, t->v[t->i + 1]);
        t->i += 2;
        return r;
    }
    t->i++;
    return *a ? 0 : 1;
}

static int t_not(tparse_t *t)
{
    if (t->i < t->n && strcmp(t->v[t->i], "!") == 0 && t->i + 1 < t->n) {
        t->i++;
        int r = t_not(t);
        return r == 2 ? 2 : !r;
    }
    return t_primary(t);
}

static int t_and(tparse_t *t)
{
    int r = t_not(t);
    while (t->i < t->n && strcmp(t->v[t->i], "-a") == 0) {
        t->i++;
        int r2 = t_not(t);
        if (r == 2 || r2 == 2) r = 2;
        else r = (r == 0 && r2 == 0) ? 0 : 1;
    }
    return r;
}

static int t_or(tparse_t *t)
{
    int r = t_and(t);
    while (t->i < t->n && strcmp(t->v[t->i], "-o") == 0) {
        t->i++;
        int r2 = t_and(t);
        if (r == 2 || r2 == 2) r = 2;
        else r = (r == 0 || r2 == 0) ? 0 : 1;
    }
    return r;
}

static int test_eval(char **v, int n);

static int test_eval(char **v, int n)
{
    switch (n) {
    case 0:
        return 1;
    case 1:
        return *v[0] ? 0 : 1;
    case 2:
        if (strcmp(v[0], "!") == 0)
            return *v[1] ? 1 : 0;
        if (test_is_unary(v[0]))
            return test_unary_op(v[0], v[1]);
        sh_warn("%s: %s: unary operator expected", test_cmd_name, v[0]);
        return 2;
    case 3:
        if (test_is_binary(v[1]))
            return test_binary_op(v[0], v[1], v[2]);
        if (strcmp(v[1], "-a") == 0)
            return (*v[0] && *v[2]) ? 0 : 1;
        if (strcmp(v[1], "-o") == 0)
            return (*v[0] || *v[2]) ? 0 : 1;
        if (strcmp(v[0], "!") == 0) {
            int r = test_eval(v + 1, 2);
            return r == 2 ? 2 : !r;
        }
        if (strcmp(v[0], "(") == 0 && strcmp(v[2], ")") == 0)
            return *v[1] ? 0 : 1;
        break;
    case 4:
        if (strcmp(v[0], "!") == 0) {
            int r = test_eval(v + 1, 3);
            return r == 2 ? 2 : !r;
        }
        if (strcmp(v[0], "(") == 0 && strcmp(v[3], ")") == 0)
            return test_eval(v + 1, 2);
        break;
    }
    tparse_t t = { v, n, 0, false };
    int r = t_or(&t);
    if (!t.err && t.i < t.n) {
        sh_warn("%s: %s: unexpected operator", test_cmd_name, t.v[t.i]);
        return 2;
    }
    return t.err ? 2 : r;
}

static int test_main(int argc, char **argv, bool bracket)
{
    test_cmd_name = bracket ? "[" : "test";
    if (bracket) {
        if (strcmp(argv[argc - 1], "]") != 0) {
            sh_warn("[: missing `]'");
            return 2;
        }
        argc--;
    }
    return test_eval(argv + 1, argc - 1);
}

static int bi_test(int argc, char **argv)    { return test_main(argc, argv, false); }
static int bi_bracket(int argc, char **argv) { return test_main(argc, argv, true); }

/* ── cd, pwd, pushd, popd, dirs ────────────────────────────────────── */

static strvec_t dirstack;

static bool same_file(const char *a, const char *b)
{
    lp_stat_t x, y;
    return lp_stat(a, &x, true) == 0 && lp_stat(b, &y, true) == 0 &&
           x.dev == y.dev && x.ino == y.ino;
}

static const char *pwd_logical(void)
{
    const char *p = var_get("PWD");
    if (p && p[0] == '/' && same_file(p, "."))
        return p;
    return NULL;
}

static char *pwd_physical(void)
{
    char buf[4096];
    if (lp_getcwd(buf, sizeof buf) < 0)
        return NULL;
    return xstrdup(buf);
}

/* At startup: trust an inherited PWD only when it really names the
 * current directory (POSIX), otherwise ask the kernel. */
static void pwd_update(void)
{
    if (pwd_logical())
        return;
    char *p = pwd_physical();
    if (p) {
        var_set("PWD", p, V_EXPORT);
        xfree(p);
    }
}

static void dirs_init(void)
{
    memset(&dirstack, 0, sizeof dirstack);
}

/* Lexically clean an absolute path: // . and .. */
static char *path_clean(const char *path)
{
    strvec_t parts = {0};
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t n = (size_t)(e - p);
        if (n == 1 && p[0] == '.') {
            /* skip */
        } else if (n == 2 && p[0] == '.' && p[1] == '.') {
            if (parts.n) {
                xfree(parts.v[--parts.n]);
                parts.v[parts.n] = NULL;
            }
        } else {
            sv_push(&parts, xstrndup(p, n));
        }
        p = e;
    }
    strbuf_t b = {0};
    for (int i = 0; i < parts.n; i++) {
        sb_putc(&b, '/');
        sb_puts(&b, parts.v[i]);
    }
    if (!b.len)
        sb_putc(&b, '/');
    sv_free(&parts);
    return sb_take(&b);
}

/* Change directory the way cd -L does: work out the new logical path,
 * go there, and fall back to the physical one if the logical path fails
 * (a symlink's parent that no longer exists, say). Returns 0 or an
 * errno. */
static long do_chdir(const char *dir, bool physical, char **newpwd)
{
    *newpwd = NULL;
    if (!physical) {
        strbuf_t b = {0};
        if (dir[0] != '/') {
            const char *cur = pwd_logical();
            char *phys = NULL;
            if (!cur) {
                phys = pwd_physical();
                cur = phys ? phys : "/";
            }
            sb_puts(&b, cur);
            sb_putc(&b, '/');
            xfree(phys);
        }
        sb_puts(&b, dir);
        char *clean = path_clean(sb_str(&b));
        sb_free(&b);
        long r = lp_chdir(clean);
        if (r == 0) {
            *newpwd = clean;
            return 0;
        }
        xfree(clean);
    }
    long r = lp_chdir(dir);
    if (r < 0)
        return -r;
    *newpwd = pwd_physical();
    return 0;
}

static void set_pwd(char *newpwd)
{
    const char *old = var_get("PWD");
    char *oldc = old ? xstrdup(old) : pwd_physical();
    if (oldc)
        var_set("OLDPWD", oldc, V_EXPORT);
    xfree(oldc);
    if (newpwd)
        var_set("PWD", newpwd, V_EXPORT);
}

static bool cd_to(const char *dir, bool physical, bool print, const char *cmd)
{
    char *newpwd = NULL;
    long err = E_NOENT;
    bool tried_cdpath = false;
    /* CDPATH, for a relative name that does not start with . or .. */
    const char *cdpath = var_get("CDPATH");
    if (cdpath && dir[0] != '/' &&
        !(dir[0] == '.' && (dir[1] == '\0' || dir[1] == '/' ||
                            (dir[1] == '.' && (dir[2] == '\0' || dir[2] == '/'))))) {
        const char *p = cdpath;
        for (;;) {
            const char *e = strchr(p, ':');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            strbuf_t b = {0};
            if (n) {
                sb_putn(&b, p, n);
                if (b.s[b.len - 1] != '/')
                    sb_putc(&b, '/');
            }
            sb_puts(&b, dir);
            lp_stat_t st;
            if (lp_stat(sb_str(&b), &st, true) == 0 &&
                (st.mode & LP_S_IFMT) == LP_S_IFDIR) {
                err = do_chdir(b.s, physical, &newpwd);
                if (err == 0) {
                    if (n)
                        print = true;
                    sb_free(&b);
                    tried_cdpath = true;
                    break;
                }
            }
            sb_free(&b);
            if (!e)
                break;
            p = e + 1;
        }
    }
    if (!tried_cdpath)
        err = do_chdir(dir, physical, &newpwd);
    if (err) {
        sh_warn("%s: %s: %s", cmd, dir, lp_strerror((int)err));
        return false;
    }
    set_pwd(newpwd);
    if (print) {
        outs(out1, newpwd ? newpwd : dir);
        outc(out1, '\n');
    }
    xfree(newpwd);
    return true;
}

static int bi_cd(int argc, char **argv)
{
    bool physical = false;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1])
            break;
        if (strcmp(a, "--") == 0) { i++; break; }
        bool ok = true;
        for (const char *p = a + 1; *p; p++) {
            if (*p == 'P') physical = true;
            else if (*p == 'L') physical = false;
            else if (*p == 'e' || *p == '@') ;
            else ok = false;
        }
        if (!ok) {
            sh_warn("cd: %s: invalid option", a);
            bi_usage("cd", "cd [-L|-P] [dir]");
            return 2;
        }
    }
    if (argc - i > 1) {
        sh_warn("cd: too many arguments");
        return 1;
    }
    const char *dir = i < argc ? argv[i] : NULL;
    bool print = false;
    if (!dir) {
        dir = var_get("HOME");
        if (!dir || !*dir) {
            sh_warn("cd: HOME not set");
            return 1;
        }
    } else if (strcmp(dir, "-") == 0) {
        dir = var_get("OLDPWD");
        if (!dir || !*dir) {
            sh_warn("cd: OLDPWD not set");
            return 1;
        }
        print = true;
    }
    if (!*dir)
        return 0;
    char *d = xstrdup(dir);
    bool ok = cd_to(d, physical, print, "cd");
    xfree(d);
    return ok ? 0 : 2;
}

static int bi_pwd(int argc, char **argv)
{
    bool physical = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-P") == 0) physical = true;
        else if (strcmp(argv[i], "-L") == 0) physical = false;
        else if (argv[i][0] == '-') {
            sh_warn("pwd: %s: invalid option", argv[i]);
            return 2;
        }
    }
    if (!physical) {
        const char *p = pwd_logical();
        if (p) {
            outs(out1, p);
            outc(out1, '\n');
            return 0;
        }
    }
    char *p = pwd_physical();
    if (!p) {
        sh_warn("pwd: cannot determine the current directory");
        return 1;
    }
    outs(out1, p);
    outc(out1, '\n');
    xfree(p);
    return 0;
}

static void dir_shown(strbuf_t *b, const char *d, bool longform)
{
    const char *home = var_get("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (!longform && home && hl > 1 && strncmp(d, home, hl) == 0 &&
        (d[hl] == '/' || d[hl] == '\0')) {
        sb_putc(b, '~');
        sb_puts(b, d + hl);
    } else {
        sb_puts(b, d);
    }
}

static const char *dirs_top(void)
{
    const char *p = pwd_logical();
    static char *phys;
    if (p)
        return p;
    xfree(phys);
    phys = pwd_physical();
    return phys ? phys : ".";
}

/* dirs prints the current directory first, then the stack. */
static void dirs_print(bool longform, bool vertical)
{
    int n = dirstack.n + 1;
    strbuf_t b = {0};
    for (int i = 0; i < n; i++) {
        const char *d = i == 0 ? dirs_top() : dirstack.v[dirstack.n - i];
        if (vertical)
            sb_printf(&b, "%2d  ", i);
        dir_shown(&b, d, longform);
        sb_putc(&b, vertical || i == n - 1 ? '\n' : ' ');
    }
    outn(out1, b.s, b.len);
    sb_free(&b);
}

static bool dirs_index(const char *a, int *idx)
{
    long long n;
    if ((a[0] != '+' && a[0] != '-') || !parse_int(a + 1, &n))
        return false;
    int total = dirstack.n + 1;
    int k = a[0] == '+' ? (int)n : total - 1 - (int)n;
    if (k < 0 || k >= total)
        return false;
    *idx = k;
    return true;
}

static int bi_dirs(int argc, char **argv)
{
    bool longform = false, vertical = false, clear = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int idx;
        if (strcmp(a, "-c") == 0) clear = true;
        else if (strcmp(a, "-l") == 0) longform = true;
        else if (strcmp(a, "-v") == 0 || strcmp(a, "-p") == 0) vertical = true;
        else if (dirs_index(a, &idx)) {
            strbuf_t b = {0};
            dir_shown(&b, idx == 0 ? dirs_top() : dirstack.v[dirstack.n - idx],
                      longform);
            sb_putc(&b, '\n');
            outn(out1, b.s, b.len);
            sb_free(&b);
            return 0;
        } else {
            sh_warn("dirs: %s: invalid option", a);
            return 2;
        }
    }
    if (clear) {
        sv_free(&dirstack);
        return 0;
    }
    dirs_print(longform, vertical);
    return 0;
}

static int bi_pushd(int argc, char **argv)
{
    bool quiet = false;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-n") == 0) { quiet = true; i++; }
    if (i < argc && strcmp(argv[i], "--") == 0) i++;
    char *cur = xstrdup(dirs_top());
    if (i >= argc) {
        if (dirstack.n == 0) {
            sh_warn("pushd: no other directory");
            xfree(cur);
            return 1;
        }
        char *top = dirstack.v[dirstack.n - 1];
        if (!cd_to(top, false, false, "pushd")) {
            xfree(cur);
            return 1;
        }
        xfree(dirstack.v[dirstack.n - 1]);
        dirstack.v[dirstack.n - 1] = cur;
    } else {
        int idx;
        if (dirs_index(argv[i], &idx)) {
            /* rotate so entry idx is on top */
            int total = dirstack.n + 1;
            char **all = xmalloc((size_t)total * sizeof(char *));
            all[0] = cur;
            for (int k = 1; k < total; k++)
                all[k] = dirstack.v[dirstack.n - k];
            char **rot = xmalloc((size_t)total * sizeof(char *));
            for (int k = 0; k < total; k++)
                rot[k] = all[(idx + k) % total];
            if (!cd_to(rot[0], false, false, "pushd")) {
                xfree(all);
                xfree(rot);
                xfree(cur);
                return 1;
            }
            for (int k = 1; k < total; k++)
                dirstack.v[dirstack.n - k] = rot[k];
            xfree(rot[0]);
            xfree(all);
            xfree(rot);
        } else {
            if (!cd_to(argv[i], false, false, "pushd")) {
                xfree(cur);
                return 1;
            }
            sv_push(&dirstack, cur);
        }
    }
    if (!quiet)
        dirs_print(false, false);
    return 0;
}

static int bi_popd(int argc, char **argv)
{
    bool quiet = false;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-n") == 0) { quiet = true; i++; }
    if (dirstack.n == 0) {
        sh_warn("popd: directory stack empty");
        return 1;
    }
    if (i < argc) {
        int idx;
        if (!dirs_index(argv[i], &idx)) {
            sh_warn("popd: %s: invalid argument", argv[i]);
            return 2;
        }
        if (idx > 0) {
            int k = dirstack.n - idx;
            xfree(dirstack.v[k]);
            for (int m = k; m < dirstack.n - 1; m++)
                dirstack.v[m] = dirstack.v[m + 1];
            dirstack.n--;
            dirstack.v[dirstack.n] = NULL;
            if (!quiet) dirs_print(false, false);
            return 0;
        }
    }
    char *top = dirstack.v[dirstack.n - 1];
    if (!cd_to(top, false, false, "popd"))
        return 1;
    xfree(top);
    dirstack.n--;
    dirstack.v[dirstack.n] = NULL;
    if (!quiet)
        dirs_print(false, false);
    return 0;
}

/* ── variables: export readonly unset local declare let ────────────── */

/* name=value or name+=value or name: split it. */
static bool split_nameval(const char *arg, char **name, const char **val,
                          bool *append)
{
    const char *eq = strchr(arg, '=');
    *append = false;
    size_t n = eq ? (size_t)(eq - arg) : strlen(arg);
    if (eq && n > 0 && arg[n - 1] == '+') {
        *append = true;
        n--;
    }
    *name = xstrndup(arg, n);
    *val = eq ? eq + 1 : NULL;
    return is_valid_name(*name);
}

static bool set_with_append(const char *name, const char *val, bool append,
                            int flags)
{
    if (!append)
        return var_set(name, val, flags);
    strbuf_t b = {0};
    const char *old = var_get(name);
    sb_puts(&b, old ? old : "");
    sb_puts(&b, val);
    bool ok = var_set(name, b.s, flags);
    sb_free(&b);
    return ok;
}

static int export_like(int argc, char **argv, int flag, const char *cmd)
{
    bool set[3] = { false, false, false };
    int i;
    char bad = bi_opts(argc, argv, flag == V_EXPORT ? "pnf" : "pf", set, &i);
    if (bad) {
        sh_warn("%s: -%c: invalid option", cmd, bad);
        return 2;
    }
    bool unexport = flag == V_EXPORT && set[1];
    if (i >= argc) {
        var_print_flag(flag, cmd);
        return 0;
    }
    int st = 0;
    for (; i < argc; i++) {
        char *name;
        const char *val;
        bool append;
        if (!split_nameval(argv[i], &name, &val, &append)) {
            sh_warn("%s: `%s': not a valid identifier", cmd, argv[i]);
            xfree(name);
            st = 1;
            continue;
        }
        if (set[flag == V_EXPORT ? 2 : 1]) {
            /* -f: functions; exporting them is bash's, accepted quietly */
            if (!func_lookup(name)) {
                sh_warn("%s: %s: not a function", cmd, name);
                st = 1;
            }
            xfree(name);
            continue;
        }
        if (unexport) {
            var_t *v = var_lookup(name);
            if (v) {
                v->flags &= ~V_EXPORT;
                env_dirty = true;
            }
            if (val)
                set_with_append(name, val, append, 0);
        } else if (!set_with_append(name, val, append, flag)) {
            st = 1;
            xfree(name);
            if (flag == V_READONLY && !toplevel_interactive)
                exitshell(2);
            continue;
        }
        xfree(name);
    }
    return st;
}

static int bi_export(int argc, char **argv)   { return export_like(argc, argv, V_EXPORT, "export"); }
static int bi_readonly(int argc, char **argv) { return export_like(argc, argv, V_READONLY, "readonly"); }

static int bi_unset(int argc, char **argv)
{
    bool funcs = false, vars = false;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (strcmp(a, "--") == 0) { i++; break; }
        for (const char *p = a + 1; *p; p++) {
            if (*p == 'f') funcs = true;
            else if (*p == 'v') vars = true;
            else if (*p == 'n') ;
            else {
                sh_warn("unset: -%c: invalid option", *p);
                return 2;
            }
        }
    }
    int st = 0;
    for (; i < argc; i++) {
        const char *name = argv[i];
        if (funcs && !vars) {
            func_remove(name);
            continue;
        }
        /* name[idx]: one element */
        const char *br = strchr(name, '[');
        if (br && name[strlen(name) - 1] == ']') {
            char *nm = xstrndup(name, (size_t)(br - name));
            char *ix = xstrndup(br + 1, strlen(br + 1) - 1);
            var_t *v = var_lookup(nm);
            if (v && (v->flags & V_READONLY)) {
                sh_warn("unset: %s: cannot unset: readonly variable", nm);
                st = 1;
            } else if (v && (v->flags & V_ARRAY)) {
                if (strcmp(ix, "@") == 0 || strcmp(ix, "*") == 0) {
                    var_unset(nm);
                } else {
                    long long idx = 0;
                    arith_eval(ix, &idx);
                    if (idx < 0) idx += v->arrn;
                    if (idx >= 0 && idx < v->arrn) {
                        xfree(v->arr[idx]);
                        v->arr[idx] = NULL;
                        while (v->arrn > 0 && !v->arr[v->arrn - 1])
                            v->arrn--;
                    }
                }
            } else if (v) {
                long long idx = 0;
                arith_eval(ix, &idx);
                if (idx == 0) var_unset(nm);
            }
            xfree(nm);
            xfree(ix);
            continue;
        }
        if (!is_valid_name(name)) {
            sh_warn("unset: `%s': not a valid identifier", name);
            st = 1;
            continue;
        }
        var_t *v = var_lookup(name);
        if (v && (v->flags & V_READONLY)) {
            sh_warn("unset: %s: cannot unset: readonly variable", name);
            st = 1;
            continue;
        }
        if (v && !(v->flags & V_UNSET)) {
            var_unset(name);
            continue;
        }
        if (v) {
            var_unset(name);
            continue;
        }
        if (!vars)
            func_remove(name);
    }
    return st;
}

static int bi_local(int argc, char **argv)
{
    if (funcnest == 0) {
        sh_warn("local: can only be used in a function");
        return 1;
    }
    int flags = 0;
    bool array = false;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (strcmp(a, "--") == 0) { i++; break; }
        for (const char *p = a + 1; *p; p++) {
            if (*p == 'r') flags |= V_READONLY;
            else if (*p == 'x') flags |= V_EXPORT;
            else if (*p == 'i') flags |= V_INTEGER;
            else if (*p == 'a') array = true;
            else {
                sh_warn("local: -%c: invalid option", *p);
                return 2;
            }
        }
    }
    int st = 0;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            /* local -: the options come back when the function returns */
            if (!opts_saved_local) {
                opts_saved_local = true;
                opts_saved_ptr = xmalloc(sizeof opt);
                memcpy(opts_saved_ptr, opt, sizeof opt);
            }
            continue;
        }
        char *name;
        const char *val;
        bool append;
        if (!split_nameval(argv[i], &name, &val, &append)) {
            sh_warn("local: `%s': not a valid identifier", argv[i]);
            xfree(name);
            st = 1;
            continue;
        }
        if (append && val) {
            const char *old = var_get(name);
            strbuf_t b = {0};
            sb_puts(&b, old ? old : "");
            sb_puts(&b, val);
            var_local(name, b.s, flags & ~V_READONLY, true);
            sb_free(&b);
        } else {
            var_local(name, val, flags & ~V_READONLY, val != NULL);
        }
        if (array) {
            var_t *v = var_lookup(name);
            if (v && !(v->flags & V_ARRAY) && !val) {
                strvec_t none = {0};
                var_set_array(name, &none, false);
            }
        }
        if (flags & V_READONLY)
            var_set(name, NULL, V_READONLY);
        xfree(name);
    }
    return st;
}

static void print_decl(var_t *v)
{
    strbuf_t b = {0};
    sb_puts(&b, "declare -");
    if (v->flags & V_ARRAY) sb_putc(&b, 'a');
    if (v->flags & V_INTEGER) sb_putc(&b, 'i');
    if (v->flags & V_READONLY) sb_putc(&b, 'r');
    if (v->flags & V_EXPORT) sb_putc(&b, 'x');
    if (!(v->flags & (V_ARRAY | V_INTEGER | V_READONLY | V_EXPORT)))
        sb_putc(&b, '-');
    sb_putc(&b, ' ');
    sb_puts(&b, v->name);
    if (v->flags & V_ARRAY) {
        sb_puts(&b, "=(");
        bool first = true;
        for (int i = 0; i < v->arrn; i++) {
            if (!v->arr[i]) continue;
            if (!first) sb_putc(&b, ' ');
            first = false;
            sb_printf(&b, "[%d]=", i);
            sb_putc(&b, '"');
            for (const char *p = v->arr[i]; *p; p++) {
                if (strchr("\"\\$`", *p)) sb_putc(&b, '\\');
                sb_putc(&b, *p);
            }
            sb_putc(&b, '"');
        }
        sb_putc(&b, ')');
    } else if (!(v->flags & V_UNSET)) {
        const char *val = var_get(v->name);
        if (val) {
            sb_puts(&b, "=\"");
            for (const char *p = val; *p; p++) {
                if (strchr("\"\\$`", *p)) sb_putc(&b, '\\');
                sb_putc(&b, *p);
            }
            sb_putc(&b, '"');
        }
    }
    sb_putc(&b, '\n');
    outn(out1, b.s, b.len);
    sb_free(&b);
}

static void print_function(func_t *f)
{
    strbuf_t b = {0};
    fmt_node(&b, f->body, 0);
    sb_putc(&b, '\n');
    outn(out1, b.s, b.len);
    sb_free(&b);
}

static int bi_declare(int argc, char **argv)
{
    int flags = 0, unflags = 0;
    bool print = false, funcs = false, fnames = false, global = false;
    bool array = false;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if ((a[0] != '-' && a[0] != '+') || !a[1]) break;
        if (strcmp(a, "--") == 0) { i++; break; }
        bool plus = a[0] == '+';
        for (const char *p = a + 1; *p; p++) {
            int f = 0;
            switch (*p) {
            case 'p': print = true; break;
            case 'f': funcs = true; break;
            case 'F': fnames = true; break;
            case 'g': global = true; break;
            case 'a': array = true; break;
            case 'A':
                sh_warn("declare: -A: associative arrays are not supported");
                return 2;
            case 'r': f = V_READONLY; break;
            case 'x': f = V_EXPORT; break;
            case 'i': f = V_INTEGER; break;
            case 'l': case 'u': case 't': case 'n': break;
            default:
                sh_warn("declare: -%c: invalid option", *p);
                return 2;
            }
            if (plus) unflags |= f; else flags |= f;
        }
    }
    if (funcs || fnames) {
        if (i >= argc) {
            for (int h = 0; h < FHASH; h++)
                for (func_t *f = ftab[h]; f; f = f->next) {
                    if (fnames) outf(out1, "declare -f %s\n", f->name);
                    else print_function(f);
                }
            return 0;
        }
        int st = 0;
        for (; i < argc; i++) {
            func_t *f = func_lookup(argv[i]);
            if (!f) { st = 1; continue; }
            if (fnames) outf(out1, "%s\n", f->name);
            else print_function(f);
        }
        return st;
    }
    if (i >= argc) {
        int n;
        char **names = var_names_sorted(&n);
        for (int k = 0; k < n; k++) {
            var_t *v = var_lookup(names[k]);
            if (!v || (v->flags & V_SPECIAL))
                continue;
            if (flags && !(v->flags & flags))
                continue;
            if (print || flags)
                print_decl(v);
            else if (!(v->flags & V_UNSET)) {
                outs(out1, v->name);
                outc(out1, '=');
                if (v->flags & V_ARRAY)
                    print_array_value(out1, v);
                else
                    print_quoted(out1, var_get(v->name) ? var_get(v->name) : "");
                outc(out1, '\n');
            }
        }
        xfree(names);
        return 0;
    }
    int st = 0;
    for (; i < argc; i++) {
        if (print) {
            var_t *v = var_lookup(argv[i]);
            if (!v) {
                sh_warn("declare: %s: not found", argv[i]);
                st = 1;
                continue;
            }
            print_decl(v);
            continue;
        }
        char *name;
        const char *val;
        bool append;
        if (!split_nameval(argv[i], &name, &val, &append)) {
            sh_warn("declare: `%s': not a valid identifier", argv[i]);
            xfree(name);
            st = 1;
            continue;
        }
        if (funcnest > 0 && !global)
            var_local(name, NULL, 0, false);
        var_t *v = var_lookup(name);
        if (v && (unflags & ~V_READONLY)) {
            v->flags &= ~(unflags & ~V_READONLY);
            env_dirty = true;
        }
        if ((flags & V_INTEGER) && val) {
            var_set(name, NULL, V_INTEGER);
            long long x = 0, y = 0;
            if (append)
                arith_eval(var_get(name) ? var_get(name) : "0", &x);
            if (!arith_eval(val, &y)) { st = 1; xfree(name); continue; }
            char nb[24];
            if (!var_set(name, itoa_s(x + y, nb), flags & ~V_READONLY))
                st = 1;
        } else if (val) {
            v = var_lookup(name);
            if (v && (v->flags & V_INTEGER)) {
                long long x = 0, y = 0;
                if (append)
                    arith_eval(var_get(name) ? var_get(name) : "0", &x);
                arith_eval(val, &y);
                char nb[24];
                if (!var_set(name, itoa_s(x + y, nb), flags & ~V_READONLY))
                    st = 1;
            } else if (!set_with_append(name, val, append, flags & ~V_READONLY)) {
                st = 1;
            }
        } else {
            if (!var_lookup(name))
                var_set(name, NULL, flags & ~V_READONLY);
            else if (flags & ~V_READONLY)
                var_set(name, NULL, flags & ~V_READONLY);
        }
        if (array) {
            v = var_lookup(name);
            if (v && !(v->flags & V_ARRAY)) {
                if (v->val || !(v->flags & V_UNSET)) {
                    strvec_t one = {0};
                    if (v->val) sv_push(&one, xstrdup(v->val));
                    var_set_array(name, &one, false);
                    sv_free(&one);
                } else {
                    strvec_t none = {0};
                    var_set_array(name, &none, false);
                }
            }
        }
        if (flags & V_READONLY)
            var_set(name, NULL, V_READONLY);
        xfree(name);
    }
    return st;
}

static int bi_let(int argc, char **argv)
{
    if (argc < 2) {
        sh_warn("let: expression expected");
        return 1;
    }
    long long v = 0;
    for (int i = 1; i < argc; i++) {
        expand_error = false;
        if (!arith_eval(argv[i], &v))
            return 1;
    }
    return v ? 0 : 1;
}

/* ── set, shift ────────────────────────────────────────────────────── */

static void print_options(bool reusable)
{
    for (int i = 0; i < NOPTS; i++) {
        if (i == O_interactive)
            continue;
        if (reusable)
            outf(out1, "set %co %s\n", opt[i] ? '-' : '+', optnames[i].name);
        else
            outf(out1, "%-15s\t%s\n", optnames[i].name, opt[i] ? "on" : "off");
    }
}

static int bi_set(int argc, char **argv)
{
    if (argc == 1) {
        var_print_all(true);
        return 0;
    }
    int i = 1;
    bool set_pos = false;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            set_pos = true;
            break;
        }
        if (strcmp(a, "-") == 0) {
            opt[O_xtrace] = opt[O_verbose] = false;
            i++;
            set_pos = i < argc;
            break;
        }
        if ((a[0] != '-' && a[0] != '+') || !a[1])
            break;
        bool on = a[0] == '-';
        for (const char *p = a + 1; *p; p++) {
            if (*p == 'o') {
                const char *nm = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                if (!nm) {
                    print_options(!on);
                    goto next;
                }
                int o = opt_index(nm);
                if (o < 0 || o == O_interactive) {
                    sh_warn("set: %s: invalid option name", nm);
                    return 2;
                }
                opt[o] = on;
                opt_changed(o);
                goto next;
            }
            int o = opt_by_letter(*p);
            if (o < 0 || o == O_interactive) {
                sh_warn("set: %c%c: invalid option", on ? '-' : '+', *p);
                return 2;
            }
            opt[o] = on;
            opt_changed(o);
        }
next:
        ;
    }
    if (i < argc || set_pos)
        pos_set(argv + i, argc - i);
    return 0;
}

static int bi_shift(int argc, char **argv)
{
    long long n = 1;
    if (argc > 1 && (!parse_int(argv[1], &n) || n < 0)) {
        sh_warn("shift: %s: numeric argument required", argv[1]);
        if (!toplevel_interactive)
            exitshell(2);
        return 2;
    }
    if (n > posc) {
        sh_warn("shift: can't shift that many");
        if (!toplevel_interactive)
            exitshell(2);
        return 2;
    }
    for (int i = 0; i < n; i++)
        xfree(posv[i]);
    memmove(posv, posv + n, (size_t)(posc - n + 1) * sizeof(char *));
    posc -= (int)n;
    return 0;
}

/* ── exit return break continue ────────────────────────────────────── */

static int bi_exit(int argc, char **argv)
{
    int st = exitstatus;
    if (argc > 1) {
        long long n;
        if (!parse_int(argv[1], &n)) {
            sh_warn("exit: %s: numeric argument required", argv[1]);
            st = 2;
        } else {
            st = (int)(n & 0xff);
        }
    }
    if (toplevel_interactive && !in_subshell) {
        if (jobs_any_stopped() && !warned_stopped) {
            warned_stopped = true;
            outs(out2, "There are stopped jobs.\n");
            return 1;
        }
    }
    if (in_trap && argc <= 1)
        st = exitstatus;
    exitshell(st);
    return st;
}

static int bi_logout(int argc, char **argv)
{
    if (!is_login) {
        sh_warn("logout: not login shell: use `exit'");
        return 1;
    }
    return bi_exit(argc, argv);
}

static int bi_return(int argc, char **argv)
{
    int st = exitstatus;
    if (argc > 1) {
        long long n;
        if (!parse_int(argv[1], &n)) {
            sh_warn("return: %s: numeric argument required", argv[1]);
            st = 2;
        } else {
            st = (int)(n & 0xff);
        }
    }
    if (funcnest == 0 && sourcenest == 0) {
        if (toplevel_interactive) {
            sh_warn("return: can only `return' from a function or sourced script");
            return 1;
        }
        /* dash: return at the top of a script ends the script */
        exitshell(st);
    }
    evalskip = SKIP_RETURN;
    exitstatus = st;
    return st;
}

static int loopctl(int argc, char **argv, int kind)
{
    long long n = 1;
    if (argc > 1 && (!parse_int(argv[1], &n) || n < 1)) {
        sh_warn("%s: %s: loop count out of range", argv[0], argv[1]);
        return 1;
    }
    if (loopnest == 0)
        return 0;
    if (n > loopnest)
        n = loopnest;
    evalskip = kind;
    skipcount = (int)n;
    return 0;
}

static int bi_break(int argc, char **argv)    { return loopctl(argc, argv, SKIP_BREAK); }
static int bi_continue(int argc, char **argv) { return loopctl(argc, argv, SKIP_CONT); }

/* ── eval, exec, . ─────────────────────────────────────────────────── */

static int bi_eval(int argc, char **argv)
{
    if (argc < 2)
        return 0;
    strbuf_t b = {0};
    for (int i = 1; i < argc; i++) {
        if (i > 1) sb_putc(&b, ' ');
        sb_puts(&b, argv[i]);
    }
    char *s = sb_take(&b);
    int st = evalstring(s, 0);
    xfree(s);
    return st;
}

static int bi_exec(int argc, char **argv)
{
    int i = 1;
    const char *arg0 = NULL;
    bool clear_env = false, login = false;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-a") == 0 && i + 1 < argc) { arg0 = argv[++i]; continue; }
        if (strcmp(a, "-c") == 0) { clear_env = true; continue; }
        if (strcmp(a, "-l") == 0) { login = true; continue; }
        if (strcmp(a, "-cl") == 0 || strcmp(a, "-lc") == 0) {
            clear_env = login = true;
            continue;
        }
        break;
    }
    if (i >= argc)
        return 0;           /* only redirections: done by the caller */
    const char *name = argv[i];
    char found[1024];
    found[0] = '\0';
    if (!strchr(name, '/') && !path_search(name, found, sizeof found, NULL, false)) {
        sh_warn("exec: %s: not found", name);
        if (!toplevel_interactive || in_subshell)
            exitshell(127);
        return 127;
    }
    if (toplevel_interactive && !in_subshell)
        history_write();
    /* the program must not inherit the interactive shell's ignored
     * signals or its hold on the terminal's process groups */
    signals_reset_for_child(false);
    set_sig_handler(20, 0);
    set_sig_handler(21, 0);
    set_sig_handler(22, 0);
    set_sig_handler(3, sig_ignored_at_entry[3] ? 1 : 0);
    set_sig_handler(15, sig_ignored_at_entry[15] ? 1 : 0);
    set_sig_handler(2, sig_ignored_at_entry[2] ? 1 : 0);
    int n = argc - i;
    char **nv = xmalloc((size_t)(n + 1) * sizeof(char *));
    for (int k = 0; k < n; k++)
        nv[k] = argv[i + k];
    nv[n] = NULL;
    strbuf_t a0 = {0};
    if (arg0 || login) {
        if (login) sb_putc(&a0, '-');
        sb_puts(&a0, arg0 ? arg0 : name);
        nv[0] = sb_str(&a0);
    }
    if (clear_env) {
        for (int h = 0; h < VHASH; h++)
            for (var_t *v = vtab[h]; v; v = v->next)
                v->flags &= ~V_EXPORT;
        env_dirty = true;
    }
    char **env = env_build();
    const char *path = found[0] ? found : name;
    flush_all();
    long r = lp_execve(path, nv, env);
    if (r == -E_NOEXEC)
        exec_as_script(path, nv, env);
    sh_warn("exec: %s: %s", name, lp_strerror((int)-r));
    if (!toplevel_interactive || in_subshell)
        exitshell(r == -E_NOENT ? 127 : 126);
    signals_init();
    sb_free(&a0);
    xfree(nv);
    return r == -E_NOENT ? 127 : 126;
}

static int bi_dot(int argc, char **argv)
{
    int i = 1;
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (i >= argc) {
        sh_warn("%s: filename argument required", argv[0]);
        bi_usage(argv[0], ". filename [arguments]");
        return 2;
    }
    const char *name = argv[i];
    char found[1024];
    const char *path = name;
    if (!strchr(name, '/')) {
        /* PATH first (POSIX); then the current directory, as bash does
         * outside posix mode - `. script` in the directory it lives in
         * is what people type */
        bool ok = false;
        const char *p = var_get("PATH");
        if (p) {
            const char *s = p;
            for (;;) {
                const char *e = strchr(s, ':');
                size_t dl = e ? (size_t)(e - s) : strlen(s);
                strbuf_t b = {0};
                if (dl) sb_putn(&b, s, dl); else sb_putc(&b, '.');
                sb_putc(&b, '/');
                sb_puts(&b, name);
                lp_stat_t st;
                if (lp_stat(sb_str(&b), &st, true) == 0 &&
                    (st.mode & LP_S_IFMT) == LP_S_IFREG) {
                    strlcpy(found, b.s, sizeof found);
                    ok = true;
                    sb_free(&b);
                    break;
                }
                sb_free(&b);
                if (!e) break;
                s = e + 1;
            }
        }
        if (ok)
            path = found;
        else if (!opt[O_posix] && lp_exists(name))
            path = name;
        else {
            sh_warn("%s: %s: file not found", argv[0], name);
            if (!toplevel_interactive || in_subshell)
                exitshell(2);
            return 2;
        }
    }
    /* arguments after the file become $1.. for its duration (bash) */
    char **save_v = NULL;
    int save_c = 0;
    bool args = argc - i > 1;
    if (args) {
        save_v = posv;
        save_c = posc;
        posv = NULL;
        posc = 0;
        pos_set(argv + i + 1, argc - i - 1);
    }
    exitstatus = 0;
    int st = run_file(path, true);
    if (args) {
        pos_free();
        posv = save_v;
        posc = save_c;
    }
    if (st < 0) {
        if (!toplevel_interactive || in_subshell)
            exitshell(2);
        return 2;
    }
    return exitstatus;
}

/* ── trap ──────────────────────────────────────────────────────────── */

static void trap_print_one(int sig, const char *cmd, const char *name)
{
    strbuf_t b = {0};
    sb_puts(&b, "trap -- ");
    sb_quoted(&b, cmd);
    sb_putc(&b, ' ');
    sb_puts(&b, name);
    sb_putc(&b, '\n');
    outn(out1, b.s, b.len);
    sb_free(&b);
    (void)sig;
}

static int bi_trap(int argc, char **argv)
{
    int i = 1;
    bool print = false;
    if (i < argc && strcmp(argv[i], "-l") == 0) {
        for (int s = 1; s < NSIG_SH; s++) {
            if (s == 32 || s == 33) continue;
            outf(out1, "%2d) SIG%-8s%s", s, sig_name(s), s % 5 == 0 ? "\n" : "\t");
        }
        outc(out1, '\n');
        return 0;
    }
    if (i < argc && strcmp(argv[i], "-p") == 0) {
        print = true;
        i++;
    }
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (i >= argc || print) {
        if (trap_cmd[0])
            trap_print_one(0, trap_cmd[0], "EXIT");
        for (int s = 1; s < NSIG_SH; s++)
            if (trap_cmd[s])
                trap_print_one(s, trap_cmd[s], sig_name(s));
        if (trap_err)
            trap_print_one(-1, trap_err, "ERR");
        return 0;
    }
    const char *action = argv[i];
    long long dummy;
    bool reset = false;
    /* `trap 1 2` - the first operand is a signal number: reset them all.
     * `trap INT` alone: bash resets it too. */
    if (parse_int(action, &dummy) || strcmp(action, "-") == 0 ||
        (argc - i == 1 && sig_number(action) >= 0)) {
        reset = true;
        if (strcmp(action, "-") == 0 || parse_int(action, &dummy))
            ;
        if (strcmp(action, "-") == 0)
            i++;
    } else {
        i++;
    }
    int st = 0;
    if (i >= argc && !reset) {
        bi_usage("trap", "trap [-lp] [[arg] signal_spec ...]");
        return 2;
    }
    for (; i < argc; i++) {
        const char *s = argv[i];
        if (strcmp(s, "ERR") == 0) {
            xfree(trap_err);
            trap_err = reset ? NULL : xstrdup(action);
            continue;
        }
        if (strcmp(s, "DEBUG") == 0 || strcmp(s, "RETURN") == 0)
            continue;
        int sig = strcmp(s, "EXIT") == 0 ? 0 : sig_number(s);
        if (sig < 0 || sig >= NSIG_SH) {
            sh_warn("trap: %s: invalid signal specification", s);
            st = 1;
            continue;
        }
        if (sig == 9 || sig == 19) {
            if (!reset) {
                sh_warn("trap: %s: cannot trap", s);
                st = 1;
            }
            continue;
        }
        if (sig && sig_ignored_at_entry[sig] && !opt[O_interactive])
            continue;               /* POSIX: stays ignored */
        trap_set(sig, reset ? NULL : action);
    }
    return st;
}

/* ── jobs, fg, bg, wait, kill, disown ──────────────────────────────── */

/* %n %+ %% %- %name %?text, or a pid */
static job_t *job_by_spec(const char *spec, const char *cmd, bool quiet)
{
    job_t *j = NULL;
    if (spec[0] != '%') {
        long long pid;
        if (parse_int(spec, &pid)) {
            for (job_t *x = jobs; x; x = x->next)
                for (int i = 0; i < x->nproc; i++)
                    if (x->procs[i].pid == pid)
                        return x;
        }
        if (!quiet)
            sh_warn("%s: %s: no such job", cmd, spec);
        return NULL;
    }
    const char *s = spec + 1;
    long long n;
    if (!*s || strcmp(s, "%") == 0 || strcmp(s, "+") == 0) {
        j = job_current(0);
    } else if (strcmp(s, "-") == 0) {
        j = job_current(1);
    } else if (parse_int(s, &n)) {
        for (job_t *x = jobs; x; x = x->next)
            if (x->id == n) j = x;
    } else if (s[0] == '?') {
        for (job_t *x = jobs; x; x = x->next)
            if (strstr(x->cmd, s + 1)) {
                if (j) {
                    if (!quiet) sh_warn("%s: %s: ambiguous job spec", cmd, spec);
                    return NULL;
                }
                j = x;
            }
    } else {
        size_t l = strlen(s);
        for (job_t *x = jobs; x; x = x->next)
            if (strncmp(x->cmd, s, l) == 0) {
                if (j) {
                    if (!quiet) sh_warn("%s: %s: ambiguous job spec", cmd, spec);
                    return NULL;
                }
                j = x;
            }
    }
    if (!j && !quiet)
        sh_warn("%s: %s: no such job", cmd, spec);
    return j;
}

static int bi_jobs(int argc, char **argv)
{
    bool longform = false, pids = false, running = false, stopped = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'l') longform = true;
            else if (*p == 'p') pids = true;
            else if (*p == 'r') running = true;
            else if (*p == 's') stopped = true;
            else if (*p == 'n') ;
            else {
                sh_warn("jobs: -%c: invalid option", *p);
                return 2;
            }
        }
    }
    dowait(false, false);
    if (i < argc) {
        int st = 0;
        for (; i < argc; i++) {
            job_t *j = job_by_spec(argv[i], "jobs", false);
            if (!j) { st = 1; continue; }
            print_job(j, out1, longform, pids);
        }
        return st;
    }
    job_t *next;
    for (job_t *j = jobs; j; j = next) {
        next = j->next;
        if (!j->bg && j->state == JS_RUNNING)
            continue;
        if (running && j->state != JS_RUNNING) continue;
        if (stopped && j->state != JS_STOPPED) continue;
        print_job(j, out1, longform, pids);
        j->changed = false;
        if (j->state == JS_DONE && opt[O_interactive] && !in_subshell)
            job_free(j);
    }
    return 0;
}

static void job_continue(job_t *j)
{
    for (int i = 0; i < j->nproc; i++)
        if (j->procs[i].state == JS_STOPPED)
            j->procs[i].state = JS_RUNNING;
    j->state = JS_RUNNING;
    j->changed = false;
    if (j->jobctl)
        lp_kill(-j->pgid, 18);
    else
        for (int i = 0; i < j->nproc; i++)
            lp_kill(j->procs[i].pid, 18);
}

static int bi_fg(int argc, char **argv)
{
    if (!job_control) {
        sh_warn("fg: no job control");
        return 1;
    }
    job_t *j = argc > 1 ? job_by_spec(argv[1], "fg", false) : job_current(0);
    if (!j) {
        if (argc <= 1) sh_warn("fg: current: no such job");
        return 1;
    }
    outs(out1, j->cmd);
    outc(out1, '\n');
    flush_out(out1);
    if (tty_fd >= 0) {
        if (j->have_tmodes)
            tty_set(&j->tmodes);
        sys_tcsetpgrp(tty_fd, j->pgid);
    }
    j->bg = false;
    job_continue(j);
    return waitforjob(j);
}

static int bi_bg(int argc, char **argv)
{
    if (!job_control) {
        sh_warn("bg: no job control");
        return 1;
    }
    int st = 0;
    int i = 1;
    do {
        job_t *j = i < argc ? job_by_spec(argv[i], "bg", false) : job_current(0);
        if (!j) {
            if (i >= argc) sh_warn("bg: current: no such job");
            st = 1;
            continue;
        }
        if (j->state == JS_RUNNING && j->bg) {
            sh_warn("bg: job %d already in background", j->id);
            continue;
        }
        j->bg = true;
        j->seq = ++job_seq;
        job_continue(j);
        char mark = j == job_current(0) ? '+' : j == job_current(1) ? '-' : ' ';
        outf(out1, "[%d]%c %s &\n", j->id, mark, j->cmd);
    } while (++i < argc);
    return st;
}

/* `wait`: for everything, or for the named jobs and pids. A trapped
 * signal interrupts it (POSIX), with status 128+signal. */
static int bi_wait(int argc, char **argv)
{
    int i = 1;
    bool any = false;
    if (i < argc && strcmp(argv[i], "-n") == 0) { any = true; i++; }
    if (i < argc && strcmp(argv[i], "--") == 0) i++;
    if (i >= argc) {
        for (;;) {
            bool running = false;
            for (job_t *j = jobs; j; j = j->next)
                if (j->state == JS_RUNNING) running = true;
            if (!running)
                break;
            int r = dowait(true, true);
            if (r == -2) {
                for (int s = 1; s < NSIG_SH; s++)
                    if (gotsig[s]) return 128 + s;
                return 130;
            }
            if (r < 0)
                break;
            if (any) {
                for (job_t *j = jobs; j; j = j->next)
                    if (j->state == JS_DONE && j->bg) {
                        int st = job_status(j);
                        if (!opt[O_interactive]) job_free(j);
                        return st;
                    }
            }
        }
        if (!any) {
            job_t *next;
            for (job_t *j = jobs; j; j = next) {
                next = j->next;
                if (j->state == JS_DONE)
                    job_free(j);
            }
        }
        return any ? 127 : 0;
    }
    int st = 0;
    for (; i < argc; i++) {
        job_t *j = job_by_spec(argv[i], "wait", argv[i][0] != '%');
        if (!j) {
            if (argv[i][0] != '%') {
                long long pid;
                if (!parse_int(argv[i], &pid)) {
                    sh_warn("wait: `%s': not a pid or valid job spec", argv[i]);
                    st = 2;
                } else {
                    st = 127;
                }
            } else {
                st = 127;
            }
            continue;
        }
        int pid = argv[i][0] == '%' ? 0 : atoi(argv[i]);
        for (;;) {
            bool done;
            if (pid) {
                done = true;
                for (int k = 0; k < j->nproc; k++)
                    if (j->procs[k].pid == pid && j->procs[k].state == JS_RUNNING)
                        done = false;
            } else {
                done = j->state != JS_RUNNING;
            }
            if (done)
                break;
            int r = dowait(true, true);
            if (r == -2) {
                for (int s = 1; s < NSIG_SH; s++)
                    if (gotsig[s]) return 128 + s;
                return 130;
            }
            if (r < 0)
                break;
        }
        if (pid) {
            for (int k = 0; k < j->nproc; k++)
                if (j->procs[k].pid == pid)
                    st = status_of(j->procs[k].status);
        } else {
            st = job_status(j);
        }
        if (j->state == JS_DONE)
            job_free(j);
    }
    return st;
}

static int bi_kill(int argc, char **argv)
{
    int sig = 15;
    int i = 1;
    if (i < argc && (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "-L") == 0)) {
        i++;
        if (i >= argc) {
            for (int s = 1; s < NSIG_SH; s++) {
                if (s == 32 || s == 33) continue;
                outf(out1, "%2d) SIG%-8s%s", s, sig_name(s), s % 5 == 0 ? "\n" : "\t");
            }
            outc(out1, '\n');
            return 0;
        }
        int st = 0;
        for (; i < argc; i++) {
            long long n;
            if (parse_int(argv[i], &n)) {
                int s = n > 128 ? (int)n - 128 : (int)n;
                if (s > 0 && s < NSIG_SH) outf(out1, "%s\n", sig_name(s));
                else { sh_warn("kill: %s: invalid signal specification", argv[i]); st = 1; }
            } else {
                int s = sig_number(argv[i]);
                if (s > 0) outf(out1, "%d\n", s);
                else { sh_warn("kill: %s: invalid signal specification", argv[i]); st = 1; }
            }
        }
        return st;
    }
    if (i < argc && (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-n") == 0)) {
        if (i + 1 >= argc) {
            bi_usage("kill", "kill [-s sigspec | -n signum | -sigspec] pid | jobspec ...");
            return 2;
        }
        sig = sig_number(argv[i + 1]);
        if (sig < 0) {
            sh_warn("kill: %s: invalid signal specification", argv[i + 1]);
            return 1;
        }
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1] &&
               strcmp(argv[i], "--") != 0) {
        const char *s = argv[i] + 1;
        long long n;
        if (!parse_int(s, &n)) {
            sig = sig_number(s);
            if (sig < 0) {
                sh_warn("kill: %s: invalid signal specification", s);
                return 1;
            }
            i++;
        } else {
            sig = (int)n;
            i++;
        }
    }
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (i >= argc) {
        bi_usage("kill", "kill [-s sigspec | -n signum | -sigspec] pid | jobspec ...");
        return 2;
    }
    int st = 0;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '%') {
            job_t *j = job_by_spec(a, "kill", false);
            if (!j) { st = 1; continue; }
            long r;
            if (j->jobctl) {
                r = lp_kill(-j->pgid, sig);
                if (j->state == JS_STOPPED && (sig == 15 || sig == 1))
                    lp_kill(-j->pgid, 18);
            } else {
                r = 0;
                for (int k = 0; k < j->nproc; k++)
                    if (j->procs[k].state != JS_DONE)
                        r = lp_kill(j->procs[k].pid, sig);
            }
            if (r < 0) {
                sh_warn("kill: %s: %s", a, lp_strerror((int)-r));
                st = 1;
            }
            continue;
        }
        long long pid;
        if (!parse_int(a, &pid)) {
            sh_warn("kill: %s: arguments must be process or job IDs", a);
            st = 1;
            continue;
        }
        long r = lp_kill((pid_t)pid, sig);
        if (r < 0) {
            sh_warn("kill: (%lld) - %s", pid, lp_strerror((int)-r));
            st = 1;
        }
    }
    return st;
}

static int bi_disown(int argc, char **argv)
{
    int i = 1;
    bool all = false, running = false;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'a') all = true;
            else if (*p == 'r') running = true;
            else if (*p == 'h') ;
            else {
                sh_warn("disown: -%c: invalid option", *p);
                return 2;
            }
        }
    }
    if (all || running) {
        job_t *next;
        for (job_t *j = jobs; j; j = next) {
            next = j->next;
            if (!running || j->state == JS_RUNNING)
                job_free(j);
        }
        return 0;
    }
    if (i >= argc) {
        job_t *j = job_current(0);
        if (!j) {
            sh_warn("disown: current: no such job");
            return 1;
        }
        job_free(j);
        return 0;
    }
    int st = 0;
    for (; i < argc; i++) {
        job_t *j = job_by_spec(argv[i], "disown", false);
        if (!j) { st = 1; continue; }
        job_free(j);
    }
    return st;
}

static int bi_suspend(int argc, char **argv)
{
    bool force = argc > 1 && strcmp(argv[1], "-f") == 0;
    if (is_login && !force) {
        sh_warn("suspend: cannot suspend a login shell");
        return 1;
    }
    jobs_release_terminal();
    lp_kill(lp_getpid(), 19);
    if (job_control && tty_fd >= 0)
        sys_tcsetpgrp(tty_fd, shell_pgid);
    return 0;
}

/* ── getopts ───────────────────────────────────────────────────────── */

static int getopts_off;         /* position inside a grouped -abc */

static int bi_getopts(int argc, char **argv)
{
    if (argc < 3) {
        bi_usage("getopts", "getopts optstring name [arg ...]");
        return 2;
    }
    const char *optstr = argv[1];
    const char *name = argv[2];
    char **args;
    int nargs;
    if (argc > 3) {
        args = argv + 3;
        nargs = argc - 3;
    } else {
        args = posv;
        nargs = posc;
    }
    bool silent = optstr[0] == ':';
    if (silent) optstr++;
    long long optind = 1;
    const char *oi = var_get("OPTIND");
    if (!oi || !parse_int(oi, &optind) || optind < 1)
        optind = 1;
    if (getopts_reset) {
        getopts_off = 0;
        getopts_reset = false;
    }
    char c;
    char buf[2] = { 0, 0 };
    int ret = 0;
    const char *p = NULL;
    if (getopts_off == 0 || optind > nargs) {
        if (optind > nargs)
            goto end;
        p = args[optind - 1];
        if (p[0] != '-' || !p[1])
            goto end;
        if (strcmp(p, "--") == 0) {
            optind++;
            goto end;
        }
        getopts_off = 1;
    } else {
        p = args[optind - 1];
        if ((int)strlen(p) <= getopts_off) {
            optind++;
            getopts_off = 0;
            if (optind > nargs)
                goto end;
            p = args[optind - 1];
            if (p[0] != '-' || !p[1])
                goto end;
            if (strcmp(p, "--") == 0) {
                optind++;
                goto end;
            }
            getopts_off = 1;
        }
    }
    c = p[getopts_off++];
    const char *k = c != ':' ? strchr(optstr, c) : NULL;
    if (!k) {
        buf[0] = c;
        if (silent) {
            var_set("OPTARG", buf, 0);
        } else {
            sh_warn("illegal option -- %c", c);
            var_unset("OPTARG");
        }
        c = '?';
    } else if (k[1] == ':') {
        if (p[getopts_off]) {
            var_set("OPTARG", p + getopts_off, 0);
            optind++;
            getopts_off = 0;
        } else if (optind < nargs) {
            var_set("OPTARG", args[optind], 0);
            optind += 2;
            getopts_off = 0;
        } else {
            buf[0] = c;
            if (silent) {
                var_set("OPTARG", buf, 0);
                c = ':';
            } else {
                sh_warn("option requires an argument -- %c", c);
                var_unset("OPTARG");
                c = '?';
            }
            optind++;
            getopts_off = 0;
        }
    } else {
        var_unset("OPTARG");
    }
    if (getopts_off && !p[getopts_off]) {
        optind++;
        getopts_off = 0;
    }
    buf[0] = c;
    var_set(name, buf, 0);
    goto store;
end:
    getopts_off = 0;
    var_set(name, "?", 0);
    ret = 1;
store:
    {
        char nb[24];
        var_set("OPTIND", itoa_s(optind, nb), 0);
        getopts_reset = false;
    }
    return ret;
}

/* ── read, mapfile ─────────────────────────────────────────────────── */

#define TCGETS__  0x5401
#define TCSETS__  0x5402

/* Wait up to ms for fd to be readable. 1 ready, 0 timeout, -1 error. */
static int fd_wait(int fd, long ms)
{
    struct { int fd; short events, revents; } pfd = { fd, 1, 0 };
    long ts[2] = { ms / 1000, (ms % 1000) * 1000000 };
    long r = sys_call5(SYS_ppoll, (long)&pfd, 1, ms < 0 ? 0 : (long)ts, 0, 8);
    if (r == -E_INTR)
        return -2;
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

typedef struct {
    int    fd;
    char   buf[256];
    size_t pos, len;
    bool   seekable;
} rbuf_t;

/* One byte, or -1 at end of input, -2 on timeout or interrupt. On a
 * seekable file, read ahead and give back what was not used; on anything
 * else, one byte at a time, so a pipe shared with the next command keeps
 * everything after the line. */
static int rb_getc(rbuf_t *r, s64 deadline)
{
    if (r->pos < r->len)
        return (u8)r->buf[r->pos++];
    if (deadline >= 0) {
        s64 left = deadline - now_ms();
        if (left < 0) left = 0;
        int w = fd_wait(r->fd, (long)left);
        if (w <= 0)
            return -2;
    }
    for (;;) {
        long n = lp_read(r->fd, r->buf, r->seekable ? sizeof r->buf : 1);
        if (n == -E_INTR) {
            if (got_sigint || pending_signals)
                return -2;
            continue;
        }
        if (n <= 0)
            return -1;
        r->pos = 0;
        r->len = (size_t)n;
        return (u8)r->buf[r->pos++];
    }
}

static void rb_done(rbuf_t *r)
{
    if (r->seekable && r->pos < r->len)
        lp_lseek(r->fd, -(off_t)(r->len - r->pos), SEEK_CUR);
    r->pos = r->len = 0;
}

static bool read_line_fd(int fd, strbuf_t *out, int delim, bool *eof)
{
    rbuf_t r;
    memset(&r, 0, sizeof r);
    r.fd = fd;
    r.seekable = lp_lseek(fd, 0, SEEK_CUR) >= 0 && !lp_isatty(fd);
    *eof = false;
    for (;;) {
        int c = rb_getc(&r, -1);
        if (c < 0) {
            *eof = true;
            break;
        }
        if (c == delim)
            break;
        sb_putc(out, (char)c);
    }
    rb_done(&r);
    sb_str(out);
    return !*eof || out->len > 0;
}

/* Split text into fields the way read does: IFS whitespace trims and
 * collapses, other IFS characters separate exactly, and the last name
 * gets the rest of the line. mask marks backslash-escaped characters,
 * which never separate. */
static void read_split(const char *s, const char *mask, size_t n,
                       int nvars, strvec_t *out)
{
    const char *ifs = ifs_value();
    size_t i = 0;
#define ISWS(k) (!mask[k] && (s[k] == ' ' || s[k] == '\t' || s[k] == '\n') && strchr(ifs, s[k]))
#define ISIFS(k) (!mask[k] && s[k] && strchr(ifs, s[k]))
    while (i < n && ISWS(i))
        i++;
    while (i < n) {
        if (out->n == nvars - 1) {
            /* the rest of the line, less trailing IFS whitespace */
            size_t e = n;
            while (e > i && ISWS(e - 1))
                e--;
            /* and one trailing non-whitespace separator with nothing
             * after it, as dash and bash both drop */
            if (e > i && ISIFS(e - 1) && !ISWS(e - 1)) {
                size_t k = e - 1;
                bool other = false;
                for (size_t m = i; m < k; m++)
                    if (ISIFS(m) && !ISWS(m)) { other = true; break; }
                if (!other)
                    e = k;
                while (e > i && ISWS(e - 1))
                    e--;
            }
            strbuf_t b = {0};
            sb_putn(&b, s + i, e - i);
            sv_push(out, sb_take(&b));
            return;
        }
        size_t st = i;
        while (i < n && !ISIFS(i))
            i++;
        strbuf_t b = {0};
        sb_putn(&b, s + st, i - st);
        sv_push(out, sb_take(&b));
        /* the delimiter: whitespace around one non-whitespace sep */
        while (i < n && ISWS(i))
            i++;
        if (i < n && ISIFS(i) && !ISWS(i)) {
            i++;
            while (i < n && ISWS(i))
                i++;
        }
    }
#undef ISWS
#undef ISIFS
}

static int bi_read(int argc, char **argv)
{
    bool raw = false, silent = false;
    const char *prompt = NULL, *arrayname = NULL;
    long long timeout_ms = -1, nchars = -1;
    bool exact = false;
    int delim = '\n';
    int fd = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (strcmp(a, "--") == 0) { i++; break; }
        for (const char *p = a + 1; *p; p++) {
            char c = *p;
            if (c == 'r') { raw = true; continue; }
            if (c == 's') { silent = true; continue; }
            if (c == 'e') continue;
            if (!strchr("pantNdu", c)) {
                sh_warn("read: -%c: invalid option", c);
                bi_usage("read", "read [-ers] [-a array] [-d delim] [-n nchars] [-N nchars] [-p prompt] [-t timeout] [-u fd] [name ...]");
                return 2;
            }
            const char *val = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
            if (!val) {
                sh_warn("read: -%c: option requires an argument", c);
                return 2;
            }
            switch (c) {
            case 'p': prompt = val; break;
            case 'a': arrayname = val; break;
            case 'd': delim = (u8)val[0]; break;
            case 'n': case 'N': {
                long long n;
                if (!parse_int(val, &n) || n < 0) {
                    sh_warn("read: %s: invalid number", val);
                    return 2;
                }
                nchars = n;
                exact = c == 'N';
                break;
            }
            case 'u': {
                long long n;
                if (!parse_int(val, &n) || n < 0 || !fd_is_open((int)n)) {
                    sh_warn("read: %s: invalid file descriptor specification", val);
                    return 2;
                }
                fd = (int)n;
                break;
            }
            case 't': {
                /* seconds, with a fraction */
                long long whole = 0, frac = 0, scale = 1000;
                const char *q = val;
                while (is_digit((u8)*q)) whole = whole * 10 + (*q++ - '0');
                if (*q == '.') {
                    q++;
                    while (is_digit((u8)*q)) {
                        scale /= 10;
                        frac += (*q++ - '0') * scale;
                    }
                }
                if (*q) {
                    sh_warn("read: %s: invalid timeout specification", val);
                    return 2;
                }
                timeout_ms = whole * 1000 + frac;
                break;
            }
            }
            break;
        }
    }
    for (int k = i; k < argc; k++) {
        if (!is_valid_name(argv[k])) {
            sh_warn("read: `%s': not a valid identifier", argv[k]);
            return 2;
        }
    }
    bool tty = lp_isatty(fd);
    if (timeout_ms == 0)
        return fd_wait(fd, 0) > 0 ? 0 : 1;
    if (prompt && tty) {
        outs(out2, prompt);
        flush_out(out2);
    }
    lp_termios_t saved;
    bool restore = false;
    if (tty && (silent || nchars >= 0 || delim != '\n')) {
        if (lp_ioctl(fd, TCGETS__, saved.raw) == 0) {
            lp_termios_t t = saved;
            u32 *fl = (u32 *)t.raw;
            if (silent)
                fl[3] &= ~(u32)(0000010 | 0000020 | 0000040 | 0001000);
            if (nchars >= 0 || delim != '\n') {
                fl[3] &= ~(u32)0000002;         /* ICANON */
                t.raw[17 + 6] = 1;              /* VMIN */
                t.raw[17 + 5] = 0;              /* VTIME */
            }
            lp_ioctl(fd, TCSETS__, t.raw);
            restore = true;
        }
    }
    rbuf_t r;
    memset(&r, 0, sizeof r);
    r.fd = fd;
    r.seekable = !tty && lp_lseek(fd, 0, SEEK_CUR) >= 0;
    s64 deadline = timeout_ms > 0 ? now_ms() + timeout_ms : -1;
    strbuf_t text = {0}, mask = {0};
    int status = 0;
    long long got = 0;
    for (;;) {
        if (nchars >= 0 && got >= nchars)
            break;
        int c = rb_getc(&r, deadline);
        if (c == -2) {
            status = timeout_ms > 0 ? 142 : 130;
            if (got_sigint && toplevel_interactive)
                status = 130;
            break;
        }
        if (c < 0) {
            status = 1;
            break;
        }
        if (c == delim && !exact)
            break;
        if (c == '\\' && !raw && !exact) {
            int d = rb_getc(&r, deadline);
            if (d < 0) {
                status = d == -2 ? 142 : 1;
                break;
            }
            if (d == '\n') {
                if (tty && prompt == NULL && toplevel_interactive) {
                    const char *ps2 = var_get("PS2");
                    outs(out2, ps2 ? ps2 : "> ");
                    flush_out(out2);
                }
                continue;
            }
            sb_putc(&text, (char)d);
            sb_putc(&mask, 1);
            got++;
            continue;
        }
        sb_putc(&text, (char)c);
        sb_putc(&mask, 0);
        /* count characters, not bytes, for -n */
        if ((c & 0xC0) != 0x80) {
            int need = utf8_seq_len((u8)c);
            if (need > 1) {
                for (int k = 1; k < need; k++) {
                    int d = rb_getc(&r, deadline);
                    if (d < 0) break;
                    sb_putc(&text, (char)d);
                    sb_putc(&mask, 0);
                }
            }
            got++;
        }
    }
    rb_done(&r);
    if (restore) {
        lp_ioctl(fd, TCSETS__, saved.raw);
        if (silent && tty) {
            outc(out2, '\n');
            flush_out(out2);
        }
    }
    const char *t = text.s ? text.s : "";
    const char *m = mask.s ? mask.s : "";
    size_t n = text.len;
    if (arrayname) {
        strvec_t fields = {0};
        read_split(t, m, n, 1 << 30, &fields);
        if (!var_set_array(arrayname, &fields, false))
            status = 2;
        sv_free(&fields);
    } else if (i >= argc) {
        /* REPLY gets the line as it is, blanks and all (bash, dash) */
        strbuf_t b = {0};
        sb_putn(&b, t, n);
        if (!var_set("REPLY", sb_str(&b), 0))
            status = 2;
        sb_free(&b);
    } else if (exact || nchars >= 0) {
        if (argc - i == 1) {
            strbuf_t b = {0};
            sb_putn(&b, t, n);
            if (!var_set(argv[i], sb_str(&b), 0)) status = 2;
            sb_free(&b);
        } else {
            strvec_t fields = {0};
            read_split(t, m, n, argc - i, &fields);
            for (int k = i; k < argc; k++) {
                int idx = k - i;
                if (!var_set(argv[k], idx < fields.n ? fields.v[idx] : "", 0))
                    status = 2;
            }
            sv_free(&fields);
        }
    } else {
        strvec_t fields = {0};
        read_split(t, m, n, argc - i, &fields);
        for (int k = i; k < argc; k++) {
            int idx = k - i;
            if (!var_set(argv[k], idx < fields.n ? fields.v[idx] : "", 0))
                status = 2;
        }
        sv_free(&fields);
    }
    sb_free(&text);
    sb_free(&mask);
    if (status == 130 && toplevel_interactive && got_sigint) {
        got_sigint = 0;
        evalskip = SKIP_ABORT;
    }
    return status;
}

static int bi_mapfile(int argc, char **argv)
{
    bool strip = false;
    long long count = 0, skip = 0, origin = 0;
    int delim = '\n', fd = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (strcmp(a, "--") == 0) { i++; break; }
        char c = a[1];
        if (c == 't' && !a[2]) { strip = true; continue; }
        const char *val = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
        if (!val || !strchr("nsdOuC c", c)) {
            sh_warn("%s: %s: invalid option", argv[0], a);
            return 2;
        }
        long long n = 0;
        switch (c) {
        case 'd': delim = (u8)val[0]; break;
        case 'n': if (!parse_int(val, &count)) return 2; break;
        case 's': if (!parse_int(val, &skip)) return 2; break;
        case 'O': if (!parse_int(val, &origin)) return 2; break;
        case 'u': if (!parse_int(val, &n)) return 2; fd = (int)n; break;
        default: break;
        }
    }
    const char *name = i < argc ? argv[i] : "MAPFILE";
    strvec_t lines = {0};
    var_t *old = var_lookup(name);
    if (origin > 0 && old && (old->flags & V_ARRAY)) {
        for (int k = 0; k < old->arrn && k < origin; k++)
            sv_push(&lines, xstrdup(old->arr[k] ? old->arr[k] : ""));
    }
    long long seen = 0, stored = 0;
    rbuf_t r;
    memset(&r, 0, sizeof r);
    r.fd = fd;
    r.seekable = !lp_isatty(fd) && lp_lseek(fd, 0, SEEK_CUR) >= 0;
    for (;;) {
        if (count && stored >= count)
            break;
        strbuf_t b = {0};
        int c;
        bool any = false;
        while ((c = rb_getc(&r, -1)) >= 0) {
            any = true;
            if (c == delim) {
                if (!strip) sb_putc(&b, (char)c);
                break;
            }
            sb_putc(&b, (char)c);
        }
        if (!any) {
            sb_free(&b);
            break;
        }
        if (seen++ < skip) {
            sb_free(&b);
            continue;
        }
        sv_push(&lines, sb_take(&b));
        stored++;
        if (c < 0)
            break;
    }
    rb_done(&r);
    bool ok = var_set_array(name, &lines, false);
    sv_free(&lines);
    return ok ? 0 : 1;
}

/* ── alias ─────────────────────────────────────────────────────────── */

#define AHASH 64
static alias_t *atab[AHASH];

static alias_t *alias_lookup(const char *name)
{
    for (alias_t *a = atab[hhash(name) % AHASH]; a; a = a->next)
        if (strcmp(a->name, name) == 0)
            return a;
    return NULL;
}

static void alias_set(const char *name, const char *val)
{
    alias_t *a = alias_lookup(name);
    if (a) {
        xfree(a->val);
        a->val = xstrdup(val);
        return;
    }
    a = xcalloc(sizeof *a);
    a->name = xstrdup(name);
    a->val = xstrdup(val);
    unsigned k = hhash(name) % AHASH;
    a->next = atab[k];
    atab[k] = a;
}

static bool alias_remove(const char *name)
{
    for (alias_t **pp = &atab[hhash(name) % AHASH]; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->name, name) == 0) {
            alias_t *a = *pp;
            if (a->busy) {
                /* being expanded right now: blank it, free later never */
                xfree(a->val);
                a->val = xstrdup("");
                *pp = a->next;
                return true;
            }
            *pp = a->next;
            xfree(a->name);
            xfree(a->val);
            xfree(a);
            return true;
        }
    }
    return false;
}

static void alias_print(alias_t *a)
{
    strbuf_t b = {0};
    sb_puts(&b, "alias ");
    sb_puts(&b, a->name);
    sb_putc(&b, '=');
    sb_quoted(&b, a->val);
    sb_putc(&b, '\n');
    outn(out1, b.s, b.len);
    sb_free(&b);
}

static int bi_alias(int argc, char **argv)
{
    int i = 1;
    if (i < argc && strcmp(argv[i], "-p") == 0)
        i++;
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (i >= argc) {
        int n = 0;
        for (int h = 0; h < AHASH; h++)
            for (alias_t *a = atab[h]; a; a = a->next)
                n++;
        char **names = xmalloc((size_t)(n + 1) * sizeof(char *));
        int k = 0;
        for (int h = 0; h < AHASH; h++)
            for (alias_t *a = atab[h]; a; a = a->next)
                names[k++] = a->name;
        sort_strs(names, k);
        for (int m = 0; m < k; m++)
            alias_print(alias_lookup(names[m]));
        xfree(names);
        return 0;
    }
    int st = 0;
    for (; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (!eq) {
            alias_t *a = alias_lookup(argv[i]);
            if (a) {
                alias_print(a);
            } else {
                sh_warn("alias: %s: not found", argv[i]);
                st = 1;
            }
            continue;
        }
        char *name = xstrndup(argv[i], (size_t)(eq - argv[i]));
        bool ok = *name != '\0';
        for (const char *p = name; *p && ok; p++)
            if (strchr(" \t\n|&;()<>$`\\\"'=/", *p))
                ok = false;
        if (!ok) {
            sh_warn("alias: `%s': invalid alias name", name);
            st = 1;
        } else {
            alias_set(name, eq + 1);
        }
        xfree(name);
    }
    return st;
}

static int bi_unalias(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        for (int h = 0; h < AHASH; h++) {
            while (atab[h])
                alias_remove(atab[h]->name);
        }
        return 0;
    }
    if (argc < 2) {
        bi_usage("unalias", "unalias [-a] name [name ...]");
        return 2;
    }
    int st = 0;
    for (int i = 1; i < argc; i++) {
        if (!alias_remove(argv[i])) {
            sh_warn("unalias: %s: not found", argv[i]);
            st = 1;
        }
    }
    return st;
}

/* ── command -v/-V, type, hash ─────────────────────────────────────── */

/* How `name` would be run. verbose is type's wording; brief is
 * command -v's. Returns false when it would not be found. */
static bool describe(const char *name, int mode /* 0 -v, 1 -V/type, 2 type -t */,
                     bool all, bool path_only, bool use_alias)
{
    bool found = false;
    if (!path_only && use_alias) {
        alias_t *a = alias_lookup(name);
        if (a) {
            found = true;
            if (mode == 0) {
                strbuf_t b = {0};
                sb_puts(&b, "alias ");
                sb_puts(&b, name);
                sb_putc(&b, '=');
                sb_quoted(&b, a->val);
                sb_putc(&b, '\n');
                outn(out1, b.s, b.len);
                sb_free(&b);
            } else if (mode == 1) {
                outf(out1, "%s is aliased to `%s'\n", name, a->val);
            } else {
                outs(out1, "alias\n");
            }
            if (!all) return true;
        }
    }
    if (!path_only && is_keyword(name)) {
        found = true;
        if (mode == 0) outf(out1, "%s\n", name);
        else if (mode == 1) outf(out1, "%s is a shell keyword\n", name);
        else outs(out1, "keyword\n");
        if (!all) return true;
    }
    const builtin_t *bi = path_only ? NULL : builtin_find(name);
    if (bi && bi->kind == CMD_SPECIAL) {
        found = true;
        if (mode == 0) outf(out1, "%s\n", name);
        else if (mode == 1) outf(out1, "%s is a special shell builtin\n", name);
        else outs(out1, "builtin\n");
        if (!all) return true;
    }
    func_t *f = path_only ? NULL : func_lookup(name);
    if (f) {
        found = true;
        if (mode == 0) {
            outf(out1, "%s\n", name);
        } else if (mode == 1) {
            outf(out1, "%s is a function\n", name);
            print_function(f);
        } else {
            outs(out1, "function\n");
        }
        if (!all) return true;
    }
    if (bi && bi->kind == CMD_BUILTIN) {
        found = true;
        if (mode == 0) outf(out1, "%s\n", name);
        else if (mode == 1) outf(out1, "%s is a shell builtin\n", name);
        else outs(out1, "builtin\n");
        if (!all) return true;
    }
    char path[1024];
    if (strchr(name, '/')) {
        if (is_exec_file(name)) {
            found = true;
            if (mode == 0) outf(out1, "%s\n", name);
            else if (mode == 1) outf(out1, "%s is %s\n", name, name);
            else outs(out1, "file\n");
        }
        return found;
    }
    hent_t *h = hash_lookup(name);
    if (h && !all && is_exec_file(h->path)) {
        if (mode == 0) outf(out1, "%s\n", h->path);
        else if (mode == 1) outf(out1, "%s is hashed (%s)\n", name, h->path);
        else outs(out1, "file\n");
        return true;
    }
    if (all) {
        const char *p = var_get("PATH");
        if (!p) p = "";
        for (;;) {
            const char *e = strchr(p, ':');
            size_t dl = e ? (size_t)(e - p) : strlen(p);
            strbuf_t b = {0};
            if (dl) sb_putn(&b, p, dl); else sb_putc(&b, '.');
            sb_putc(&b, '/');
            sb_puts(&b, name);
            if (is_exec_file(sb_str(&b))) {
                found = true;
                if (mode == 0) outf(out1, "%s\n", b.s);
                else if (mode == 1) outf(out1, "%s is %s\n", name, b.s);
                else outs(out1, "file\n");
            }
            sb_free(&b);
            if (!e) break;
            p = e + 1;
        }
        return found;
    }
    if (path_search(name, path, sizeof path, NULL, false)) {
        if (mode == 0) outf(out1, "%s\n", path);
        else if (mode == 1) outf(out1, "%s is %s\n", name, path);
        else outs(out1, "file\n");
        return true;
    }
    if (bi) {
        if (mode == 0) outf(out1, "%s\n", name);
        else if (mode == 1) outf(out1, "%s is a shell builtin\n", name);
        else outs(out1, "builtin\n");
        return true;
    }
    return found;
}

static int bi_command(int argc, char **argv)
{
    /* only -v and -V reach here; running a command is exec.c's */
    int mode = -1;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'v') mode = 0;
            else if (*p == 'V') mode = 1;
            else if (*p == 'p') ;
            else {
                sh_warn("command: -%c: invalid option", *p);
                return 2;
            }
        }
    }
    if (mode < 0 || i >= argc)
        return 0;
    int st = 0;
    for (; i < argc; i++) {
        if (!describe(argv[i], mode, false, false, true)) {
            if (mode == 1)
                sh_warn("command: %s: not found", argv[i]);
            st = 1;
        }
    }
    return st;
}

static int bi_type(int argc, char **argv)
{
    bool all = false, tflag = false, pflag = false, Pflag = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'a') all = true;
            else if (*p == 't') tflag = true;
            else if (*p == 'p') pflag = true;
            else if (*p == 'P') Pflag = true;
            else if (*p == 'f') ;
            else {
                sh_warn("type: -%c: invalid option", *p);
                return 2;
            }
        }
    }
    int st = 0;
    for (; i < argc; i++) {
        bool ok;
        if (pflag || Pflag) {
            char path[1024];
            if (!Pflag && (builtin_find(argv[i]) || func_lookup(argv[i]) ||
                           alias_lookup(argv[i]) || is_keyword(argv[i]))) {
                ok = true;
            } else if (path_search(argv[i], path, sizeof path, NULL, false)) {
                outf(out1, "%s\n", path);
                ok = true;
            } else {
                ok = false;
            }
        } else {
            ok = describe(argv[i], tflag ? 2 : 1, all, false, true);
            if (!ok && !tflag)
                sh_warn("type: %s: not found", argv[i]);
        }
        if (!ok)
            st = 1;
    }
    return st;
}

static int bi_hash(int argc, char **argv)
{
    int i = 1;
    bool forget = false, del = false, show = false;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'r') forget = true;
            else if (*p == 'd') del = true;
            else if (*p == 't') show = true;
            else if (*p == 'l') ;
            else if (*p == 'p') {
                if (i + 2 < argc) {
                    hash_add(argv[i + 2], argv[i + 1]);
                    return 0;
                }
                return 2;
            } else {
                sh_warn("hash: -%c: invalid option", *p);
                return 2;
            }
        }
    }
    if (forget)
        hash_clear();
    if (i >= argc) {
        if (forget)
            return 0;
        bool any = false;
        for (int h = 0; h < HHASH; h++)
            for (hent_t *e = htab[h]; e; e = e->next) {
                if (!any) outs(out1, "hits\tcommand\n");
                any = true;
                outf(out1, "%4d\t%s\n", e->hits, e->path);
            }
        if (!any)
            outs(out2, "hash: hash table empty\n");
        return 0;
    }
    int st = 0;
    for (; i < argc; i++) {
        if (del) {
            hash_remove(argv[i]);
            continue;
        }
        if (show) {
            hent_t *e = hash_lookup(argv[i]);
            if (e) outf(out1, "%s\n", e->path);
            else { sh_warn("hash: %s: not found", argv[i]); st = 1; }
            continue;
        }
        if (strchr(argv[i], '/') || builtin_find(argv[i]))
            continue;
        char path[1024];
        hash_remove(argv[i]);
        if (!path_search(argv[i], path, sizeof path, NULL, true)) {
            sh_warn("hash: %s: not found", argv[i]);
            st = 1;
        }
    }
    return st;
}

/* ── umask, ulimit, times ──────────────────────────────────────────── */

static int bi_umask(int argc, char **argv)
{
    bool symbolic = false;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-S") == 0) { symbolic = true; i++; }
    if (i < argc && strcmp(argv[i], "-p") == 0) i++;
    long cur = sys_umask(0);
    sys_umask(cur);
    if (i >= argc) {
        if (symbolic) {
            long m = ~cur & 0777;
            strbuf_t b = {0};
            const char *who = "ugo";
            for (int k = 0; k < 3; k++) {
                if (k) sb_putc(&b, ',');
                sb_putc(&b, who[k]);
                sb_putc(&b, '=');
                int bits = (int)(m >> (6 - 3 * k)) & 7;
                if (bits & 4) sb_putc(&b, 'r');
                if (bits & 2) sb_putc(&b, 'w');
                if (bits & 1) sb_putc(&b, 'x');
            }
            sb_putc(&b, '\n');
            outn(out1, b.s, b.len);
            sb_free(&b);
        } else {
            outf(out1, "%04lo\n", (unsigned long)cur);
        }
        return 0;
    }
    const char *m = argv[i];
    if (is_digit((u8)m[0])) {
        long v = 0;
        for (const char *p = m; *p; p++) {
            if (*p < '0' || *p > '7') {
                sh_warn("umask: %s: octal number out of range", m);
                return 1;
            }
            v = v * 8 + (*p - '0');
        }
        sys_umask(v & 0777);
        return 0;
    }
    /* symbolic: operate on the permissions (the complement of the mask) */
    long perm = ~cur & 0777;
    const char *p = m;
    while (*p) {
        int who = 0;
        while (*p && strchr("ugoa", *p)) {
            if (*p == 'u') who |= 0700;
            else if (*p == 'g') who |= 0070;
            else if (*p == 'o') who |= 0007;
            else who |= 0777;
            p++;
        }
        if (!who) who = 0777;
        if (!*p || !strchr("=+-", *p)) {
            sh_warn("umask: `%c': invalid symbolic mode operator", *p ? *p : ' ');
            return 1;
        }
        while (*p && strchr("=+-", *p)) {
            char op = *p++;
            int bits = 0;
            while (*p && strchr("rwxXst", *p)) {
                if (*p == 'r') bits |= 0444;
                else if (*p == 'w') bits |= 0222;
                else if (*p == 'x' || *p == 'X') bits |= 0111;
                p++;
            }
            bits &= who;
            if (op == '=') perm = (perm & ~who) | bits;
            else if (op == '+') perm |= bits;
            else perm &= ~bits;
        }
        if (*p == ',') p++;
        else if (*p) {
            sh_warn("umask: `%c': invalid symbolic mode character", *p);
            return 1;
        }
    }
    sys_umask(~perm & 0777);
    return 0;
}

static const struct {
    char        opt;
    int         res;
    int         unit;
    const char *desc;
    const char *what;
} limits[] = {
    { 't', 0, 1, "time", "seconds" },
    { 'f', 1, 512, "file", "blocks" },
    { 'd', 2, 1024, "data", "kbytes" },
    { 's', 3, 1024, "stack", "kbytes" },
    { 'c', 4, 512, "coredump", "blocks" },
    { 'm', 5, 1024, "memory", "kbytes" },
    { 'l', 8, 1024, "locked memory", "kbytes" },
    { 'u', 6, 1, "process", "" },
    { 'n', 7, 1, "nofiles", "" },
    { 'v', 9, 1024, "vmemory", "kbytes" },
    { 'x', 10, 1, "locks", "" },
    { 'i', 11, 1, "sigpending", "" },
    { 'q', 12, 1, "msgqueue", "bytes" },
    { 'e', 13, 1, "nice", "" },
    { 'r', 14, 1, "rtprio", "" },
};

static int bi_ulimit(int argc, char **argv)
{
    bool hard = false, soft = false, all = false;
    int which = 1;          /* -f */
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'H') hard = true;
            else if (*p == 'S') soft = true;
            else if (*p == 'a') all = true;
            else {
                int k = -1;
                for (unsigned m = 0; m < sizeof limits / sizeof *limits; m++)
                    if (limits[m].opt == *p) k = (int)m;
                if (k < 0) {
                    sh_warn("ulimit: -%c: invalid option", *p);
                    return 2;
                }
                which = k;
            }
        }
    }
    if (which == 1) {
        for (unsigned m = 0; m < sizeof limits / sizeof *limits; m++)
            if (limits[m].opt == 'f') which = (int)m;
    }
    if (!hard && !soft)
        soft = true;
    const u64 INF = ~(u64)0;
    if (all) {
        for (unsigned m = 0; m < sizeof limits / sizeof *limits; m++) {
            u64 lim[2];
            if (sys_call4(SYS_prlimit64, 0, limits[m].res, 0, (long)lim) < 0)
                continue;
            u64 v = hard && !soft ? lim[1] : lim[0];
            char label[40];
            if (limits[m].what[0])
                snprintf(label, sizeof label, "%s(%s)", limits[m].desc, limits[m].what);
            else
                snprintf(label, sizeof label, "%s", limits[m].desc);
            if (v == INF)
                outf(out1, "%-20s (-%c) unlimited\n", label, limits[m].opt);
            else
                outf(out1, "%-20s (-%c) %llu\n", label, limits[m].opt,
                     (unsigned long long)(v / (u64)limits[m].unit));
        }
        return 0;
    }
    int res = limits[which].res;
    u64 lim[2];
    if (sys_call4(SYS_prlimit64, 0, res, 0, (long)lim) < 0) {
        sh_warn("ulimit: cannot get limit");
        return 1;
    }
    if (i >= argc) {
        u64 v = soft ? lim[0] : lim[1];
        if (v == INF)
            outs(out1, "unlimited\n");
        else
            outf(out1, "%llu\n", (unsigned long long)(v / (u64)limits[which].unit));
        return 0;
    }
    const char *val = argv[i];
    u64 nv;
    if (strcmp(val, "unlimited") == 0) {
        nv = INF;
    } else if (strcmp(val, "hard") == 0) {
        nv = lim[1];
    } else if (strcmp(val, "soft") == 0) {
        nv = lim[0];
    } else {
        long long n;
        if (!parse_int(val, &n) || n < 0) {
            sh_warn("ulimit: %s: invalid number", val);
            return 1;
        }
        nv = (u64)n * (u64)limits[which].unit;
    }
    if (soft) lim[0] = nv;
    if (hard) lim[1] = nv;
    long r = sys_call4(SYS_prlimit64, 0, res, (long)lim, 0);
    if (r < 0) {
        sh_warn("ulimit: %s: cannot modify limit: %s", val, lp_strerror((int)-r));
        return 1;
    }
    return 0;
}

static void fmt_ticks(strbuf_t *b, long t)
{
    long hz = 100;
    long ms = t * (1000 / hz);
    sb_printf(b, "%ldm%ld.%03lds", ms / 60000, (ms / 1000) % 60, ms % 1000);
}

static int bi_times(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    long t[4] = { 0, 0, 0, 0 };
    sys_times(t);
    strbuf_t b = {0};
    fmt_ticks(&b, t[0]);
    sb_putc(&b, ' ');
    fmt_ticks(&b, t[1]);
    sb_putc(&b, '\n');
    fmt_ticks(&b, t[2]);
    sb_putc(&b, ' ');
    fmt_ticks(&b, t[3]);
    sb_putc(&b, '\n');
    outn(out1, b.s, b.len);
    sb_free(&b);
    return 0;
}

/* ── shopt ─────────────────────────────────────────────────────────── */

static int bi_shopt(int argc, char **argv)
{
    int mode = 0;       /* 0 print, 1 set, 2 unset */
    bool quiet = false, reusable = false, o_opts = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 's') mode = 1;
            else if (*p == 'u') mode = 2;
            else if (*p == 'q') quiet = true;
            else if (*p == 'p') reusable = true;
            else if (*p == 'o') o_opts = true;
            else {
                sh_warn("shopt: -%c: invalid option", *p);
                return 2;
            }
        }
    }
    unsigned n = sizeof shopts / sizeof *shopts;
    if (o_opts) {
        if (i >= argc) {
            print_options(reusable);
            return 0;
        }
        int st = 0;
        for (; i < argc; i++) {
            int o = opt_index(argv[i]);
            if (o < 0) {
                sh_warn("shopt: %s: invalid option name", argv[i]);
                st = 1;
                continue;
            }
            if (mode == 1) { opt[o] = true; opt_changed(o); }
            else if (mode == 2) { opt[o] = false; opt_changed(o); }
            else if (!opt[o]) st = 1;
        }
        return st;
    }
    if (i >= argc) {
        for (unsigned k = 0; k < n; k++) {
            bool on = *shopts[k].flag;
            if (mode == 1 && !on) continue;
            if (mode == 2 && on) continue;
            if (reusable)
                outf(out1, "shopt %s %s\n", on ? "-s" : "-u", shopts[k].name);
            else
                outf(out1, "%-15s\t%s\n", shopts[k].name, on ? "on" : "off");
        }
        return 0;
    }
    int st = 0;
    for (; i < argc; i++) {
        int k = -1;
        for (unsigned m = 0; m < n; m++)
            if (strcmp(shopts[m].name, argv[i]) == 0) k = (int)m;
        if (k < 0) {
            sh_warn("shopt: %s: invalid shell option name", argv[i]);
            st = 1;
            continue;
        }
        if (mode == 0) {
            bool on = *shopts[k].flag;
            if (!quiet) {
                if (reusable)
                    outf(out1, "shopt %s %s\n", on ? "-s" : "-u", shopts[k].name);
                else
                    outf(out1, "%-15s\t%s\n", shopts[k].name, on ? "on" : "off");
            }
            if (!on) st = 1;
            continue;
        }
        if (shopts[k].fixed) {
            if (mode == 2) {
                sh_warn("shopt: %s: cannot be turned off in this shell", argv[i]);
                st = 1;
            }
            continue;
        }
        *shopts[k].flag = mode == 1;
    }
    return st;
}

/* ── history, fc ───────────────────────────────────────────────────── */

static int bi_history(int argc, char **argv)
{
    int i = 1;
    if (i < argc && argv[i][0] == '-' && argv[i][1]) {
        const char *a = argv[i];
        if (strcmp(a, "-c") == 0) {
            history_clear();
            return 0;
        }
        if (strcmp(a, "-d") == 0) {
            long long n;
            if (i + 1 >= argc || !parse_int(argv[i + 1], &n) ||
                !history_delete((int)n)) {
                sh_warn("history: %s: history position out of range",
                        i + 1 < argc ? argv[i + 1] : "");
                return 1;
            }
            return 0;
        }
        if (strcmp(a, "-w") == 0 || strcmp(a, "-a") == 0) {
            history_write();
            return 0;
        }
        if (strcmp(a, "-r") == 0 || strcmp(a, "-n") == 0)
            return 0;
        if (strcmp(a, "-s") == 0) {
            strbuf_t b = {0};
            for (int k = i + 1; k < argc; k++) {
                if (k > i + 1) sb_putc(&b, ' ');
                sb_puts(&b, argv[k]);
            }
            history_add(sb_str(&b));
            sb_free(&b);
            return 0;
        }
        sh_warn("history: %s: invalid option", a);
        return 2;
    }
    long long n = -1;
    if (i < argc && !parse_int(argv[i], &n)) {
        sh_warn("history: %s: numeric argument required", argv[i]);
        return 1;
    }
    history_list((int)n);
    return 0;
}

static int bi_fc(int argc, char **argv)
{
    bool list = false, nonum = false, rev = false, subst = false;
    const char *editor = NULL;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] && !is_digit((u8)argv[i][1]); i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'l') list = true;
            else if (*p == 'n') nonum = true;
            else if (*p == 'r') rev = true;
            else if (*p == 's') subst = true;
            else if (*p == 'e') {
                if (i + 1 >= argc) return 2;
                editor = argv[++i];
                break;
            } else {
                sh_warn("fc: -%c: invalid option", *p);
                return 2;
            }
        }
    }
    int total = history_count();
    /* the fc command itself is the last entry; do not count it */
    int last_idx = total - 1;
    if (last_idx < 1) {
        sh_warn("fc: history specification out of range");
        return 1;
    }
    if (subst) {
        const char *old = NULL, *new = NULL;
        if (i < argc && strchr(argv[i], '=')) {
            char *eq = strchr(argv[i], '=');
            *eq = '\0';
            old = argv[i];
            new = eq + 1;
            i++;
        }
        int idx = last_idx - 1;
        if (i < argc)
            idx = history_find(argv[i], last_idx);
        if (idx < 0) {
            sh_warn("fc: no command found");
            return 1;
        }
        strbuf_t cmd = {0};
        const char *h = history_get(idx);
        if (old && *old) {
            const char *hit = strstr(h, old);
            if (hit) {
                sb_putn(&cmd, h, (size_t)(hit - h));
                sb_puts(&cmd, new);
                sb_puts(&cmd, hit + strlen(old));
            } else {
                sb_puts(&cmd, h);
            }
        } else {
            sb_puts(&cmd, h);
        }
        char *s = sb_take(&cmd);
        outs(out2, s);
        outc(out2, '\n');
        flush_out(out2);
        history_replace_last(s);
        int st = evalstring(s, 0);
        xfree(s);
        return st;
    }
    int first, last;
    if (list) {
        first = last_idx - 16;
        last = last_idx - 1;
        if (list && i >= argc) {
            first = last_idx - 16;
            if (first < 0) first = 0;
        }
    } else {
        first = last = last_idx - 1;
    }
    if (i < argc) {
        first = history_find(argv[i], last_idx);
        last = list ? last_idx - 1 : first;
        i++;
    }
    if (i < argc)
        last = history_find(argv[i], last_idx);
    if (first < 0 || last < 0) {
        sh_warn("fc: history specification out of range");
        return 1;
    }
    if (first > last) {
        int t = first; first = last; last = t;
        rev = !rev;
    }
    if (list) {
        for (int k = 0; k <= last - first; k++) {
            int idx = rev ? last - k : first + k;
            if (nonum)
                outf(out1, "\t %s\n", history_get(idx));
            else
                outf(out1, "%d\t %s\n", history_number(idx), history_get(idx));
        }
        return 0;
    }
    /* edit: write the range to a file, run the editor, run the result */
    if (!editor) editor = var_get("FCEDIT");
    if (!editor) editor = var_get("VISUAL");
    if (!editor) editor = var_get("EDITOR");
    if (!editor) editor = lp_exists("/usr/bin/nano") ? "nano" : "vi";
    char tmp[64];
    snprintf(tmp, sizeof tmp, "/tmp/lpsh-fc.%d", lp_getpid());
    long fd = lp_open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        sh_warn("fc: %s: %s", tmp, lp_strerror((int)-fd));
        return 1;
    }
    for (int k = first; k <= last; k++) {
        const char *h = history_get(k);
        xwrite_all((int)fd, h, strlen(h));
        xwrite_all((int)fd, "\n", 1);
    }
    lp_close((int)fd);
    strbuf_t run = {0};
    sb_puts(&run, editor);
    sb_putc(&run, ' ');
    sb_puts(&run, tmp);
    char *rs = sb_take(&run);
    int st = evalstring(rs, 0);
    xfree(rs);
    if (st != 0) {
        lp_unlink(tmp);
        return st;
    }
    fd = lp_open(tmp, O_RDONLY, 0);
    strbuf_t body = {0};
    if (fd >= 0) {
        char buf[1024];
        long n;
        while ((n = xread((int)fd, buf, sizeof buf)) > 0)
            sb_putn(&body, buf, (size_t)n);
        lp_close((int)fd);
    }
    lp_unlink(tmp);
    char *s = sb_take(&body);
    size_t sl = strlen(s);
    while (sl && s[sl - 1] == '\n') s[--sl] = '\0';
    outs(out2, s);
    outc(out2, '\n');
    flush_out(out2);
    history_replace_last(s);
    st = evalstring(s, 0);
    xfree(s);
    return st;
}

/* ── help ──────────────────────────────────────────────────────────── */

static int bi_help(int argc, char **argv);

/* ── the table ─────────────────────────────────────────────────────── *
 *
 * Sorted by name, for the binary search every command does. */

#define SP CMD_SPECIAL
#define BI CMD_BUILTIN

static const builtin_t builtins[] = {
    { ".", bi_dot, SP, ". filename [arguments]",
      "Run the commands in FILENAME in this shell. A name without a slash is\n"
      "looked for along PATH, then in the current directory." },
    { ":", bi_true, SP, ":", "Do nothing, successfully. Arguments are expanded." },
    { "[", bi_bracket, BI, "[ expression ]", "Same as test, with a closing ]." },
    { "alias", bi_alias, BI, "alias [name[=value] ...]",
      "Define or show aliases. Without arguments, list them all." },
    { "bg", bi_bg, BI, "bg [job ...]", "Resume stopped jobs in the background." },
    { "break", bi_break, SP, "break [n]", "Leave the innermost n loops." },
    { "builtin", bi_true, BI, "builtin shell-builtin [arguments]",
      "Run a shell builtin, even if a function has the same name." },
    { "cd", bi_cd, BI, "cd [-L|-P] [dir]",
      "Change the current directory. No dir: $HOME. `cd -' goes back to the\n"
      "previous directory. CDPATH is searched for relative names." },
    { "command", bi_command, BI, "command [-pVv] command [arg ...]",
      "Run a command without looking for a function of that name, or with\n"
      "-v/-V say how a name would be run." },
    { "continue", bi_continue, SP, "continue [n]",
      "Go on with the next round of the n-th enclosing loop." },
    { "declare", bi_declare, BI, "declare [-aFfgiprx] [name[=value] ...]",
      "Set variable values and attributes, or show them. In a function the\n"
      "variables are local unless -g is given." },
    { "dirs", bi_dirs, BI, "dirs [-clpv] [+N] [-N]", "Show the directory stack." },
    { "disown", bi_disown, BI, "disown [-ar] [job ...]",
      "Forget jobs: the shell no longer tracks them." },
    { "echo", bi_echo, BI, "echo [-neE] [arg ...]",
      "Write the arguments. Backslash escapes are interpreted (as in dash);\n"
      "-n: no newline at the end; -E: escapes off." },
    { "eval", bi_eval, SP, "eval [arg ...]",
      "Join the arguments with spaces and run the result as shell code." },
    { "exec", bi_exec, SP, "exec [-cl] [-a name] [command [arg ...]] [redirection ...]",
      "Replace the shell with command. Without one, make the redirections\n"
      "permanent for this shell." },
    { "exit", bi_exit, SP, "exit [n]", "Leave the shell with status n (default: $?)." },
    { "export", bi_export, SP, "export [-np] [name[=value] ...]",
      "Mark variables to be passed to programs the shell runs." },
    { "false", bi_false, BI, "false", "Do nothing, unsuccessfully." },
    { "fc", bi_fc, BI, "fc [-e ename] [-lnr] [first] [last] or fc -s [pat=rep] [command]",
      "List, edit and re-run commands from the history." },
    { "fg", bi_fg, BI, "fg [job]", "Bring a job to the foreground." },
    { "getopts", bi_getopts, BI, "getopts optstring name [arg ...]",
      "Parse options, one per call; see OPTIND and OPTARG." },
    { "hash", bi_hash, BI, "hash [-lr] [-p pathname] [-dt] [name ...]",
      "Remember or show where commands were found." },
    { "help", bi_help, BI, "help [pattern ...]",
      "Show help for the shell's builtins. For the other commands on this\n"
      "system, see `help -a' or the program /bin/help." },
    { "history", bi_history, BI, "history [-c] [-d offset] [-w] [n]",
      "Show or change the command history." },
    { "jobs", bi_jobs, BI, "jobs [-lnprs] [job ...]", "List the jobs." },
    { "kill", bi_kill, BI, "kill [-s sigspec | -n signum | -sigspec] pid | job ... or kill -l [sig]",
      "Send a signal to processes or jobs." },
    { "let", bi_let, BI, "let arg [arg ...]", "Evaluate arithmetic expressions." },
    { "local", bi_local, BI, "local [-airx] [name[=value] ...] or local -",
      "Make variables local to the running function." },
    { "logout", bi_logout, BI, "logout [n]", "Leave a login shell." },
    { "mapfile", bi_mapfile, BI, "mapfile [-d delim] [-n count] [-O origin] [-s count] [-t] [-u fd] [array]",
      "Read lines into an indexed array (default MAPFILE)." },
    { "popd", bi_popd, BI, "popd [-n] [+N | -N]",
      "Remove the top of the directory stack and change to the new top." },
    { "printf", bi_printf, BI, "printf [-v var] format [arguments]",
      "Format and print the arguments under control of the format." },
    { "pushd", bi_pushd, BI, "pushd [-n] [+N | -N | dir]",
      "Change to dir, remembering the current one on the directory stack." },
    { "pwd", bi_pwd, BI, "pwd [-LP]", "Print the current directory." },
    { "read", bi_read, BI, "read [-ers] [-a array] [-d delim] [-n nchars] [-N nchars] [-p prompt] [-t timeout] [-u fd] [name ...]",
      "Read a line and split it into the named variables (default REPLY)." },
    { "readarray", bi_mapfile, BI, "readarray [-d delim] [-n count] [-O origin] [-s count] [-t] [-u fd] [array]",
      "The same as mapfile." },
    { "readonly", bi_readonly, SP, "readonly [-p] [name[=value] ...]",
      "Mark variables unchangeable." },
    { "return", bi_return, SP, "return [n]",
      "Return from a function or a sourced file with status n." },
    { "set", bi_set, SP, "set [-abCefhmnuvx] [-o option] [--] [arg ...]",
      "Set shell options and the positional parameters. Without arguments,\n"
      "list the variables. `set -o' lists the options." },
    { "shift", bi_shift, SP, "shift [n]", "Drop the first n positional parameters." },
    { "shopt", bi_shopt, BI, "shopt [-pqsu] [-o] [optname ...]",
      "Set or show the bash-style options this shell has: autocd, dotglob,\n"
      "failglob, nullglob (and fixed ones that are always on)." },
    { "source", bi_dot, BI, "source filename [arguments]", "The same as `.'." },
    { "suspend", bi_suspend, BI, "suspend [-f]", "Stop this shell until it gets SIGCONT." },
    { "test", bi_test, BI, "test [expression]",
      "Evaluate a conditional expression: -e -f -d -r -w -x -s -z -n, = != < >,\n"
      "-eq -ne -lt -le -gt -ge, -nt -ot -ef, ! -a -o ( )." },
    { "times", bi_times, SP, "times", "Show the time used by the shell and its children." },
    { "trap", bi_trap, SP, "trap [-lp] [[action] signal ...]",
      "Run action when the shell receives a signal, or on EXIT." },
    { "true", bi_true, BI, "true", "Do nothing, successfully." },
    { "type", bi_type, BI, "type [-afptP] name [name ...]",
      "Say how each name would be interpreted as a command." },
    { "typeset", bi_declare, BI, "typeset [-aFfgiprx] [name[=value] ...]",
      "The same as declare." },
    { "ulimit", bi_ulimit, BI, "ulimit [-SHa] [-cdefilmnqrstuvx] [limit]",
      "Show or set resource limits." },
    { "umask", bi_umask, BI, "umask [-p] [-S] [mode]",
      "Show or set the file creation mask." },
    { "unalias", bi_unalias, BI, "unalias [-a] name [name ...]", "Remove aliases." },
    { "unset", bi_unset, SP, "unset [-f] [-v] [name ...]", "Remove variables or functions." },
    { "wait", bi_wait, BI, "wait [-n] [pid | job ...]",
      "Wait for background jobs and return their status." },
};

#undef SP
#undef BI

static const builtin_t *builtin_find(const char *name)
{
    int lo = 0, hi = (int)(sizeof builtins / sizeof *builtins) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(name, builtins[mid].name);
        if (c == 0)
            return &builtins[mid];
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return NULL;
}

static int bi_help(int argc, char **argv)
{
    int i = 1;
    bool shortform = false;
    if (i < argc && strcmp(argv[i], "-s") == 0) { shortform = true; i++; }
    if (i < argc && (strcmp(argv[i], "-a") == 0 ||
                     (!builtin_find(argv[i]) && !is_keyword(argv[i]) &&
                      !has_glob_chars(argv[i])))) {
        /* not about the shell: the system's own help lists commands */
        if (lp_exists("/bin/help")) {
            flush_all();
            strbuf_t cmd = {0};
            sb_puts(&cmd, "/bin/help");
            for (int k = i; k < argc; k++) {
                sb_putc(&cmd, ' ');
                sb_quoted(&cmd, argv[k]);
            }
            char *s = sb_take(&cmd);
            int st = evalstring(s, 0);
            xfree(s);
            return st;
        }
        if (strcmp(argv[i], "-a") != 0) {
            sh_warn("help: no help topics match `%s'. Try `help help'.", argv[i]);
            return 1;
        }
    }
    unsigned n = sizeof builtins / sizeof *builtins;
    if (i >= argc) {
        outs(out1, "lpsh, the LP shell. These commands are part of the shell itself.\n"
                   "Type `help name' for more about one of them, and `help -a' for\n"
                   "every command installed on this system.\n\n");
        for (unsigned k = 0; k < n; k++) {
            outf(out1, " %-37.37s", builtins[k].usage);
            outc(out1, k % 2 ? '\n' : ' ');
        }
        if (n % 2) outc(out1, '\n');
        outs(out1, "\nKeywords: ! [[ ]] case do done elif else esac fi for function if in\n"
                   "          select then time until while { } (( ))\n");
        return 0;
    }
    int st = 0;
    for (; i < argc; i++) {
        bool any = false;
        for (unsigned k = 0; k < n; k++) {
            if (!pmatch(argv[i], builtins[k].name, false))
                continue;
            any = true;
            outf(out1, "%s: %s\n", builtins[k].name, builtins[k].usage);
            if (!shortform) {
                const char *h = builtins[k].help;
                while (*h) {
                    const char *e = strchr(h, '\n');
                    size_t l = e ? (size_t)(e - h) : strlen(h);
                    outs(out1, "    ");
                    outn(out1, h, l);
                    outc(out1, '\n');
                    h += l + (e ? 1 : 0);
                }
            }
        }
        if (!any && is_keyword(argv[i])) {
            outf(out1, "%s: a shell keyword - see the shell grammar (man dash, man bash)\n",
                 argv[i]);
            any = true;
        }
        if (!any) {
            sh_warn("help: no help topics match `%s'", argv[i]);
            st = 1;
        }
    }
    return st;
}
