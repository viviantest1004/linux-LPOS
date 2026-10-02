/* sudoers.h - reading /etc/sudoers, and answering "may this user run
 * this command as that user on this host", for sudo and visudo.
 *
 * ── How much of sudoers(5) ──
 *
 * The part people actually write, parsed the way sudo parses it:
 *
 *   Defaults[:user|@host|>runas|!cmnd]  flag, !flag, name=value, name+=value, name-=value
 *   User_Alias / Runas_Alias / Host_Alias / Cmnd_Alias (Cmd_Alias)  NAME = list [: NAME = list]
 *   user_list host_list = [(runas_users[:runas_groups])] [TAG:]... command [, ...] [: host_list = ...]
 *   @include / #include file, @includedir / #includedir dir
 *
 * with ALL, !negation, %group, %#gid, #uid, aliases, NOPASSWD/PASSWD,
 * SETENV/NOSETENV, command paths with arguments, "" for "no arguments",
 * a directory ending in '/', and shell wildcards in paths and arguments.
 * The last rule that matches decides, as in sudo.
 *
 * ── What happens to the rest ──
 *
 * The rule the whole file is built on: when this parser meets something
 * it does not implement, it must never quietly grant more than the
 * author meant. So:
 *
 *   - Anything that restricts or changes WHO is asked for a password,
 *     or what a command may do once running (NOEXEC, INTERCEPT, CWD=,
 *     CHROOT=, TIMEOUT=, rootpw, targetpw, runaspw, digests, regular
 *     expressions, netgroups, sudoedit, timestamp_type other than tty)
 *     is a parse error. sudo then refuses to run for anybody - the same
 *     thing real sudo does with a file it cannot read - and visudo
 *     says which line.
 *   - Defaults that only affect mail, I/O logging, PAM or cosmetics
 *     are accepted and ignored: ignoring them takes nothing away.
 *   - A Defaults name nobody has heard of is a warning, as in sudo.
 *
 * A file that does not parse at all refuses everybody, root included.
 * That is sudo's documented behaviour and the reason visudo exists: an
 * administrator who breaks sudoers by hand has to fix it as root by
 * other means, which is inconvenient, and far better than a half-read
 * policy that lets the wrong person in.
 *
 * ── Why a header ──
 *
 * sudo and visudo must agree exactly about what is valid - visudo
 * installing a file that sudo then refuses is the lock-out it exists
 * to prevent. The Makefile builds each program from one .c file, so
 * the shared code is a header of static functions, included by both.
 */
#ifndef LP_SUDOERS_H
#define LP_SUDOERS_H

#include "lp-auth.h"

#define S_UNUSED __attribute__((unused))

enum { SK_USER = 1, SK_RUNAS, SK_HOST, SK_CMND, SK_GROUP };

typedef struct s_member {
    char *name;          /* "bob", "%wheel", "#1000", "ALIAS", "/usr/bin/apt" */
    char *args;          /* commands: NULL any arguments, "" none, else a pattern */
    bool  neg, all, alias;
    struct s_member *next;
} s_member;

typedef struct s_alias {
    int   kind;          /* SK_USER, SK_RUNAS, SK_HOST, SK_CMND */
    char *name;
    s_member *members;
    const char *file;
    int   line;
    struct s_alias *next;
} s_alias;

/* One command of one user specification, with the runas list and tags
 * that apply to it already carried forward from the ones before it. */
typedef struct s_rule {
    s_member *users, *hosts;
    s_member *runas_users, *runas_groups;
    bool  runas_given;
    int   nopasswd;      /* -1 not said (PASSWD), 0 PASSWD, 1 NOPASSWD */
    int   setenv;        /* -1 not said, 0 NOSETENV, 1 SETENV */
    s_member *cmnd;
    int   priv;          /* which "host = ..." group it came from, for -l */
    const char *file;
    int   line;
    struct s_rule *next;
} s_rule;

typedef struct s_default {
    char  scope;         /* 0 everywhere, ':' user, '@' host, '>' runas, '!' command */
    s_member *binding;
    char *name;
    int   op;            /* 'T' true, 'F' false, '=' set, '+' add, '-' remove */
    char *value;
    const char *file;
    int   line;
    struct s_default *next;
} s_default;

typedef struct {
    s_alias   *aliases;
    s_rule    *rules, *rules_tail;
    s_default *defs, *defs_tail;
    int        nprivs;
    bool       failed;
    char       err[512];     /* "/etc/sudoers:12: ..." */
    bool       check_perms;  /* sudo: owner root, not group/world writable */
    bool       quiet_warn;
    int        depth;
} sudoers_t;

/* ── Small helpers ────────────────────────────────────────────────── */

static char *s_dup(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (!d) { dprintf(2, "sudo: out of memory\n"); lp_exit(1); }
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static S_UNUSED bool s_streq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
    }
    return *a == *b;
}

