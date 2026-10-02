/* sudo - run a command as root (or another user), after proving who you
 * are with your own password.
 *
 *   sudo command [args]      as root
 *   sudo -u user command     as someone else
 *   sudo -i [command]        root's login shell, in root's home
 *   sudo -s [command]        your $SHELL, as root
 *   sudo -l [command]        what you may run
 *   sudo -v                  ask now, so the next five minutes do not
 *   sudo -k / -K             forget the password on this terminal / everywhere
 *   sudo -E command          keep your environment (if sudoers allows it)
 *
 * This is the most security-sensitive program on the machine: it is
 * setuid root, and it turns a password typed by whoever is at the
 * keyboard into root. So it does what sudo(8) documents and nothing
 * cleverer, and where it has to choose it refuses:
 *
 *   - The policy is /etc/sudoers (and what it includes), in the subset
 *     sudoers.h describes. A file that does not parse, or is writable by
 *     anybody but root, stops sudo for everybody, root included - the
 *     same rule real sudo keeps, and why visudo checks before saving.
 *     With no /etc/sudoers at all the policy is Debian's default: root
 *     and group sudo, with their own password.
 *   - The password is the user's own (never the target's, never root's),
 *     checked against the shadow file with the libc's SHA-512 crypt. An
 *     empty or locked password is never accepted. Three tries, two
 *     seconds apart after a wrong one, the way pam_unix paces them.
 *   - A correct password is remembered for five minutes per terminal
 *     session in /run/sudo/ts, which only root can read. The record is
 *     tied to the terminal, the session and its leader's start time, and
 *     the boot, and measured on CLOCK_BOOTTIME - a timestamp on the wall
 *     clock could be kept alive for ever with `sudo date -s`.
 *   - PATH is never the user's. Commands are looked up in secure_path,
 *     which is always set (sudoers may change it, not remove it), and
 *     the command runs with that PATH.
 *   - The environment is rebuilt from a short list (env_reset); -E is
 *     refused unless the rule allows SETENV. LD_*, bash functions and
 *     their kind are dropped even then.
 *   - Everything is logged to /var/log/auth.log, in the shape Debian's
 *     sudo writes, and to the system log logd keeps.
 */
#include "sudoers.h"

#define SUDO_VERSION "1.9.13-lp"
#define DEFAULT_SECURE_PATH "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

/* ── Settings, after the Defaults lines are applied ─────────────────── */

#define MAXLIST 96
typedef struct { char *v[MAXLIST]; int n; } slist;

typedef struct {
    bool  env_reset, setenv, authenticate, requiretty, pwfeedback, shell_noargs;
    int   lecture;                 /* 0 never, 1 once, 2 always */
    s64   timeout_ms;              /* < 0 never expires; 0 always ask */
    long  tries, umask;
    char  secure_path[1024];
    char  passprompt[256];
    char  badpass[256];
    char  logfile[256];
    char  runas_default[64];
    slist env_keep, env_check, env_delete;
} cfg_t;

static void sl_add(slist *l, const char *w)
{
    for (int i = 0; i < l->n; i++) if (strcmp(l->v[i], w) == 0) return;
    if (l->n < MAXLIST) l->v[l->n++] = s_dup(w, strlen(w));
}

static void sl_del(slist *l, const char *w)
{
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->v[i], w) == 0) { l->v[i] = l->v[--l->n]; return; }
}

static void sl_words(slist *l, const char *s, int op)
{
    if (op == '=') l->n = 0;
    char w[256];
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        size_t n = 0;
        while (*s && *s != ' ' && *s != '\t') { if (n + 1 < sizeof w) w[n++] = *s; s++; }
        w[n] = '\0';
        if (n) { if (op == '-') sl_del(l, w); else sl_add(l, w); }
    }
}

static bool sl_match(const slist *l, const char *name, size_t nlen)
{
    for (int i = 0; i < l->n; i++) {
        const char *p = l->v[i];
        size_t pl = strlen(p);
        if (pl && p[pl - 1] == '*') {
            if (nlen >= pl - 1 && strncmp(name, p, pl - 1) == 0) return true;
        } else if (pl == nlen && strncmp(name, p, nlen) == 0) {
            return true;
        }
    }
    return false;
}

static void cfg_init(cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->env_reset = true;
    c->authenticate = true;
    c->lecture = 1;
    c->timeout_ms = 5 * 60000;
    c->tries = 3;
    c->umask = 022;
    strlcpy(c->secure_path, DEFAULT_SECURE_PATH, sizeof c->secure_path);
    strlcpy(c->passprompt, "[sudo] password for %p: ", sizeof c->passprompt);
    strlcpy(c->badpass, "Sorry, try again.", sizeof c->badpass);
    strlcpy(c->runas_default, "root", sizeof c->runas_default);
    sl_words(&c->env_check, "COLORTERM LANG LANGUAGE LC_* LINGUAS TERM TZ", '=');
    sl_words(&c->env_keep, "COLORS DISPLAY HOSTNAME KRB5CCNAME LS_COLORS PS1 PS2 "
             "XAUTHORITY XAUTHORIZATION XDG_CURRENT_DESKTOP", '=');
    sl_words(&c->env_delete,
             "IFS CDPATH LOCALDOMAIN RES_OPTIONS HOSTALIASES NLSPATH PATH_LOCALE LD_* _RLD* "
             "TERMINFO TERMINFO_DIRS TERMPATH TERMCAP ENV BASH_ENV PS4 GLOBIGNORE BASHOPTS "
             "SHELLOPTS JAVA_TOOL_OPTIONS PERLIO_DEBUG PERLLIB PERL5LIB PERL5OPT PERL5DB "
             "FPATH NULLCMD READNULLCMD ZDOTDIR TMPPREFIX PYTHONHOME PYTHONPATH PYTHONINSPECT "
             "PYTHONUSERBASE RUBYLIB RUBYOPT KRB5_CONFIG KRB5_KTNAME VAR_ACE USR_ACE DLC_ACE "
             "TZDIR SUDO_* GCONV_PATH MALLOC_* GLIBC_TUNABLES", '=');
}

