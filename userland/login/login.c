/* login - the console's front door: a user name, that user's password,
 * and then their shell.
 *
 *   login              ask for a user name, then the password
 *   login alice        ask only for alice's password
 *   login -h host      remember where the login came from (for wtmp)
 *
 * init runs this on the console in normal mode (in recovery mode it
 * runs its own authenticated root shell instead). On the boards it used
 * to go straight to a root shell: there was no password anywhere to
 * check. On the desktop every account has one, root's is locked, and
 * the console is the one place anybody with the machine in their hands
 * can type - so it asks, like every Linux console does.
 *
 * What it does, in order, and why that order:
 *
 *   1. /etc/issue, then "<host> login: " and "Password: " with the echo
 *      off. An unknown user is asked for a password too, and fails the
 *      same way, so the prompt does not tell a stranger which names
 *      exist. A locked or empty password never matches.
 *   2. After every third failure, three seconds of nothing. That is
 *      the whole of the brute-force defence a console needs: a person
 *      at the keyboard gets 60 guesses a minute at most.
 *   3. On success: the auth log, then utmp and wtmp (so who, last and
 *      "Last login:" know about it), the terminal handed to the user
 *      (mode 0620), groups, gid, uid - in that order, and checked.
 *   4. A fresh environment: TERM kept, HOME/SHELL/USER/LOGNAME/PATH/
 *      MAIL set, the language from /etc/default/locale and then the
 *      user's own ~/.config/locale.conf (Settings writes the per-user
 *      one; it applies at the next login, which is this).
 *   5. The message of the day (lp-motd) unless ~/.hushlogin exists,
 *      then the shell as a login shell ("-lpsh").
 */
#include "../sudo/lp-auth.h"
#include "wtmp.h"

#define ROOT_PATH "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
#define USER_PATH "/usr/local/bin:/usr/bin:/bin:/usr/local/games:/usr/games"

static char tty_line[64] = "console";     /* "tty1", "ttyS0", "pts/0" */
static char host[128];

/* The name of the terminal on standard input, from /proc/self/fd/0. */
static void find_tty(void)
{
    char buf[128];
    long n = lp_readlink("/proc/self/fd/0", buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = '\0';
    if (strncmp(buf, "/dev/", 5) == 0) strlcpy(tty_line, buf + 5, sizeof tty_line);
}

/* /etc/issue with agetty's common escapes: \n host, \l tty, \s system,
 * \r kernel release, \m machine, \\ a backslash. */
static void print_issue(void)
{
    char *text = a_slurp("/etc/issue", 8192, NULL);
    if (!text) return;
    u8 uts[390];
    memset(uts, 0, sizeof uts);
    lp_uname(uts);
    for (const char *p = text; *p; p++) {
        if (*p != '\\' || !p[1]) { putchar(*p); continue; }
        p++;
        switch (*p) {
        case 'n': printf("%s", host); break;
        case 'l': printf("%s", tty_line); break;
        case 's': printf("%s", (char *)uts); break;
        case 'r': printf("%s", (char *)uts + 130); break;
        case 'v': printf("%s", (char *)uts + 195); break;
        case 'm': printf("%s", (char *)uts + 260); break;
        case '\\': putchar('\\'); break;
        default: break;                   /* \e colour escapes and the like: dropped */
        }
    }
    free(text);
}

/* A line from the terminal with the echo on; false at end of input. */
static bool read_line(char *out, size_t cap)
{
    size_t n = 0;
    bool got = false;
    for (;;) {
        char c;
        long r = lp_read(STDIN_FILENO, &c, 1);
        if (r <= 0) return got && n > 0 ? (out[n] = '\0', true) : false;
        got = true;
        if (c == '\n' || c == '\r') break;
        if (n + 1 < cap) out[n++] = c;
    }
    out[n] = '\0';
    return true;
}

static bool valid_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 32 || s[0] == '-') return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              *s == '_' || *s == '-' || *s == '.' || *s == '$'))
            return false;
    return true;
}

