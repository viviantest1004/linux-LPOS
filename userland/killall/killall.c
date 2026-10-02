/* killall - signal every process with this exact name.
 *
 *   killall sshd            TERM to each of them
 *   killall -9 stuck        KILL instead
 *   killall -i firefox      ask before each one
 *   killall -w rsync        do not return until they are actually gone
 *
 * ── The one thing to get right: the match is exact ──
 * pkill matches a substring, killall matches the whole name. That is
 * the entire reason both commands exist, and it is not a detail:
 *
 *   pkill ssh     kills sshd, ssh-agent and the ssh you are typing into
 *   killall ssh   kills nothing unless a process is called exactly "ssh"
 *
 * Somebody who has learned "killall ssh is safe" on Ubuntu and finds a
 * killall here that quietly matched substrings would lose their SSH
 * daemon the first time they used it. So the default comparison is
 * strcmp against /proc/<pid>/comm and nothing else. -r asks for a
 * regular expression, explicitly, and then substrings are the point.
 *
 * ── Long names ──
 * The kernel only keeps 15 characters of a process name, so comm for a
 * longer program is truncated and a plain strcmp against the real name
 * can never succeed. When comm is 15 characters this reads
 * /proc/<pid>/cmdline and compares the basename of argv[0] instead,
 * which is the whole name again. A kernel thread has no cmdline, so
 * there the truncated 15 characters are all there is and the comparison
 * is a prefix - that is what -e refuses, saying which process it
 * skipped rather than silently not killing it.
 *
 * ── pid 1 ──
 * Unlike psmisc, this never signals pid 1. On this machine pid 1 is
 * init, and killing it is not "killing a process", it is stopping the
 * board - there is `poweroff` and `reboot` for that, and neither of
 * them is what somebody typing a program name meant. It says so rather
 * than skipping quietly.
 *
 * Not here: -g (process group), -y/-o (younger/older than), -n
 * (namespaces), -Z (SELinux context). Nothing on this system has an
 * SELinux context, and the other three have no user yet.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "regex.h"

/* The kernel's TASK_COMM_LEN. 15 characters plus the NUL. */
#define COMM_LEN      16
#define MAX_NAMES     64
#define MAX_PIDS      4096
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

static const char *prog = "killall";

static bool opt_exact, opt_icase, opt_inter, opt_quiet;
static bool opt_regex, opt_verbose, opt_wait;
static int  sig = SIGTERM;
static int  want_uid = -1;

/* Signal 1 to 31 on every architecture this system builds for. The
 * numbers differ on mips and alpha, which are not among them. */
static const char *const signame[] = {
    NULL,
    "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE",
    "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT",
    "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG", "XCPU",
    "XFSZ", "VTALRM", "PROF", "WINCH", "POLL", "PWR", "SYS"
};
#define NSIGNAMES ((int)(sizeof signame / sizeof signame[0]))

static int upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static bool same_fold(const char *a, const char *b)
{
    while (*a && *b) {
        if (upper((unsigned char)*a) != upper((unsigned char)*b))
            return false;
        a++; b++;
    }
    return *a == *b;
}

/* strncasecmp over n bytes, stopping at the first NUL in either. */
static bool same_fold_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (upper((unsigned char)a[i]) != upper((unsigned char)b[i]))
            return false;
        if (!a[i])
            return true;
    }
    return true;
}

/* "9", "KILL" and "SIGKILL" all mean 9. -1 when it is none of them.
 *
 * The comparison is case sensitive on purpose: psmisc rejects `-s term`
 * and accepts `-s TERM`, and a killall here that took both would teach
 * that `-s term` works. */
static int signal_by_name(const char *s)
{
    if (*s >= '0' && *s <= '9') {
        for (const char *p = s; *p; p++)
            if (*p < '0' || *p > '9')
                return -1;
        return atoi(s);
    }
    if (strncmp(s, "SIG", 3) == 0 && s[3])
        s += 3;
    for (int i = 1; i < NSIGNAMES; i++)
        if (strcmp(s, signame[i]) == 0)
            return i;
    return -1;
}

/* psmisc wraps this at 80 columns, and a learner comparing the two
 * outputs would notice the line break moving. */
static void list_signals(void)
{
    int col = 0;
    for (int i = 1; i < NSIGNAMES; i++) {
        int w = (int)strlen(signame[i]);
        if (col && col + 1 + w > 80) {
            printf("\n");
            col = 0;
        }
        printf("%s%s", col ? " " : "", signame[i]);
        col += (col ? 1 : 0) + w;
    }
    printf("\n");
}

static void usage(void)
{
    dprintf(STDERR_FILENO,
        "Usage: killall [OPTION]... [--] NAME...\n"
        "       killall -l, --list\n"
        "\n"
        "  -e,--exact          require exact match for very long names\n"
        "  -I,--ignore-case    case insensitive process name match\n"
        "  -i,--interactive    ask for confirmation before killing\n"
        "  -l,--list           list all known signal names\n"
        "  -q,--quiet          don't print complaints\n"
        "  -r,--regexp         interpret NAME as an extended regular expression\n"
        "  -s,--signal SIGNAL  send this signal instead of SIGTERM\n"
        "  -u,--user USER      kill only process(es) running as USER\n"
        "  -v,--verbose        report if the signal was successfully sent\n"
        "  -w,--wait           wait for processes to die\n"
        "\n");
}

