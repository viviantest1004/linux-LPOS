/* pgrep, pkill - find or signal processes by name.
 *
 *   pgrep sshd            the pids
 *   pgrep -l sshd         pid and name
 *   pgrep -f 'python m'   match the whole command line, not just the name
 *   pkill -HUP nginx      signal every one of them
 *
 * `ps | grep foo` is the usual substitute and it is wrong twice over:
 * the grep matches its own command line, and it matches any process
 * that merely mentions foo in an argument. This matches /proc/<pid>/comm
 * by default - the program name and nothing else - and skips itself.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "regex.h"
#include "syscall.h"

#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

#define SIGTERM 15

static const char *prog = "pgrep";
static bool killing = false;

static bool opt_full, opt_list, opt_newest, opt_oldest, opt_exact;
static bool opt_count, opt_invert, opt_ignore_case, opt_echo;
static int  sig = SIGTERM;
static int  want_uid = -1;
static pid_t only_parent = -1;

static int signal_by_name(const char *s)
{
    static const struct { const char *n; int v; } tab[] = {
        {"HUP",1},{"INT",2},{"QUIT",3},{"KILL",9},{"USR1",10},{"USR2",12},
        {"TERM",15},{"CONT",18},{"STOP",19},{"ALRM",14},{"PIPE",13},{0,0}
    };
    if (s[0] >= '0' && s[0] <= '9') return atoi(s);
    if (strncmp(s, "SIG", 3) == 0) s += 3;
    for (int i = 0; tab[i].n; i++)
        if (strcmp(s, tab[i].n) == 0) return tab[i].v;
    return -1;
}

static bool read_first_line(const char *path, char *out, size_t n)
{
    char buf[4096];
    long r = proc_read(path, buf, sizeof buf);
    if (r <= 0) return false;
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    strlcpy(out, buf, n);
    return true;
}

/* The command line, with the NUL separators turned into spaces. */
static bool cmdline_of(pid_t pid, char *out, size_t n)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    char buf[4096];
    long r = proc_read(path, buf, sizeof buf);
    if (r <= 0) return false;
    for (long i = 0; i < r - 1; i++)
        if (buf[i] == '\0') buf[i] = ' ';
    buf[r - 1 < (long)sizeof buf ? r - 1 : (long)sizeof buf - 1] = '\0';
    strlcpy(out, buf, n);
    return out[0] != '\0';
}

static int uid_of(pid_t pid)
{
    char path[64], buf[4096];
    snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    if (proc_read(path, buf, sizeof buf) <= 0) return -1;
    char *p = strstr(buf, "Uid:");
    if (!p) return -1;
    return (int)strtol(p + 4, NULL, 10);
}

