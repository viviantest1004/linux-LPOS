/* pathchk - will this name work as a file name?
 *
 *   pathchk NAME...          can this system use it, right now
 *   pathchk -p NAME...       could any POSIX system use it
 *   pathchk -P NAME...       is it empty, or does a part of it start with -
 *   pathchk --portability    -p and -P together
 *
 * The three questions are different and the difference is the whole
 * point. Without an option pathchk asks the kernel: it walks the name
 * and complains only about what actually went wrong here - a component
 * that is not a directory, one that cannot be searched, a name longer
 * than this filesystem allows. A name that does not exist yet is fine;
 * that is usually what is being checked.
 *
 * -p asks a different question and does not touch the disk at all. The
 * POSIX portable file name character set is A-Z a-z 0-9 . _ - and
 * nothing else, the smallest path any POSIX system must accept is 256
 * bytes and the smallest component 14. Those numbers are floors, not
 * what this machine does: Linux allows 4096 and 255. So `pathchk -p`
 * complaining about a name that works perfectly well here is the
 * correct answer to the question it was asked.
 *
 * -P is for the other trap: a name that begins with '-' is a valid file
 * name and an option to almost every command that will ever be handed
 * it, and an empty name is not a name at all.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define ENOENT 2

/* POSIX's floors. Deliberately not this kernel's limits - see the top. */
#define POSIX_PATH_MAX 256
#define POSIX_NAME_MAX 14

static bool basic = false;      /* -p */
static bool extra = false;      /* -P */

/* ── Quoting the way coreutils does ──
 *
 * A diagnostic that names a file has to be unambiguous about where the
 * name starts and stops, because the name may contain a space, a
 * newline or a byte that does not print at all. coreutils wraps it in
 * single quotes, switches to double quotes when the name itself
 * contains a single quote, and drops out of the quotes to spell an
 * unprintable byte as $'\t' or $'\303'. Reproducing that exactly
 * matters here: for pathchk the whole output is one of these lines.
 */
static bool printable(unsigned char c) { return c >= 0x20 && c < 0x7f; }

static void put_octal(char *out, size_t cap, size_t *n, unsigned char c)
{
    if (*n + 4 < cap) {
        out[(*n)++] = '\\';
        out[(*n)++] = (char)('0' + (c >> 6));
        out[(*n)++] = (char)('0' + ((c >> 3) & 7));
        out[(*n)++] = (char)('0' + (c & 7));
    }
}

static void quote(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    bool has_quote = false, has_special = false;

    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '\'') has_quote = true;
        else if (!printable(*p) || *p == '"' || *p == '\\' || *p == '$' || *p == '`')
            has_special = true;
    }

    /* One single quote and nothing worse reads better in double quotes
     * than as the '\'' dance. coreutils makes the same choice. */
    if (has_quote && !has_special) {
        snprintf(out, cap, "\"%s\"", s);
        return;
    }

    if (n + 1 < cap) out[n++] = '\'';
    bool inside = true;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (printable(*p) && *p != '\'') {
            /* Close the $'...' group and open a plain one: the pieces
             * sit side by side, 'a'$'\t''b', and the shell joins them. */
            if (!inside) {
                if (n + 2 < cap) { out[n++] = '\''; out[n++] = '\''; }
                inside = true;
            }
            if (n + 1 < cap) out[n++] = (char)*p;
            continue;
        }
        if (inside) {
            if (n + 3 < cap) { out[n++] = '\''; out[n++] = '$'; out[n++] = '\''; }
            inside = false;
        }
        switch (*p) {
        case '\a': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 'a'; } break;
        case '\b': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 'b'; } break;
        case '\f': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 'f'; } break;
        case '\n': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 'n'; } break;
        case '\r': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 'r'; } break;
        case '\t': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 't'; } break;
        case '\v': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 'v'; } break;
        case '\'': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = '\''; } break;
        default:   put_octal(out, cap, &n, *p); break;
        }
    }
    if (!inside && n + 1 < cap) out[n++] = '\'';
    if (inside && n + 1 < cap)  out[n++] = '\'';
    if (n < cap) out[n] = '\0';
    else if (cap) out[cap - 1] = '\0';
}

static char qbuf[8192];

/* The offending byte itself is quoted in the plainer style coreutils
 * keeps for single characters: one pair of quotes and a backslash
 * escape inside, so a tab reads '\t' rather than the ''$'\t'' the file
 * name form would produce. */
static void quote_char(char *out, size_t cap, unsigned char c)
{
    size_t n = 0;
    if (n + 1 < cap) out[n++] = '\'';
    switch (c) {
    case '\a': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 'a'; } break;
    case '\b': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 'b'; } break;
    case '\f': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 'f'; } break;
    case '\n': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 'n'; } break;
    case '\r': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 'r'; } break;
    case '\t': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 't'; } break;
    case '\v': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 'v'; } break;
    case '\'': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = '\''; } break;
    case '\\': if (n + 2 < cap) { out[n++] = '\\'; out[n++] = '\\'; } break;
    default:
        if (printable(c)) { if (n + 1 < cap) out[n++] = (char)c; }
        else              put_octal(out, cap, &n, c);
        break;
    }
    if (n + 1 < cap) out[n++] = '\'';
    if (n < cap) out[n] = '\0';
    else if (cap) out[cap - 1] = '\0';
}