static bool s_is_alias_name(const char *w)
{
    if (!(*w >= 'A' && *w <= 'Z')) return false;
    for (const char *p = w; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'))
            return false;
    return strcmp(w, "ALL") != 0;
}

static bool s_has_glob(const char *s)
{
    for (; *s; s++) {
        if (*s == '\\' && s[1]) { s++; continue; }
        if (*s == '*' || *s == '?' || *s == '[') return true;
    }
    return false;
}

/* fnmatch(3) with the two flags sudo uses: FNM_PATHNAME for command
 * paths (a wildcard never matches a slash, so "/usr/bin/<star>" is not
 * "/usr/bin/../../tmp/x"), none for argument strings. Backslash quotes
 * the next character in both. */
static bool s_fnmatch(const char *pat, const char *str, bool pathname)
{
    while (*pat) {
        char c = *pat++;
        switch (c) {
        case '?':
            if (!*str || (pathname && *str == '/')) return false;
            str++;
            break;
        case '*':
            while (*pat == '*') pat++;
            if (!*pat) return pathname ? strchr(str, '/') == NULL : true;
            for (;;) {
                if (s_fnmatch(pat, str, pathname)) return true;
                if (!*str || (pathname && *str == '/')) return false;
                str++;
            }
        case '[': {
            if (!*str || (pathname && *str == '/')) return false;
            bool negate = (*pat == '!' || *pat == '^');
            if (negate) pat++;
            bool hit = false;
            const char *start = pat;
            while (*pat && (*pat != ']' || pat == start)) {
                char lo = *pat++;
                if (lo == '\\' && *pat) lo = *pat++;
                char hi = lo;
                if (*pat == '-' && pat[1] && pat[1] != ']') {
                    pat++;
                    hi = *pat++;
                    if (hi == '\\' && *pat) hi = *pat++;
                }
                if ((unsigned char)*str >= (unsigned char)lo &&
                    (unsigned char)*str <= (unsigned char)hi)
                    hit = true;
            }
            if (*pat != ']') return false;      /* unterminated: no match */
            pat++;
            if (hit == negate) return false;
            str++;
            break;
        }
        case '\\':
            if (*pat) c = *pat++;
            /* fall through */
        default:
            if (*str != c) return false;
            str++;
        }
    }
    return *str == '\0';
}

/* ── Errors ───────────────────────────────────────────────────────── */

typedef struct {
    sudoers_t  *s;
    const char *file;
    int         line;
    const char *p;
} s_cur;

static bool s_fail(s_cur *c, const char *what)
{
    if (!c->s->failed) {
        snprintf(c->s->err, sizeof c->s->err, "%s:%d: %s", c->file, c->line, what);
        c->s->failed = true;
    }
    return false;
}

static void s_ws(s_cur *c)
{
    while (*c->p == ' ' || *c->p == '\t') c->p++;
}

/* A name: user, group, host, alias, "#1000". Stops at whitespace and at
 * the characters the grammar gives a meaning to, unless escaped with a
 * backslash; "double quotes" hold anything but a quote. */
static bool s_word(s_cur *c, char *out, size_t cap, bool stop_at_colon)
{
    size_t n = 0;
    if (*c->p == '"') {
        c->p++;
        while (*c->p && *c->p != '"') {
            char ch = *c->p++;
            if (ch == '\\' && *c->p) ch = *c->p++;
            if (n + 1 >= cap) return s_fail(c, "name too long");
            out[n++] = ch;
        }
        if (*c->p != '"') return s_fail(c, "unterminated quoted name");
        c->p++;
    } else {
        while (*c->p) {
            char ch = *c->p;
            if (ch == ' ' || ch == '\t' || ch == ',' || ch == '=' || ch == '(' ||
                ch == ')' || ch == '!' || (stop_at_colon && ch == ':'))
                break;
            c->p++;
            if (ch == '\\' && *c->p) ch = *c->p++;
            if (n + 1 >= cap) return s_fail(c, "name too long");
            out[n++] = ch;
        }
    }
    out[n] = '\0';
    return n > 0;
}

static s_alias *s_find_alias(sudoers_t *s, int kind, const char *name)
{
    for (s_alias *a = s->aliases; a; a = a->next)
        if (a->kind == kind && strcmp(a->name, name) == 0) return a;
    return NULL;
}

/* One member of a user, runas, group or host list. */
static s_member *s_member_of(s_cur *c, int kind, bool stop_at_colon)
{
    s_ws(c);
    bool neg = false;
    while (*c->p == '!') { neg = !neg; c->p++; s_ws(c); }
    char w[256];
    if (!s_word(c, w, sizeof w, stop_at_colon)) {
        s_fail(c, "syntax error: a name was expected here");
        return NULL;
    }
    s_member *m = calloc(1, sizeof *m);
    if (!m) lp_exit(1);
    m->neg = neg;
    m->name = s_dup(w, strlen(w));
    if (strcmp(w, "ALL") == 0) {
        m->all = true;
    } else if (s_is_alias_name(w)) {
        m->alias = true;
    } else if (w[0] == '+') {
        s_fail(c, "netgroups (+name) are not supported by this sudo");
        return NULL;
    } else if (w[0] == '%' && w[1] == ':') {
        s_fail(c, "non-Unix groups (%:name) are not supported by this sudo");
        return NULL;
    } else if (kind == SK_HOST && (strchr(w, '/') || (w[0] >= '0' && w[0] <= '9'))) {
        /* An address or a network. Accepted, and it never matches: this
         * machine is matched by name, and a rule that cannot match takes
         * nothing away from anybody. */
    } else if (w[0] == '#' || (w[0] == '%' && w[1] == '#')) {
        u32 id;
        if (!a_parse_id(w + (w[0] == '#' ? 1 : 2), &id)) {
            s_fail(c, "syntax error: bad numeric id");
            return NULL;
        }
    }
    return m;
}

static s_member *s_list(s_cur *c, int kind, bool stop_at_colon)
{
    s_member *head = NULL, **tail = &head;
    for (;;) {
        s_member *m = s_member_of(c, kind, stop_at_colon);
        if (!m) return NULL;
        *tail = m;
        tail = &m->next;
        s_ws(c);
        if (*c->p != ',') return head;
        c->p++;
    }
}

/* A command: ALL, an alias, or an absolute path with optional arguments.
 * The arguments run to the next unescaped ',' or ':'; in them "\," "\:"
 * "\=" and "\\" are the characters themselves, and other backslashes are
 * left for the wildcard matcher. */
static s_member *s_command(s_cur *c)
{
    s_ws(c);
    bool neg = false;
    while (*c->p == '!') { neg = !neg; c->p++; s_ws(c); }
    s_member *m = calloc(1, sizeof *m);
    if (!m) lp_exit(1);
    m->neg = neg;

    char path[1024];
    size_t n = 0;
    while (*c->p && *c->p != ' ' && *c->p != '\t' && *c->p != ',' && *c->p != ':' &&
           *c->p != '=') {
        char ch = *c->p++;
        if (ch == '\\' && *c->p) {
            char nx = *c->p;
            if (nx == ',' || nx == ':' || nx == '=' || nx == ' ' || nx == '\\') {
                ch = nx;
                c->p++;
            } else {
                if (n + 2 >= sizeof path) { s_fail(c, "command too long"); return NULL; }
                path[n++] = '\\';
                ch = *c->p++;
            }
        }
        if (n + 1 >= sizeof path) { s_fail(c, "command too long"); return NULL; }
        path[n++] = ch;
    }
    path[n] = '\0';
    if (n == 0) { s_fail(c, "syntax error: a command was expected here"); return NULL; }
    m->name = s_dup(path, n);

    if (strcmp(path, "ALL") == 0) { m->all = true; return m; }
    if (s_is_alias_name(path)) { m->alias = true; return m; }
    if (strcmp(path, "sudoedit") == 0 || strncmp(path, "sudoedit ", 9) == 0) {
        s_fail(c, "sudoedit is not supported by this sudo");
        return NULL;
    }
    if (path[0] == '^') { s_fail(c, "regular expressions are not supported by this sudo"); return NULL; }
    if (path[0] != '/') {
        s_fail(c, "a command must be ALL, an alias, or a fully qualified path");
        return NULL;
    }

    /* Arguments, if any. */
    const char *save = c->p;
    s_ws(c);
    if (*c->p && *c->p != ',' && *c->p != ':') {
        char args[2048];
        size_t k = 0;
        while (*c->p && *c->p != ',' && *c->p != ':') {
            char ch = *c->p++;
            if (ch == '\\' && *c->p) {
                char nx = *c->p;
                if (nx == ',' || nx == ':' || nx == '=' || nx == '\\') { ch = nx; c->p++; }
                else { if (k + 2 >= sizeof args) break; args[k++] = '\\'; ch = *c->p++; }
            }
            if (k + 1 >= sizeof args) { s_fail(c, "arguments too long"); return NULL; }
            args[k++] = ch;
        }
        while (k > 0 && (args[k - 1] == ' ' || args[k - 1] == '\t')) k--;
        args[k] = '\0';
        m->args = strcmp(args, "\"\"") == 0 ? s_dup("", 0) : s_dup(args, k);
        if (path[n - 1] == '/') {
            s_fail(c, "a directory (a path ending in /) cannot have arguments");
            return NULL;
        }
    } else {
        c->p = save;
    }
    return m;
}

/* ── Defaults ─────────────────────────────────────────────────────── */

/* What this sudo knows about each Defaults name.
 *   'b' a flag; 'n' a number; 's' a string; 'l' a list;
 *   'i' accepted and ignored (takes nothing away);
 *   'r' refused: ignoring it would grant more than was written. */
static const struct { const char *name; char type; } s_defnames[] = {
    { "env_reset", 'b' }, { "setenv", 'b' }, { "authenticate", 'b' },
    { "requiretty", 'b' }, { "lecture", 's' }, { "pwfeedback", 'b' },
    { "shell_noargs", 'b' }, { "env_editor", 'b' }, { "insults", 'b' },
    { "timestamp_timeout", 'n' }, { "passwd_tries", 'n' }, { "umask", 'n' },
    { "secure_path", 's' }, { "passprompt", 's' }, { "badpass_message", 's' },
    { "logfile", 's' }, { "editor", 's' }, { "runas_default", 's' },
    { "timestamp_type", 's' },
    { "env_keep", 'l' }, { "env_check", 'l' }, { "env_delete", 'l' },
    /* harmless to ignore: mail, logging detail, PAM, I/O logs, looks */
    { "mail_badpass", 'i' }, { "mail_always", 'i' }, { "mail_no_user", 'i' },
    { "mail_no_host", 'i' }, { "mail_no_perms", 'i' }, { "mailto", 'i' },
    { "mailsub", 'i' }, { "mailerpath", 'i' }, { "mailerflags", 'i' },
    { "mailfrom", 'i' }, { "use_pty", 'i' }, { "log_input", 'i' },
    { "log_output", 'i' }, { "iolog_dir", 'i' }, { "iolog_file", 'i' },
    { "syslog", 'i' }, { "syslog_goodpri", 'i' }, { "syslog_badpri", 'i' },
    { "log_host", 'i' }, { "log_year", 'i' }, { "loglinelen", 'i' },
    { "fqdn", 'i' }, { "ignore_dot", 'i' }, { "always_set_home", 'i' },
    { "set_home", 'i' }, { "tty_tickets", 'i' }, { "visiblepw", 'i' },
    { "passwd_timeout", 'i' }, { "lecture_file", 'i' }, { "lecture_status_dir", 'i' },
    { "path_info", 'i' }, { "compress_io", 'i' }, { "pam_session", 'i' },
    { "pam_setcred", 'i' }, { "pam_service", 'i' }, { "pam_login_service", 'i' },
    { "sudoers_locale", 'i' }, { "closefrom", 'i' }, { "closefrom_override", 'i' },
    { "match_group_by_gid", 'i' }, { "exempt_group", 'i' }, { "umask_override", 'i' },
    { "preserve_groups", 'i' }, { "noexec_file", 'i' }, { "askpass", 'i' },
    { "log_allowed", 'i' }, { "log_denied", 'i' }, { "log_format", 'i' },
    { "use_netgroups", 'i' }, { "user_command_timeouts", 'i' }, { "admin_flag", 'i' },
    /* refused: they narrow who may authenticate, or confine what runs */
    { "rootpw", 'r' }, { "targetpw", 'r' }, { "runaspw", 'r' },
    { "noexec", 'r' }, { "intercept", 'r' }, { "env_file", 'r' },
    { "restricted_env_file", 'r' }, { "group_plugin", 'r' }, { "timestampdir", 'r' },
    { "timestampowner", 'r' }, { "runchroot", 'r' }, { "runcwd", 'r' },
    { "command_timeout", 'r' }, { "selinux", 'r' }, { "apparmor_profile", 'r' },
    { "targetpw_prompt", 'r' }, { "requiretty_all", 'r' },
    { NULL, 0 }
};

static char s_deftype(const char *name)
{
    for (int i = 0; s_defnames[i].name; i++)
        if (strcmp(s_defnames[i].name, name) == 0) return s_defnames[i].type;
    return 0;
}

/* Does this value parse as the number sudo would read? timestamp_timeout
 * takes minutes with a fraction ("2.5", "-1"); umask is octal. */
static bool s_parse_minutes_ms(const char *v, s64 *ms)
{
    bool neg = false;
    if (*v == '-') { neg = true; v++; }
    if (!*v) return false;
    s64 whole = 0, frac = 0, scale = 1;
    while (*v >= '0' && *v <= '9') { whole = whole * 10 + (*v++ - '0'); if (whole > 1000000) return false; }
    if (*v == '.') {
        v++;
        while (*v >= '0' && *v <= '9') { if (scale < 1000000) { frac = frac * 10 + (*v - '0'); scale *= 10; } v++; }
    }
    if (*v) return false;
    s64 r = whole * 60000 + frac * 60000 / scale;
    *ms = neg ? -r : r;
    return true;
}

static bool s_parse_int(const char *v, int base, long *out)
{
    if (!*v) return false;
    long r = 0;
    for (; *v; v++) {
        int d = *v - '0';
        if (d < 0 || d >= base) return false;
        r = r * base + d;
        if (r > 1000000) return false;
    }
    *out = r;
    return true;
}

static bool s_check_default(s_cur *c, const char *name, int op, const char *value)
{
    char t = s_deftype(name);
    char msg[160];
    if (t == 0) {
        if (!c->s->quiet_warn)
            dprintf(2, "sudo: %s:%d: unknown defaults entry \"%s\"\n", c->file, c->line, name);
        return true;
    }
    if (t == 'r') {
        snprintf(msg, sizeof msg, "Defaults \"%s\" is not supported by this sudo", name);
        return s_fail(c, msg);
    }
    if (t == 'i') return true;
    if (t == 'b' && (op == '=' || op == '+' || op == '-')) {
        snprintf(msg, sizeof msg, "\"%s\" is a flag and takes no value", name);
        return s_fail(c, msg);
    }
    if ((t == 'n' || t == 's') && (op == '+' || op == '-')) {
        snprintf(msg, sizeof msg, "\"%s\" is not a list", name);
        return s_fail(c, msg);
    }
    if ((t == 'n' || t == 'l') && op == 'T') {
        snprintf(msg, sizeof msg, "\"%s\" needs a value", name);
        return s_fail(c, msg);
    }
    if (t == 'n' && op == '=') {
        s64 ms;
        long v;
        bool ok = strcmp(name, "timestamp_timeout") == 0 ? s_parse_minutes_ms(value, &ms)
                : strcmp(name, "umask") == 0 ? s_parse_int(value, 8, &v)
                : s_parse_int(value, 10, &v);
        if (ok && strcmp(name, "passwd_tries") == 0 && (v < 1 || v > 100)) ok = false;
        if (!ok) {
            snprintf(msg, sizeof msg, "value \"%s\" is invalid for option \"%s\"", value, name);
            return s_fail(c, msg);
        }
    }
    if (strcmp(name, "timestamp_type") == 0 && op == '=' && strcmp(value, "tty") != 0)
        return s_fail(c, "only timestamp_type=tty is supported by this sudo");
    if (strcmp(name, "secure_path") == 0 && op == '=') {
        /* Every element absolute: a relative one would be looked up in
         * whatever directory the user ran sudo from. */
        const char *p = value;
        while (*p) {
            if (*p != '/') return s_fail(c, "every secure_path entry must be an absolute directory");
            while (*p && *p != ':') p++;
            if (*p == ':') p++;
        }
    }
    if (strcmp(name, "lecture") == 0 && op == '=' && strcmp(value, "always") &&
        strcmp(value, "once") && strcmp(value, "never"))
        return s_fail(c, "lecture must be always, once or never");
    return true;
}

static bool s_defaults_line(s_cur *c)
{
    s_default proto;
    memset(&proto, 0, sizeof proto);
    proto.file = c->file;
    proto.line = c->line;
    c->p += 8;                                   /* "Defaults" */
    char sc = *c->p;
    if (sc == ':' || sc == '@' || sc == '>' || sc == '!') {
        c->p++;
        proto.scope = sc;
        if (sc == '!') {
            s_member *head = NULL, **tail = &head;
            for (;;) {
                s_member *m = s_command(c);
                if (!m) return false;
                *tail = m; tail = &m->next;
                s_ws(c);
                if (*c->p != ',') break;
                c->p++;
            }
            proto.binding = head;
        } else {
            /* The binding list ends at the first space: "Defaults:bob,alice !lecture". */
            s_member *head = NULL, **tail = &head;
            for (;;) {
                s_member *m = s_member_of(c, sc == ':' ? SK_USER : sc == '@' ? SK_HOST : SK_RUNAS, false);
                if (!m) return false;
                *tail = m; tail = &m->next;
                if (*c->p != ',') break;
                c->p++;
            }
            proto.binding = head;
        }
    } else if (sc != ' ' && sc != '\t') {
        return s_fail(c, "syntax error after \"Defaults\"");
    }

    for (;;) {
        s_ws(c);
        int op = 'T';
        while (*c->p == '!') { op = op == 'T' ? 'F' : 'T'; c->p++; s_ws(c); }
        char name[64];
        size_t n = 0;
        while ((*c->p >= 'a' && *c->p <= 'z') || (*c->p >= 'A' && *c->p <= 'Z') ||
               (*c->p >= '0' && *c->p <= '9') || *c->p == '_') {
            if (n + 1 >= sizeof name) return s_fail(c, "Defaults name too long");
            name[n++] = *c->p++;
        }
        name[n] = '\0';
        if (n == 0) return s_fail(c, "syntax error: a Defaults option was expected");
        s_ws(c);
        char value[1024];
        value[0] = '\0';
        if (*c->p == '=' || ((*c->p == '+' || *c->p == '-') && c->p[1] == '=')) {
            if (op == 'F') return s_fail(c, "a negated option cannot take a value");
            op = *c->p == '=' ? '=' : *c->p;
            c->p += op == '=' ? 1 : 2;
            s_ws(c);
            size_t k = 0;
            if (*c->p == '"') {
                c->p++;
                while (*c->p && *c->p != '"') {
                    char ch = *c->p++;
                    if (ch == '\\' && *c->p) ch = *c->p++;
                    if (k + 1 >= sizeof value) return s_fail(c, "value too long");
                    value[k++] = ch;
                }
                if (*c->p != '"') return s_fail(c, "unterminated quoted value");
                c->p++;
            } else {
                while (*c->p && *c->p != ',' && *c->p != ' ' && *c->p != '\t') {
                    char ch = *c->p++;
                    if (ch == '\\' && *c->p) ch = *c->p++;
                    if (k + 1 >= sizeof value) return s_fail(c, "value too long");
                    value[k++] = ch;
                }
            }
            value[k] = '\0';
        }
        if (!s_check_default(c, name, op, value)) return false;
        s_default *d = malloc(sizeof *d);
        if (!d) lp_exit(1);
        *d = proto;
        d->name = s_dup(name, n);
        d->op = op;
        d->value = s_dup(value, strlen(value));
        d->next = NULL;
        if (c->s->defs_tail) c->s->defs_tail->next = d; else c->s->defs = d;
        c->s->defs_tail = d;
        s_ws(c);
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p) return s_fail(c, "syntax error: unexpected text after a Defaults option");
        return true;
    }
}