static void cfg_apply(cfg_t *c, const s_default *d)
{
    const char *n = d->name, *v = d->value;
    bool on = d->op != 'F';
    if (!strcmp(n, "env_reset")) c->env_reset = on;
    else if (!strcmp(n, "setenv")) c->setenv = on;
    else if (!strcmp(n, "authenticate")) c->authenticate = on;
    else if (!strcmp(n, "requiretty")) c->requiretty = on;
    else if (!strcmp(n, "pwfeedback")) c->pwfeedback = on;
    else if (!strcmp(n, "shell_noargs")) c->shell_noargs = on;
    else if (!strcmp(n, "lecture"))
        c->lecture = d->op == 'F' ? 0 : d->op == 'T' ? 1 :
                     !strcmp(v, "always") ? 2 : !strcmp(v, "never") ? 0 : 1;
    else if (!strcmp(n, "timestamp_timeout")) {
        if (d->op == 'F') c->timeout_ms = 0;
        else s_parse_minutes_ms(v, &c->timeout_ms);
    } else if (!strcmp(n, "passwd_tries")) {
        if (d->op == '=') s_parse_int(v, 10, &c->tries);
    } else if (!strcmp(n, "umask")) {
        if (d->op == 'F') c->umask = 0;
        else s_parse_int(v, 8, &c->umask);
    } else if (!strcmp(n, "secure_path")) {
        /* "!secure_path" is not honoured: root never gets a PATH the user
         * chose. The default list stays. */
        if (d->op == '=' && *v) strlcpy(c->secure_path, v, sizeof c->secure_path);
    } else if (!strcmp(n, "passprompt")) {
        if (d->op == '=') strlcpy(c->passprompt, v, sizeof c->passprompt);
    } else if (!strcmp(n, "badpass_message")) {
        if (d->op == '=') strlcpy(c->badpass, v, sizeof c->badpass);
    } else if (!strcmp(n, "logfile")) {
        if (d->op == '=' && v[0] == '/') strlcpy(c->logfile, v, sizeof c->logfile);
        else if (d->op == 'F') c->logfile[0] = '\0';
    } else if (!strcmp(n, "runas_default")) {
        if (d->op == '=') strlcpy(c->runas_default, v, sizeof c->runas_default);
    } else if (!strcmp(n, "env_keep") || !strcmp(n, "env_check") || !strcmp(n, "env_delete")) {
        slist *l = !strcmp(n, "env_keep") ? &c->env_keep : !strcmp(n, "env_check") ? &c->env_check
                                                                                  : &c->env_delete;
        if (d->op == 'F') l->n = 0;
        else sl_words(l, v, d->op);
    }
}

/* Defaults in sudo's order of precedence: global, host, user, runas,
 * command; later wins. `pass` picks which scopes are ready to apply
 * (the command is not known when the first ones are needed). */
static void cfg_apply_scope(cfg_t *c, sudoers_t *s, const s_ctx *x, char scope)
{
    for (const s_default *d = s->defs; d; d = d->next) {
        if (d->scope != scope) continue;
        int kind = scope == ':' ? SK_USER : scope == '@' ? SK_HOST :
                   scope == '>' ? SK_RUNAS : scope == '!' ? SK_CMND : 0;
        if (kind && s_match_list(s, d->binding, kind, x, 0) != 1) continue;
        cfg_apply(c, d);
    }
}

/* ── The invocation ───────────────────────────────────────────────── */

static struct {
    bool login, shell, list, validate, kill, killall, preserve, stdin_pw, noninteractive;
    bool set_home;
    const char *runas, *prompt, *other_user, *preserve_list;
    int  list_count;
} opt;

static a_pw_t  me, target;
static s_ctx   ctx;
static cfg_t   cfg;
static sudoers_t pol;
static a_tty_t tty;
static char    cwd[1024];
static char  **user_env;           /* the environment as the user passed it */
static int     old_umask;

static void usage(int fd)
{
    dprintf(fd,
        "usage: sudo -h | -K | -k | -V\n"
        "usage: sudo -v [-knS] [-p prompt] [-u user]\n"
        "usage: sudo -l [-knS] [-p prompt] [-U user] [-u user] [command [arg ...]]\n"
        "usage: sudo [-EHknS] [-p prompt] [-u user] [VAR=value] [-i | -s] [command [arg ...]]\n");
}

static void help(void)
{
    printf("sudo - execute a command as another user\n\n");
    usage(1);
    printf("\nOptions:\n"
        "  -E, --preserve-env            preserve user environment when running command\n"
        "      --preserve-env=list       preserve specific environment variables\n"
        "  -H, --set-home                set HOME variable to target user's home dir\n"
        "  -h, --help                    display help message and exit\n"
        "  -i, --login                   run login shell as the target user; a command may\n"
        "                                also be specified\n"
        "  -K, --remove-timestamp        remove timestamp file completely\n"
        "  -k, --reset-timestamp         invalidate timestamp file\n"
        "  -l, --list                    list user's privileges or check a specific command;\n"
        "                                use twice for longer format\n"
        "  -n, --non-interactive         non-interactive mode, no prompts are used\n"
        "  -p, --prompt=prompt           use the specified password prompt\n"
        "  -S, --stdin                   read password from standard input\n"
        "  -s, --shell                   run shell as the target user; a command may also be\n"
        "                                specified\n"
        "  -U, --other-user=user         in list mode, display privileges for user\n"
        "  -u, --user=user               run command (or edit file) as specified user name\n"
        "                                or ID\n"
        "  -V, --version                 display version information and exit\n"
        "  -v, --validate                update user's timestamp without running a command\n"
        "  --                            stop processing command line arguments\n\n"
        "The policy is /etc/sudoers; edit it with visudo. The password asked for is\n"
        "your own, and it is remembered for 5 minutes on this terminal.\n");
}

static void die(const char *msg)
{
    dprintf(2, "sudo: %s\n", msg);
    lp_exit(1);
}

/* ── Logging ──────────────────────────────────────────────────────── */

static char log_cmd[4096];              /* "/usr/bin/apt install x", for the log */

