/* su - become another user, with THAT user's password.
 *
 *   su                     a shell as root
 *   su - [user]            a login shell: clean environment, their home
 *   su user -c 'command'   one command as them
 *   su -s /bin/sh user     a different shell (root, or a shell in /etc/shells)
 *   su -m user             keep the whole environment
 *
 * ── What changed, and why ──
 *
 * This used to ask for no password at all: nothing on the board checked
 * one, and the kernel alone kept users from becoming root. The desktop
 * is a different machine. Every account has a password, root's is
 * locked, and administration goes through sudo with the user's own
 * password. So su now does what util-linux su does:
 *
 *   - root may become anybody without a password;
 *   - anybody else types the password of the account they want to
 *     become (su proves you know THAT account's secret - sudo is the
 *     tool that asks for your own);
 *   - a locked or empty password can never be typed correctly, and su
 *     says so instead of prompting for it. For root, whose password is
 *     locked on purpose, the answer is the one thing that works:
 *     "root login is disabled; use sudo".
 *   - one attempt, then a two-second pause and "Authentication
 *     failure", as with pam_unix; every attempt is logged to
 *     /var/log/auth.log in the lines Debian's su writes.
 *
 * It is setuid root and shares lp-auth.h with sudo and login for
 * everything that touches a password or a uid.
 */
#include "../sudo/lp-auth.h"

#define ROOT_PATH "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
#define USER_PATH "/usr/local/bin:/usr/bin:/bin:/usr/local/games:/usr/games"

static void usage(int fd)
{
    dprintf(fd,
        "\nUsage:\n"
        " su [options] [-] [<user> [<argument>...]]\n\n"
        "Change the effective user ID and group ID to that of <user>.\n"
        "A mere - implies -l.  If <user> is not given, root is assumed.\n\n"
        "Options:\n"
        " -m, -p, --preserve-environment  do not reset environment variables\n"
        " -l, --login                     make the shell a login shell\n"
        " -c, --command <command>         pass a single command to the shell with -c\n"
        " -s, --shell <shell>             run <shell> if /etc/shells allows it\n\n"
        " -h, --help                      display this help\n"
        " -V, --version                   display version\n\n"
        "The password asked for is the one of <user>. The root account is\n"
        "locked on this system; use sudo, which asks for your own password.\n");
}

/* Is `shell` listed in /etc/shells? A user may pick their shell with -s
 * only from that list, as util-linux enforces for a restricted target. */
static bool listed_shell(const char *shell)
{
    char *buf = a_slurp("/etc/shells", 1 << 16, NULL);
    if (!buf) return false;
    bool ok = false;
    for (char *line = buf; line && *line && !ok; ) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (line[0] == '/' && strcmp(line, shell) == 0) ok = true;
        line = next;
    }
    free(buf);
    return ok;
}