/* /proc/<pid>/comm without its newline. false when the process is gone. */
static bool comm_of(pid_t pid, char *out, size_t n)
{
    char path[64], buf[256];
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
    long r = proc_read(path, buf, sizeof buf);
    if (r <= 0)
        return false;
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    strlcpy(out, buf, n);
    return out[0] != '\0';
}

/* The basename of argv[0]. This is what a name longer than 15
 * characters has to be compared against, comm having lost the rest. */
static bool longname_of(pid_t pid, char *out, size_t n)
{
    char path[64], buf[4096];
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    long r = proc_read(path, buf, sizeof buf);
    if (r <= 0)
        return false;
    buf[r < (long)sizeof buf ? r : (long)sizeof buf - 1] = '\0';
    char *slash = strrchr(buf, '/');
    strlcpy(out, slash ? slash + 1 : buf, n);
    return out[0] != '\0';
}

/* The real uid, field one of "Uid:" in /proc/<pid>/status. */
static int uid_of(pid_t pid)
{
    char path[64], buf[4096];
    snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    if (proc_read(path, buf, sizeof buf) <= 0)
        return -1;
    char *p = strstr(buf, "Uid:");
    if (!p)
        return -1;
    return (int)strtol(p + 4, NULL, 10);
}

static bool confirm(const char *name, pid_t pid)
{
    dprintf(STDERR_FILENO, "Kill %s(%d) ? (y/N) ", name, (int)pid);
    char answer[64];
    if (readline(STDIN_FILENO, answer, sizeof answer) < 0)
        return false;
    return answer[0] == 'y' || answer[0] == 'Y';
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "exact", 0, 'e' }, { "ignore-case", 0, 'I' },
        { "interactive", 0, 'i' }, { "list", 0, 'l' }, { "quiet", 0, 'q' },
        { "regexp", 0, 'r' }, { "signal", 1, 's' }, { "user", 1, 'u' },
        { "verbose", 0, 'v' }, { "wait", 0, 'w' }, { "help", 0, 'h' },
        { 0, 0, 0 }
    };
    static const char *const SHORT = "eIilqrs:u:vwh";

    /* killall -9, killall -KILL: the signal spelled as an option.
     *
     * Only an argument whose first letter is a digit or a capital is
     * considered, which is how psmisc tells the two cases apart: -Z is
     * a capital and comes out as "Z: unknown signal", -x is not and
     * comes out as the usage message. Getting that backwards would make
     * every typo an unknown-signal complaint.
     *
     * The argument is taken out of the list rather than replaced with a
     * harmless option: every option here means something. */
    char *args[256], *tail[MAX_NAMES];
    int   nargs = 0, ntail = 0;
    bool  past_dashdash = false;

    for (int i = 0; i < argc && nargs < 255; i++) {
        const char *a = argv[i];
        if (i == 0) { args[nargs++] = argv[i]; continue; }

        /* Everything after "--" is held here rather than handed to
         * lp_getopt. That shuffles argv without shrinking argc, so a
         * "--" in the line leaves the last operand in argv twice, and
         * killall would report it as not found twice. */
        if (past_dashdash) {
            if (ntail < MAX_NAMES) tail[ntail++] = argv[i];
            continue;
        }
        if (strcmp(a, "--") == 0) { past_dashdash = true; continue; }

        char c = a[0] == '-' ? a[1] : '\0';
        bool signalish = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
        if (!signalish || strchr(SHORT, c)) { args[nargs++] = argv[i]; continue; }

        int s = signal_by_name(a + 1);
        if (s < 0) {
            dprintf(STDERR_FILENO,
                    "%s: unknown signal; killall -l lists signals.\n", a + 1);
            return 1;
        }
        sig = s;
    }
    argc = nargs;
    argv = args;

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, SHORT, lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'e': opt_exact = true; break;
        case 'I': opt_icase = true; break;
        case 'i': opt_inter = true; break;
        case 'l': list_signals(); return 0;
        case 'q': opt_quiet = true; break;
        case 'r': opt_regex = true; break;
        case 'v': opt_verbose = true; break;
        case 'w': opt_wait = true; break;
        case 's':
            sig = signal_by_name(g.arg);
            if (sig < 0) {
                dprintf(STDERR_FILENO,
                        "%s: unknown signal; killall -l lists signals.\n",
                        g.arg);
                return 1;
            }
            break;
        case 'u': {
            /* A name, never a number: psmisc refuses `-u 0` and so does
             * this, or `killall -u 0` would mean two different things
             * on two machines. */
            lp_user_t u;
            if (!lp_user_by_name(g.arg, &u)) {
                dprintf(STDERR_FILENO, "Cannot find user %s\n", g.arg);
                return 1;
            }
            want_uid = (int)u.uid;
            break;
        }
        case 'h': usage(); return 1;
        default:  usage(); return 1;
        }
    }

    char *names[MAX_NAMES];
    int   nnames = 0;
    for (int i = g.ind; i < argc && nnames < MAX_NAMES; i++)
        names[nnames++] = argv[i];
    for (int i = 0; i < ntail && nnames < MAX_NAMES; i++)
        names[nnames++] = tail[i];

    if (nnames <= 0) {
        usage();
        return 1;
    }

    /* A regular expression is compiled once per name, before /proc is
     * walked: a bad pattern should be a complaint, not half a killing. */
    lpre *re[MAX_NAMES];
    for (int i = 0; i < nnames; i++) {
        re[i] = NULL;
        if (!opt_regex)
            continue;
        const char *err = NULL;
        re[i] = re_compile(names[i], true, opt_icase, &err);
        if (!re[i]) {
            /* psmisc prints its own name and the pattern, not the
             * matcher's complaint, so the two agree here. */
            (void)err;
            dprintf(STDERR_FILENO, "%s: Bad regular expression: %s\n",
                    prog, names[i]);
            return 1;
        }
    }

    long dir = lp_open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (dir < 0) {
        dprintf(STDERR_FILENO, "%s: cannot read /proc\n", prog);
        return 1;
    }

    pid_t me = lp_getpid();
    pid_t killed[MAX_PIDS];
    int   nkilled = 0;
    bool  hit[MAX_NAMES];
    bool  saw_init = false;
    for (int i = 0; i < nnames; i++)
        hit[i] = false;

    char dbuf[8192];
    for (;;) {
        long got = sys_getdents((int)dir, dbuf, sizeof dbuf);
        if (got <= 0)
            break;
        for (long off = 0; off < got && nkilled < MAX_PIDS; ) {
            char *rec = dbuf + off;
            u16 reclen = *(u16 *)(rec + DIRENT_RECLEN);
            char *ent = rec + DIRENT_NAME;
            if (reclen == 0)
                break;
            off += reclen;
            if (ent[0] < '1' || ent[0] > '9')
                continue;

            pid_t pid = (pid_t)atoi(ent);
            if (pid == me)
                continue;

            char comm[COMM_LEN + 8];
            if (!comm_of(pid, comm, sizeof comm))
                continue;

            /* comm is all the kernel kept. When it is full, the real
             * name may be longer, and only cmdline still has it. */
            char full[256];
            bool got_long = false;
            const char *subject = comm;
            if (strlen(comm) == COMM_LEN - 1 &&
                longname_of(pid, full, sizeof full)) {
                subject = full;
                got_long = true;
            }
            if (opt_exact && !got_long && strlen(comm) == COMM_LEN - 1) {
                if (opt_verbose)
                    dprintf(STDERR_FILENO,
                            "%s: skipping partial match %s(%d)\n",
                            prog, comm, (int)pid);
                continue;
            }

            for (int i = 0; i < nnames; i++) {
                bool match;
                if (opt_regex) {
                    int caps[RE_MAX_CAPS];
                    match = re_search(re[i], subject, 0, false, caps);
                } else if (got_long) {
                    match = opt_icase ? same_fold(names[i], subject)
                                      : strcmp(names[i], subject) == 0;
                } else {
                    /* Compare at most the 15 characters the kernel
                     * keeps, so a truncated name still matches its
                     * own process; both strings end inside that, so
                     * this is an exact comparison for ordinary names. */
                    match = opt_icase
                        ? same_fold_n(names[i], comm, COMM_LEN - 1)
                        : strncmp(names[i], comm, COMM_LEN - 1) == 0;
                }
                if (!match)
                    continue;

                if (want_uid >= 0 && uid_of(pid) != want_uid)
                    continue;

                if (pid == 1) {
                    saw_init = true;
                    continue;
                }
                if (opt_inter && !confirm(subject, pid))
                    continue;

                if (lp_kill(pid, sig) == 0) {
                    hit[i] = true;
                    killed[nkilled++] = pid;
                    if (opt_verbose)
                        dprintf(STDERR_FILENO,
                                "Killed %s(%d) with signal %d\n",
                                subject, (int)pid, sig);
                }
                break;                      /* one name per process */
            }
        }
    }
    lp_close((int)dir);

    if (saw_init && !opt_quiet)
        dprintf(STDERR_FILENO,
                "%s: pid 1 is init and is not signalled here - `poweroff`"
                " stops this machine\n", prog);

    int rc = 0;
    for (int i = 0; i < nnames; i++) {
        if (hit[i])
            continue;
        rc = 1;
        if (!opt_quiet)
            dprintf(STDERR_FILENO, "%s: no process found\n", names[i]);
    }

    /* -w: signal 0 asks whether the pid still exists without sending
     * anything, which is the only way to tell "it has exited" from "it
     * is ignoring TERM". A process that ignores it is waited for
     * forever, as psmisc does - the alternative is returning while the
     * thing you asked to be gone is still there. */
    while (opt_wait && nkilled) {
        int alive = 0;
        for (int i = 0; i < nkilled; i++)
            if (killed[i] && lp_kill(killed[i], 0) == 0)
                alive++;
            else
                killed[i] = 0;
        if (!alive)
            break;
        lp_sleep_ms(100);
    }

    return rc;
}