static void sudo_log(const char *what)
{
    char msg[5120];
    snprintf(msg, sizeof msg, "%8s : %s%sTTY=%s ; PWD=%s ; USER=%s ; COMMAND=%s",
             me.name, what ? what : "", what ? " ; " : "", tty.name, cwd,
             ctx.runas ? ctx.runas : "root", log_cmd[0] ? log_cmd : "list");
    a_authlog("sudo", msg);
    if (cfg.logfile[0]) {
        long fd = lp_open(cfg.logfile, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | A_O_NOFOLLOW, 0600);
        if (fd >= 0) {
            char ts[64], line[5300];
            lp_tm_t tm;
            lp_localtime(lp_time(), &tm);
            lp_strftime(ts, sizeof ts, "%b %e %H:%M:%S", &tm);
            int n = snprintf(line, sizeof line, "%s : %s\n", ts, msg);
            if (n > 0) lp_write((int)fd, line, (size_t)(n < (int)sizeof line ? n : (int)sizeof line - 1));
            lp_close((int)fd);
        }
    }
}

/* ── Timestamps ───────────────────────────────────────────────────── */

#define TS_MAGIC 0x5354504cu              /* "LPTS" */

typedef struct {
    u32  magic, version, uid, kind;       /* kind 1 tty, 2 parent process */
    u64  tty, sid_start, ppid_start;
    s32  sid, ppid;
    s64  when_ms;
    char boot_id[40];
} ts_rec;

static char ts_dir[64];
static bool ts_usable;

/* A directory that must be root's and nobody else's: created if missing,
 * refused if it is anything else (a symlink, someone else's). */
static bool ts_secure_dir(const char *path, mode_t mode)
{
    lp_stat_t st;
    if (lp_stat(path, &st, false) < 0) {
        if (lp_mkdir(path, mode) < 0) return false;
        if (lp_stat(path, &st, false) < 0) return false;
    }
    if ((st.mode & LP_S_IFMT) != LP_S_IFDIR || st.uid != 0) {
        dprintf(2, "sudo: %s is not a directory owned by root; not using it\n", path);
        return false;
    }
    if ((st.mode & 07777) != mode) lp_chmod(path, mode);
    return true;
}

static void ts_setup(void)
{
    const char *base = lp_is_dir("/run") ? "/run/sudo" : "/var/run/sudo";
    snprintf(ts_dir, sizeof ts_dir, "%s/ts", base);
    ts_usable = ts_secure_dir(base, 0711) && ts_secure_dir(ts_dir, 0700);
}

static void boot_id(char *out, size_t cap)
{
    char b[64];
    memset(out, 0, cap);
    if (proc_read("/proc/sys/kernel/random/boot_id", b, sizeof b) > 0) {
        char *nl = strchr(b, '\n');
        if (nl) *nl = '\0';
        strlcpy(out, b, cap);
    }
}

static void ts_path(char *out, size_t cap)
{
    if (tty.tty) snprintf(out, cap, "%s/%u-t%llu", ts_dir, (unsigned)me.uid, (unsigned long long)tty.tty);
    else snprintf(out, cap, "%s/%u-p%d", ts_dir, (unsigned)me.uid, tty.ppid);
}

static void ts_fill(ts_rec *r)
{
    memset(r, 0, sizeof *r);
    r->magic = TS_MAGIC;
    r->version = 1;
    r->uid = me.uid;
    if (tty.tty) {
        r->kind = 1;
        r->tty = tty.tty;
        r->sid = tty.sid;
        r->sid_start = tty.sid_start;
    } else {
        r->kind = 2;
        r->ppid = tty.ppid;
        r->ppid_start = tty.ppid_start;
    }
    boot_id(r->boot_id, sizeof r->boot_id);
}

static bool ts_valid(void)
{
    if (!ts_usable || cfg.timeout_ms == 0) return false;
    char path[128];
    ts_path(path, sizeof path);
    long fd = lp_open(path, O_RDONLY | O_CLOEXEC | A_O_NOFOLLOW, 0);
    if (fd < 0) return false;
    a_stat_t st;
    ts_rec r, want;
    bool ok = a_fstat((int)fd, &st) == 0 && (st.mode & LP_S_IFMT) == LP_S_IFREG &&
              st.uid == 0 && (st.mode & 077) == 0 && st.size == sizeof r &&
              lp_read((int)fd, &r, sizeof r) == (long)sizeof r;
    lp_close((int)fd);
    if (!ok) return false;
    ts_fill(&want);
    if (r.magic != want.magic || r.version != want.version || r.uid != want.uid ||
        r.kind != want.kind || r.tty != want.tty || r.sid != want.sid ||
        r.sid_start != want.sid_start || r.ppid != want.ppid ||
        r.ppid_start != want.ppid_start || memcmp(r.boot_id, want.boot_id, sizeof r.boot_id))
        return false;
    s64 now = a_boottime_ms();
    if (now < 0 || r.when_ms > now) return false;
    return cfg.timeout_ms < 0 || now - r.when_ms < cfg.timeout_ms;
}

static void ts_update(void)
{
    if (!ts_usable || cfg.timeout_ms == 0) return;
    ts_rec r;
    ts_fill(&r);
    r.when_ms = a_boottime_ms();
    char path[128], tmp[160];
    ts_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s/.new-%d", ts_dir, (int)lp_getpid());
    lp_unlink(tmp);
    long fd = lp_open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | A_O_NOFOLLOW, 0600);
    if (fd < 0) return;
    bool ok = lp_write((int)fd, &r, sizeof r) == (long)sizeof r;
    lp_close((int)fd);
    if (!ok || lp_rename(tmp, path) < 0) lp_unlink(tmp);
}

/* -k: this terminal's record. -K: every record of this user. */
static void ts_remove(bool all)
{
    if (!ts_usable) return;
    if (!all) {
        char path[128];
        ts_path(path, sizeof path);
        lp_unlink(path);
        return;
    }
    char prefix[24];
    snprintf(prefix, sizeof prefix, "%u-", (unsigned)me.uid);
    long fd = lp_open(ts_dir, O_RDONLY | O_CLOEXEC | A_O_DIRECTORY, 0);
    if (fd < 0) return;
    u8 buf[4096];
    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0) break;
        for (long off = 0; off < got; ) {
            u16 reclen = *(u16 *)(buf + off + 16);
            const char *name = (const char *)(buf + off + 19);
            off += reclen;
            if (strncmp(name, prefix, strlen(prefix)) == 0) {
                char full[160];
                snprintf(full, sizeof full, "%s/%s", ts_dir, name);
                lp_unlink(full);
            }
        }
    }
    lp_close((int)fd);
}

/* ── Authentication ───────────────────────────────────────────────── */

