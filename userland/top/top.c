/* top - what is running, what it is using, and how to stop it.
 *
 *   top              interactive, refreshing
 *   top -1           print once and exit (for scripts, or a slow link)
 *   top -n <count>   refresh <count> times, then exit (-n 1 = once)
 *   top -r 10        only the busiest ten
 *
 * Keys while running:
 *   q          quit                    r   refresh now
 *   m / c      sort by memory / CPU    p   sort by pid
 *   k          kill a process (asks for the pid, then TERM or KILL)
 *   h          help
 *
 * Where the numbers come from:
 *   /proc/<pid>/stat    utime+stime, in clock ticks since boot
 *   /proc/<pid>/statm   resident pages
 *   /proc/stat          total CPU ticks, for the percentage
 *
 * CPU percent is the difference between two samples, not an average
 * since boot - a process that burned the CPU an hour ago should not
 * still look busy. That means the first frame has nothing to compare
 * against and shows 0.0 for everything; the second is real.
 */
#include "types.h"
#include "osname.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

/* linux_dirent64 offsets (same as ls.c) */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

#define MAX_PROCS     512
#define REFRESH_MS    2000
#define PAGE_KB       4          /* arm64 with 4K pages */

/* Signals. We only need the two that matter for stopping something. */
#define SIGTERM 15
#define SIGKILL 9

typedef struct {
    int  pid;
    int  ppid;
    char name[32];
    char state;
    u64  cpu_ticks;      /* utime + stime */
    u64  prev_ticks;
    long rss_kb;
    u32    cpu_tenths;      /* percent times ten - see below */
} proc_t;

static proc_t procs[MAX_PROCS];
static int    nprocs = 0;

/* Previous sample, so we can show CPU over an interval. */
typedef struct { int pid; u64 ticks; } prev_t;
static prev_t prev[MAX_PROCS];
static int    nprev = 0;
static u64    prev_total = 0;

typedef enum { SORT_CPU, SORT_MEM, SORT_PID } sort_t;
static sort_t sort_by = SORT_CPU;
static int    limit   = 0;      /* 0 = as many as fit */

static lp_termios_t saved_term;
static bool         raw_mode = false;

/* ── reading /proc ─────────────────────────────────────────────── */

static long slurp(const char *path, char *buf, size_t cap)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return -1;
    long n = lp_read((int)fd, buf, cap - 1);
    lp_close((int)fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return n;
}

/* Total CPU ticks across all cores, from the first line of /proc/stat. */
static u64 read_total_ticks(void)
{
    char buf[512];
    if (slurp("/proc/stat", buf, sizeof(buf)) <= 0)
        return 0;

    /* "cpu  user nice system idle iowait irq softirq ..." */
    const char *p = buf;
    while (*p && *p != ' ') p++;
    u64 total = 0;
    for (;;) {
        while (*p == ' ') p++;
        if (*p < '0' || *p > '9')
            break;
        u64 v = 0;
        while (*p >= '0' && *p <= '9')
            v = v * 10 + (u64)(*p++ - '0');
        total += v;
    }
    return total;
}

static bool all_digits(const char *s)
{
    if (!*s) return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9')
            return false;
    return true;
}

/* /proc/<pid>/stat is one line. The name is in parentheses and may
 * itself contain spaces and parentheses, so we find the LAST ')'
 * rather than splitting on whitespace. */
static bool read_stat(int pid, proc_t *p)
{
    char path[64], buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (slurp(path, buf, sizeof(buf)) <= 0)
        return false;

    char *open_paren = strchr(buf, '(');
    if (!open_paren)
        return false;
    char *close_paren = NULL;
    for (char *c = buf; *c; c++)
        if (*c == ')')
            close_paren = c;
    if (!close_paren || close_paren < open_paren)
        return false;

    size_t len = (size_t)(close_paren - open_paren - 1);
    if (len >= sizeof(p->name))
        len = sizeof(p->name) - 1;
    memcpy(p->name, open_paren + 1, len);
    p->name[len] = '\0';

    /* Fields after ')': state ppid pgrp session tty tpgid flags
     * minflt cminflt majflt cmajflt utime stime ...
     * utime is field 14 overall, which is the 3rd here (1-based from
     * state). Count them off. */
    const char *f = close_paren + 1;
    int   idx = 2;              /* we are about to read field 3 (state) */
    char  state = '?';
    u64   utime = 0, stime = 0;
    int   ppid = 0;

    while (*f) {
        while (*f == ' ') f++;
        if (!*f) break;
        idx++;

        if (idx == 3) {
            state = *f;
        } else if (idx == 4) {
            ppid = atoi(f);
        } else if (idx == 14) {
            utime = 0;
            for (const char *c = f; *c >= '0' && *c <= '9'; c++)
                utime = utime * 10 + (u64)(*c - '0');
        } else if (idx == 15) {
            stime = 0;
            for (const char *c = f; *c >= '0' && *c <= '9'; c++)
                stime = stime * 10 + (u64)(*c - '0');
            break;
        }
        while (*f && *f != ' ') f++;
    }

    p->pid       = pid;
    p->ppid      = ppid;
    p->state     = state;
    p->cpu_ticks = utime + stime;
    return true;
}