/* The kernel's own complaints name the file the way the shell would
 * have to be told it: quoted only when something in it would otherwise
 * be lost, which is why `pathchk a.txt/x` says a.txt/x and `pathchk ''`
 * says ''. */
static void quote_if_needed(char *out, size_t cap, const char *s)
{
    bool need = (*s == '\0' || *s == '~' || *s == '#');
    for (const unsigned char *p = (const unsigned char *)s; *p && !need; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9'))
            continue;
        if (!strchr("%+,-./:=@_", (char)*p))
            need = true;
    }
    if (need) quote(out, cap, s);
    else      strlcpy(out, s, cap);
}

static bool portable_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
           c == '/';
}

/* Where a component starts, skipping the slashes in front of it. */
static const char *component_start(const char *p)
{
    while (*p == '/') p++;
    return p;
}

static size_t component_len(const char *p)
{
    size_t n = 0;
    while (p[n] && p[n] != '/') n++;
    return n;
}

static bool check(const char *file)
{
    size_t len = strlen(file);

    /* -P first: a leading '-' is reported even when -p would also have
     * something to say about the same name. */
    if (extra) {
        for (const char *p = component_start(file); *p; ) {
            if (*p == '-') {
                quote(qbuf, sizeof qbuf, file);
                dprintf(STDERR_FILENO,
                        "pathchk: leading '-' in a component of file name %s\n", qbuf);
                return false;
            }
            p += component_len(p);
            p = component_start(p);
        }
    }

    if (basic) {
        for (size_t i = 0; i < len; i++) {
            if (portable_char((unsigned char)file[i]))
                continue;
            char cbuf[16];
            quote_char(cbuf, sizeof cbuf, (unsigned char)file[i]);
            quote(qbuf, sizeof qbuf, file);
            dprintf(STDERR_FILENO,
                    "pathchk: non-portable character %s in file name %s\n",
                    cbuf, qbuf);
            return false;
        }
    }

    if (basic || extra) {
        if (len == 0) {
            dprintf(STDERR_FILENO, "pathchk: empty file name\n");
            return false;
        }
    } else {
        /* No portability question was asked, so the only authority is
         * the kernel. ENOENT is not a failure: a name that is not there
         * yet is exactly what people run this on. */
        lp_stat_t st;
        long r = lp_stat(file, &st, false);
        if (r < 0 && (-r != ENOENT || len == 0)) {
            quote_if_needed(qbuf, sizeof qbuf, file);
            dprintf(STDERR_FILENO, "pathchk: %s: %s\n",
                    qbuf, lp_strerror((int)-r));
            return false;
        }
    }

    if (basic) {
        if (len >= POSIX_PATH_MAX) {
            quote(qbuf, sizeof qbuf, file);
            dprintf(STDERR_FILENO,
                    "pathchk: limit %d exceeded by length %lu of file name %s\n",
                    POSIX_PATH_MAX - 1, (unsigned long)len, qbuf);
            return false;
        }
        for (const char *p = component_start(file); *p; ) {
            size_t clen = component_len(p);
            if (clen > POSIX_NAME_MAX) {
                char comp[512];
                size_t keep = clen < sizeof comp - 1 ? clen : sizeof comp - 1;
                memcpy(comp, p, keep);
                comp[keep] = '\0';
                quote(qbuf, sizeof qbuf, comp);
                dprintf(STDERR_FILENO,
                        "pathchk: limit %d exceeded by length %lu of file name "
                        "component %s\n",
                        POSIX_NAME_MAX, (unsigned long)clen, qbuf);
                return false;
            }
            p = component_start(p + clen);
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "portability", 0, 'X' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "pP", lo);

    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'p': basic = true; break;
        case 'P': extra = true; break;
        case 'X': basic = extra = true; break;
        case 'H':
            printf("Usage: pathchk [OPTION]... NAME...\n"
                   "Diagnose invalid or non-portable file names.\n\n"
                   "  -p                  check for most POSIX systems\n"
                   "  -P                  check for empty names and leading \"-\"\n"
                   "      --portability   check for all POSIX systems "
                   "(equivalent to -p -P)\n"
                   "      --help        display this help and exit\n");
            return 0;
        default: lp_getopt_err("pathchk", &g); return 1;
        }
    }

    if (g.ind >= argc) {
        dprintf(STDERR_FILENO, "pathchk: missing operand\n");
        dprintf(STDERR_FILENO, "Try 'pathchk --help' for more information.\n");
        return 1;
    }

    int rc = 0;
    for (int i = g.ind; i < argc; i++)
        if (!check(argv[i]))
            rc = 1;
    return rc;
}