static void lecture(void)
{
    if (cfg.lecture == 0) return;
    char path[160];
    snprintf(path, sizeof path, "/var/lib/sudo/lectured/%s", me.name);
    if (cfg.lecture == 1 && lp_exists(path)) return;
    dprintf(2, "\nWe trust you have received the usual lecture from the local System\n"
               "Administrator. It usually boils down to these three things:\n\n"
               "    #1) Respect the privacy of others.\n"
               "    #2) Think before you type.\n"
               "    #3) With great power comes great responsibility.\n\n");
    if (ts_secure_dir("/var/lib/sudo", 0711) && ts_secure_dir("/var/lib/sudo/lectured", 0700)) {
        long fd = lp_open(path, O_WRONLY | O_CREAT | O_CLOEXEC | A_O_NOFOLLOW, 0600);
        if (fd >= 0) lp_close((int)fd);
    }
}

static void expand_prompt(char *out, size_t cap)
{
    const char *src = opt.prompt ? opt.prompt : cfg.passprompt;
    char host[128];
    size_t n = 0;
    for (const char *p = src; *p && n + 1 < cap; p++) {
        const char *ins = NULL;
        if (*p == '%' && p[1]) {
            switch (p[1]) {
            case 'p': case 'u': ins = me.name; break;
            case 'U': ins = target.name; break;
            case 'h': strlcpy(host, ctx.shorthost, sizeof host); ins = host; break;
            case 'H': strlcpy(host, ctx.host, sizeof host); ins = host; break;
            case '%': ins = "%"; break;
            }
            if (ins) {
                p++;
                while (*ins && n + 1 < cap) out[n++] = *ins++;
                continue;
            }
        }
        out[n++] = *p;
    }
    out[n] = '\0';
}

/* true when the user proved it. Exits on the failures that are final. */
static bool authenticate(void)
{
    if (opt.noninteractive) {
        sudo_log("a password is required");
        die("a password is required");
    }
    if (!opt.stdin_pw) {
        long t = lp_open("/dev/tty", O_RDWR | O_CLOEXEC, 0);
        if (t < 0) {
            sudo_log("no tty present and no askpass program specified");
            dprintf(2, "sudo: a terminal is required to read the password; either use the -S "
                       "option to read from standard input or configure an askpass helper\n");
            die("a password is required");
        }
        lp_close((int)t);
    }
    lecture();
    char prompt[512];
    expand_prompt(prompt, sizeof prompt);
    const char *shadow = a_shadow_path();
    int wrong = 0;
    for (long attempt = 0; attempt < cfg.tries; attempt++) {
        char pw[LP_CRYPT6_PW_MAX];
        if (!a_read_password(prompt, opt.stdin_pw, pw, sizeof pw)) {
            a_wipe(pw, sizeof pw);
            if (wrong) {
                char m[96];
                snprintf(m, sizeof m, "%d incorrect password attempt%s", wrong, wrong == 1 ? "" : "s");
                sudo_log(m);
                dprintf(2, "sudo: %s\n", m);
                lp_exit(1);
            }
            die("no password was provided");
        }
        int r = lp_shadow_check(shadow, me.name, pw);
        a_wipe(pw, sizeof pw);
        if (r == LP_SHADOW_OK) {
            if (wrong) {
                char m[96];
                snprintf(m, sizeof m, "%d incorrect password attempt%s", wrong, wrong == 1 ? "" : "s");
                sudo_log(m);
            }
            return true;
        }
        const char *why = a_shadow_why(r);
        if (why) {
            char m[256];
            snprintf(m, sizeof m, "authentication failure: %s", why);
            sudo_log(m);
            dprintf(2, "sudo: %s\n", why);
            lp_exit(1);
        }
        wrong++;
        lp_sleep_ms(2000);
        if (attempt + 1 < cfg.tries) dprintf(2, "%s\n", cfg.badpass);
    }
    char m[96];
    snprintf(m, sizeof m, "%d incorrect password attempt%s", wrong, wrong == 1 ? "" : "s");
    sudo_log(m);
    dprintf(2, "sudo: %s\n", m);
    lp_exit(1);
}

/* Ask for the password unless something says it is not needed. */
static void check_user(bool nopasswd, bool may_use_ts, bool update_ts)
{
    if (me.uid == 0) return;
    if (!cfg.authenticate || nopasswd) return;
    if (cfg.requiretty && !tty.tty) {
        sudo_log("no tty");
        die("sorry, you must have a tty to run sudo");
    }
    if (may_use_ts && ts_valid()) {
        if (update_ts) ts_update();
        return;
    }
    authenticate();
    if (update_ts) ts_update();
}

/* ── Finding the command ──────────────────────────────────────────── */

static bool is_exec_file(const char *path, lp_stat_t *st)
{
    return lp_stat(path, st, true) == 0 && (st->mode & LP_S_IFMT) == LP_S_IFREG &&
           (st->mode & 0111);
}

/* Look `name` up in secure_path. Names with a '/' are taken as given,
 * made absolute against the current directory. */
static bool find_command(const char *name, char *out, size_t cap)
{
    lp_stat_t st;
    if (strchr(name, '/')) {
        if (name[0] == '/') strlcpy(out, name, cap);
        else snprintf(out, cap, "%s/%s", cwd, name);
        return is_exec_file(out, &st);
    }
    const char *p = cfg.secure_path;
    while (*p) {
        const char *e = strchr(p, ':');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len && p[0] == '/' && len + strlen(name) + 2 < cap) {
            memcpy(out, p, len);
            out[len] = '/';
            strlcpy(out + len + 1, name, cap - len - 1);
            if (is_exec_file(out, &st)) return true;
        }
        if (!e) break;
        p = e + 1;
    }
    return false;
}

/* sudo -s / -i with a command: the words joined for `sh -c`, with every
 * character that is not a letter, digit, '_', '-' or '$' escaped - the
 * same quoting sudo applies, so the shell sees the words it was given. */
static char *shell_quote(char **args, int n)
{
    size_t cap = 1;
    for (int i = 0; i < n; i++) cap += strlen(args[i]) * 2 + 1;
    char *out = malloc(cap);
    if (!out) die("out of memory");
    size_t k = 0;
    for (int i = 0; i < n; i++) {
        if (i) out[k++] = ' ';
        for (const char *p = args[i]; *p; p++) {
            char ch = *p;
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                  ch == '_' || ch == '-' || ch == '$' || (unsigned char)ch >= 0x80))
                out[k++] = '\\';
            out[k++] = ch;
        }
    }
    out[k] = '\0';
    return out;
}