/* Resident set size, in KB. statm's second field is resident pages. */
static long read_rss_kb(int pid)
{
    char path[64], buf[128];
    snprintf(path, sizeof(path), "/proc/%d/statm", pid);
    if (slurp(path, buf, sizeof(buf)) <= 0)
        return 0;

    const char *p = buf;
    while (*p && *p != ' ') p++;      /* skip total size */
    while (*p == ' ') p++;
    long pages = atoi(p);
    return pages * PAGE_KB;
}

static u64 lookup_prev(int pid)
{
    for (int i = 0; i < nprev; i++)
        if (prev[i].pid == pid)
            return prev[i].ticks;
    return 0;
}

static void scan_procs(void)
{
    u64 total = read_total_ticks();
    u64 delta_total = (total > prev_total) ? total - prev_total : 0;

    nprocs = 0;

    long fd = lp_open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;

    char dbuf[8192];
    for (;;) {
        long n = sys_getdents((int)fd, dbuf, sizeof(dbuf));
        if (n <= 0)
            break;

        for (long off = 0; off < n && nprocs < MAX_PROCS; ) {
            char       *rec  = dbuf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (!all_digits(name))
                continue;

            proc_t *p = &procs[nprocs];
            memset(p, 0, sizeof(*p));
            if (!read_stat(atoi(name), p))
                continue;
            p->rss_kb     = read_rss_kb(p->pid);
            p->prev_ticks = lookup_prev(p->pid);

            /* Percent of all CPU time in the interval. On a 4-core box
             * one fully busy thread shows 25%, which is what /proc's
             * numbers actually mean. */
            u64 used = (p->cpu_ticks > p->prev_ticks)
                       ? p->cpu_ticks - p->prev_ticks : 0;
            /* Percent to one decimal, in integers. This used to go
             * through a double for one number on the screen, which
             * pulled the whole floating-point runtime into a libc that
             * has none - and on ARMv6, where the conversion itself is a
             * library call, it did not link at all. used*1000 cannot
             * overflow here: these are tick counts over one interval. */
            p->cpu_tenths = (delta_total > 0)
                          ? (u32)(used * 1000 / delta_total) : 0;

            nprocs++;
        }
    }
    lp_close((int)fd);

    /* Remember this sample for the next frame. */
    nprev = (nprocs < MAX_PROCS) ? nprocs : MAX_PROCS;
    for (int i = 0; i < nprev; i++) {
        prev[i].pid   = procs[i].pid;
        prev[i].ticks = procs[i].cpu_ticks;
    }
    prev_total = total;
}

/* Insertion sort. A few hundred entries, once every two seconds -
 * anything cleverer would be for its own sake. */
static void sort_procs(void)
{
    for (int i = 1; i < nprocs; i++) {
        proc_t key = procs[i];
        int j = i - 1;
        while (j >= 0) {
            bool after;
            switch (sort_by) {
            case SORT_MEM: after = procs[j].rss_kb  < key.rss_kb;  break;
            case SORT_PID: after = procs[j].pid     > key.pid;     break;
            default:       after = procs[j].cpu_tenths < key.cpu_tenths; break;
            }
            if (!after) break;
            procs[j + 1] = procs[j];
            j--;
        }
        procs[j + 1] = key;
    }
}

/* ── the machine itself ────────────────────────────────────────── */

static long meminfo_kb(const char *key)
{
    char buf[2048];
    if (slurp("/proc/meminfo", buf, sizeof(buf)) <= 0)
        return 0;
    size_t klen = strlen(key);
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
            const char *v = line + klen + 1;
            while (*v == ' ') v++;
            long r = atoi(v);
            if (nl) *nl = '\n';
            return r;
        }
        if (!nl) break;
        *nl = '\n';
        line = nl + 1;
    }
    return 0;
}

static int count_cpus(void)
{
    char buf[8192];
    if (slurp("/proc/cpuinfo", buf, sizeof(buf)) <= 0)
        return 1;
    int n = 0;
    for (const char *p = buf; (p = strstr(p, "processor")); p += 9)
        n++;
    return n ? n : 1;
}

