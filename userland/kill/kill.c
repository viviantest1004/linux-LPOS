/* kill - stop a process.
 *
 *   kill <pid>...          ask it to stop (TERM)
 *   kill <name>...         the same, by program name - every match
 *   kill -9 <pid|name>...  force it (KILL)
 *   kill -l                list the signals we know
 *
 * TERM asks; a program can catch it, save its work and exit. KILL
 * cannot be caught, so anything unwritten is lost. Try TERM first.
 *
 * A name rather than a pid is what killall and pkill are for elsewhere.
 * Here it is the same command, because the only difference is how the
 * process was named, and two programs for that is one too many. A name
 * signals every process running under it and says how many that was -
 * silence would be the wrong answer when you expected one and got four.
 *
 * top can do this interactively - this is the same thing for scripts.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"

/* dirent, as getdents64 lays it out */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

typedef struct { int num; const char *name; const char *what; } sig_t;

static const sig_t SIGNALS[] = {
    {  1, "HUP",  "hung up - many daemons reread their config" },
    {  2, "INT",  "interrupt, same as Ctrl-C" },
    {  9, "KILL", "cannot be caught or ignored" },
    { 15, "TERM", "please stop (the default)" },
    { 18, "CONT", "continue a stopped process" },
    { 19, "STOP", "pause it, cannot be caught" },
    {  0, NULL, NULL }
};

static int signal_by_name(const char *s)
{
    if (*s >= '0' && *s <= '9')
        return atoi(s);
    if (strncmp(s, "SIG", 3) == 0)
        s += 3;
    for (int i = 0; SIGNALS[i].name; i++)
        if (strcmp(SIGNALS[i].name, s) == 0)
            return SIGNALS[i].num;
    return -1;
}

/* Signal every process running under this name.
 *
 * The name comes from /proc/<pid>/comm, which is the first fifteen
 * characters of the program name and nothing else - no path, no
 * arguments. That is what makes it safe to compare: a match cannot be
 * something that merely mentions the name on its command line.
 *
 * Returns how many were signalled, or -1 when none matched. Our own pid
 * and pid 1 are skipped - killing yourself halfway through the list
 * leaves the rest of it undone, and pid 1 takes the machine with it. */
static int kill_by_name(const char *name, int sig)
{
    long fd = lp_open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return -1;

    pid_t me = lp_getpid();
    char  buf[4096];
    int   hits = 0;

    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (long off = 0; off < n; ) {
            char *rec  = buf + off;
            u16   len  = *(u16 *)(rec + DIRENT_RECLEN);
            char *ent  = rec + DIRENT_NAME;
            if (len == 0)
                break;
            off += len;

            int pid = atoi(ent);
            if (pid <= 1 || pid == (int)me)
                continue;

            char path[64], comm[64];
            snprintf(path, sizeof path, "/proc/%d/comm", pid);
            if (proc_read(path, comm, sizeof comm) <= 0)
                continue;
            for (int j = 0; comm[j]; j++)
                if (comm[j] == '\n') { comm[j] = '\0'; break; }

            if (strcmp(comm, name) != 0)
                continue;
            if (lp_kill(pid, sig) >= 0)
                hits++;
        }
    }
    lp_close((int)fd);
    return hits ? hits : -1;
}

int main(int argc, char **argv)
{
    int sig   = 15;                 /* TERM */
    int first = 1;

    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        printf("signals:\n");
        for (int i = 0; SIGNALS[i].name; i++)
            printf("  %2d  %-5s %s\n",
                   SIGNALS[i].num, SIGNALS[i].name, SIGNALS[i].what);
        return 0;
    }

    if (argc > 1 && argv[1][0] == '-' && argv[1][1] != '\0') {
        sig = signal_by_name(argv[1] + 1);
        if (sig < 0) {
            dprintf(STDERR_FILENO,
                    "kill: unknown signal: %s   (kill -l lists them)\n",
                    argv[1] + 1);
            return 2;
        }
        first = 2;
    }

    if (first >= argc) {
        dprintf(STDERR_FILENO,
                "usage: kill [-signal] <pid|name>...\n"
                "       kill -l          list signals\n");
        return 2;
    }

    int rc = 0;
    for (int i = first; i < argc; i++) {
        int pid = atoi(argv[i]);
        if (pid <= 0) {
            /* Not a number, so treat it as a program name. */
            int hit = kill_by_name(argv[i], sig);
            if (hit < 0) {
                dprintf(STDERR_FILENO,
                        "kill: nothing called \"%s\" is running\n", argv[i]);
                rc = 1;
            } else {
                printf("kill: %s - %d process%s\n",
                       argv[i], hit, hit == 1 ? "" : "es");
            }
            continue;
        }
        /* Signalling pid 1 takes the machine down. reboot and poweroff
         * are the ways to do that on purpose. */
        if (pid == 1) {
            dprintf(STDERR_FILENO,
                    "kill: refusing pid 1 (init). Use reboot or poweroff.\n");
            rc = 1;
            continue;
        }
        long r = lp_kill(pid, sig);
        if (r < 0) {
            dprintf(STDERR_FILENO, "kill: %d: %s (%ld)\n", pid,
                    -r == 3 ? "no such process" :
                    -r == 1 ? "not permitted" : "failed", -r);
            rc = 1;
        }
    }
    return rc;
}