static void join_args(char *out, size_t cap, char **args, int n)
{
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (i) strlcat(out, " ", cap);
        strlcat(out, args[i], cap);
    }
}

/* ── The environment ──────────────────────────────────────────────── */

typedef struct { char **v; int n, cap; } envb;

static void eb_set(envb *e, const char *name, const char *value)
{
    size_t nl = strlen(name);
    size_t len = nl + strlen(value) + 2;
    char *kv = malloc(len);
    if (!kv) die("out of memory");
    snprintf(kv, len, "%s=%s", name, value);
    for (int i = 0; i < e->n; i++)
        if (strncmp(e->v[i], name, nl) == 0 && e->v[i][nl] == '=') { e->v[i] = kv; return; }
    if (e->n + 2 > e->cap) {
        e->cap = e->cap ? e->cap * 2 : 64;
        char **nv = realloc(e->v, (size_t)e->cap * sizeof *nv);
        if (!nv) die("out of memory");
        e->v = nv;
    }
    e->v[e->n++] = kv;
    e->v[e->n] = NULL;
}

static void eb_put(envb *e, const char *kv)
{
    const char *eq = strchr(kv, '=');
    char name[256];
    size_t nl = (size_t)(eq - kv);
    if (nl >= sizeof name) return;
    memcpy(name, kv, nl);
    name[nl] = '\0';
    eb_set(e, name, eq + 1);
}

static const char *uenv(const char *name)
{
    size_t nl = strlen(name);
    for (char **e = user_env; e && *e; e++)
        if (strncmp(*e, name, nl) == 0 && (*e)[nl] == '=') return *e + nl + 1;
    return NULL;
}

/* TZ as sudo accepts it: a zone name or a file under the zone
 * directory, never "..", nothing unprintable. A TZ pointing anywhere
 * else would have a root program parse a file of the user's choosing. */
static bool tz_safe(const char *v)
{
    if (*v == ':') v++;
    if (*v == '/' && strncmp(v, "/usr/share/zoneinfo/", 20) != 0) return false;
    if (strstr(v, "..")) return false;
    if (strlen(v) >= 256) return false;
    for (; *v; v++) if ((unsigned char)*v <= ' ' || *v == 0x7f) return false;
    return true;
}

/* May this "NAME=value" reach the command? */
static bool env_ok(const char *kv, bool keep_mode)
{
    const char *eq = strchr(kv, '=');
    if (!eq || eq == kv) return false;
    size_t nl = (size_t)(eq - kv);
    const char *val = eq + 1;
    if (strncmp(val, "() ", 3) == 0 || strncmp(kv, "BASH_FUNC_", 10) == 0) return false;
    if (sl_match(&cfg.env_check, kv, nl)) {
        if (nl == 2 && strncmp(kv, "TZ", 2) == 0) return tz_safe(val);
        return !strchr(val, '/') && !strchr(val, '%');
    }
    if (keep_mode) return sl_match(&cfg.env_keep, kv, nl);
    return !sl_match(&cfg.env_delete, kv, nl);
}

static char **build_env(bool preserve, char **assign, int nassign, bool may_set)
{
    envb e = { NULL, 0, 0 };
    bool reset = cfg.env_reset && !preserve;
    for (char **p = user_env; p && *p; p++)
        if (strchr(*p, '=') && env_ok(*p, reset)) eb_put(&e, *p);

    if (reset || opt.login) {
        eb_set(&e, "HOME", target.home);
        eb_set(&e, "SHELL", target.shell);
        const char *ps1 = uenv("SUDO_PS1");
        if (ps1) eb_set(&e, "PS1", ps1);
        if (!uenv("TERM")) eb_set(&e, "TERM", "unknown");
    } else if (opt.set_home) {
        eb_set(&e, "HOME", target.home);
    }
    char mail[128];
    snprintf(mail, sizeof mail, "/var/mail/%s", target.name);
    eb_set(&e, "MAIL", mail);
    eb_set(&e, "LOGNAME", target.name);
    eb_set(&e, "USER", target.name);
    eb_set(&e, "PATH", cfg.secure_path);
    eb_set(&e, "SUDO_COMMAND", log_cmd);
    eb_set(&e, "SUDO_USER", me.name);
    char id[16];
    snprintf(id, sizeof id, "%u", (unsigned)me.uid);
    eb_set(&e, "SUDO_UID", id);
    snprintf(id, sizeof id, "%u", (unsigned)me.gid);
    eb_set(&e, "SUDO_GID", id);

    for (int i = 0; i < nassign; i++) {
        if (!may_set && !env_ok(assign[i], true)) {
            char *eq = strchr(assign[i], '=');
            dprintf(2, "sudo: sorry, you are not allowed to set the following environment "
                       "variables: %.*s\n", (int)(eq - assign[i]), assign[i]);
            lp_exit(1);
        }
        eb_put(&e, assign[i]);
    }
    if (!e.v) eb_set(&e, "PATH", cfg.secure_path);
    return e.v;
}

/* ── -l ───────────────────────────────────────────────────────────── */

static void print_member(const s_member *m)
{
    printf("%s%s", m->neg ? "!" : "", m->name);
    if (m->args) printf(" %s", m->args[0] ? m->args : "\"\"");
}

static void print_members(const s_member *m)
{
    for (bool first = true; m; m = m->next, first = false) {
        if (!first) printf(", ");
        print_member(m);
    }
}

static void print_value(const char *v)
{
    for (; *v; v++) {
        if (*v == ':' || *v == ',' || *v == '\\') putchar('\\');
        putchar(*v);
    }
}