/* ── Aliases ──────────────────────────────────────────────────────── */

static bool s_alias_line(s_cur *c, int kind)
{
    for (;;) {
        s_ws(c);
        char name[128];
        if (!s_word(c, name, sizeof name, true)) return s_fail(c, "syntax error: alias name expected");
        if (!s_is_alias_name(name))
            return s_fail(c, "an alias name must be upper case letters, digits and _, and not ALL");
        if (s_find_alias(c->s, kind, name)) return s_fail(c, "alias defined twice");
        s_ws(c);
        if (*c->p != '=') return s_fail(c, "syntax error: '=' expected after the alias name");
        c->p++;
        s_member *list;
        if (kind == SK_CMND) {
            s_member *head = NULL, **tail = &head;
            for (;;) {
                s_member *m = s_command(c);
                if (!m) return false;
                *tail = m; tail = &m->next;
                s_ws(c);
                if (*c->p != ',') break;
                c->p++;
            }
            list = head;
        } else {
            list = s_list(c, kind, true);
            if (!list) return false;
        }
        s_alias *a = calloc(1, sizeof *a);
        if (!a) lp_exit(1);
        a->kind = kind;
        a->name = s_dup(name, strlen(name));
        a->members = list;
        a->file = c->file;
        a->line = c->line;
        a->next = c->s->aliases;
        c->s->aliases = a;
        s_ws(c);
        if (*c->p == ':') { c->p++; continue; }
        if (*c->p) return s_fail(c, "syntax error in alias definition");
        return true;
    }
}