static pid_t ppid_of(pid_t pid)
{
    pid_t ppid = 0, pgid, sid;
    int   tty;
    if (lp_proc_ids(pid, &ppid, &pgid, &sid, &tty))
        return ppid;
    return -1;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "full", 0, 'f' }, { "list-name", 0, 'l' }, { "list-full", 0, 'a' },
        { "newest", 0, 'n' }, { "oldest", 0, 'o' }, { "exact", 0, 'x' },
        { "count", 0, 'c' }, { "inverse", 0, 'v' }, { "ignore-case", 0, 'i' },
        { "signal", 1, 's' }, { "euid", 1, 'u' }, { "parent", 1, 'P' },
        { "echo", 0, 'e' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    prog = base;
    killing = (strcmp(base, "pkill") == 0);

    /* pkill -HUP name: the signal can be spelled as an option. */
    for (int i = 1; i < argc; i++) {
        if (!killing) break;
        if (argv[i][0] != '-' || argv[i][1] == '-' || !argv[i][1]) continue;
        int s = signal_by_name(argv[i] + 1);
        if (s > 0 && signal_by_name(argv[i] + 1) != -1 &&
            !strchr("flanoxcvisuPeH", argv[i][1])) {
            sig = s;
            argv[i] = (char *)"-q";        /* consumed; -q is a no-op here */
        }
    }

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "flanoxcvis:u:P:eq", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'f': opt_full = true; break;
        case 'l': opt_list = true; break;
        case 'a': opt_list = true; opt_full = true; break;
        case 'n': opt_newest = true; break;
        case 'o': opt_oldest = true; break;
        case 'x': opt_exact = true; break;
        case 'c': opt_count = true; break;
        case 'v': opt_invert = true; break;
        case 'i': opt_ignore_case = true; break;
        case 'e': opt_echo = true; break;
        case 'q': break;
        case 's': sig = signal_by_name(g.arg);
                  if (sig < 0) {
                      dprintf(STDERR_FILENO, "%s: unknown signal '%s'\n", prog, g.arg);
                      return 2;
                  }
                  break;
        case 'u': {
            lp_user_t u;
            if (lp_user_by_name(g.arg, &u)) want_uid = (int)u.uid;
            else want_uid = atoi(g.arg);
            break;
        }
        case 'P': only_parent = (pid_t)atoi(g.arg); break;
        case 'H':
            printf("Usage: %s [OPTION]... PATTERN\n"
                   "%s processes by name or other attributes.\n\n"
                   "  -f, --full           match against the full command line\n"
                   "  -l, --list-name      list PID and process name\n"
                   "  -a, --list-full      list PID and full command line\n"
                   "  -c, --count          count of matching processes\n"
                   "  -n, --newest         select the most recently started\n"
                   "  -o, --oldest         select the least recently started\n"
                   "  -x, --exact          match exactly, not as a substring\n"
                   "  -v, --inverse        negate the match\n"
                   "  -i, --ignore-case    match case insensitively\n"
                   "  -u, --euid=USER      only processes of this user\n"
                   "  -P, --parent=PPID    only children of this process\n"
                   "%s"
                   "      --help     display this help and exit\n",
                   prog, killing ? "Signal" : "Look up",
                   killing ? "  -SIGNAL, -s SIGNAL   the signal to send (default TERM)\n" : "");
            return 0;
        default: lp_getopt_err(prog, &g); return 2;
        }
    }

    if (g.ind >= argc) {
        dprintf(STDERR_FILENO, "%s: no matching criteria specified\n", prog);
        dprintf(STDERR_FILENO, "Try '%s --help' for more information.\n", prog);
        return 2;
    }

    const char *err = NULL;
    lpre *re = re_compile(argv[g.ind], true, opt_ignore_case, &err);
    if (!re) {
        dprintf(STDERR_FILENO, "%s: invalid pattern: %s\n", prog,
                err ? err : "cannot compile");
        return 2;
    }

    long fd = lp_open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "%s: cannot read /proc\n", prog);
        return 2;
    }

    pid_t me = lp_getpid();
    pid_t hits[4096];
    char  names[4096][64];
    int   n = 0;
    char  buf[8192];

    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0) break;
        for (long off = 0; off < got && n < 4096; ) {
            char *rec = buf + off;
            u16 len = *(u16 *)(rec + DIRENT_RECLEN);
            char *name = rec + DIRENT_NAME;
            if (len == 0) break;
            off += len;
            if (name[0] < '1' || name[0] > '9') continue;

            pid_t pid = (pid_t)atoi(name);
            if (pid == me) continue;

            char comm[64] = "", path[64];
            snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
            if (!read_first_line(path, comm, sizeof comm)) continue;

            char cmd[4096];
            const char *subject = comm;
            if (opt_full && cmdline_of(pid, cmd, sizeof cmd)) subject = cmd;

            int caps[RE_MAX_CAPS];
            bool hit;
            if (opt_exact)
                hit = re_search(re, subject, 0, false, caps) &&
                      caps[0] == 0 && caps[1] == (int)strlen(subject);
            else
                hit = re_search(re, subject, 0, false, caps);
            if (opt_invert) hit = !hit;
            if (!hit) continue;

            if (want_uid >= 0 && uid_of(pid) != want_uid) continue;
            if (only_parent >= 0 && ppid_of(pid) != only_parent) continue;

            hits[n] = pid;
            strlcpy(names[n], opt_full ? subject : comm, 64);
            n++;
        }
    }
    lp_close((int)fd);

    if (n == 0) return 1;

    int from = 0, to = n;
    if (opt_oldest) to = 1;
    if (opt_newest) from = n - 1;

    if (opt_count) { printf("%d\n", to - from); return 0; }

    int acted = 0;
    for (int i = from; i < to; i++) {
        if (killing) {
            if (lp_kill(hits[i], sig) == 0) acted++;
            if (opt_echo) printf("%s killed (pid %d)\n", names[i], (int)hits[i]);
        } else if (opt_list) {
            printf("%d %s\n", (int)hits[i], names[i]);
            acted++;
        } else {
            printf("%d\n", (int)hits[i]);
            acted++;
        }
    }
    return acted > 0 ? 0 : 1;
}
