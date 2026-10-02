/* visudo - edit /etc/sudoers without locking everybody out.
 *
 *   visudo            edit a copy, check it, install it only if it parses
 *   visudo -c         check the installed policy and its permissions
 *   visudo -f FILE    edit (or with -c, check) FILE instead
 *   visudo -q         quiet: only errors
 *
 * sudo refuses to run at all while /etc/sudoers does not parse - for
 * root too - so a typo saved straight into the file takes away the tool
 * that would fix it. visudo is the way around that: the edit happens in
 * /etc/sudoers.tmp (which doubles as the lock, created O_EXCL), the copy
 * is parsed with exactly the code sudo uses (sudo/sudoers.h), and only a
 * copy that parses replaces the real file - mode 0440, owner root,
 * fsync'd and renamed over it, so a power cut leaves the old policy or
 * the new one and never half of either.
 *
 * The editor is $SUDO_EDITOR, $VISUAL or $EDITOR, else the first of
 * editor, nano, vi and edit that exists.
 */
#include "../sudo/sudoers.h"

static const char *target = "/etc/sudoers";
static bool quiet;

static bool copy_file(const char *from, int to_fd)
{
    long fd = lp_open(from, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return false;
    char buf[8192];
    long n;
    bool ok = true;
    while ((n = lp_read((int)fd, buf, sizeof buf)) > 0)
        if (lp_write(to_fd, buf, (size_t)n) != n) { ok = false; break; }
    lp_close((int)fd);
    return ok && n == 0;
}

static bool same_contents(const char *a, const char *b)
{
    long la, lb;
    char *x = a_slurp(a, 1 << 20, &la), *y = a_slurp(b, 1 << 20, &lb);
    bool same = x && y && la == lb && memcmp(x, y, (size_t)la) == 0;
    free(x);
    free(y);
    return same;
}

/* Parse `path` as sudo would. Prints the error the way visudo does. */
static bool check(const char *path, const char *shown_as, bool perms)
{
    sudoers_t s;
    memset(&s, 0, sizeof s);
    s.check_perms = perms;
    s.quiet_warn = false;
    if (!sudoers_read(&s, path)) {
        /* Show the error against the name the user knows, not the temp file. */
        const char *e = s.err;
        size_t pl = strlen(path);
        if (strncmp(e, path, pl) == 0) {
            dprintf(2, ">>> %s%s <<<\n", shown_as, e + pl);
        } else {
            dprintf(2, ">>> %s <<<\n", e);
        }
        return false;
    }
    return true;
}

static int run_editor(const char *file)
{
    const char *ed = NULL;
    const char *vars[] = { "SUDO_EDITOR", "VISUAL", "EDITOR" };
    for (int i = 0; i < 3 && !ed; i++) {
        const char *v = getenv(vars[i]);
        if (v && *v) ed = v;
    }
    static const char *fallback[] = { "/usr/bin/editor", "/usr/bin/nano", "/bin/nano",
                                      "/usr/bin/vi", "/bin/vi", "/bin/edit", NULL };
    for (int i = 0; !ed && fallback[i]; i++) if (lp_exists(fallback[i])) ed = fallback[i];
    if (!ed) { dprintf(2, "visudo: no editor found (set EDITOR)\n"); return -1; }

    /* "vim -u NONE" is an editor with arguments: split on spaces. */
    char buf[512];
    strlcpy(buf, ed, sizeof buf);
    char *av[32];
    int n = 0;
    for (char *p = buf; *p && n < 30; ) {
        while (*p == ' ') *p++ = '\0';
        if (!*p) break;
        av[n++] = p;
        while (*p && *p != ' ') p++;
    }
    av[n++] = (char *)file;
    av[n] = NULL;
    char path[512];
    if (strchr(av[0], '/')) {
        strlcpy(path, av[0], sizeof path);
    } else {
        static const char *dirs[] = { "/usr/local/bin", "/usr/bin", "/bin", "/usr/sbin", "/sbin", NULL };
        path[0] = '\0';
        for (int i = 0; dirs[i]; i++) {
            char c[512];
            snprintf(c, sizeof c, "%s/%s", dirs[i], av[0]);
            if (lp_exists(c)) { strlcpy(path, c, sizeof path); break; }
        }
        if (!path[0]) { dprintf(2, "visudo: %s: command not found\n", av[0]); return -1; }
    }
    pid_t pid = lp_fork();
    if (pid == 0) {
        lp_execve(path, av, environ);
        dprintf(2, "visudo: unable to run %s\n", path);
        lp_exit(127);
    }
    int st = 0;
    if (pid < 0 || lp_waitpid(pid, &st, 0) < 0) return -1;
    return LP_WIFEXITED(st) ? LP_WEXITSTATUS(st) : -1;
}

/* Put the checked copy in place: owner root, mode 0440, durable. */
static bool install(const char *tmp)
{
    long fd = lp_open(tmp, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return false;
    a_fchown((int)fd, 0, 0);
    a_fchmod((int)fd, 0440);
    lp_fsync((int)fd);
    lp_close((int)fd);
    if (lp_rename(tmp, target) < 0) return false;
    char dir[512];
    strlcpy(dir, target, sizeof dir);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = '\0'; long dfd = lp_open(dir[0] ? dir : "/", O_RDONLY, 0); if (dfd >= 0) { lp_fsync((int)dfd); lp_close((int)dfd); } }
    return true;
}

int main(int argc, char **argv)
{
    bool check_only = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-c") || !strcmp(a, "--check")) check_only = true;
        else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) quiet = true;
        else if (!strcmp(a, "-s") || !strcmp(a, "--strict")) continue;
        else if ((!strcmp(a, "-f") || !strcmp(a, "--file")) && i + 1 < argc) target = argv[++i];
        else if (!strncmp(a, "--file=", 7)) target = a + 7;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: visudo [-chqs] [-f sudoers]\n\n"
                   "visudo - safely edit the sudoers file\n\n"
                   "Options:\n"
                   "  -c, --check              check-only mode\n"
                   "  -f, --file=sudoers       specify sudoers file location\n"
                   "  -h, --help               display help message and exit\n"
                   "  -q, --quiet              less verbose (quiet) syntax error messages\n"
                   "  -s, --strict             strict syntax checking (always on here)\n");
            return 0;
        } else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            printf("visudo version 1.9.13-lp\n");
            return 0;
        } else {
            dprintf(2, "usage: visudo [-chqs] [-f sudoers]\n");
            return 1;
        }
    }

    if (check_only) {
        bool is_default = strcmp(target, "/etc/sudoers") == 0;
        if (is_default) {
            lp_stat_t st;
            if (lp_stat(target, &st, true) == 0 && ((st.mode & 07777) != 0440 || st.uid != 0)) {
                dprintf(2, "%s: bad permissions, should be mode 0440 and owner root\n", target);
                return 1;
            }
        }
        if (!check(target, target, is_default)) {
            dprintf(2, "parse error in %s\n", target);
            return 1;
        }
        if (!quiet) printf("%s: parsed OK\n", target);
        return 0;
    }

    if (a_geteuid() != 0) {
        dprintf(2, "visudo: you must be root to edit %s (try: sudo visudo)\n", target);
        return 1;
    }
    a_umask(077);
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", target);
    long fd = lp_open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | A_O_NOFOLLOW, 0600);
    if (fd < 0) {
        dprintf(2, "visudo: %s busy, try again later\n", tmp);
        return 1;
    }
    bool existed = lp_exists(target);
    if (existed && !copy_file(target, (int)fd)) {
        lp_close((int)fd);
        lp_unlink(tmp);
        dprintf(2, "visudo: unable to read %s\n", target);
        return 1;
    }
    if (!existed) {
        const char *seed =
            "# /etc/sudoers - who may run what as whom. Edit with visudo.\n"
            "Defaults\tenv_reset\n"
            "Defaults\tsecure_path=\"/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\"\n\n"
            "root\tALL=(ALL:ALL) ALL\n"
            "%sudo\tALL=(ALL:ALL) ALL\n\n"
            "@includedir /etc/sudoers.d\n";
        lp_write((int)fd, seed, strlen(seed));
    }
    lp_close((int)fd);

    for (;;) {
        int r = run_editor(tmp);
        if (r != 0) {
            dprintf(2, "visudo: editor exited %s; %s unchanged\n", r < 0 ? "abnormally" : "with an error", target);
            lp_unlink(tmp);
            return 1;
        }
        if (existed && same_contents(tmp, target)) {
            if (!quiet) dprintf(2, "visudo: %s unchanged\n", tmp);
            lp_unlink(tmp);
            return 0;
        }
        if (check(tmp, target, false)) break;
        if (!lp_isatty(STDIN_FILENO)) {
            dprintf(2, "visudo: not saving %s; nothing was changed\n", target);
            lp_unlink(tmp);
            return 1;
        }
        dprintf(2, "What now? (e)dit again, e(x)it without saving: ");
        char ans[16];
        long n = lp_read(STDIN_FILENO, ans, sizeof ans - 1);
        if (n <= 0 || ans[0] != 'e') {
            lp_unlink(tmp);
            dprintf(2, "visudo: %s unchanged\n", target);
            return 1;
        }
    }
    if (!install(tmp)) {
        dprintf(2, "visudo: unable to install %s; it is unchanged\n", target);
        lp_unlink(tmp);
        return 1;
    }
    return 0;
}