/* ── User specifications ──────────────────────────────────────────── */

static bool s_tag(s_cur *c, int *nopasswd, int *setenv)
{
    static const char *const ignored[] = {
        "MAIL", "NOMAIL", "LOG_INPUT", "NOLOG_INPUT", "LOG_OUTPUT", "NOLOG_OUTPUT",
        "FOLLOW", "NOFOLLOW", "EXEC", NULL };
    static const char *const refused[] = { "NOEXEC", "INTERCEPT", "NOINTERCEPT", NULL };
    static const char *const options[] = {
        "CWD", "CHROOT", "ROLE", "TYPE", "TIMEOUT", "NOTBEFORE", "NOTAFTER",
        "APPARMOR_PROFILE", "PRIVS", "LIMITPRIVS", NULL };
    const char *p = c->p;
    char w[32];
    size_t n = 0;
    while (((*p >= 'A' && *p <= 'Z') || *p == '_') && n + 1 < sizeof w) w[n++] = *p++;
    w[n] = '\0';
    if (n == 0) return false;
    if (*p == '=') {
        for (int i = 0; options[i]; i++)
            if (strcmp(w, options[i]) == 0) {
                char msg[96];
                snprintf(msg, sizeof msg, "%s= is not supported by this sudo", w);
                s_fail(c, msg);
                return false;
            }
        return false;
    }
    if (*p != ':') return false;
    if (strcmp(w, "NOPASSWD") == 0) *nopasswd = 1;
    else if (strcmp(w, "PASSWD") == 0) *nopasswd = 0;
    else if (strcmp(w, "SETENV") == 0) *setenv = 1;
    else if (strcmp(w, "NOSETENV") == 0) *setenv = 0;
    else {
        bool known = false;
        for (int i = 0; ignored[i]; i++) if (strcmp(w, ignored[i]) == 0) known = true;
        for (int i = 0; refused[i]; i++)
            if (strcmp(w, refused[i]) == 0) {
                char msg[96];
                snprintf(msg, sizeof msg, "the %s tag is not supported by this sudo", w);
                s_fail(c, msg);
                return false;
            }
        if (!known) return false;        /* an alias name followed by ':' */
    }
    c->p = p + 1;
    return true;
}