int main(int argc, char **argv)
{
    bool login = false, preserve = false;
    const char *command = NULL, *shell_opt = NULL;
    int i = 1;
    /* Options before the user name; words after it go to the shell. */
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-") == 0) { login = true; continue; }
        if (a[0] != '-') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (!strcmp(a, "-l") || !strcmp(a, "--login")) login = true;
        else if (!strcmp(a, "-m") || !strcmp(a, "-p") || !strcmp(a, "--preserve-environment")) preserve = true;
        else if (!strcmp(a, "-c") || !strcmp(a, "--command") || !strncmp(a, "--command=", 10)) {
            if (a[9] == '=') command = a + 10;
            else if (i + 1 < argc) command = argv[++i];
            else { dprintf(2, "su: option requires an argument -- 'c'\n"); usage(2); return 1; }
        } else if (!strcmp(a, "-s") || !strcmp(a, "--shell") || !strncmp(a, "--shell=", 8)) {
            if (a[7] == '=') shell_opt = a + 8;
            else if (i + 1 < argc) shell_opt = argv[++i];
            else { dprintf(2, "su: option requires an argument -- 's'\n"); usage(2); return 1; }
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(1); return 0; }
        else if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("su from LP (util-linux compatible)\n"); return 0; }
        else if (a[1] == 'c' && a[2]) command = a + 2;
        else if (a[1] == 's' && a[2]) shell_opt = a + 2;
        else {
            dprintf(2, "su: invalid option -- '%s'\n", a + 1);
            dprintf(2, "Try 'su --help' for more information.\n");
            return 1;
        }
    }
    const char *who = "root";
    if (i < argc) who = argv[i++];
    char **extra = argv + i;
    int nextra = argc - i;

    if (a_geteuid() != 0) {
        dprintf(2, "su: /bin/su must be owned by root and have the setuid bit set\n");
        return 1;
    }
    a_umask(022);

    a_pw_t me, target;
    uid_t ruid = (uid_t)a_getuid();
    if (!a_getpw(NULL, ruid, &me)) {
        dprintf(2, "su: cannot determine your user name\n");
        return 1;
    }
    if (!a_getpw(who, 0, &target)) {
        dprintf(2, "su: user %s does not exist or the user entry does not contain all the "
                   "required fields\n", who);
        return 1;
    }

    a_tty_t tty;
    a_tty_info(&tty);
    char logmsg[512];

    if (ruid != 0) {
        int state = a_shadow_state(target.name);
        if (state == LP_SHADOW_LOCKED && target.uid == 0) {
            snprintf(logmsg, sizeof logmsg, "FAILED SU (to %s) %s on %s: root login is disabled",
                     target.name, me.name, tty.name);
            a_authlog("su", logmsg);
            dprintf(2, "su: root login is disabled; use sudo\n");
            return 1;
        }
        const char *why = state == LP_SHADOW_OK ? NULL : a_shadow_why(state);
        if (why) {
            snprintf(logmsg, sizeof logmsg, "FAILED SU (to %s) %s on %s: %s",
                     target.name, me.name, tty.name, why);
            a_authlog("su", logmsg);
            dprintf(2, "su: %s: %s\n", target.name, why);
            return 1;
        }
        char pw[LP_CRYPT6_PW_MAX];
        if (!a_read_password("Password: ", false, pw, sizeof pw)) {
            dprintf(2, "su: a terminal is required to read the password\n");
            return 1;
        }
        int r = lp_shadow_check(a_shadow_path(), target.name, pw);
        a_wipe(pw, sizeof pw);
        if (r != LP_SHADOW_OK) {
            snprintf(logmsg, sizeof logmsg,
                     "pam_unix(su:auth): authentication failure; logname=%s uid=%u euid=0 "
                     "tty=/dev/%s ruser=%s rhost=  user=%s",
                     me.name, (unsigned)me.uid, tty.name, me.name, target.name);
            a_authlog("su", logmsg);
            snprintf(logmsg, sizeof logmsg, "FAILED SU (to %s) %s on %s", target.name, me.name, tty.name);
            a_authlog("su", logmsg);
            lp_sleep_ms(2000);
            dprintf(2, "su: Authentication failure\n");
            return 1;
        }
    }
    snprintf(logmsg, sizeof logmsg, "(to %s) %s on %s", target.name, me.name, tty.name);
    a_authlog("su", logmsg);
    snprintf(logmsg, sizeof logmsg,
             "pam_unix(su:session): session opened for user %s(uid=%u) by %s(uid=%u)",
             target.name, (unsigned)target.uid, me.name, (unsigned)me.uid);
    a_authlog("su", logmsg);

    /* The shell: -s only when root asks or the target's own shell is a
     * normal one and the requested one is in /etc/shells. */
    const char *shell = target.shell;
    if (shell_opt) {
        if (ruid == 0 || (listed_shell(target.shell) && listed_shell(shell_opt)))
            shell = shell_opt;
        else
            dprintf(2, "su: using restricted shell %s\n", target.shell);
    }
    if (preserve && !login && !shell_opt) {
        const char *envsh = getenv("SHELL");
        if (envsh && envsh[0] == '/' && (ruid == 0 || listed_shell(target.shell))) shell = envsh;
    }

    /* The environment, util-linux's way, built as a new array rather
     * than by editing the inherited one. */
    char **env = calloc(512, sizeof(char *));
    if (!env) return 1;
    int ne = 0;
    for (char **e = environ; e && *e && ne < 500; e++) {
        bool keep = login ? (!strncmp(*e, "TERM=", 5) || !strncmp(*e, "COLORTERM=", 10))
                          : strncmp(*e, "TZDIR=", 6) != 0;
        if (keep) env[ne++] = *e;
    }
#define SETENV(name, value) do {                                            \
        size_t nl = strlen(name), l = nl + strlen(value) + 2;               \
        char *kv = malloc(l);                                               \
        if (!kv) return 1;                                                  \
        snprintf(kv, l, "%s=%s", name, value);                              \
        int at = ne;                                                        \
        for (int q = 0; q < ne; q++)                                        \
            if (!strncmp(env[q], name, nl) && env[q][nl] == '=') at = q;    \
        env[at] = kv;                                                       \
        if (at == ne && ne < 510) ne++;                                     \
    } while (0)
    if (login) SETENV("PATH", target.uid == 0 ? ROOT_PATH : USER_PATH);
    if (!preserve || login) {
        SETENV("HOME", target.home);
        SETENV("SHELL", shell);
        if (login || target.uid != 0) {
            SETENV("USER", target.name);
            SETENV("LOGNAME", target.name);
        }
    }
    env[ne] = NULL;

    gid_t groups[256];
    int ngroups = a_groups(target.name, target.gid, groups, 256);
    if (!a_become(target.uid, target.gid, groups, ngroups)) {
        dprintf(2, "su: cannot set user id\n");
        return 1;
    }
    if (login && lp_chdir(target.home) < 0) {
        dprintf(2, "su: warning: cannot change directory to %s\n", target.home);
        lp_chdir("/");
    }

    const char *base = strrchr(shell, '/');
    base = base ? base + 1 : shell;
    char arg0[128];
    snprintf(arg0, sizeof arg0, "%s%s", login ? "-" : "", base);
    char **av = calloc((size_t)nextra + 4, sizeof(char *));
    if (!av) return 1;
    int k = 0;
    av[k++] = arg0;
    if (command) { av[k++] = (char *)"-c"; av[k++] = (char *)command; }
    for (int j = 0; j < nextra; j++) av[k++] = extra[j];
    av[k] = NULL;
    for (int fd = 3; fd < 1024; fd++) lp_close(fd);
    long r = lp_execve(shell, av, env);
    dprintf(2, "su: failed to execute %s: %s\n", shell, lp_strerror((int)r));
    return 126;
}
