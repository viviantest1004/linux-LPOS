/* timeout - run something, and stop it if it takes too long.
 *
 *   timeout 30 wget http://slow/thing
 *   timeout -s KILL 5 ./stuck
 *   timeout -k 5 30 ./polite      TERM, then KILL five seconds later
 *
 * On a machine with no operator sitting in front of it, this is what
 * stands between one wedged command and a service that never comes back.
 * Exit status 124 means it ran out of time, which is what scripts test.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define SIGTERM 15
#define SIGKILL  9
#define SIGINT   2
#define SIGHUP   1
#define SIGQUIT  3
#define SIGUSR1 10
#define SIGALRM 14

static const char *prog = "timeout";

static int signal_by_name(const char *s)
{
    if (s[0] >= '0' && s[0] <= '9') return atoi(s);
    if (strncmp(s, "SIG", 3) == 0) s += 3;
    if (strcmp(s, "TERM") == 0) return SIGTERM;
    if (strcmp(s, "KILL") == 0) return SIGKILL;
    if (strcmp(s, "INT")  == 0) return SIGINT;
    if (strcmp(s, "HUP")  == 0) return SIGHUP;
    if (strcmp(s, "QUIT") == 0) return SIGQUIT;
    if (strcmp(s, "USR1") == 0) return SIGUSR1;
    if (strcmp(s, "ALRM") == 0) return SIGALRM;
    return -1;
}

/* "30", "1.5", "2m", "1h", "10s" - seconds, as milliseconds. */
static long parse_duration(const char *s)
{
    char *end;
    long whole = strtol(s, &end, 10);
    long ms = whole * 1000;
    if (*end == '.') {
        end++;
        long scale = 100;
        while (*end >= '0' && *end <= '9' && scale > 0) {
            ms += (*end - '0') * scale;
            scale /= 10;
            end++;
        }
    }
    switch (*end) {
    case 'm': ms *= 60; break;
    case 'h': ms *= 3600; break;
    case 'd': ms *= 86400; break;
    default: break;
    }
    return ms;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "signal", 1, 's' }, { "kill-after", 1, 'k' },
        { "preserve-status", 0, 'p' }, { "foreground", 0, 'f' },
        { "verbose", 0, 'v' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    int  sig = SIGTERM;
    long kill_after = -1;
    bool preserve = false, verbose = false;

    lp_getopt_t g;
    lp_getopt_init_ex(&g, argc, argv, "s:k:pfv", lo, LP_GETOPT_STOP_AT_OPERAND);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 's': sig = signal_by_name(g.arg);
                  if (sig < 0) {
                      dprintf(STDERR_FILENO, "%s: '%s': invalid signal\n", prog, g.arg);
                      return 125;
                  }
                  break;
        case 'k': kill_after = parse_duration(g.arg); break;
        case 'p': preserve = true; break;
        case 'f': case 'v': verbose = true; break;
        case 'H':
            printf("Usage: timeout [OPTION] DURATION COMMAND [ARG]...\n"
                   "Start COMMAND, and kill it if still running after DURATION.\n\n"
                   "  -k, --kill-after=DURATION  also send a KILL signal after DURATION\n"
                   "  -s, --signal=SIGNAL        the signal to send on timeout (default TERM)\n"
                   "  -p, --preserve-status      exit with the same status as COMMAND\n"
                   "      --help     display this help and exit\n\n"
                   "DURATION is a number with an optional suffix: s, m, h, d.\n"
                   "Exit status 124 means the command timed out.\n");
            return 0;
        default: lp_getopt_err(prog, &g); return 125;
        }
    }

    if (argc - g.ind < 2) {
        dprintf(STDERR_FILENO, "%s: missing operand\n", prog);
        dprintf(STDERR_FILENO, "Try '%s --help' for more information.\n", prog);
        return 125;
    }

    long ms = parse_duration(argv[g.ind]);
    char **cmd = &argv[g.ind + 1];

    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "%s: cannot fork\n", prog);
        return 125;
    }
    if (pid == 0) {
        lp_execve(cmd[0], cmd, environ);
        /* Not an absolute path: try PATH the way a shell would. */
        if (!strchr(cmd[0], '/')) {
            const char *path = getenv("PATH");
            if (!path) path = "/bin:/sbin:/usr/bin:/usr/sbin";
            char full[1024];
            const char *p = path;
            while (*p) {
                const char *e = strchr(p, ':');
                size_t n = e ? (size_t)(e - p) : strlen(p);
                if (n && n < sizeof full - 2) {
                    memcpy(full, p, n);
                    full[n] = '/';
                    strlcpy(full + n + 1, cmd[0], sizeof full - n - 1);
                    lp_execve(full, cmd, environ);
                }
                if (!e) break;
                p = e + 1;
            }
        }
        dprintf(STDERR_FILENO, "%s: failed to run command '%s'\n", prog, cmd[0]);
        lp_exit(127);
    }

    /* Poll rather than use SIGALRM: there is no alarm() here, and a
     * 20ms tick costs nothing next to a command measured in seconds. */
    long waited = 0;
    int  status = 0;
    bool timed_out = false;

    for (;;) {
        pid_t r = lp_waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (waited >= ms) {
            if (verbose)
                dprintf(STDERR_FILENO, "%s: sending signal %d to command '%s'\n",
                        prog, sig, cmd[0]);
            lp_kill(pid, sig);
            timed_out = true;
            /* Give it a moment, then insist if -k said to. */
            long grace = 0;
            for (;;) {
                r = lp_waitpid(pid, &status, WNOHANG);
                if (r == pid) break;
                if (kill_after >= 0 && grace >= kill_after) {
                    lp_kill(pid, SIGKILL);
                    lp_waitpid(pid, &status, 0);
                    break;
                }
                if (kill_after < 0 && grace >= 5000) {
                    lp_waitpid(pid, &status, 0);
                    break;
                }
                lp_sleep_ms(20);
                grace += 20;
            }
            break;
        }
        lp_sleep_ms(20);
        waited += 20;
    }

    /* GNU signals the whole process group, so a KILL kills timeout too
     * and the caller sees 137 rather than 124. That difference is what
     * a script checks to tell "it would not stop" from "it ran long",
     * so it has to be the same here. */
    if (timed_out && !preserve && sig == SIGKILL)
        lp_kill(lp_getpid(), SIGKILL);
    if (timed_out && !preserve) return 124;
    if ((status & 0x7f) != 0) return 128 + (status & 0x7f);
    return (status >> 8) & 0xff;
}