static bool s_userspec(s_cur *c)
{
    s_member *users = s_list(c, SK_USER, false);
    if (!users) return c->s->failed ? false : s_fail(c, "syntax error");
    for (;;) {
        s_ws(c);
        s_member *hosts = s_list(c, SK_HOST, false);
        if (!hosts) return false;
        s_ws(c);
        if (*c->p != '=') return s_fail(c, "syntax error: '=' expected after the host list");
        c->p++;
        int priv = ++c->s->nprivs;

        s_member *ru = NULL, *rg = NULL;
        bool given = false;
        int nopw = -1, se = -1;
        for (;;) {
            s_ws(c);
            if (*c->p == '(') {
                c->p++;
                given = true;
                ru = rg = NULL;
                s_ws(c);
                if (*c->p != ':' && *c->p != ')') {
                    ru = s_list(c, SK_RUNAS, true);
                    if (!ru) return false;
                    s_ws(c);
                }
                if (*c->p == ':') {
                    c->p++;
                    s_ws(c);
                    if (*c->p != ')') {
                        rg = s_list(c, SK_GROUP, true);
                        if (!rg) return false;
                        s_ws(c);
                    }
                }
                if (*c->p != ')') return s_fail(c, "syntax error: ')' expected");
                c->p++;
                s_ws(c);
            }
            while (s_tag(c, &nopw, &se)) s_ws(c);
            if (c->s->failed) return false;

            s_member *cm = s_command(c);
            if (!cm) return false;
            s_rule *r = calloc(1, sizeof *r);
            if (!r) lp_exit(1);
            r->users = users;
            r->hosts = hosts;
            r->runas_users = ru;
            r->runas_groups = rg;
            r->runas_given = given;
            r->nopasswd = nopw;
            r->setenv = se;
            r->cmnd = cm;
            r->priv = priv;
            r->file = c->file;
            r->line = c->line;
            if (c->s->rules_tail) c->s->rules_tail->next = r; else c->s->rules = r;
            c->s->rules_tail = r;

            s_ws(c);
            if (*c->p == ',') { c->p++; continue; }
            break;
        }
        if (*c->p == ':') { c->p++; continue; }
        if (*c->p) return s_fail(c, "syntax error: unexpected text after the command list");
        return true;
    }
}