static void print_header(void)
{
    long total  = meminfo_kb("MemTotal");
    long avail  = meminfo_kb("MemAvailable");
    long used   = total - avail;
    long swtot  = meminfo_kb("SwapTotal");
    long swfree = meminfo_kb("SwapFree");

    char buf[256];
    long up = 0;
    if (slurp("/proc/uptime", buf, sizeof(buf)) > 0)
        up = atoi(buf);

    lp_tm_t tm;
    lp_gmtime(lp_time(), &tm);

    printf("\x1b[7m " LP_OS_NAME "  %d processes   %d cpu   "
           "up %ldd %ldh %ldm   %02d:%02d:%02d UTC \x1b[0m\n",
           nprocs, count_cpus(),
           up / 86400, (up % 86400) / 3600, (up % 3600) / 60,
           tm.hour, tm.min, tm.sec);

    printf(" memory  %ld MB total   %ld MB used (%ld%%)   %ld MB free\n",
           total / 1024, used / 1024,
           total ? used * 100 / total : 0, avail / 1024);

    if (swtot > 0)
        printf(" swap    %ld MB total   %ld MB used            (zram, compressed)\n",
               swtot / 1024, (swtot - swfree) / 1024);
    else
        printf(" swap    none\n");

    char load[128] = "";
    if (slurp("/proc/loadavg", load, sizeof(load)) > 0) {
        char *nl = strchr(load, '\n');
        if (nl) *nl = '\0';
        printf(" load    %s\n", load);
    }

    printf("\n\x1b[7m%7s %7s %6s %9s %s %s\x1b[0m\n",
           "PID", "PPID", "CPU%", "MEM", "S", "COMMAND");
}

static void print_procs(int rows)
{
    int shown = 0;
    int cap = limit ? limit : (rows > 0 ? rows : nprocs);

    for (int i = 0; i < nprocs && shown < cap; i++, shown++) {
        proc_t *p = &procs[i];

        char mem[16];
        if (p->rss_kb >= 1024)
            snprintf(mem, sizeof(mem), "%ld.%ld MB",
                     p->rss_kb / 1024, (p->rss_kb % 1024) * 10 / 1024);
        else
            snprintf(mem, sizeof(mem), "%ld KB", p->rss_kb);

        /* No %f in our printf, and no doubles anywhere: the value is
         * already tenths of a percent. */
        long whole = (long)(p->cpu_tenths / 10);
        long frac  = (long)(p->cpu_tenths % 10);

        printf("%7d %7d %4ld.%ld %9s %c %s\n",
               p->pid, p->ppid, whole, frac, mem, p->state, p->name);
    }
}

/* ── interactive ───────────────────────────────────────────────── */

static void restore_term(void)
{
    if (raw_mode) {
        lp_term_restore(STDIN_FILENO, &saved_term);
        raw_mode = false;
    }
}

/* Read a line while in raw mode - we have to echo it ourselves. */
static bool prompt_line(const char *msg, char *out, size_t cap)
{
    printf("\x1b[7m%s\x1b[0m", msg);

    size_t n = 0;
    for (;;) {
        char c;
        if (lp_read(STDIN_FILENO, &c, 1) != 1)
            return false;

        if (c == '\r' || c == '\n') {
            printf("\n");
            break;
        }
        if (c == 27) {                     /* ESC cancels */
            printf("\n");
            return false;
        }
        if ((c == 127 || c == 8) && n > 0) {
            n--;
            printf("\b \b");
            continue;
        }
        if (c >= 32 && c < 127 && n < cap - 1) {
            out[n++] = c;
            printf("%c", c);
        }
    }
    out[n] = '\0';
    return n > 0;
}

static const char *proc_name(int pid)
{
    for (int i = 0; i < nprocs; i++)
        if (procs[i].pid == pid)
            return procs[i].name;
    return NULL;
}

static void do_kill(void)
{
    char buf[32];
    if (!prompt_line(" pid to stop (ESC cancels): ", buf, sizeof(buf)))
        return;

    int pid = atoi(buf);
    if (pid <= 0) {
        printf(" not a pid\n");
        lp_sleep_ms(1200);
        return;
    }

    const char *name = proc_name(pid);
    if (!name) {
        printf(" no process %d\n", pid);
        lp_sleep_ms(1200);
        return;
    }

    /* Killing init takes the whole machine down with it. */
    if (pid == 1) {
        printf(" refusing: pid 1 is init. Use 'reboot' or 'poweroff'.\n");
        lp_sleep_ms(2000);
        return;
    }

    char q[128];
    snprintf(q, sizeof(q), " stop %d (%s)?  t=ask nicely  k=force  ESC=cancel: ",
             pid, name);
    printf("\x1b[7m%s\x1b[0m", q);

    char c;
    if (lp_read(STDIN_FILENO, &c, 1) != 1)
        return;
    printf("\n");

    int sig;
    if      (c == 't' || c == 'T') sig = SIGTERM;
    else if (c == 'k' || c == 'K') sig = SIGKILL;
    else return;

    long r = lp_kill(pid, sig);
    if (r < 0)
        printf(" could not signal %d (%ld)%s\n", pid, -r,
               -r == 1 ? " - not permitted" : "");
    else
        printf(" sent %s to %d (%s)\n",
               sig == SIGTERM ? "TERM" : "KILL", pid, name);
    lp_sleep_ms(1200);
}