/* utmp: replace this terminal's slot, or add one; wtmp: append. */
static void write_records(const a_pw_t *pw)
{
    u8 rec[UT_RECLEN];
    memset(rec, 0, sizeof rec);
    short type = UT_USER_PROCESS;
    memcpy(rec + UT_TYPE, &type, sizeof type);
    s32 pid = (s32)lp_getpid();
    memcpy(rec + UT_PID, &pid, sizeof pid);
    strncpy((char *)rec + UT_LINE, tty_line, 32);
    const char *id = tty_line;
    size_t il = strlen(id);
    if (strncmp(id, "tty", 3) == 0) id += 3;
    else if (il > 4) id += il - 4;
    strncpy((char *)rec + UT_ID, id, 4);
    strncpy((char *)rec + UT_USER, pw->name, 32);
    strncpy((char *)rec + UT_HOST, host[0] ? host : "", 256);
    memcpy(rec + UT_SESSION, &pid, sizeof pid);
    long ns;
    s64 now = lp_time_ns(&ns);
    s32 sec = (s32)now, usec = (s32)(ns / 1000);
    memcpy(rec + UT_TV_SEC, &sec, sizeof sec);
    memcpy(rec + UT_TV_USEC, &usec, sizeof usec);

    long fd = lp_open(UTMP_PATH, O_RDWR | O_CREAT | O_CLOEXEC | A_O_NOFOLLOW, 0664);
    if (fd >= 0) {
        u8 old[UT_RECLEN];
        s64 at = -1, off = 0;
        while (lp_read((int)fd, old, sizeof old) == (long)sizeof old) {
            if (memcmp(old + UT_LINE, rec + UT_LINE, 32) == 0) { at = off; break; }
            off += UT_RECLEN;
        }
        lp_lseek((int)fd, at >= 0 ? at : off, 0);
        lp_write((int)fd, rec, sizeof rec);
        lp_close((int)fd);
    }
    fd = lp_open(WTMP_PATH, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | A_O_NOFOLLOW, 0664);
    if (fd >= 0) {
        lp_write((int)fd, rec, sizeof rec);
        lp_close((int)fd);
    }
}

/* KEY=value lines from a locale file; only the locale variables, and
 * only values that look like a locale name. */
static void read_locale(const char *path, char **env, int *ne, int cap)
{
    char *text = a_slurp(path, 8192, NULL);
    if (!text) return;
    for (char *line = text; line && *line; ) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        char *eq = strchr(line, '=');
        if (eq && (!strncmp(line, "LANG=", 5) || !strncmp(line, "LANGUAGE=", 9) ||
                   !strncmp(line, "LC_", 3))) {
            char *v = eq + 1;
            size_t vl = strlen(v);
            if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) { v[vl - 1] = '\0'; v++; }
            bool ok = *v != '\0';
            for (const char *q = v; *q; q++)
                if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') ||
                      *q == '_' || *q == '.' || *q == '@' || *q == ':' || *q == '-'))
                    ok = false;
            size_t nl = (size_t)(eq - line);
            if (ok) {
                size_t l = nl + strlen(v) + 2;
                char *kv = malloc(l);
                if (kv) {
                    snprintf(kv, l, "%.*s=%s", (int)nl, line, v);
                    int at = *ne;
                    for (int i = 0; i < *ne; i++)
                        if (!strncmp(env[i], line, nl) && env[i][nl] == '=') at = i;
                    if (at < cap - 1) { env[at] = kv; if (at == *ne) (*ne)++; }
                }
            }
        }
        line = next;
    }
    free(text);
}

static char *kv(const char *k, const char *v)
{
    size_t l = strlen(k) + strlen(v) + 2;
    char *s = malloc(l);
    if (s) snprintf(s, l, "%s=%s", k, v);
    return s;
}