/* ── Files ────────────────────────────────────────────────────────── */

static bool s_load(sudoers_t *s, const char *path);

static bool s_include(s_cur *c, const char *arg, bool dir, const char *curfile)
{
    char path[512];
    /* %h is the short host name, as in sudo. */
    char expanded[512];
    size_t n = 0;
    for (const char *p = arg; *p && n + 1 < sizeof expanded; p++) {
        if (p[0] == '%' && p[1] == 'h') {
            char host[64];
            a_hostname(host, sizeof host);
            char *dot = strchr(host, '.');
            if (dot) *dot = '\0';
            for (const char *h = host; *h && n + 1 < sizeof expanded; h++) expanded[n++] = *h;
            p++;
        } else {
            expanded[n++] = *p;
        }
    }
    expanded[n] = '\0';
    if (expanded[0] == '/') {
        strlcpy(path, expanded, sizeof path);
    } else {
        /* Relative to the directory of the file that says it (sudo 1.9.1+). */
        strlcpy(path, curfile, sizeof path);
        char *sl = strrchr(path, '/');
        if (sl) sl[1] = '\0'; else path[0] = '\0';
        strlcat(path, expanded, sizeof path);
    }
    if (!dir) return s_load(c->s, path);

    long fd = lp_open(path, O_RDONLY | O_CLOEXEC | A_O_DIRECTORY, 0);
    if (fd < 0) return true;             /* no such directory: nothing to include */
    char *names[256];
    int count = 0;
    u8 buf[4096];
    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0) break;
        for (long off = 0; off < got; ) {
            u16 reclen = *(u16 *)(buf + off + 16);
            const char *name = (const char *)(buf + off + 19);
            off += reclen;
            size_t len = strlen(name);
            /* sudo's rule: skip names ending in '~' or containing '.',
             * which keeps editor backups and package manager leftovers
             * (foo.dpkg-old) from becoming policy. */
            if (!len || name[len - 1] == '~' || strchr(name, '.')) continue;
            if (count < 256) names[count++] = s_dup(name, len);
        }
    }
    lp_close((int)fd);
    for (int i = 1; i < count; i++)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char *t = names[j]; names[j] = names[j - 1]; names[j - 1] = t;
        }
    for (int i = 0; i < count; i++) {
        char full[768];
        snprintf(full, sizeof full, "%s/%s", path, names[i]);
        if (!s_load(c->s, full)) return false;
    }
    return true;
}

/* Parse text as a sudoers file named `file`. */
static bool s_parse_text(sudoers_t *s, const char *file, char *text)
{
    s_cur c;
    c.s = s;
    c.file = file;
    c.line = 0;
    char *p = text;
    int next_line = 1;
    while (*p) {
        /* Join continuation lines into one logical line. */
        char *start = p, *w = p;
        c.line = next_line;
        while (*p && *p != '\n') {
            if (*p == '\\' && p[1] == '\n') { p += 2; next_line++; continue; }
            *w++ = *p++;
        }
        if (*p == '\n') { p++; next_line++; }
        *w = '\0';

        /* Strip a comment: a '#' at the start of a token that is not a
         * number (#1000 is a uid) and not an #include directive. */
        char *q = start;
        while (*q == ' ' || *q == '\t') q++;
        bool directive = strncmp(q, "#include", 8) == 0 && (q[8] == ' ' || q[8] == '\t' ||
                         strncmp(q + 8, "dir", 3) == 0);
        if (!directive) {
            bool inq = false;
            for (char *r = start; *r; r++) {
                if (*r == '\\' && r[1]) { r++; continue; }
                if (*r == '"') inq = !inq;
                if (*r == '#' && !inq && (r == start || r[-1] == ' ' || r[-1] == '\t' ||
                    r[-1] == ',' || r[-1] == '=' || r[-1] == '(' || r[-1] == ':' || r[-1] == '!') &&
                    !(r[1] >= '0' && r[1] <= '9')) {
                    *r = '\0';
                    break;
                }
            }
        }
        /* trim */
        size_t len = strlen(q);
        while (len && (q[len - 1] == ' ' || q[len - 1] == '\t' || q[len - 1] == '\r')) q[--len] = '\0';
        if (!*q) continue;
        c.p = q;

        if (strncmp(q, "@includedir", 11) == 0 || strncmp(q, "#includedir", 11) == 0) {
            c.p = q + 11;
            s_ws(&c);
            if (!*c.p) return s_fail(&c, "@includedir needs a directory");
            if (!s_include(&c, c.p, true, file)) return false;
        } else if (strncmp(q, "@include", 8) == 0 || strncmp(q, "#include", 8) == 0) {
            c.p = q + 8;
            s_ws(&c);
            if (!*c.p) return s_fail(&c, "@include needs a file");
            if (!s_include(&c, c.p, false, file)) return false;
        } else if (strncmp(q, "Defaults", 8) == 0) {
            if (!s_defaults_line(&c)) return false;
        } else if (strncmp(q, "User_Alias", 10) == 0 && (q[10] == ' ' || q[10] == '\t')) {
            c.p = q + 10; if (!s_alias_line(&c, SK_USER)) return false;
        } else if (strncmp(q, "Runas_Alias", 11) == 0 && (q[11] == ' ' || q[11] == '\t')) {
            c.p = q + 11; if (!s_alias_line(&c, SK_RUNAS)) return false;
        } else if (strncmp(q, "Host_Alias", 10) == 0 && (q[10] == ' ' || q[10] == '\t')) {
            c.p = q + 10; if (!s_alias_line(&c, SK_HOST)) return false;
        } else if (strncmp(q, "Cmnd_Alias", 10) == 0 && (q[10] == ' ' || q[10] == '\t')) {
            c.p = q + 10; if (!s_alias_line(&c, SK_CMND)) return false;
        } else if (strncmp(q, "Cmd_Alias", 9) == 0 && (q[9] == ' ' || q[9] == '\t')) {
            c.p = q + 9; if (!s_alias_line(&c, SK_CMND)) return false;
        } else {
            if (!s_userspec(&c)) return false;
        }
    }
    return true;
}