static void show_help(void)
{
    printf("\x1b[2J\x1b[H");
    printf("top - what is running\n\n");
    printf("  q        quit\n");
    printf("  r        refresh now\n");
    printf("  c        sort by CPU\n");
    printf("  m        sort by memory\n");
    printf("  p        sort by pid\n");
    printf("  k        stop a process\n");
    printf("  h        this help\n\n");
    printf("CPU%% is measured between two refreshes, so the first\n");
    printf("frame shows 0.0 for everything. On a 4-core board one\n");
    printf("busy thread reads as 25%%.\n\n");
    printf("MEM is resident memory - what is actually in RAM.\n\n");
    printf("S is the process state:\n");
    printf("  R running   S sleeping   D waiting on disk\n");
    printf("  Z zombie    T stopped\n\n");
    printf("press any key...");
    char c;
    lp_read(STDIN_FILENO, &c, 1);
}

int main(int argc, char **argv)
{
    bool once = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-1") == 0) {
            once = true;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            /* -n is the number of REFRESHES, the way it is everywhere
             * else. It used to be the number of processes to show, so
             * `top -n 1` - which every other top on earth reads as
             * "print once and quit" - opened the full-screen display
             * and sat there waiting for a keypress. In a script that is
             * a hang with no output and no clue, and it is exactly what
             * somebody will type first.
             *
             * The row limit moved to -r. */
            int reps = atoi(argv[++i]);
            if (reps <= 1)
                once = true;
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: top [-1] [-n count] [-r rows]\n");
            printf("  -1        print once and exit\n");
            printf("  -n <n>    refresh n times, then exit (-n 1 = once)\n");
            printf("  -r <n>    show only the top n processes\n");
            printf("\nkeys: q quit  c/m/p sort  k kill  h help\n");
            return 0;
        } else {
            dprintf(STDERR_FILENO, "top: unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    if (once) {
        /* Two samples a moment apart, so the CPU column means something. */
        scan_procs();
        lp_sleep_ms(300);
        scan_procs();
        sort_procs();
        print_header();
        print_procs(0);
        return 0;
    }

    /* cbreak, not raw: we want keys without Enter, but we print whole
     * lines and need \n to still return the cursor to column one. Full
     * raw mode turns that off and the listing walks off the right of the
     * screen, one column further with every row. */
    if (lp_term_cbreak(STDIN_FILENO, &saved_term) < 0) {
        /* No terminal - fall back to printing once rather than failing. */
        scan_procs();
        lp_sleep_ms(300);
        scan_procs();
        sort_procs();
        print_header();
        print_procs(0);
        return 0;
    }
    raw_mode = true;

    int rows = 24, cols = 80;
    printf("\x1b[2J");

    for (;;) {
        lp_term_size(STDOUT_FILENO, &rows, &cols);
        scan_procs();
        sort_procs();

        printf("\x1b[H\x1b[J");          /* home, clear below */
        print_header();
        print_procs(rows - 8);
        printf("\n q quit   c/m/p sort   k stop a process   h help   "
               "[sorted by %s]",
               sort_by == SORT_CPU ? "cpu" :
               sort_by == SORT_MEM ? "memory" : "pid");

        /* Wait for a key, but no longer than the refresh interval.
         * VMIN=1 makes read block, so poll in short slices instead. */
        bool acted = false;
        for (int waited = 0; waited < REFRESH_MS && !acted; waited += 100) {
            char c;
            long n = lp_read(STDIN_FILENO, &c, 1);
            if (n != 1) {
                lp_sleep_ms(100);
                continue;
            }
            acted = true;
            switch (c) {
            /* Ctrl-C and Ctrl-D as well as q. The terminal is in
             * cbreak mode, so Ctrl-C arrives as a byte rather than as a
             * signal - if we do not act on it here, nothing does, and
             * the one key everybody reaches for does nothing at all. */
            case 'q': case 'Q': case 3: case 4:
                restore_term();
                printf("\x1b[2J\x1b[H");
                return 0;
            case 'c': case 'C': sort_by = SORT_CPU; break;
            case 'm': case 'M': sort_by = SORT_MEM; break;
            case 'p': case 'P': sort_by = SORT_PID; break;
            case 'k': case 'K': printf("\n"); do_kill(); break;
            case 'h': case 'H': show_help(); printf("\x1b[2J"); break;
            default: break;                /* r and anything else: refresh */
            }
        }
    }
}