static int do_list(char **cmdv, int cmdc)
{
    if (cmdc > 0) {
        /* sudo -l command: the full path when allowed, nothing and 1 when not. */
        return ctx.cmnd && sudoers_check(&pol, &ctx, cfg.runas_default).verdict == 1
               ? (printf("%s%s%s\n", ctx.cmnd, ctx.args[0] ? " " : "", ctx.args), 0) : 1;
    }
    (void)cmdv;
    bool any = false;
    printf("Matching Defaults entries for %s on %s:\n    ", ctx.user, ctx.shorthost);
    bool first = true;
    for (const s_default *d = pol.defs; d; d = d->next) {
        if (d->scope == '>' || d->scope == '!') continue;
        int kind = d->scope == ':' ? SK_USER : d->scope == '@' ? SK_HOST : 0;
        if (kind && s_match_list(&pol, d->binding, kind, &ctx, 0) != 1) continue;
        if (!first) printf(", ");
        first = false;
        if (d->op == 'F') printf("!%s", d->name);
        else if (d->op == 'T') printf("%s", d->name);
        else {
            printf("%s%s", d->name, d->op == '+' ? "+=" : d->op == '-' ? "-=" : "=");
            if (s_deftype(d->name) == 'l') { printf("\"%s\"", d->value); }
            else print_value(d->value);
        }
    }
    printf("\n\n");

    int last_priv = -1;
    const s_member *last_ru = NULL, *last_rg = NULL;
    bool last_given = false;
    int last_nopw = -2, last_se = -2;
    for (const s_rule *r = pol.rules; r; r = r->next) {
        if (!s_rule_applies(&pol, r, &ctx)) continue;
        if (!any) printf("User %s may run the following commands on %s:\n", ctx.user, ctx.shorthost);
        any = true;
        bool same = r->priv == last_priv && r->runas_users == last_ru &&
                    r->runas_groups == last_rg && r->runas_given == last_given;
        if (!same) {
            if (last_priv != -1) printf("\n");
            printf("    (");
            if (!r->runas_given) printf("%s", cfg.runas_default);
            else if (r->runas_users) print_members(r->runas_users);
            if (r->runas_groups) { printf(" : "); print_members(r->runas_groups); }
            printf(") ");
            last_nopw = last_se = -2;
        } else {
            printf(", ");
        }
        if (r->nopasswd != last_nopw && r->nopasswd != -1) printf("%s", r->nopasswd ? "NOPASSWD: " : "PASSWD: ");
        if (r->setenv != last_se && r->setenv != -1) printf("%s", r->setenv ? "SETENV: " : "NOSETENV: ");
        print_member(r->cmnd);
        last_priv = r->priv;
        last_ru = r->runas_users;
        last_rg = r->runas_groups;
        last_given = r->runas_given;
        last_nopw = r->nopasswd;
        last_se = r->setenv;
    }
    if (!any) {
        printf("User %s is not allowed to run sudo on %s.\n", ctx.user, ctx.shorthost);
        return 1;
    }
    printf("\n");
    return 0;
}

/* ── Main ─────────────────────────────────────────────────────────── */

static void load_user_groups(const a_pw_t *pw, gid_t *g, int *n)
{
    *n = a_groups(pw->name, pw->gid, g, 256);
}

static bool lookup_runas(const char *spec, a_pw_t *out)
{
    if (spec[0] == '#') {
        u32 id;
        return a_parse_id(spec + 1, &id) && a_getpw(NULL, id, out);
    }
    return a_getpw(spec, 0, out);
}