static bool s_load(sudoers_t *s, const char *path)
{
    if (++s->depth > 16) {
        snprintf(s->err, sizeof s->err, "%s: too many levels of includes", path);
        s->failed = true;
        return false;
    }
    long fd = lp_open(path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
        snprintf(s->err, sizeof s->err, "unable to open %s: %s", path, lp_strerror((int)fd));
        s->failed = true;
        return false;
    }
    a_stat_t st;
    if (a_fstat((int)fd, &st) < 0 || (st.mode & LP_S_IFMT) != LP_S_IFREG) {
        lp_close((int)fd);
        snprintf(s->err, sizeof s->err, "%s is not a regular file", path);
        s->failed = true;
        return false;
    }
    if (s->check_perms) {
        if (st.uid != 0) {
            lp_close((int)fd);
            snprintf(s->err, sizeof s->err, "%s is owned by uid %u, should be 0", path, st.uid);
            s->failed = true;
            return false;
        }
        if (st.mode & 022) {
            lp_close((int)fd);
            snprintf(s->err, sizeof s->err, "%s is %s writable", path,
                     (st.mode & 002) ? "world" : "group");
            s->failed = true;
            return false;
        }
    }
    if (st.size > (1 << 20)) {
        lp_close((int)fd);
        snprintf(s->err, sizeof s->err, "%s is too large", path);
        s->failed = true;
        return false;
    }
    char *text = malloc((size_t)st.size + 1);
    if (!text) lp_exit(1);
    long n = 0, r;
    while (n < (long)st.size && (r = lp_read((int)fd, text + n, (size_t)st.size - (size_t)n)) > 0)
        n += r;
    lp_close((int)fd);
    text[n] = '\0';
    if ((long)strlen(text) != n) {
        snprintf(s->err, sizeof s->err, "%s contains a NUL byte", path);
        s->failed = true;
        return false;
    }
    /* The file name outlives this call: rules point at it. */
    char *name = s_dup(path, strlen(path));
    bool ok = s_parse_text(s, name, text);
    s->depth--;
    return ok;
}

/* After everything is read: every alias used must be defined, with the
 * right kind, and no alias may contain itself. */
static bool s_check_refs_list(sudoers_t *s, s_member *m, int kind, const char *file, int line, int depth);

static bool s_check_alias(sudoers_t *s, const char *name, int kind, const char *file, int line, int depth)
{
    s_alias *a = s_find_alias(s, kind, name);
    if (!a) {
        snprintf(s->err, sizeof s->err, "%s:%d: alias \"%s\" is used but not defined",
                 file, line, name);
        s->failed = true;
        return false;
    }
    if (depth > 32) {
        snprintf(s->err, sizeof s->err, "%s:%d: alias \"%s\" refers to itself", a->file, a->line, name);
        s->failed = true;
        return false;
    }
    return s_check_refs_list(s, a->members, kind, a->file, a->line, depth + 1);
}

static bool s_check_refs_list(sudoers_t *s, s_member *m, int kind, const char *file, int line, int depth)
{
    for (; m; m = m->next)
        if (m->alias && !s_check_alias(s, m->name, kind == SK_GROUP ? SK_RUNAS : kind, file, line, depth))
            return false;
    return true;
}

static bool s_check_refs(sudoers_t *s)
{
    for (s_rule *r = s->rules; r; r = r->next) {
        if (!s_check_refs_list(s, r->users, SK_USER, r->file, r->line, 0) ||
            !s_check_refs_list(s, r->hosts, SK_HOST, r->file, r->line, 0) ||
            !s_check_refs_list(s, r->runas_users, SK_RUNAS, r->file, r->line, 0) ||
            !s_check_refs_list(s, r->runas_groups, SK_RUNAS, r->file, r->line, 0) ||
            !s_check_refs_list(s, r->cmnd, SK_CMND, r->file, r->line, 0))
            return false;
    }
    for (s_default *d = s->defs; d; d = d->next) {
        int kind = d->scope == ':' ? SK_USER : d->scope == '@' ? SK_HOST :
                   d->scope == '>' ? SK_RUNAS : d->scope == '!' ? SK_CMND : 0;
        if (kind && !s_check_refs_list(s, d->binding, kind, d->file, d->line, 0)) return false;
    }
    return true;
}

/* Read the policy from `path`. false, with s->err set, when it cannot be
 * used at all. */
static S_UNUSED bool sudoers_read(sudoers_t *s, const char *path)
{
    return s_load(s, path) && s_check_refs(s);
}

/* The policy used when there is no /etc/sudoers at all: Debian's
 * default, which is also what this system has always promised - root,
 * and members of group sudo, with their own password. */
static S_UNUSED const char s_builtin_policy[] =
    "Defaults env_reset\n"
    "Defaults secure_path=\"/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\"\n"
    "root ALL=(ALL:ALL) ALL\n"
    "%sudo ALL=(ALL:ALL) ALL\n";

/* ── Matching ─────────────────────────────────────────────────────── */

typedef struct {
    const char *user;            /* who is asking */
    uid_t uid;
    gid_t groups[256];
    int   ngroups;
    char  host[128], shorthost[128];
    const char *runas;           /* whom they want to be */
    uid_t runas_uid;
    gid_t runas_groups[256];
    int   nrunas_groups;
    const char *cmnd;            /* absolute path of the command; NULL for -l/-v */
    const char *args;            /* its arguments joined by single spaces */
    bool  cmnd_stat;
    u64   cmnd_dev, cmnd_ino;
} s_ctx;