int main(int argc, char **argv)
{
    const char *preset = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") && i + 1 < argc) strlcpy(host, argv[++i], sizeof host);
        else if (!strcmp(argv[i], "-p")) continue;
        else if (!strcmp(argv[i], "--help")) {
            printf("Usage: login [-h host] [username]\n\n"
                   "Log in on this terminal: a user name, that user's password, then\n"
                   "their shell. Run by init on the console.\n");
            return 0;
        } else if (argv[i][0] == '-') {
            dprintf(2, "login: unknown option %s\n", argv[i]);
            return 1;
        } else preset = argv[i];
    }
    if (a_geteuid() != 0) {
        dprintf(2, "login: Cannot possibly work without effective root\n");
        return 1;
    }
    a_umask(022);
    find_tty();
    char hostname[128];
    a_hostname(hostname, sizeof hostname);

    char user[64];
    int failures = 0;
    bool first = true;
    a_pw_t pw;
    for (;;) {
        if (preset && first) {
            strlcpy(user, preset, sizeof user);
        } else {
            if (first) print_issue();
            printf("\n%s login: ", hostname);
            if (!read_line(user, sizeof user)) return 0;   /* end of input: init starts us again */
        }
        first = false;
        char *u = user;
        while (*u == ' ' || *u == '\t') u++;
        size_t ul = strlen(u);
        while (ul && (u[ul - 1] == ' ' || u[ul - 1] == '\t')) u[--ul] = '\0';
        if (!*u) continue;

        char pwbuf[LP_CRYPT6_PW_MAX];
        if (!a_read_password("Password: ", false, pwbuf, sizeof pwbuf)) {
            /* No /dev/tty to turn the echo off on: read it plainly
             * rather than not at all (a serial line with no ctty). */
            if (!a_read_password("Password: ", true, pwbuf, sizeof pwbuf)) return 0;
        }
        bool known = valid_name(u) && a_getpw(u, 0, &pw);
        int state = known ? a_shadow_state(pw.name) : LP_SHADOW_NOUSER;
        int r = state == LP_SHADOW_OK ? lp_shadow_check(a_shadow_path(), pw.name, pwbuf) : state;
        a_wipe(pwbuf, sizeof pwbuf);
        if (r == LP_SHADOW_OK) break;

        failures++;
        char msg[256];
        snprintf(msg, sizeof msg, "FAILED LOGIN (%d) on '/dev/%s' FOR '%s', %s", failures,
                 tty_line, known ? pw.name : "UNKNOWN",
                 state == LP_SHADOW_OK ? "Authentication failure" : "Account locked or without password");
        a_authlog("login", msg);
        if (known && pw.uid == 0 && state == LP_SHADOW_LOCKED)
            printf("\nroot login is disabled; log in as your own user and use sudo\n");
        else
            printf("\nLogin incorrect\n");
        if (failures % 3 == 0) lp_sleep_ms(3000);
    }

    /* /etc/nologin keeps everybody but root out, and says why. */
    if (pw.uid != 0 && lp_exists("/etc/nologin")) {
        char *why = a_slurp("/etc/nologin", 4096, NULL);
        printf("%s", why && *why ? why : "System is going down or being maintained; logins are disabled.\n");
        lp_sleep_ms(3000);
        return 1;
    }

    char msg[256];
    snprintf(msg, sizeof msg, "pam_unix(login:session): session opened for user %s(uid=%u) by LOGIN(uid=0)",
             pw.name, (unsigned)pw.uid);
    a_authlog("login", msg);
    snprintf(msg, sizeof msg, "LOGIN ON %s BY %s%s%s", tty_line, pw.name, host[0] ? " FROM " : "", host);
    a_authlog("login", msg);

    wtmp_last_t last;
    bool have_last = wtmp_last(pw.name, &last);
    write_records(&pw);

    /* The terminal becomes the user's: they may write to it, their group
     * (tty, for `write` and `wall`) may too, nobody else may read it. */
    gid_t tty_gid = pw.gid;
    a_getgr("tty", 0, &tty_gid, NULL, 0);
    a_fchown(STDIN_FILENO, pw.uid, tty_gid);
    a_fchmod(STDIN_FILENO, 0620);

    /* The environment starts empty. */
    char *env[64];
    int ne = 0;
    const char *term = getenv("TERM");
    env[ne++] = kv("TERM", term && *term ? term : "linux");
    env[ne++] = kv("HOME", pw.home);
    env[ne++] = kv("SHELL", pw.shell);
    env[ne++] = kv("USER", pw.name);
    env[ne++] = kv("LOGNAME", pw.name);
    env[ne++] = kv("PATH", pw.uid == 0 ? ROOT_PATH : USER_PATH);
    char mail[128];
    snprintf(mail, sizeof mail, "/var/mail/%s", pw.name);
    env[ne++] = kv("MAIL", mail);
    read_locale("/etc/default/locale", env, &ne, 60);
    for (int i = 0; i < ne; i++) if (!env[i]) return 1;

    gid_t groups[256];
    int ngroups = a_groups(pw.name, pw.gid, groups, 256);
    if (!a_become(pw.uid, pw.gid, groups, ngroups)) {
        dprintf(2, "login: cannot set the user and group ids\n");
        return 1;
    }
    if (lp_chdir(pw.home) < 0) {
        printf("No directory, logging in with HOME=/\n");
        lp_chdir("/");
        env[1] = kv("HOME", "/");
    }
    /* The per-user language, read as the user, so a symlink in their
     * home cannot make root read something else. */
    if (lp_chdir(pw.home) == 0) read_locale(".config/locale.conf", env, &ne, 60);
    env[ne] = NULL;

    char hush[300];
    snprintf(hush, sizeof hush, "%s/.hushlogin", pw.home);
    if (!lp_exists(hush) && lp_exists("/bin/lp-motd")) {
        char lastarg[400];
        char *mav[4] = { (char *)"lp-motd", NULL, NULL, NULL };
        if (have_last) {
            snprintf(lastarg, sizeof lastarg, "%lld|%s|%s", (long long)last.when, last.line, last.host);
            mav[1] = (char *)"--last";
            mav[2] = lastarg;
        } else {
            mav[1] = (char *)"--first";
        }
        pid_t child = lp_fork();
        if (child == 0) {
            lp_execve("/bin/lp-motd", mav, env);
            lp_exit(127);
        }
        if (child > 0) { int st; lp_waitpid(child, &st, 0); }
    }

    const char *base = strrchr(pw.shell, '/');
    base = base ? base + 1 : pw.shell;
    char arg0[80];
    snprintf(arg0, sizeof arg0, "-%s", base);
    char *sav[2] = { arg0, NULL };
    lp_execve(pw.shell, sav, env);
    dprintf(2, "login: no shell: %s\n", pw.shell);
    return 1;
}