int main(int argc, char **argv)
{
    /* Keep the user's environment for building the command's, then
     * strip from our own the variables this libc would read as root. */
    {
        int n = 0;
        while (environ && environ[n]) n++;
        user_env = malloc(sizeof(char *) * (size_t)(n + 1));
        if (!user_env) return 1;
        for (int i = 0; i < n; i++) user_env[i] = s_dup(environ[i], strlen(environ[i]));
        user_env[n] = NULL;
        unsetenv("TZ");
        unsetenv("TZDIR");
        unsetenv("LC_ALL");
        unsetenv("LC_TIME");
        unsetenv("LANG");
    }

    /* Options end at the first word that is not one, or at "--", and
     * are never looked for after it: `sudo ls -l` must not read -l as
     * sudo's. Parsed here rather than by the libc's lp_getopt, which
     * permutes argv and, when "--" is present, leaves argc counting one
     * word more than it put back - a duplicated last argument, which
     * for this program would be a command run with words nobody typed. */
    static const struct { const char *name; int arg; char c; } lopts[] = {
        { "user", 1, 'u' }, { "login", 0, 'i' }, { "shell", 0, 's' }, { "list", 0, 'l' },
        { "validate", 0, 'v' }, { "reset-timestamp", 0, 'k' }, { "remove-timestamp", 0, 'K' },
        { "preserve-env", 2, 'E' }, { "stdin", 0, 'S' }, { "non-interactive", 0, 'n' },
        { "help", 0, 'h' }, { "version", 0, 'V' }, { "prompt", 1, 'p' },
        { "set-home", 0, 'H' }, { "other-user", 1, 'U' }, { "edit", 0, 'e' },
        { "group", 1, 'g' }, { "askpass", 0, 'A' }, { "background", 0, 'b' },
        { NULL, 0, 0 }
    };
    const char *shortargs = "upUg";          /* the letters that take a value */
    int ai = 1;
    while (ai < argc) {
        char *a = argv[ai];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { ai++; break; }
        char letters[64];
        const char *vals[64];
        int nl = 0;
        if (a[1] == '-') {
            const char *nm = a + 2, *eq = strchr(nm, '=');
            size_t len = eq ? (size_t)(eq - nm) : strlen(nm);
            int hit = -1, hits = 0;
            for (int k = 0; lopts[k].name; k++)
                if (strncmp(lopts[k].name, nm, len) == 0) {
                    if (strlen(lopts[k].name) == len) { hit = k; hits = 1; break; }
                    hit = k;
                    hits++;
                }
            if (hits != 1) {
                dprintf(2, "sudo: %s option '%s'\n", hits ? "ambiguous" : "unrecognized", a);
                usage(2);
                return 1;
            }
            const char *val = NULL;
            if (eq) {
                if (lopts[hit].arg == 0) {
                    dprintf(2, "sudo: option '--%s' doesn't allow an argument\n", lopts[hit].name);
                    return 1;
                }
                val = eq + 1;
            } else if (lopts[hit].arg == 1) {
                if (ai + 1 >= argc) {
                    dprintf(2, "sudo: option '--%s' requires an argument\n", lopts[hit].name);
                    usage(2);
                    return 1;
                }
                val = argv[++ai];
            }
            letters[nl] = lopts[hit].c;
            vals[nl++] = val;
        } else {
            for (const char *p = a + 1; *p && nl < 63; p++) {
                if (strchr(shortargs, *p)) {
                    const char *val = p[1] ? p + 1 : (ai + 1 < argc ? argv[++ai] : NULL);
                    if (!val) {
                        dprintf(2, "sudo: option requires an argument -- '%c'\n", *p);
                        usage(2);
                        return 1;
                    }
                    letters[nl] = *p;
                    vals[nl++] = val;
                    break;
                }
                letters[nl] = *p;
                vals[nl++] = NULL;
            }
        }
        ai++;
        for (int k = 0; k < nl; k++) {
            const char *arg = vals[k];
            switch (letters[k]) {
            case 'u': opt.runas = arg; break;
            case 'i': opt.login = true; break;
            case 's': opt.shell = true; break;
            case 'l': opt.list = true; opt.list_count++; break;
            case 'v': opt.validate = true; break;
            case 'k': opt.kill = true; break;
            case 'K': opt.killall = true; break;
            case 'E':
                if (arg) opt.preserve_list = arg; else opt.preserve = true;
                break;
            case 'S': opt.stdin_pw = true; break;
            case 'n': opt.noninteractive = true; break;
            case 'h': help(); return 0;
            case 'V':
                printf("Sudo version %s\nSudoers policy plugin version %s\n"
                       "Sudoers file grammar version 50 (the subset described in sudoers.h)\n",
                       SUDO_VERSION, SUDO_VERSION);
                return 0;
            case 'p': opt.prompt = arg; break;
            case 'H': opt.set_home = true; break;
            case 'U': opt.other_user = arg; break;
            case 'e': die("sudoedit is not supported here; run your editor through sudo instead");
            case 'g': die("-g is not supported by this sudo");
            case 'A': die("no askpass program specified; use -S to read the password from standard input");
            case 'b': die("-b is not supported by this sudo; run the command with & instead");
            default:
                dprintf(2, "sudo: invalid option -- '%c'\n", letters[k]);
                usage(2);
                return 1;
            }
        }
    }
    char **rest = argv + ai;
    int nrest = argc - ai;

    if (opt.login && opt.shell) die("you may not specify both the -i and -s options");
    if ((opt.list || opt.validate) && (opt.login || opt.shell || opt.preserve))
        die("the -l and -v options may not be combined with -i, -s or -E");
    if (opt.killall && (nrest > 0 || opt.list || opt.validate))
        die("the -K option may not be combined with other options or a command");
    if (opt.other_user && !opt.list) die("the -U option may only be used with the -l option");

    if (a_geteuid() != 0)
        die("/bin/sudo must be owned by uid 0 and have the setuid bit set");

    old_umask = (int)a_umask(022);
    lp_setrlimit(LP_RLIMIT_FSIZE, ~(u64)0, ~(u64)0);

    uid_t ruid = (uid_t)a_getuid();
    if (!a_getpw(NULL, ruid, &me)) {
        dprintf(2, "sudo: you do not exist in the passwd database\n");
        return 1;
    }
    a_tty_info(&tty);
    if (lp_getcwd(cwd, sizeof cwd) < 0) strlcpy(cwd, "unknown", sizeof cwd);
    ts_setup();

    if (opt.killall || (opt.kill && nrest == 0 && !opt.list && !opt.validate)) {
        ts_remove(opt.killall);
        return 0;
    }
    if (nrest == 0 && !opt.list && !opt.validate && !opt.login && !opt.shell) {
        usage(2);
        return 1;
    }

    /* The policy. */
    pol.check_perms = true;
    lp_stat_t sst;
    bool ok;
    if (lp_stat("/etc/sudoers", &sst, true) < 0) {
        char *text = s_dup(s_builtin_policy, sizeof s_builtin_policy - 1);
        ok = s_parse_text(&pol, "(built-in policy)", text) && s_check_refs(&pol);
    } else {
        ok = sudoers_read(&pol, "/etc/sudoers");
    }
    if (!ok) {
        dprintf(2, "sudo: %s\n", pol.err);
        dprintf(2, "sudo: no valid sudoers sources found, quitting\n");
        a_authlog("sudo", "unable to parse sudoers; refusing to run");
        return 1;
    }

    /* Who is asking, from where. */
    a_pw_t listed;
    ctx.user = me.name;
    ctx.uid = me.uid;
    load_user_groups(&me, ctx.groups, &ctx.ngroups);
    a_hostname(ctx.host, sizeof ctx.host);
    strlcpy(ctx.shorthost, ctx.host, sizeof ctx.shorthost);
    { char *dot = strchr(ctx.shorthost, '.'); if (dot) *dot = '\0'; }

    cfg_init(&cfg);
    cfg_apply_scope(&cfg, &pol, &ctx, 0);
    cfg_apply_scope(&cfg, &pol, &ctx, '@');
    cfg_apply_scope(&cfg, &pol, &ctx, ':');

    if (opt.other_user) {
        if (me.uid != 0) die("only root may list the privileges of another user");
        if (!a_getpw(opt.other_user, 0, &listed)) {
            dprintf(2, "sudo: unknown user %s\n", opt.other_user);
            return 1;
        }
        ctx.user = listed.name;
        ctx.uid = listed.uid;
        load_user_groups(&listed, ctx.groups, &ctx.ngroups);
    }

    /* Whom they want to be. */
    const char *runas = opt.runas ? opt.runas : cfg.runas_default;
    if (!lookup_runas(runas, &target)) {
        dprintf(2, "sudo: unknown user %s\n", runas);
        return 1;
    }
    ctx.runas = target.name;
    ctx.runas_uid = target.uid;
    load_user_groups(&target, ctx.runas_groups, &ctx.nrunas_groups);
    cfg_apply_scope(&cfg, &pol, &ctx, '>');

    /* VAR=value words before the command. */
    char *assign[64];
    int nassign = 0;
    while (nrest > 0 && !opt.list && strchr(rest[0], '=') && rest[0][0] != '=' && rest[0][0] != '/') {
        bool name_ok = true;
        for (const char *p = rest[0]; *p != '='; p++)
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_' ||
                  (p != rest[0] && *p >= '0' && *p <= '9'))) name_ok = false;
        if (!name_ok || nassign == 64) break;
        assign[nassign++] = rest[0];
        rest++;
        nrest--;
    }
    if (opt.preserve_list) {
        char buf[1024];
        strlcpy(buf, opt.preserve_list, sizeof buf);
        for (char *w = buf; w && *w && nassign < 64; ) {
            char *comma = strchr(w, ',');
            if (comma) *comma = '\0';
            const char *v = uenv(w);
            if (v) {
                size_t l = strlen(w) + strlen(v) + 2;
                char *kv = malloc(l);
                if (!kv) die("out of memory");
                snprintf(kv, l, "%s=%s", w, v);
                assign[nassign++] = kv;
            }
            w = comma ? comma + 1 : NULL;
        }
    }

    /* The command: what will be exec'd, and what the policy is asked about. */
    char path[1024];
    char **exec_argv = NULL;
    char argstr[4096];
    argstr[0] = '\0';
    bool shell_mode = opt.login || opt.shell || (nrest == 0 && cfg.shell_noargs && !opt.list && !opt.validate);
    if (shell_mode) {
        const char *sh;
        if (opt.login) sh = target.shell;
        else { sh = uenv("SHELL"); if (!sh || sh[0] != '/') sh = me.shell; }
        strlcpy(path, sh, sizeof path);
        lp_stat_t st;
        if (!is_exec_file(path, &st)) {
            dprintf(2, "sudo: %s: command not found\n", path);
            return 1;
        }
        exec_argv = calloc(4, sizeof(char *));
        if (!exec_argv) die("out of memory");
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (opt.login) {
            size_t l = strlen(base) + 2;
            char *a0 = malloc(l);
            if (!a0) die("out of memory");
            snprintf(a0, l, "-%s", base);
            exec_argv[0] = a0;
        } else {
            exec_argv[0] = s_dup(base, strlen(base));
        }
        if (nrest > 0) {
            char *q = shell_quote(rest, nrest);
            exec_argv[1] = (char *)"-c";
            exec_argv[2] = q;
            snprintf(argstr, sizeof argstr, "-c %s", q);
        }
        ctx.cmnd = path;
    } else if (nrest > 0) {
        if (!find_command(rest[0], path, sizeof path)) {
            dprintf(2, "sudo: %s: command not found\n", rest[0]);
            return 1;
        }
        join_args(argstr, sizeof argstr, rest + 1, nrest - 1);
        exec_argv = calloc((size_t)nrest + 1, sizeof(char *));
        if (!exec_argv) die("out of memory");
        for (int i = 0; i < nrest; i++) exec_argv[i] = rest[i];
        ctx.cmnd = path;
    }
    ctx.args = argstr;
    if (ctx.cmnd) {
        lp_stat_t st;
        if (lp_stat(ctx.cmnd, &st, true) == 0) {
            ctx.cmnd_stat = true;
            ctx.cmnd_dev = st.dev;
            ctx.cmnd_ino = st.ino;
        }
        snprintf(log_cmd, sizeof log_cmd, "%s%s%s", ctx.cmnd, argstr[0] ? " " : "", argstr);
        cfg_apply_scope(&cfg, &pol, &ctx, '!');
    }

    /* -l and -v. */
    if (opt.list || opt.validate) {
        bool any_nopw = false, all_nopw = true, listed_any = false;
        for (const s_rule *r = pol.rules; r; r = r->next) {
            if (!s_rule_applies(&pol, r, &ctx)) continue;
            listed_any = true;
            if (r->nopasswd == 1) any_nopw = true; else all_nopw = false;
        }
        if (opt.list) {
            check_user(any_nopw || opt.other_user, !opt.kill, !opt.kill);
            return do_list(rest, nrest);
        }
        check_user(listed_any && all_nopw, !opt.kill, true);
        if (!listed_any) {
            dprintf(2, "Sorry, user %s may not run sudo on %s.\n", me.name, ctx.shorthost);
            return 1;
        }
        return 0;
    }

    /* Run. */
    s_verdict v = sudoers_check(&pol, &ctx, cfg.runas_default);
    bool nopw = v.verdict == 1 && v.rule && v.rule->nopasswd == 1;
    bool self = target.uid == me.uid;
    check_user(nopw || self, !opt.kill, !opt.kill && !nopw);

    if (v.verdict != 1) {
        if (!v.user_listed) {
            sudo_log("user NOT in sudoers");
            dprintf(2, "%s is not in the sudoers file.  This incident will be reported.\n", me.name);
        } else {
            sudo_log("command not allowed");
            dprintf(2, "Sorry, user %s is not allowed to execute '%s' as %s on %s.\n",
                    me.name, log_cmd, target.name, ctx.shorthost);
        }
        return 1;
    }

    bool may_set = cfg.setenv || v.rule->setenv == 1 ||
                   (v.rule->setenv == -1 && v.rule->cmnd->all && !v.rule->cmnd->neg);
    if (opt.preserve && !may_set) {
        sudo_log("user not allowed to preserve the environment");
        die("sorry, you are not allowed to preserve the environment");
    }
    char **envp = build_env(opt.preserve, assign, nassign, may_set);

    sudo_log(NULL);

    gid_t groups[256];
    int ngroups = a_groups(target.name, target.gid, groups, 256);
    if (!a_become(target.uid, target.gid, groups, ngroups))
        die("unable to change to the target user and groups");
    a_umask((long)(old_umask | (int)cfg.umask));

    if (opt.login && lp_chdir(target.home) < 0)
        dprintf(2, "sudo: unable to change directory to %s\n", target.home);

    for (int fd = 3; fd < 1024; fd++) lp_close(fd);
    lp_signal_default(SIGINT);
    lp_signal_default(SIGQUIT);
    lp_signal_default(SIGTSTP);
    lp_signal_default(13);          /* SIGPIPE */

    long r = lp_execve(path, exec_argv, envp);
    if (r == -8) {                    /* ENOEXEC: a script with no #! line */
        char *shv[258];
        int k = 0;
        shv[k++] = (char *)"sh";
        shv[k++] = path;
        for (int i = 1; exec_argv[i] && k < 257; i++) shv[k++] = exec_argv[i];
        shv[k] = NULL;
        r = lp_execve("/bin/sh", shv, envp);
    }
    dprintf(2, "sudo: unable to execute %s: %s\n", path, lp_strerror((int)r));
    return 1;
}