static bool s_group_hit(const char *spec, const gid_t *g, int n)
{
    gid_t want;
    if (spec[0] == '#') {
        u32 id;
        if (!a_parse_id(spec + 1, &id)) return false;
        want = id;
    } else if (!a_getgr(spec, 0, &want, NULL, 0)) {
        return false;
    }
    for (int i = 0; i < n; i++) if (g[i] == want) return true;
    return false;
}

static bool s_stat_path(const char *path, u64 *dev, u64 *ino)
{
    long fd = lp_open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);
    if (fd < 0) return false;
    a_stat_t st;
    bool ok = a_fstat((int)fd, &st) == 0;
    lp_close((int)fd);
    if (ok) { *dev = st.dev; *ino = st.ino; }
    return ok;
}

static int s_match_list(sudoers_t *s, s_member *m, int kind, const s_ctx *x, int depth);

static int s_match_cmnd(const s_member *m, const s_ctx *x)
{
    const char *rule = m->name;
    size_t rl = strlen(rule);
    bool path_ok;
    if (rule[rl - 1] == '/') {
        /* A directory: any command directly inside it. */
        const char *slash = strrchr(x->cmnd, '/');
        size_t dl = (size_t)(slash - x->cmnd) + 1;
        path_ok = dl == rl && strncmp(x->cmnd, rule, rl) == 0;
        if (!path_ok && x->cmnd_stat) {
            char dir[1024];
            if (dl < sizeof dir) {
                memcpy(dir, x->cmnd, dl);
                dir[dl] = '\0';
                u64 d1, i1, d2, i2;
                path_ok = s_stat_path(dir, &d1, &i1) && s_stat_path(rule, &d2, &i2) &&
                          d1 == d2 && i1 == i2;
            }
        }
    } else if (s_has_glob(rule)) {
        path_ok = s_fnmatch(rule, x->cmnd, true);
    } else {
        path_ok = strcmp(rule, x->cmnd) == 0;
        if (!path_ok && x->cmnd_stat) {
            /* The same file by another name (/bin/ls and /usr/bin/ls on a
             * merged /usr), as sudo compares device and inode. */
            u64 d, i;
            path_ok = s_stat_path(rule, &d, &i) && d == x->cmnd_dev && i == x->cmnd_ino;
        }
    }
    if (!path_ok) return -1;
    if (m->args == NULL) return 1;
    if (m->args[0] == '\0') return x->args[0] == '\0' ? 1 : -1;
    return s_fnmatch(m->args, x->args, false) ? 1 : -1;
}

/* 1 matched, 0 matched and negated (denied), -1 not mentioned. */
static int s_match_one(sudoers_t *s, s_member *m, int kind, const s_ctx *x, int depth)
{
    int r = -1;
    if (m->all) {
        r = 1;
    } else if (m->alias) {
        s_alias *a = s_find_alias(s, kind == SK_GROUP ? SK_RUNAS : kind, m->name);
        r = (a && depth < 32) ? s_match_list(s, a->members, kind, x, depth + 1) : -1;
    } else {
        const char *n = m->name;
        switch (kind) {
        case SK_USER:
            if (n[0] == '%') r = s_group_hit(n + 1, x->groups, x->ngroups) ? 1 : -1;
            else if (n[0] == '#') { u32 id; r = (a_parse_id(n + 1, &id) && id == x->uid) ? 1 : -1; }
            else r = strcmp(n, x->user) == 0 ? 1 : -1;
            break;
        case SK_RUNAS:
            if (n[0] == '%') r = s_group_hit(n + 1, x->runas_groups, x->nrunas_groups) ? 1 : -1;
            else if (n[0] == '#') { u32 id; r = (a_parse_id(n + 1, &id) && id == x->runas_uid) ? 1 : -1; }
            else r = strcmp(n, x->runas) == 0 ? 1 : -1;
            break;
        case SK_GROUP:
            r = -1;                      /* -g is not offered, so no group is ever asked for */
            break;
        case SK_HOST:
            if (s_has_glob(n)) r = (s_fnmatch(n, x->host, false) || s_fnmatch(n, x->shorthost, false)) ? 1 : -1;
            else r = (s_streq_ci(n, x->host) || s_streq_ci(n, x->shorthost)) ? 1 : -1;
            break;
        case SK_CMND:
            r = x->cmnd ? s_match_cmnd(m, x) : -1;
            break;
        }
    }
    if (r != -1 && m->neg) r = !r;
    return r;
}

static int s_match_list(sudoers_t *s, s_member *m, int kind, const s_ctx *x, int depth)
{
    int result = -1;
    for (; m; m = m->next) {
        int r = s_match_one(s, m, kind, x, depth);
        if (r != -1) result = r;
    }
    return result;
}

/* Does the rule's runas part allow x->runas? */
static bool s_runas_ok(sudoers_t *s, const s_rule *r, const s_ctx *x, const char *runas_default)
{
    if (!r->runas_given) return strcmp(x->runas, runas_default) == 0;
    if (!r->runas_users) return x->runas_uid == x->uid;   /* "(:group)" or "()" */
    return s_match_list(s, r->runas_users, SK_RUNAS, x, 0) == 1;
}

static bool s_rule_applies(sudoers_t *s, const s_rule *r, const s_ctx *x)
{
    return s_match_list(s, r->users, SK_USER, x, 0) == 1 &&
           s_match_list(s, r->hosts, SK_HOST, x, 0) == 1;
}

/* The command's verdict: the last matching rule's. */
typedef struct {
    int   verdict;               /* 1 allowed, 0 denied by a rule, -1 nothing matched */
    bool  user_listed;           /* some rule names this user on this host */
    const s_rule *rule;
} s_verdict;

static S_UNUSED s_verdict sudoers_check(sudoers_t *s, const s_ctx *x, const char *runas_default)
{
    s_verdict v = { -1, false, NULL };
    for (const s_rule *r = s->rules; r; r = r->next) {
        if (!s_rule_applies(s, r, x)) continue;
        v.user_listed = true;
        if (!x->cmnd || !s_runas_ok(s, r, x, runas_default)) continue;
        int m = s_match_list(s, r->cmnd, SK_CMND, x, 0);
        if (m != -1) { v.verdict = m; v.rule = m ? r : NULL; }
    }
    return v;
}

#endif /* LP_SUDOERS_H */
