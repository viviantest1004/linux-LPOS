/* exec.c - running the parse tree: simple commands, pipelines,
 * subshells, redirections, functions, jobs, signals and traps.
 * Part of sh.c; see sh.h.
 *
 * ── The shape of it ──
 *
 * evaltree() walks a node and returns its exit status, which is also
 * left in exitstatus ($?). A handful of flags travel down the walk:
 *
 *   EV_EXIT    this process exits as soon as the node is done. A simple
 *              command that runs a program then execs it in place
 *              instead of forking first - that is why `sh -c 'prog'`
 *              and every stage of a pipeline cost one process, not two,
 *              and why a signal sent to the shell's pid reaches prog.
 *   EV_TESTED  the status is being tested (if/while condition, the
 *              left of && or ||, the operand of !), so `set -e` must
 *              not act on it. It is inherited into function bodies, the
 *              way dash and bash both do it.
 *
 * Unwinding - break, continue, return, and abandoning a command after
 * an error at the prompt - is not done with longjmp. A builtin sets
 * evalskip, and every loop in here checks it after each step and
 * returns. That keeps every cleanup (restoring redirections, popping a
 * function's locals, freeing a parse arena) on the ordinary return path,
 * where it cannot be skipped.
 *
 * ── Errors ──
 *
 * The consequences follow POSIX XCU 2.8.1 with dash's exit codes, since
 * dash is what Debian's scripts are tested against: an expansion error,
 * a failed assignment to a readonly variable, a redirection error on a
 * special builtin or compound command end a non-interactive shell with
 * status 2; an interactive one abandons the command and prompts again.
 * The messages are worded the way bash words them, because that is the
 * shell the owner's fingers know. */

/* ── wait statuses ─────────────────────────────────────────────────── *
 *
 * Our own decoding: the libc's LP_WIFSIGNALED also answers yes for a
 * stopped child (0x7f in the low byte), which job control cannot live
 * with. */
#define WS_EXITED(s)    (((s) & 0x7f) == 0)
#define WS_EXITCODE(s)  (((s) >> 8) & 0xff)
#define WS_STOPPED(s)   (((s) & 0xff) == 0x7f)
#define WS_STOPSIG(s)   (((s) >> 8) & 0xff)
#define WS_CONTINUED(s) ((s) == 0xffff)
#define WS_TERMSIG(s)   ((s) & 0x7f)
#define WS_CORE(s)      (((s) & 0x80) != 0)
#define W_NOHANG     1
#define W_UNTRACED   2
#define W_CONTINUED  8

#define E_NOENT   2
#define E_INTR    4
#define E_BADF    9
#define E_CHILD   10
#define E_ACCES   13
#define E_EXIST   17
#define E_NOTDIR  20
#define E_ISDIR   21
#define E_NOEXEC  8
#define E_PIPE    32

static bool pipestatus_done;

static int status_of(int ws)
{
    if (WS_EXITED(ws))
        return WS_EXITCODE(ws);
    if (WS_STOPPED(ws))
        return 128 + WS_STOPSIG(ws);
    return 128 + WS_TERMSIG(ws);
}

/* ── signal names ──────────────────────────────────────────────────── *
 *
 * Linux numbers these the same way on x86-64, arm64 and 32-bit ARM, so
 * one table serves every build. */
static const char *const signames[32] = {
    "EXIT", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE",
    "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT",
    "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG", "XCPU", "XFSZ",
    "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS"
};

static const char *sig_name(int sig)
{
    static char buf[16];
    if (sig >= 0 && sig < 32)
        return signames[sig];
    if (sig >= 34 && sig <= 64) {
        if (sig <= 49)
            snprintf(buf, sizeof buf, sig == 34 ? "RTMIN" : "RTMIN+%d",
                     sig - 34);
        else
            snprintf(buf, sizeof buf, sig == 64 ? "RTMAX" : "RTMAX-%d",
                     64 - sig);
        return buf;
    }
    snprintf(buf, sizeof buf, "%d", sig);
    return buf;
}

static int strcasecmp_ascii(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (u8)*a, y = (u8)*b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y || !x)
            return x - y;
    }
}

/* A signal by name (HUP, SIGHUP, hup) or number. -1 if neither. */
static int sig_number(const char *s)
{
    long long n;
    if (parse_int(s, &n))
        return n >= 0 && n < NSIG_SH ? (int)n : -1;
    if ((s[0] == 'S' || s[0] == 's') && (s[1] == 'I' || s[1] == 'i') &&
        (s[2] == 'G' || s[2] == 'g') && s[3])
        s += 3;
    for (int i = 0; i < 32; i++)
        if (strcasecmp_ascii(s, signames[i]) == 0)
            return i;
    if (strcasecmp_ascii(s, "CLD") == 0) return 17;
    if (strcasecmp_ascii(s, "POLL") == 0) return 29;
    if (strcasecmp_ascii(s, "IOT") == 0) return 6;
    if (strncmp(s, "RTMIN", 5) == 0) {
        if (!s[5]) return 34;
        if (s[5] == '+' && parse_int(s + 6, &n) && n >= 0 && n <= 30)
            return 34 + (int)n;
    }
    if (strncmp(s, "RTMAX", 5) == 0) {
        if (!s[5]) return 64;
        if (s[5] == '-' && parse_int(s + 6, &n) && n >= 0 && n <= 30)
            return 64 - (int)n;
    }
    return -1;
}

/* What bash prints when a job dies of a signal. */
static const char *sig_desc(int sig)
{
    static const char *const d[32] = {
        "", "Hangup", "Interrupt", "Quit", "Illegal instruction",
        "Trace/breakpoint trap", "Aborted", "Bus error",
        "Floating point exception", "Killed", "User defined signal 1",
        "Segmentation fault", "User defined signal 2", "Broken pipe",
        "Alarm clock", "Terminated", "Stack fault", "Child exited",
        "Continued", "Stopped (signal)", "Stopped", "Stopped (tty input)",
        "Stopped (tty output)", "Urgent I/O condition",
        "CPU time limit exceeded", "File size limit exceeded",
        "Virtual timer expired", "Profiling timer expired",
        "Window changed", "I/O possible", "Power failure",
        "Bad system call"
    };
    static char buf[32];
    if (sig > 0 && sig < 32)
        return d[sig];
    snprintf(buf, sizeof buf, "Real-time signal %d", sig - 34);
    return buf;
}

/* ── signals and traps ─────────────────────────────────────────────── *
 *
 * A caught signal only sets a flag; the trap runs later at a safe
 * point - between commands, or when `wait` is interrupted. Running shell
 * code inside a signal handler would re-enter the allocator and the
 * parser, which is the kind of bug that shows up once a month. The
 * handlers are installed without SA_RESTART on purpose: a blocking
 * waitpid or read then returns EINTR, which is what lets `wait` and the
 * prompt notice the signal at all. */

static volatile int gotsig[NSIG_SH];
static volatile int got_sigint;
static char *trap_err;              /* bash's ERR pseudo-signal */
static bool  in_trap;
static int   trap_depth;

static void onsig(int sig)
{
    if (sig <= 0 || sig >= NSIG_SH)
        return;
    if (sig == 2 && !trap_cmd[2])
        got_sigint = 1;
    else
        gotsig[sig] = 1;
    pending_signals = 1;
}

static void set_sig_handler(int sig, int how)   /* 0 default, 1 ignore, 2 catch */
{
    if (sig <= 0 || sig >= NSIG_SH || sig == 9 || sig == 19)
        return;
    if (how == 0)
        lp_signal_default(sig);
    else if (how == 1)
        lp_signal_ignore(sig);
    else
        lp_signal_handler(sig, onsig);
}

static bool have_traps(void)
{
    if (trap_err)
        return true;
    for (int i = 0; i < NSIG_SH; i++)
        if (trap_cmd[i] && (i == 0 || trap_cmd[i][0]))
            return true;
    return false;
}

/* What a signal's disposition should be when no trap names it. */
static int sig_default_how(int sig)
{
    if (sig_ignored_at_entry[sig] && !opt[O_interactive])
        return 1;
    if (opt[O_interactive] && !in_subshell) {
        if (sig == 2)
            return 2;                       /* Ctrl-C abandons the line */
        if (sig == 3 || sig == 15)
            return 1;                       /* as bash: not the shell */
        if (sig == 1)
            return 2;                       /* hang up: clean exit */
    }
    if (job_control && (sig == 20 || sig == 21 || sig == 22))
        return 1;
    return 0;
}

static void trap_set(int sig, const char *cmd)
{
    if (sig < 0 || sig >= NSIG_SH)
        return;
    xfree(trap_cmd[sig]);
    trap_cmd[sig] = cmd ? xstrdup(cmd) : NULL;
    if (sig == 0)
        return;
    if (!cmd)
        set_sig_handler(sig, sig_default_how(sig));
    else if (!*cmd)
        set_sig_handler(sig, 1);
    else
        set_sig_handler(sig, 2);
}

static void signals_init(void)
{
    for (int s = 1; s < NSIG_SH; s++) {
        if (s == 9 || s == 19 || s == 32 || s == 33)
            continue;
        long h;
        sig_disposition(s, &h);
        sig_ignored_at_entry[s] = h == 1;
    }
    /* A shell started with a blocked signal mask (some daemons do that)
     * would never see SIGINT or a trapped signal at all. */
    u64 none = 0;
    sys_sigmask(2 /* SIG_SETMASK */, &none, NULL);
    for (int s = 1; s < 32; s++) {
        int how = sig_default_how(s);
        if (how)
            set_sig_handler(s, how);
    }
}

/* In a forked child - a subshell, a pipeline stage, a background job:
 * traps that run commands go back to the default (POSIX), ignored
 * signals stay ignored, and the interactive shell's own arrangements
 * for Ctrl-C and job control are undone. A background job without job
 * control ignores SIGINT and SIGQUIT, so Ctrl-C at the prompt does not
 * kill what was started with &. */
static void signals_reset_for_child(bool async_nojc)
{
    for (int s = 1; s < NSIG_SH; s++) {
        if (s == 9 || s == 19 || s == 32 || s == 33)
            continue;
        if (trap_cmd[s] && trap_cmd[s][0]) {
            xfree(trap_cmd[s]);
            trap_cmd[s] = NULL;
            set_sig_handler(s, sig_ignored_at_entry[s] ? 1 : 0);
        } else if (!trap_cmd[s]) {
            bool ign = sig_ignored_at_entry[s];
            set_sig_handler(s, ign ? 1 : 0);
        }
        gotsig[s] = 0;
    }
    xfree(trap_cmd[0]);
    trap_cmd[0] = NULL;
    xfree(trap_err);
    trap_err = NULL;
    if (async_nojc) {
        set_sig_handler(2, 1);
        set_sig_handler(3, 1);
    }
    got_sigint = 0;
    pending_signals = 0;
}

/* Ctrl-C at an interactive shell while it runs its own code (a loop of
 * builtins, say): abandon whatever it was doing and go back to the
 * prompt, as bash does. */
static void sigint_check(void)
{
    if (!got_sigint)
        return;
    got_sigint = 0;
    if (trap_cmd[2])
        return;
    if (opt[O_interactive] && !in_subshell) {
        exitstatus = 130;
        evalskip = SKIP_ABORT;
    } else {
        /* A non-interactive shell with SIGINT caught only because it is
         * interactive-by-flag: behave as if killed by it. */
        flush_all();
        lp_signal_default(2);
        lp_kill(lp_getpid(), 2);
        lp_exit(130);
    }
}

static void run_trap_cmd(const char *cmd)
{
    char *copy = xstrdup(cmd);
    int save_status = exitstatus;
    int save_skip = evalskip;
    int save_line = cmd_lineno;
    evalskip = SKIP_NONE;
    in_trap = true;
    trap_depth++;
    evalstring(copy, 0);
    trap_depth--;
    in_trap = trap_depth > 0;
    xfree(copy);
    cmd_lineno = save_line;
    if (evalskip == SKIP_NONE || evalskip == SKIP_ABORT) {
        /* POSIX: $? after a trap is what it was before it ran. */
        exitstatus = save_status;
        if (evalskip == SKIP_NONE)
            evalskip = save_skip;
    }
}

static void jobs_hup_all(void);

static void run_pending_traps(void)
{
    if (!pending_signals || in_trap)
        return;
    pending_signals = 0;
    sigint_check();
    for (int s = 1; s < NSIG_SH; s++) {
        if (!gotsig[s])
            continue;
        gotsig[s] = 0;
        if (trap_cmd[s] && trap_cmd[s][0]) {
            run_trap_cmd(trap_cmd[s]);
        } else if (s == 1 && opt[O_interactive] && !in_subshell) {
            /* The terminal went away: take the jobs with us, save the
             * history and go - bash's behaviour. */
            jobs_hup_all();
            exitshell(129);
        }
    }
}

/* ── jobs ──────────────────────────────────────────────────────────── */

enum { JS_RUNNING, JS_STOPPED, JS_DONE };

typedef struct {
    int   pid;
    int   status;           /* the wait status once it has changed */
    u8    state;
} proc_t;

typedef struct job {
    struct job   *next;
    int           id;
    int           pgid;
    proc_t       *procs;
    int           nproc, capproc;
    u8            state;
    bool          bg;           /* not what the shell is waiting on */
    bool          jobctl;       /* in a process group of its own */
    bool          changed;      /* state changed, not yet reported */
    bool          reported_bg;  /* "[1] 1234" already printed */
    lp_termios_t  tmodes;       /* the terminal as the job left it */
    bool          have_tmodes;
    u32           seq;          /* recency, for %+ and %- */
    char         *cmd;
} job_t;

static job_t *jobs;
static u32    job_seq;
static int    shell_orig_pgrp = -1;
static lp_termios_t shell_tmodes;
static bool   have_shell_tmodes;
static bool   warned_stopped;

#define TCGETS_  0x5401
#define TCSETSW_ 0x5403

static int job_free_id(void)
{
    for (int id = 1;; id++) {
        bool used = false;
        for (job_t *j = jobs; j; j = j->next)
            if (j->id == id) { used = true; break; }
        if (!used)
            return id;
    }
}

static job_t *job_new(node_t *n, bool bg)
{
    job_t *j = xcalloc(sizeof *j);
    j->id = job_free_id();
    j->bg = bg;
    j->jobctl = job_control;
    j->seq = ++job_seq;
    strbuf_t b = {0};
    if (n)
        fmt_node(&b, n, -1);
    j->cmd = sb_take(&b);
    /* keep the list ordered by id so `jobs` prints in order */
    job_t **pp = &jobs;
    while (*pp && (*pp)->id < j->id)
        pp = &(*pp)->next;
    j->next = *pp;
    *pp = j;
    return j;
}

static void job_add_proc(job_t *j, int pid)
{
    if (j->nproc == j->capproc) {
        j->capproc = j->capproc ? j->capproc * 2 : 4;
        j->procs = xrealloc(j->procs, (size_t)j->capproc * sizeof *j->procs);
    }
    proc_t *p = &j->procs[j->nproc++];
    p->pid = pid;
    p->status = 0;
    p->state = JS_RUNNING;
    if (!j->pgid)
        j->pgid = pid;
}

static void job_free(job_t *j)
{
    for (job_t **pp = &jobs; *pp; pp = &(*pp)->next) {
        if (*pp == j) {
            *pp = j->next;
            break;
        }
    }
    xfree(j->procs);
    xfree(j->cmd);
    xfree(j);
}

static void jobs_clear_all(void)
{
    while (jobs)
        job_free(jobs);
}

static void job_update_state(job_t *j)
{
    bool running = false, stopped = false;
    for (int i = 0; i < j->nproc; i++) {
        if (j->procs[i].state == JS_RUNNING) running = true;
        else if (j->procs[i].state == JS_STOPPED) stopped = true;
    }
    u8 st = running ? JS_RUNNING : stopped ? JS_STOPPED : JS_DONE;
    if (st != j->state) {
        j->state = st;
        j->changed = true;
        if (st == JS_STOPPED)
            j->seq = ++job_seq;
    }
}

/* The exit status of a whole job: the last process, or with pipefail the
 * last one that failed. */
static int job_status(job_t *j)
{
    if (j->nproc == 0)
        return 0;
    int st = status_of(j->procs[j->nproc - 1].status);
    if (j->state == JS_STOPPED) {
        for (int i = 0; i < j->nproc; i++)
            if (j->procs[i].state == JS_STOPPED)
                return status_of(j->procs[i].status);
    }
    if (opt[O_pipefail]) {
        for (int i = j->nproc - 1; i >= 0; i--) {
            int s = status_of(j->procs[i].status);
            if (s) return s;
        }
    }
    return st;
}

static void record_status(int pid, int ws)
{
    for (job_t *j = jobs; j; j = j->next) {
        for (int i = 0; i < j->nproc; i++) {
            proc_t *p = &j->procs[i];
            if (p->pid != pid)
                continue;
            if (WS_CONTINUED(ws)) {
                p->state = JS_RUNNING;
            } else if (WS_STOPPED(ws)) {
                p->state = JS_STOPPED;
                p->status = ws;
            } else {
                p->state = JS_DONE;
                p->status = ws;
            }
            job_update_state(j);
            return;
        }
    }
}

/* Collect children. block: wait for one change. Returns 1 when something
 * was collected, 0 when nothing was there (non-blocking), -1 when there
 * are no children at all, and -2 when a trapped signal interrupted a
 * blocking wait and `interruptible` asked to be told. */
static int dowait(bool block, bool interruptible)
{
    int flags = (block ? 0 : W_NOHANG) |
                (job_control ? (W_UNTRACED | W_CONTINUED) : 0);
    int got = 0;
    for (;;) {
        int ws = 0;
        long pid = lp_waitpid(-1, &ws, flags);
        if (pid == -E_INTR) {
            if (interruptible && pending_signals) {
                for (int s = 1; s < NSIG_SH; s++)
                    if (gotsig[s] && trap_cmd[s] && trap_cmd[s][0])
                        return -2;
                if (got_sigint && opt[O_interactive] && !in_subshell)
                    return -2;
            }
            continue;
        }
        if (pid < 0)
            return got ? 1 : -1;
        if (pid == 0)
            return got;
        record_status((int)pid, ws);
        got = 1;
        if (block)
            return 1;
    }
}

static void tty_get(lp_termios_t *t, bool *ok)
{
    *ok = tty_fd >= 0 && lp_ioctl(tty_fd, TCGETS_, t->raw) == 0;
}

static void tty_set(const lp_termios_t *t)
{
    if (tty_fd >= 0)
        lp_ioctl(tty_fd, TCSETSW_, (void *)t->raw);
}

static void term_save_mode(void)
{
    tty_get(&shell_tmodes, &have_shell_tmodes);
}

static void term_restore_mode(void)
{
    if (have_shell_tmodes)
        tty_set(&shell_tmodes);
}

static void print_job(job_t *j, out_t *o, bool longform, bool pidonly);
static void pipestatus_set(const int *st, int n);

/* Wait for a foreground job to finish or stop, then take the terminal
 * back. The status is the job's; a job stopped with Ctrl-Z stays in the
 * table and says so. */
static int waitforjob(job_t *j)
{
    while (j->state == JS_RUNNING) {
        if (dowait(true, false) < 0)
            break;
    }
    int st = job_status(j);
    {
        int small[8];
        int *v = j->nproc <= 8 ? small : xmalloc((size_t)j->nproc * sizeof(int));
        for (int i = 0; i < j->nproc; i++)
            v[i] = status_of(j->procs[i].status);
        pipestatus_set(v, j->nproc);
        if (v != small)
            xfree(v);
    }
    if (j->jobctl && tty_fd >= 0 && !in_subshell) {
        if (j->state == JS_STOPPED) {
            tty_get(&j->tmodes, &j->have_tmodes);
        }
        sys_tcsetpgrp(tty_fd, shell_pgid);
        if (j->state == JS_STOPPED) {
            term_restore_mode();
        } else {
            /* A job that died of a signal may have left the terminal
             * raw; one that exited normally left it as it meant to
             * (stty exits normally), so its modes become ours. */
            int last = j->procs[j->nproc - 1].status;
            if (!WS_EXITED(last))
                term_restore_mode();
            else
                term_save_mode();
        }
    }
    if (j->state == JS_STOPPED) {
        j->bg = true;
        j->changed = false;
        j->seq = ++job_seq;
        if (opt[O_interactive] && !in_subshell) {
            outc(out2, '\n');
            print_job(j, out2, false, false);
            flush_out(out2);
        }
        return st;
    }
    /* Report a death by a signal the way bash does. SIGINT and SIGPIPE
     * are the ordinary ways a pipeline ends and say nothing. */
    int last = j->procs[j->nproc - 1].status;
    if (!WS_EXITED(last) && !WS_STOPPED(last)) {
        int sig = WS_TERMSIG(last);
        if (sig == 2) {
            if (opt[O_interactive] && !in_subshell) {
                outc(out2, '\n');
                flush_out(out2);
                if (!trap_cmd[2]) {
                    evalskip = SKIP_ABORT;
                    got_sigint = 0;
                }
            }
        } else if (sig != 13) {
            outs(out2, sig_desc(sig));
            if (WS_CORE(last))
                outs(out2, " (core dumped)");
            outc(out2, '\n');
            flush_out(out2);
        }
    }
    job_free(j);
    return st;
}

static const char *job_state_text(job_t *j, char *buf, size_t n)
{
    if (j->state == JS_RUNNING)
        return "Running";
    if (j->state == JS_STOPPED) {
        for (int i = 0; i < j->nproc; i++)
            if (j->procs[i].state == JS_STOPPED) {
                int sig = WS_STOPSIG(j->procs[i].status);
                return sig == 20 ? "Stopped" : sig_desc(sig);
            }
        return "Stopped";
    }
    int ws = j->procs[j->nproc - 1].status;
    if (WS_EXITED(ws)) {
        if (WS_EXITCODE(ws) == 0)
            return "Done";
        snprintf(buf, n, "Exit %d", WS_EXITCODE(ws));
        return buf;
    }
    snprintf(buf, n, "%s%s", sig_desc(WS_TERMSIG(ws)),
             WS_CORE(ws) ? " (core dumped)" : "");
    return buf;
}

static job_t *job_current(int which)    /* 0: %+, 1: %- */
{
    job_t *best = NULL, *second = NULL;
    for (job_t *j = jobs; j; j = j->next) {
        if (!j->bg && j->state == JS_RUNNING)
            continue;
        if (!best || j->seq > best->seq) {
            second = best;
            best = j;
        } else if (!second || j->seq > second->seq) {
            second = j;
        }
    }
    return which ? second : best;
}

static void print_job(job_t *j, out_t *o, bool longform, bool pidonly)
{
    if (pidonly) {
        outf(o, "%d\n", j->pgid);
        return;
    }
    char mark = j == job_current(0) ? '+' : j == job_current(1) ? '-' : ' ';
    char buf[64];
    const char *st = job_state_text(j, buf, sizeof buf);
    if (longform)
        outf(o, "[%d]%c %d %-24s%s\n", j->id, mark, j->pgid, st, j->cmd);
    else
        outf(o, "[%d]%c  %-24s%s%s\n", j->id, mark, st, j->cmd,
             j->state == JS_RUNNING ? " &" : "");
}

/* Before a prompt: say which background jobs finished or stopped since
 * the last one, and forget the finished ones. */
static void jobs_notify(bool all)
{
    dowait(false, false);
    job_t *next;
    for (job_t *j = jobs; j; j = next) {
        next = j->next;
        if (!j->bg)
            continue;
        if (j->changed || (all && j->state == JS_DONE)) {
            if (opt[O_interactive] && !in_subshell)
                print_job(j, out2, false, false);
            j->changed = false;
        }
        if (j->state == JS_DONE && opt[O_interactive] && !in_subshell)
            job_free(j);
    }
    flush_out(out2);
}

/* A non-interactive shell keeps finished background jobs so `wait pid`
 * can still report their status, but not without limit: a script that
 * starts a thousand jobs and never waits must not grow forever. */
static void jobs_trim(void)
{
    int n = 0;
    for (job_t *j = jobs; j; j = j->next)
        if (j->state == JS_DONE && j->bg) n++;
    job_t *next;
    for (job_t *j = jobs; j && n > 256; j = next) {
        next = j->next;
        if (j->state == JS_DONE && j->bg && j->nproc &&
            j->procs[j->nproc - 1].pid != lastbgpid) {
            job_free(j);
            n--;
        }
    }
}

static bool jobs_any_stopped(void)
{
    for (job_t *j = jobs; j; j = j->next)
        if (j->state == JS_STOPPED)
            return true;
    return false;
}

static void jobs_hup_all(void)
{
    for (job_t *j = jobs; j; j = j->next) {
        if (j->state == JS_DONE)
            continue;
        if (j->jobctl) {
            lp_kill(-j->pgid, 1);
            if (j->state == JS_STOPPED)
                lp_kill(-j->pgid, 18);
        } else {
            for (int i = 0; i < j->nproc; i++)
                lp_kill(j->procs[i].pid, 1);
        }
    }
}

/* Take the terminal and a process group of our own, if this is an
 * interactive shell on a terminal. Without both, Ctrl-Z would stop the
 * shell together with the job, and fg would have nothing to give the
 * terminal back to. */
static void jobs_init(void)
{
    job_control = false;
    if (!opt[O_monitor])
        return;
    long fd = lp_open("/dev/tty", O_RDWR | O_CLOEXEC, 0);
    if (fd < 0) {
        if (lp_isatty(2)) fd = lp_dup(2);
        else if (lp_isatty(0)) fd = lp_dup(0);
    }
    if (fd < 0)
        goto fail;
    tty_fd = fd_move_high((int)fd);
    if (tty_fd < 0)
        goto fail;
    for (int tries = 0;; tries++) {
        int fg = sys_tcgetpgrp(tty_fd);
        if (fg < 0)
            goto fail;
        int me = sys_getpgrp();
        if (fg == me)
            break;
        if (tries > 50)
            goto fail;
        /* Started in the background: wait until we are brought to the
         * foreground rather than fight over the terminal. */
        lp_kill(0, 21);
    }
    shell_orig_pgrp = sys_getpgrp();
    shell_pgid = lp_getpid();
    if (shell_orig_pgrp != shell_pgid && sys_setpgid(0, shell_pgid) < 0)
        shell_pgid = shell_orig_pgrp;
    if (sys_tcsetpgrp(tty_fd, shell_pgid) < 0)
        goto fail;
    job_control = true;
    set_sig_handler(20, 1);
    set_sig_handler(21, 1);
    set_sig_handler(22, 1);
    /* Something (the boards' lp_term_sane) may have switched the suspend
     * key off because there used to be no job control. There is now. */
    lp_termios_t t;
    bool ok;
    tty_get(&t, &ok);
    if (ok && t.raw[17 + 10] == 0) {
        t.raw[17 + 10] = 26;            /* VSUSP = ^Z */
        tty_set(&t);
    }
    term_save_mode();
    return;
fail:
    if (tty_fd >= 0 && !opt[O_interactive]) {
        lp_close(tty_fd);
        tty_fd = -1;
    }
    opt[O_monitor] = false;
}

static void jobs_release_terminal(void)
{
    if (job_control && tty_fd >= 0 && shell_orig_pgrp > 0 &&
        shell_orig_pgrp != shell_pgid) {
        sys_setpgid(0, shell_orig_pgrp);
        sys_tcsetpgrp(tty_fd, shell_orig_pgrp);
    }
}

/* ── forking ───────────────────────────────────────────────────────── */

enum { FORK_FG, FORK_BG, FORK_NOJOB };

/* The child side of every fork that goes on to run shell code. */
static void child_init(job_t *j, int mode)
{
    bool was_jc = job_control;
    in_subshell = true;
    toplevel_interactive = false;
    if (j && j->jobctl && was_jc) {
        int pgid = j->pgid ? j->pgid : lp_getpid();
        sys_setpgid(0, pgid);
        if (mode == FORK_FG && tty_fd >= 0)
            sys_tcsetpgrp(tty_fd, pgid);
    }
    job_control = false;
    signals_reset_for_child(mode == FORK_BG && !was_jc);
    if (was_jc) {
        set_sig_handler(20, 0);
        set_sig_handler(21, 0);
        set_sig_handler(22, 0);
    }
    if (mode == FORK_BG && !was_jc) {
        /* POSIX: without job control an async list's stdin is /dev/null
         * unless it redirects it itself. */
        long fd = lp_open("/dev/null", O_RDONLY, 0);
        if (fd >= 0 && fd != 0) {
            lp_dup2((int)fd, 0);
            lp_close((int)fd);
        }
    }
    jobs_clear_all();
}

/* Fork for job j. Returns the child's pid in the parent and 0 in the
 * child (which has been set up already). */
static int forkjob(job_t *j, int mode)
{
    flush_all();
    int pid = (int)lp_fork();
    if (pid < 0) {
        sh_error(2, "fork: %s", lp_strerror(-pid));
        return -1;
    }
    if (pid == 0) {
        child_init(j, mode);
        return 0;
    }
    if (j) {
        job_add_proc(j, pid);
        if (j->jobctl && job_control)
            sys_setpgid(pid, j->pgid);
        if (mode == FORK_FG && j->jobctl && job_control && tty_fd >= 0 &&
            j->nproc == 1)
            sys_tcsetpgrp(tty_fd, j->pgid);
    }
    return pid;
}

/* ── redirections ──────────────────────────────────────────────────── */

typedef struct {
    int fd;         /* the fd that was redirected */
    int saved;      /* a high copy of what it was, or -1: it was closed */
} rsave_t;

typedef struct rframe {
    struct rframe *prev;
    rsave_t       *v;
    int            n, cap;
} rframe_t;

static rframe_t *rstack;

static bool rframe_has(rframe_t *f, int fd)
{
    for (int i = 0; i < f->n; i++)
        if (f->v[i].fd == fd)
            return true;
    return false;
}

/* Is fd one the shell holds for itself: a script it is reading, the
 * terminal, a saved copy? A redirection naming it must not clobber it,
 * so the shell's copy moves out of the way first. */
static void fd_protect(int fd)
{
    if (fd < 10)
        return;
    if (fd == tty_fd) {
        int n = fd_dup_high(fd);
        if (n >= 0) { lp_close(fd); tty_fd = n; }
    }
    for (insrc_t *s = in; s; s = s->prev) {
        if (s->fd == fd && (s->kind == IS_FILE || s->kind == IS_TTY)) {
            int n = fd_dup_high(fd);
            if (n >= 0) { lp_close(fd); s->fd = n; }
        }
    }
    for (rframe_t *f = rstack; f; f = f->prev) {
        for (int i = 0; i < f->n; i++) {
            if (f->v[i].saved == fd) {
                int n = fd_dup_high(fd);
                if (n >= 0) { lp_close(fd); f->v[i].saved = n; }
            }
        }
    }
}

static void rsave(rframe_t *f, int fd)
{
    if (!f || rframe_has(f, fd))
        return;
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 4;
        f->v = xrealloc(f->v, (size_t)f->cap * sizeof *f->v);
    }
    int saved = fd_dup_high(fd);        /* -EBADF when fd is not open */
    f->v[f->n].fd = fd;
    f->v[f->n].saved = saved >= 0 ? saved : -1;
    f->n++;
}

static void rframe_restore(rframe_t *f)
{
    for (int i = f->n - 1; i >= 0; i--) {
        if (f->v[i].saved >= 0) {
            lp_dup2(f->v[i].saved, f->v[i].fd);
            lp_close(f->v[i].saved);
        } else {
            lp_close(f->v[i].fd);
        }
    }
    xfree(f->v);
    f->v = NULL;
    f->n = f->cap = 0;
}

/* A here-document or here-string becomes the read end of a pipe. A small
 * body is written straight into the pipe; one bigger than a pipe is
 * guaranteed to hold - a single page, when the system is short of pipe
 * memory - is fed by a child so neither side can block the other. */
static int heredoc_pipe(const char *body, size_t len)
{
    int p[2];
    long r = lp_pipe(p);
    if (r < 0) {
        sh_perror("pipe", r);
        return -1;
    }
    if (len <= 4096) {
        xwrite_all(p[1], body, len);
        lp_close(p[1]);
        return p[0];
    }
    flush_all();
    long pid = lp_fork();
    if (pid == 0) {
        lp_close(p[0]);
        lp_signal_default(13);
        xwrite_all(p[1], body, len);
        lp_exit(0);
    }
    lp_close(p[1]);
    if (pid < 0) {
        lp_close(p[0]);
        sh_perror("fork", pid);
        return -1;
    }
    return p[0];
}

static bool fd_number(const char *s, int *fd)
{
    long long n;
    if (!*s || !is_digit((u8)*s))
        return false;
    for (const char *p = s; *p; p++)
        if (!is_digit((u8)*p))
            return false;
    if (!parse_int(s, &n) || n > 1023)
        return false;
    *fd = (int)n;
    return true;
}

/* Apply one redirection. f is the frame to save the old fds in, or NULL
 * for a permanent change (exec, and anything in a child about to exec). */
static bool redirect_one(redir_t *r, rframe_t *f)
{
    int fd = r->fd;
    char *target = NULL;
    int newfd = -1;
    bool close_it = false;

    if (r->type == R_HEREDOC) {
        char *body = r->hexpand ? expand_heredoc(r->hbody ? r->hbody : "")
                                : xstrdup(r->hbody ? r->hbody : "");
        if (!body || expand_error) {
            xfree(body);
            return false;
        }
        newfd = heredoc_pipe(body, strlen(body));
        xfree(body);
        if (newfd < 0)
            return false;
        goto install;
    }
    target = expand_str(r->target, X_TILDE);
    if (!target || expand_error) {
        xfree(target);
        return false;
    }
    switch (r->type) {
    case R_HERESTR: {
        size_t n = strlen(target);
        target = xrealloc(target, n + 2);
        target[n] = '\n';
        target[n + 1] = '\0';
        newfd = heredoc_pipe(target, n + 1);
        break;
    }
    case R_IN:
        newfd = (int)lp_open(target, O_RDONLY, 0);
        break;
    case R_OUT:
        if (opt[O_noclobber]) {
            lp_stat_t st;
            if (lp_stat(target, &st, true) == 0) {
                if ((st.mode & LP_S_IFMT) == LP_S_IFREG) {
                    sh_warn("%s: cannot overwrite existing file", target);
                    xfree(target);
                    return false;
                }
                newfd = (int)lp_open(target, O_WRONLY, 0);
            } else {
                newfd = (int)lp_open(target, O_WRONLY | O_CREAT | O_EXCL,
                                     0666);
            }
            break;
        }
        /* fall through */
    case R_CLOBBER:
        newfd = (int)lp_open(target, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        break;
    case R_APPEND:
        newfd = (int)lp_open(target, O_WRONLY | O_CREAT | O_APPEND, 0666);
        break;
    case R_RDWR:
        newfd = (int)lp_open(target, O_RDWR | O_CREAT, 0666);
        break;
    case R_OUTERR:
    case R_APPENDERR:
        newfd = (int)lp_open(target, O_WRONLY | O_CREAT |
                             (r->type == R_APPENDERR ? O_APPEND : O_TRUNC),
                             0666);
        break;
    case R_DUPIN:
    case R_DUPOUT: {
        if (strcmp(target, "-") == 0) {
            close_it = true;
            break;
        }
        size_t tl = strlen(target);
        bool move = tl > 1 && target[tl - 1] == '-';
        if (move)
            target[tl - 1] = '\0';
        int src;
        if (!fd_number(target, &src)) {
            if (r->type == R_DUPOUT && fd == 1 && !move) {
                /* bash: >&file is &>file */
                newfd = (int)lp_open(target, O_WRONLY | O_CREAT | O_TRUNC,
                                     0666);
                if (newfd < 0) break;
                rsave(f, 1);
                rsave(f, 2);
                lp_dup2(newfd, 1);
                lp_dup2(newfd, 2);
                lp_close(newfd);
                xfree(target);
                return true;
            }
            sh_warn("%s: ambiguous redirect", target);
            xfree(target);
            return false;
        }
        if (!fd_is_open(src) || (src >= 10 && src == tty_fd)) {
            sh_warn("%d: Bad file descriptor", src);
            xfree(target);
            return false;
        }
        if (src == fd) {
            xfree(target);
            return true;
        }
        fd_protect(fd);
        rsave(f, fd);
        lp_dup2(src, fd);
        if (move) {
            rsave(f, src);
            lp_close(src);
        }
        xfree(target);
        return true;
    }
    default:
        break;
    }
    if (close_it) {
        fd_protect(fd);
        rsave(f, fd);
        lp_close(fd);
        xfree(target);
        return true;
    }
    if (newfd < 0) {
        const char *why = newfd == -E_EXIST
                          ? "cannot overwrite existing file"
                          : lp_strerror(-newfd);
        sh_warn("%s: %s", target ? target : "", why);
        xfree(target);
        return false;
    }
    xfree(target);
install:
    if (r->type == R_OUTERR || r->type == R_APPENDERR) {
        rsave(f, 1);
        rsave(f, 2);
        lp_dup2(newfd, 1);
        lp_dup2(newfd, 2);
        if (newfd != 1 && newfd != 2)
            lp_close(newfd);
        return true;
    }
    if (newfd == fd) {
        /* open() handed back the very fd being redirected, which means
         * it had been closed: nothing to save beyond "it was closed". */
        if (f && !rframe_has(f, fd)) {
            if (f->n == f->cap) {
                f->cap = f->cap ? f->cap * 2 : 4;
                f->v = xrealloc(f->v, (size_t)f->cap * sizeof *f->v);
            }
            f->v[f->n].fd = fd;
            f->v[f->n].saved = -1;
            f->n++;
        }
        return true;
    }
    fd_protect(fd);
    rsave(f, fd);
    lp_dup2(newfd, fd);
    lp_close(newfd);
    return true;
}

/* Apply a list of redirections. With save, a frame is pushed that
 * redirect_pop() undoes; on failure everything this call did is undone
 * and false comes back. */
static bool redirect_push(redir_t *r, bool save)
{
    if (save) {
        rframe_t *f = xcalloc(sizeof *f);
        f->prev = rstack;
        rstack = f;
    }
    flush_all();
    for (; r; r = r->next) {
        if (!redirect_one(r, save ? rstack : NULL)) {
            if (save) {
                rframe_t *f = rstack;
                rframe_restore(f);
                rstack = f->prev;
                xfree(f);
            }
            return false;
        }
    }
    return true;
}

static void redirect_pop(void)
{
    rframe_t *f = rstack;
    if (!f)
        return;
    flush_all();
    rframe_restore(f);
    rstack = f->prev;
    xfree(f);
}

/* `exec 3>file` inside a function whose body is itself redirected: the
 * permanent change must survive the function's frame being undone, so
 * the frames forget any fd exec just set. */
static void redirect_forget(redir_t *r)
{
    for (; r; r = r->next) {
        for (rframe_t *f = rstack; f; f = f->prev) {
            for (int i = 0; i < f->n; i++) {
                if (f->v[i].fd == r->fd ||
                    ((r->type == R_OUTERR || r->type == R_APPENDERR) &&
                     (f->v[i].fd == 1 || f->v[i].fd == 2))) {
                    if (f->v[i].saved >= 0)
                        lp_close(f->v[i].saved);
                    f->v[i] = f->v[--f->n];
                    i--;
                }
            }
        }
    }
}

/* ── command lookup ────────────────────────────────────────────────── */

typedef struct hent {
    struct hent *next;
    char        *name;
    char        *path;
    int          hits;
} hent_t;

#define HHASH 64
static hent_t *htab[HHASH];

static void hash_clear(void)
{
    for (int i = 0; i < HHASH; i++) {
        hent_t *h = htab[i];
        while (h) {
            hent_t *n = h->next;
            xfree(h->name);
            xfree(h->path);
            xfree(h);
            h = n;
        }
        htab[i] = NULL;
    }
}

static void path_changed(void)
{
    hash_clear();
}

static unsigned hhash(const char *s)
{
    unsigned h = 5381;
    while (*s)
        h = h * 33 + (u8)*s++;
    return h % HHASH;
}

static hent_t *hash_lookup(const char *name)
{
    for (hent_t *h = htab[hhash(name)]; h; h = h->next)
        if (strcmp(h->name, name) == 0)
            return h;
    return NULL;
}

static void hash_add(const char *name, const char *path)
{
    hent_t *h = hash_lookup(name);
    if (h) {
        xfree(h->path);
        h->path = xstrdup(path);
        return;
    }
    h = xcalloc(sizeof *h);
    h->name = xstrdup(name);
    h->path = xstrdup(path);
    unsigned k = hhash(name);
    h->next = htab[k];
    htab[k] = h;
}

static void hash_remove(const char *name)
{
    for (hent_t **pp = &htab[hhash(name)]; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->name, name) == 0) {
            hent_t *h = *pp;
            *pp = h->next;
            xfree(h->name);
            xfree(h->path);
            xfree(h);
            return;
        }
    }
}

static bool is_exec_file(const char *path)
{
    lp_stat_t st;
    if (lp_stat(path, &st, true) != 0)
        return false;
    if ((st.mode & LP_S_IFMT) != LP_S_IFREG)
        return false;
    return lp_access(path, X_OK) == 0;
}

/* Look name up along path (PATH when NULL). out receives the full path.
 * With use_hash the remembered location is tried first and a fresh find
 * is remembered, the way `hash` describes it. */
static bool path_search(const char *name, char *out, size_t size,
                        const char *path, bool use_hash)
{
    if (use_hash) {
        hent_t *h = hash_lookup(name);
        if (h) {
            if (is_exec_file(h->path)) {
                strlcpy(out, h->path, size);
                h->hits++;
                return true;
            }
            hash_remove(name);
        }
    }
    if (!path)
        path = var_get("PATH");
    if (!path)
        path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    const char *p = path;
    for (;;) {
        const char *e = strchr(p, ':');
        size_t dl = e ? (size_t)(e - p) : strlen(p);
        strbuf_t b = {0};
        if (dl == 0)
            sb_putc(&b, '.');
        else
            sb_putn(&b, p, dl);
        sb_putc(&b, '/');
        sb_puts(&b, name);
        bool ok = is_exec_file(sb_str(&b));
        if (ok) {
            strlcpy(out, b.s, size);
            sb_free(&b);
            if (use_hash && dl != 0 && out[0] == '/')
                hash_add(name, out);
            return true;
        }
        sb_free(&b);
        if (!e)
            break;
        p = e + 1;
    }
    return false;
}

/* ── functions ─────────────────────────────────────────────────────── */

#define FHASH 128
static func_t *ftab[FHASH];

static func_t *func_lookup(const char *name)
{
    for (func_t *f = ftab[hhash(name) % FHASH]; f; f = f->next)
        if (strcmp(f->name, name) == 0)
            return f;
    return NULL;
}

static void func_define(node_t *fn)
{
    arena_ref(fn->arena);
    func_t *f = func_lookup(fn->name);
    if (f) {
        arena_t *old = f->body->arena;
        f->body = fn;
        arena_unref(old);
        return;
    }
    f = xcalloc(sizeof *f);
    f->name = xstrdup(fn->name);
    f->body = fn;
    unsigned k = hhash(fn->name) % FHASH;
    f->next = ftab[k];
    ftab[k] = f;
}

static bool func_remove(const char *name)
{
    for (func_t **pp = &ftab[hhash(name) % FHASH]; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->name, name) == 0) {
            func_t *f = *pp;
            *pp = f->next;
            arena_unref(f->body->arena);
            xfree(f->name);
            xfree(f);
            return true;
        }
    }
    return false;
}

static bool opts_saved_local;       /* `local -` in the current function */
static bool *opts_saved_ptr;

static int callfunction(func_t *f, int argc, char **argv, int flags)
{
    if (funcnest >= 4000) {
        sh_error(2, "%s: maximum function nesting level exceeded (%d)",
                 f->name, funcnest);
        return 2;
    }
    node_t *fn = f->body;
    arena_t *ar = fn->arena;
    arena_ref(ar);
    char **save_posv = posv;
    int save_posc = posc;
    posv = NULL;
    posc = 0;
    pos_set(argv + 1, argc - 1);
    int save_loop = loopnest;
    bool save_optlocal = opts_saved_local;
    bool *save_optptr = opts_saved_ptr;
    opts_saved_local = false;
    opts_saved_ptr = NULL;
    loopnest = 0;
    funcnest++;
    int mark = locals_mark();
    evaltree(fn->a, flags & EV_TESTED);
    if (evalskip == SKIP_RETURN || evalskip == SKIP_BREAK ||
        evalskip == SKIP_CONT)
        evalskip = SKIP_NONE;
    locals_restore(mark);
    funcnest--;
    if (opts_saved_local && opts_saved_ptr) {
        for (int i = 0; i < NOPTS; i++)
            if (i != O_interactive)
                opt[i] = opts_saved_ptr[i];
        xfree(opts_saved_ptr);
    }
    opts_saved_local = save_optlocal;
    opts_saved_ptr = save_optptr;
    loopnest = save_loop;
    pos_free();
    posv = save_posv;
    posc = save_posc;
    arena_unref(ar);
    return exitstatus;
}

/* ── exec ──────────────────────────────────────────────────────────── */

static const char *self_exe(void)
{
    static char buf[256];
    long n = lp_readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        return buf;
    }
    if (lp_exists("/bin/lpsh"))
        return "/bin/lpsh";
    return "/bin/sh";
}

/* A file execve refused with ENOEXEC: a script without #!. POSIX says
 * the shell runs it itself; we do that by starting a fresh copy of this
 * shell on it - unless it is plainly not text, in which case running it
 * as a script would only print a screenful of syntax errors. */
static void exec_as_script(const char *path, char **argv, char **env)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd >= 0) {
        char head[256];
        long n = lp_read((int)fd, head, sizeof head);
        lp_close((int)fd);
        for (long i = 0; i < n; i++) {
            if (head[i] == '\n')
                break;
            if (head[i] == '\0') {
                sh_warn("%s: cannot execute binary file: Exec format error",
                        path);
                lp_exit(126);
            }
        }
    }
    int argc = 0;
    while (argv[argc])
        argc++;
    char **nv = xmalloc((size_t)(argc + 2) * sizeof(char *));
    nv[0] = (char *)"sh";
    nv[1] = (char *)path;
    for (int i = 1; i < argc; i++)
        nv[i + 1] = argv[i];
    nv[argc + 1] = NULL;
    lp_execve(self_exe(), nv, env);
    sh_warn("%s: %s", path, lp_strerror(E_NOEXEC));
    lp_exit(126);
}

/* Replace this process with argv[0]. Never returns. */
static void shellexec(char **argv, const char *found) __attribute__((noreturn));
static void shellexec(char **argv, const char *found)
{
    char **env = env_build();
    const char *name = argv[0];
    const char *path = found ? found : name;
    flush_all();
    long r = lp_execve(path, argv, env);
    if (r == -E_NOEXEC)
        exec_as_script(path, argv, env);
    int code = 126;
    if (r == -E_NOENT) {
        code = 127;
        if (strchr(name, '/'))
            sh_warn("%s: No such file or directory", name);
        else
            sh_warn("%s: command not found", name);
    } else if (r == -E_ACCES) {
        lp_stat_t st;
        if (lp_stat(path, &st, true) == 0 &&
            (st.mode & LP_S_IFMT) == LP_S_IFDIR)
            sh_warn("%s: Is a directory", name);
        else
            sh_warn("%s: Permission denied", name);
    } else {
        sh_warn("%s: %s", name, lp_strerror((int)-r));
    }
    lp_exit(code);
}

/* ── xtrace ────────────────────────────────────────────────────────── */

static char *expand_prompt_var(const char *val);

static bool needs_quote(const char *s)
{
    if (!*s)
        return true;
    for (; *s; s++) {
        u8 c = (u8)*s;
        if (c >= 0x80)
            continue;
        if (!(is_name_char(c) || strchr("@%+=:,./-_^", c)))
            return true;
    }
    return false;
}

static void xtrace_word(strbuf_t *b, const char *s)
{
    if (needs_quote(s))
        sb_quoted(b, s);
    else
        sb_puts(b, s);
}

static void xtrace_begin(strbuf_t *b)
{
    const char *ps4 = var_get("PS4");
    if (!ps4)
        ps4 = "+ ";
    char *e = expand_prompt_var(ps4);
    sb_puts(b, e);
    xfree(e);
}

static void xtrace_end(strbuf_t *b)
{
    sb_putc(b, '\n');
    flush_all();
    xwrite_all(2, b->s, b->len);
    sb_free(b);
}

/* ── assignments ───────────────────────────────────────────────────── *
 *
 * The assignments in front of a command are expanded once, in this
 * process, before anything is run - as dash does. What happens to the
 * results depends on the command: they stay (no command, or a special
 * builtin), last for the command (a builtin or function), or go only
 * into the environment of a program. Expanding first and applying later
 * is what keeps `x=$((i+=1)) prog` from incrementing i twice or not at
 * all, and lets xtrace show the values without running $(...) again. */

typedef struct {
    strbuf_t name;
    word_t  *sub;           /* name[sub]= */
    bool     append;        /* += */
    word_t   val;           /* the value, as a word of its own */
    wpart_t  first;         /* storage for val's first part */
    wpart_t *array;         /* name=(...) */
} assign_t;

/* Split an assignment word into its name and value. The parser has
 * already checked the shape (word_is_assign). */
static void assign_parse(word_t *w, assign_t *a)
{
    memset(a, 0, sizeof *a);
    wpart_t *p = w->parts;
    size_t i = 0;
    const char *s = p->text;
    while (i < p->len && is_name_char((u8)s[i]))
        i++;
    sb_putn(&a->name, s, i);
    sb_str(&a->name);
    if (i < p->len && s[i] == '[' && p->next && p->next->sub) {
        a->sub = p->next->sub;
        p = p->next->next;          /* the part holding "]=..." */
        if (!p)
            return;
        s = p->text;
        i = 1;                      /* past ']' */
    }
    if (i < p->len && s[i] == '+') {
        a->append = true;
        i++;
    }
    i++;                            /* past '=' */
    wpart_t *rest = p->next;
    if (i < p->len) {
        a->first = *p;
        a->first.text = p->text + i;
        a->first.len = p->len - i;
        a->first.next = rest;
        a->val.parts = &a->first;
    } else {
        a->val.parts = rest;
    }
    if (a->val.parts && a->val.parts->type == WP_ARRAY &&
        !a->val.parts->next)
        a->array = a->val.parts;
}

typedef struct {
    char    *name;
    bool     append;
    bool     has_idx;
    long long idx;
    bool     is_array;
    strvec_t vals;          /* is_array */
    char    *val;           /* otherwise */
} xassign_t;

static void xassigns_free(xassign_t *v, int n)
{
    for (int i = 0; i < n; i++) {
        xfree(v[i].name);
        xfree(v[i].val);
        sv_free(&v[i].vals);
    }
    xfree(v);
}

/* Expand every assignment word of a command. trace, when given, gets
 * the "name=value " text for xtrace. */
static bool expand_assigns(word_t *w, xassign_t **out, int *nout,
                           strbuf_t *trace)
{
    *out = NULL;
    *nout = 0;
    int n = 0;
    for (word_t *x = w; x; x = x->next)
        n++;
    if (!n)
        return true;
    xassign_t *v = xcalloc((size_t)n * sizeof *v);
    int k = 0;
    for (; w; w = w->next, k++) {
        assign_t a;
        assign_parse(w, &a);
        xassign_t *x = &v[k];
        x->name = sb_take(&a.name);
        x->append = a.append;
        if (a.array) {
            x->is_array = true;
            for (word_t *e = a.array->list; e; e = e->next) {
                word_t one = *e;
                one.next = NULL;
                if (!expand_words(&one, &x->vals, X_SPLIT | X_GLOB | X_TILDE)) {
                    xassigns_free(v, k + 1);
                    return false;
                }
            }
            if (trace) {
                sb_puts(trace, x->name);
                sb_puts(trace, a.append ? "+=(" : "=(");
                for (int i = 0; i < x->vals.n; i++) {
                    if (i) sb_putc(trace, ' ');
                    xtrace_word(trace, x->vals.v[i]);
                }
                sb_puts(trace, ") ");
            }
            continue;
        }
        if (a.sub) {
            char *idxs = expand_str(a.sub, 0);
            if (!idxs || expand_error || !arith_eval(idxs, &x->idx)) {
                xfree(idxs);
                xassigns_free(v, k + 1);
                expand_error = true;
                return false;
            }
            xfree(idxs);
            x->has_idx = true;
        }
        x->val = expand_str(&a.val, X_TILDE | X_ASSIGN);
        if (!x->val || expand_error) {
            xassigns_free(v, k + 1);
            return false;
        }
        if (trace) {
            sb_puts(trace, x->name);
            if (x->has_idx)
                sb_printf(trace, "[%lld]", x->idx);
            sb_puts(trace, a.append ? "+=" : "=");
            xtrace_word(trace, x->val);
            sb_putc(trace, ' ');
        }
    }
    *out = v;
    *nout = n;
    return true;
}

static bool apply_assign(xassign_t *x, int vflags)
{
    const char *name = x->name;
    bool ok;
    if (x->is_array) {
        ok = var_set_array(name, &x->vals, x->append);
        if (ok && vflags)
            ok = var_set(name, NULL, vflags);
        return ok;
    }
    var_t *v = var_lookup(name);
    bool integer = v && (v->flags & V_INTEGER);
    char nb[24];
    const char *val = x->val;
    if (integer) {
        long long a = 0, b = 0;
        if (!arith_eval(val, &b))
            return false;
        if (x->append) {
            const char *old = x->has_idx ? var_elem(v, x->idx) : var_get(name);
            arith_eval(old && *old ? old : "0", &a);
        }
        val = itoa_s(a + b, nb);
    } else if (x->append) {
        const char *old = x->has_idx ? var_elem(v, x->idx) : var_get(name);
        strbuf_t b = {0};
        sb_puts(&b, old ? old : "");
        sb_puts(&b, val);
        ok = x->has_idx ? var_set_elem(name, x->idx, b.s)
                        : var_set(name, b.s, vflags);
        sb_free(&b);
        if (ok && x->has_idx && vflags)
            ok = var_set(name, NULL, vflags);
        return ok;
    }
    if (x->has_idx) {
        ok = var_set_elem(name, x->idx, val);
        if (ok && vflags)
            ok = var_set(name, NULL, vflags);
        return ok;
    }
    return var_set(name, val, vflags);
}

/* ── simple commands ───────────────────────────────────────────────── */

static bool is_decl_builtin(const char *s)
{
    return strcmp(s, "export") == 0 || strcmp(s, "readonly") == 0 ||
           strcmp(s, "local") == 0 || strcmp(s, "declare") == 0 ||
           strcmp(s, "typeset") == 0;
}

static bool word_is_assign(word_t *w);

/* Array literals handed to a declaration builtin (local a=(1 2)): the
 * builtin sees just the name, and the elements are assigned around it -
 * after `local` has made the name local, before `readonly` locks it. */
typedef struct {
    xassign_t x;
    bool      before;
} declarr_t;

/* Expand the words of a simple command into argv. The operands of
 * export/readonly/local/declare that look like assignments are expanded
 * as assignments - no field splitting, tilde after = and : - which is
 * what POSIX 2024 and every current shell do, and what makes
 * `local x=$y` safe when $y has spaces. */
static bool expand_command_words(word_t *words, strvec_t *argv,
                                 declarr_t **arrs, int *narr)
{
    *narr = 0;
    word_t *w = words;
    if (!w)
        return true;
    word_t one = *w;
    one.next = NULL;
    if (!expand_words(&one, argv, X_SPLIT | X_GLOB | X_TILDE))
        return false;
    w = w->next;
    bool decl = argv->n > 0 && words->lit && is_decl_builtin(argv->v[0]);
    for (; w; w = w->next) {
        if (decl && word_is_assign(w)) {
            word_t solo = *w;
            solo.next = NULL;
            xassign_t *x;
            int nx;
            if (!expand_assigns(&solo, &x, &nx, NULL))
                return false;
            if (x->is_array || x->has_idx) {
                *arrs = xrealloc(*arrs, (size_t)(*narr + 1) * sizeof **arrs);
                (*arrs)[*narr].x = *x;
                (*arrs)[*narr].before =
                    strcmp(argv->v[0], "export") == 0 ||
                    strcmp(argv->v[0], "readonly") == 0;
                (*narr)++;
                sv_push(argv, xstrdup(x->name));
                xfree(x);
                continue;
            }
            strbuf_t b = {0};
            sb_puts(&b, x->name);
            sb_puts(&b, x->append ? "+=" : "=");
            sb_puts(&b, x->val);
            sv_push(argv, sb_take(&b));
            xassigns_free(x, nx);
            continue;
        }
        word_t one2 = *w;
        one2.next = NULL;
        if (!expand_words(&one2, argv, X_SPLIT | X_GLOB | X_TILDE))
            return false;
    }
    return true;
}

static void declarrs_free(declarr_t *a, int n)
{
    for (int i = 0; i < n; i++) {
        xfree(a[i].x.name);
        xfree(a[i].x.val);
        sv_free(&a[i].x.vals);
    }
    xfree(a);
}

static int  run_builtin(const builtin_t *b, int argc, char **argv);

static void set_underscore(strvec_t *args)
{
    if (args->n > 0 && !in_subshell)
        var_set("_", args->v[args->n - 1], 0);
}

/* The error side of an expansion or assignment failure: a
 * non-interactive shell exits, an interactive one abandons the command
 * and goes back to the prompt. */
static int command_error(int status)
{
    exitstatus = status;
    if (!toplevel_interactive || in_subshell)
        exitshell(status);
    evalskip = SKIP_ABORT;
    return status;
}

static int evalcommand(node_t *n, int flags)
{
    strvec_t args = {0};
    declarr_t *arrs = NULL;
    int narr = 0;
    xassign_t *asg = NULL;
    int nasg = 0;
    int status = 0;
    cmd_lineno = n->lineno;
    input_sync_fd0();

    /* 1. the words, then the assignments - both in this process */
    exitstatus = 0;                 /* $(...) in them sets it */
    bool ok = expand_command_words(n->words, &args, &arrs, &narr);
    strbuf_t tr = {0};
    bool tracing = opt[O_xtrace];
    if (ok)
        ok = expand_assigns(n->assigns, &asg, &nasg, tracing ? &tr : NULL);
    if (!ok) {
        sv_free(&args);
        declarrs_free(arrs, narr);
        sb_free(&tr);
        return command_error(2);
    }
    int sub_status = exitstatus;

    /* 2. `command` and `builtin` are unwrapped here, so that what they
     * run is dispatched exactly the way a command would be */
    bool skip_func = false, force_builtin = false, via_command = false;
    int argi = 0;
    while (argi < args.n) {
        const char *a0 = args.v[argi];
        if (strcmp(a0, "command") == 0 && !func_lookup("command")) {
            int j = argi + 1;
            while (j < args.n && strcmp(args.v[j], "-p") == 0)
                j++;
            if (j < args.n && strcmp(args.v[j], "--") == 0)
                j++;
            if (j >= args.n || (args.v[j][0] == '-' && args.v[j][1] &&
                                j == argi + 1))
                break;          /* `command -v`, or nothing: the builtin */
            argi = j;
            skip_func = true;
            via_command = true;
            continue;
        }
        if (strcmp(a0, "builtin") == 0 && argi + 1 < args.n &&
            !func_lookup("builtin")) {
            argi++;
            force_builtin = true;
            skip_func = true;
            continue;
        }
        break;
    }
    int argc = args.n - argi;
    char **argv = args.v + argi;

    if (tracing) {
        for (int i = 0; i < argc; i++) {
            xtrace_word(&tr, argv[i]);
            sb_putc(&tr, ' ');
        }
        if (tr.len || n->redirs) {
            strbuf_t line = {0};
            xtrace_begin(&line);
            if (tr.len && tr.s[tr.len - 1] == ' ')
                tr.len--;
            sb_putn(&line, tr.s ? tr.s : "", tr.len);
            xtrace_end(&line);
        }
        sb_free(&tr);
    }

    /* 3. no command: assignments to the shell itself; redirections are
     * performed and undone (`> file` makes an empty file) */
    if (argc == 0) {
        for (int i = 0; i < nasg && ok; i++)
            ok = apply_assign(&asg[i], 0);
        xassigns_free(asg, nasg);
        sv_free(&args);
        declarrs_free(arrs, narr);
        if (!ok)
            return command_error(2);
        status = sub_status;
        if (n->redirs) {
            if (!redirect_push(n->redirs, true))
                status = 1;
            else
                redirect_pop();
        }
        return status;
    }

    /* 4. what is it */
    const char *name = argv[0];
    const builtin_t *bi = NULL;
    func_t *fn = NULL;
    int kind;
    char found[1024];
    found[0] = '\0';
    if (strchr(name, '/')) {
        kind = CMD_EXTERNAL;
    } else {
        bi = builtin_find(name);
        if (force_builtin) {
            if (!bi) {
                sh_warn("builtin: %s: not a shell builtin", name);
                xassigns_free(asg, nasg);
                sv_free(&args);
                declarrs_free(arrs, narr);
                return 1;
            }
            kind = CMD_BUILTIN;
        } else if (bi && bi->kind == CMD_SPECIAL) {
            kind = via_command ? CMD_BUILTIN : CMD_SPECIAL;
        } else if (!skip_func && (fn = func_lookup(name))) {
            kind = CMD_FUNC;
        } else if (bi && bi->kind == CMD_BUILTIN) {
            kind = CMD_BUILTIN;
        } else if (path_search(name, found, sizeof found, NULL, true)) {
            kind = CMD_EXTERNAL;
        } else if (bi) {
            kind = CMD_BUILTIN;         /* a fallback builtin */
        } else {
            kind = CMD_NONE;
        }
    }

    /* 5. a program */
    if (kind == CMD_EXTERNAL || kind == CMD_NONE) {
        if (kind == CMD_NONE) {
            /* not found: said here, without a fork */
            bool shown = false;
            if (n->redirs) {
                if (redirect_push(n->redirs, true)) {
                    sh_warn("%s: command not found", name);
                    redirect_pop();
                }
                shown = true;
            }
            if (!shown)
                sh_warn("%s: command not found", name);
            set_underscore(&args);
            xassigns_free(asg, nasg);
            sv_free(&args);
            declarrs_free(arrs, narr);
            if (flags & EV_EXIT)
                exitshell(127);
            return 127;
        }
        job_t *j = NULL;
        int pid = 0;
        bool direct = (flags & EV_EXIT) && !have_traps();
        if (!direct) {
            j = job_new(n, false);
            pid = forkjob(j, FORK_FG);
            if (pid < 0) {
                job_free(j);
                xassigns_free(asg, nasg);
                sv_free(&args);
                declarrs_free(arrs, narr);
                return 2;
            }
        }
        if (direct || pid == 0) {
            /* the child, or the last command of a process that ends */
            for (int i = 0; i < nasg; i++)
                if (!apply_assign(&asg[i], V_EXPORT))
                    lp_exit(2);
            if (n->redirs && !redirect_push(n->redirs, false))
                lp_exit(1);
            shellexec(argv, found[0] ? found : NULL);
        }
        status = waitforjob(j);
        set_underscore(&args);
        xassigns_free(asg, nasg);
        sv_free(&args);
        declarrs_free(arrs, narr);
        return status;
    }

    /* 6. a builtin or a function, in this process */
    bool special = kind == CMD_SPECIAL;
    bool is_exec = special && strcmp(name, "exec") == 0;
    bool scoped = !special;

    if (scoped)
        var_tmp_scope_begin();
    for (int i = 0; i < nasg && ok; i++) {
        if (scoped)
            var_local(asg[i].name, NULL, 0, false);
        ok = apply_assign(&asg[i], kind == CMD_FUNC ? V_EXPORT : 0);
    }
    xassigns_free(asg, nasg);
    if (!ok) {
        if (scoped)
            var_tmp_scope_end();
        sv_free(&args);
        declarrs_free(arrs, narr);
        return command_error(2);
    }

    bool pushed = false;
    if (n->redirs) {
        if (is_exec && argc == 1) {
            if (!redirect_push(n->redirs, false)) {
                sv_free(&args);
                declarrs_free(arrs, narr);
                if (!toplevel_interactive || in_subshell)
                    exitshell(2);
                return 2;
            }
            redirect_forget(n->redirs);
        } else {
            if (!redirect_push(n->redirs, true)) {
                if (scoped)
                    var_tmp_scope_end();
                sv_free(&args);
                declarrs_free(arrs, narr);
                if (special && (!toplevel_interactive || in_subshell))
                    exitshell(2);
                return special ? 2 : 1;
            }
            pushed = true;
        }
    }

    for (int i = 0; i < narr; i++)
        if (arrs[i].before)
            apply_assign(&arrs[i].x, 0);

    if (kind == CMD_FUNC)
        status = callfunction(fn, argc, argv, flags);
    else
        status = run_builtin(bi, argc, argv);

    for (int i = 0; i < narr; i++)
        if (!arrs[i].before)
            apply_assign(&arrs[i].x, 0);

    if (pushed)
        redirect_pop();
    else
        flush_all();
    if (scoped)
        var_tmp_scope_end();
    set_underscore(&args);
    sv_free(&args);
    declarrs_free(arrs, narr);
    return status;
}

/* Run a builtin with its output going through out1/out2, and turn a
 * failed write - `echo hi >&-`, a full disk - into an error, not a
 * silent success. */
static int run_builtin(const builtin_t *b, int argc, char **argv)
{
    out1->err = out2->err = false;
    int st = b->fn(argc, argv);
    flush_out(out1);
    if (out1->err) {
        out1->err = false;
        sh_warn("%s: write error: %s", argv[0], lp_strerror(out1->errnum));
        if (st == 0)
            st = 1;
    }
    flush_out(out2);
    return st;
}

/* $PIPESTATUS, bash's: the status of every stage of the last
 * foreground pipeline. */
static void pipestatus_set(const int *st, int n)
{
    strvec_t v = {0};
    char buf[24];
    for (int i = 0; i < n; i++)
        sv_push(&v, xstrdup(itoa_s(st[i], buf)));
    var_t *pv = var_lookup("PIPESTATUS");
    if (!pv || !(pv->flags & V_READONLY))
        var_set_array("PIPESTATUS", &v, false);
    sv_free(&v);
    pipestatus_done = true;
}

/* ── compound commands ─────────────────────────────────────────────── */

static int evalloop(node_t *n, int flags)
{
    int status = 0;
    loopnest++;
    for (;;) {
        int c = evaltree(n->a, EV_TESTED);
        if (evalskip) {
            if ((evalskip == SKIP_BREAK || evalskip == SKIP_CONT)) {
                bool brk = evalskip == SKIP_BREAK;
                if (--skipcount <= 0) {
                    evalskip = SKIP_NONE;
                    if (brk) break;
                    continue;
                }
                evalskip = SKIP_BREAK;
            }
            break;
        }
        if (n->type == N_WHILE ? c != 0 : c == 0)
            break;
        status = evaltree(n->b, flags & ~EV_EXIT);
        if (evalskip) {
            if (evalskip == SKIP_BREAK || evalskip == SKIP_CONT) {
                bool brk = evalskip == SKIP_BREAK;
                if (--skipcount <= 0) {
                    evalskip = SKIP_NONE;
                    if (brk) break;
                    continue;
                }
                evalskip = SKIP_BREAK;
            }
            break;
        }
    }
    loopnest--;
    return status;
}

static int evalfor(node_t *n, int flags)
{
    strvec_t items = {0};
    if (n->flags) {
        if (!expand_words(n->words, &items, X_SPLIT | X_GLOB | X_TILDE)) {
            sv_free(&items);
            return command_error(2);
        }
    } else {
        for (int i = 0; i < posc; i++)
            sv_push(&items, xstrdup(posv[i]));
    }
    int status = 0;
    loopnest++;
    for (int i = 0; i < items.n; i++) {
        if (!var_set(n->name, items.v[i], 0)) {
            status = command_error(2);
            break;
        }
        status = evaltree(n->b, flags & ~EV_EXIT);
        if (evalskip) {
            if (evalskip == SKIP_BREAK || evalskip == SKIP_CONT) {
                bool brk = evalskip == SKIP_BREAK;
                if (--skipcount <= 0) {
                    evalskip = SKIP_NONE;
                    if (brk) break;
                    continue;
                }
                evalskip = SKIP_BREAK;
            }
            break;
        }
    }
    loopnest--;
    sv_free(&items);
    return status;
}

/* for ((init; cond; step)): the three expressions are split out of the
 * word at its top-level semicolons and each is expanded afresh every time
 * it is evaluated, so `i<$n` sees $n change. */
static void split_arith3(word_t *w, word_t **out)
{
    wbuild_t b[3];
    int k = 0;
    for (int i = 0; i < 3; i++)
        wb_init(&b[i]);
    for (wpart_t *p = w->parts; p; p = p->next) {
        if (p->type != WP_LIT) {
            if (k < 3) {
                wpart_t *c = pa_alloc(sizeof *c);
                *c = *p;
                c->next = NULL;
                wb_part(&b[k], c);
            }
            continue;
        }
        for (size_t i = 0; i < p->len; i++) {
            if (p->text[i] == ';' && k < 2) {
                k++;
                continue;
            }
            if (k < 3)
                wb_char(&b[k], p->text[i], p->quoted);
        }
    }
    for (int i = 0; i < 3; i++)
        out[i] = wb_finish(&b[i]);
}

static bool arith_word(word_t *w, long long *v, bool empty_true)
{
    char *s = expand_str(w, 0);
    if (!s || expand_error) {
        xfree(s);
        return false;
    }
    const char *p = s;
    while (is_blank(*p) || *p == '\n') p++;
    bool ok;
    if (!*p) {
        *v = empty_true ? 1 : 0;
        ok = true;
    } else {
        ok = arith_eval(s, v);
    }
    xfree(s);
    return ok;
}

static int evalforarith(node_t *n, int flags)
{
    arena_t *save = cur_arena;
    arena_t *tmp = arena_new();
    cur_arena = tmp;
    word_t *e[3];
    split_arith3(n->words, e);
    cur_arena = save;
    int status = 0;
    long long v;
    loopnest++;
    if (!arith_word(e[0], &v, false))
        goto fail;
    for (;;) {
        if (!arith_word(e[1], &v, true))
            goto fail;
        if (!v)
            break;
        status = evaltree(n->b, flags & ~EV_EXIT);
        if (evalskip) {
            if (evalskip == SKIP_BREAK || evalskip == SKIP_CONT) {
                bool brk = evalskip == SKIP_BREAK;
                if (--skipcount <= 0) {
                    evalskip = SKIP_NONE;
                    if (brk) break;
                    goto step;
                }
                evalskip = SKIP_BREAK;
            }
            break;
        }
step:
        if (!arith_word(e[2], &v, false))
            goto fail;
    }
    loopnest--;
    arena_unref(tmp);
    return status;
fail:
    loopnest--;
    arena_unref(tmp);
    return command_error(1);
}

static int evalcase(node_t *n, int flags)
{
    char *subj = expand_str(n->words, X_TILDE);
    if (!subj || expand_error) {
        xfree(subj);
        return command_error(2);
    }
    int status = 0;
    caseitem_t *ci = n->items;
    for (; ci; ci = ci->next) {
        bool hit = false;
        for (word_t *p = ci->pats; p && !hit; p = p->next) {
            char *pat = expand_str(p, X_PATTERN | X_TILDE);
            if (!pat || expand_error) {
                xfree(pat);
                xfree(subj);
                return command_error(2);
            }
            hit = pmatch(pat, subj, false);
            xfree(pat);
        }
        if (!hit)
            continue;
        for (;;) {
            status = ci->body ? evaltree(ci->body, flags & ~EV_EXIT) : 0;
            if (evalskip || ci->term == CT_BREAK)
                goto done;
            if (ci->term == CT_FALL) {
                ci = ci->next;
                if (!ci) goto done;
                continue;
            }
            break;          /* ;;& - go on testing the next patterns */
        }
    }
done:
    xfree(subj);
    return status;
}

static int evalsubshell(node_t *n, int flags)
{
    if ((flags & EV_EXIT) && !have_traps()) {
        in_subshell = true;
        if (n->redirs && !redirect_push(n->redirs, false))
            exitshell(1);
        evaltree(n->a, flags);
        exitshell(exitstatus);
    }
    job_t *j = job_new(n, false);
    int pid = forkjob(j, FORK_FG);
    if (pid < 0) {
        job_free(j);
        return 2;
    }
    if (pid == 0) {
        if (n->redirs && !redirect_push(n->redirs, false))
            exitshell(1);
        evaltree(n->a, EV_EXIT | (flags & EV_TESTED));
        exitshell(exitstatus);
    }
    return waitforjob(j);
}

static int evalpipe(node_t *n, int flags)
{
    bool bg = (flags & EV_BG) != 0;
    job_t *j = job_new(n, bg);
    int prev = -1;
    for (node_t *c = n->a; c; c = c->next) {
        int pip[2] = { -1, -1 };
        if (c->next) {
            long r = lp_pipe(pip);
            if (r < 0) {
                if (prev >= 0) lp_close(prev);
                sh_perror("pipe", r);
                break;
            }
        }
        int pid = forkjob(j, bg ? FORK_BG : FORK_FG);
        if (pid < 0) {
            if (prev >= 0) lp_close(prev);
            if (pip[0] >= 0) { lp_close(pip[0]); lp_close(pip[1]); }
            break;
        }
        if (pid == 0) {
            if (prev >= 0) {
                if (prev != 0) {
                    lp_dup2(prev, 0);
                    lp_close(prev);
                }
            }
            if (pip[1] >= 0) {
                lp_close(pip[0]);
                if (pip[1] != 1) {
                    lp_dup2(pip[1], 1);
                    lp_close(pip[1]);
                }
            }
            evaltree(c, EV_EXIT);
            exitshell(exitstatus);
        }
        if (prev >= 0)
            lp_close(prev);
        if (pip[1] >= 0)
            lp_close(pip[1]);
        prev = pip[0];
    }
    if (prev >= 0)
        lp_close(prev);
    if (j->nproc == 0) {
        job_free(j);
        return 2;
    }
    if (bg) {
        lastbgpid = j->procs[j->nproc - 1].pid;
        return 0;
    }
    int status = waitforjob(j);
    return status;
}

static int evalbg(node_t *n, int flags)
{
    (void)flags;
    node_t *c = n->a;
    job_t *j;
    if (c->type == N_PIPE) {
        evalpipe(c, EV_BG);
        j = NULL;
        for (job_t *x = jobs; x; x = x->next)
            if (x->nproc && x->procs[x->nproc - 1].pid == lastbgpid)
                j = x;
    } else {
        j = job_new(c, true);
        int pid = forkjob(j, FORK_BG);
        if (pid < 0) {
            job_free(j);
            return 2;
        }
        if (pid == 0) {
            evaltree(c, EV_EXIT);
            exitshell(exitstatus);
        }
        lastbgpid = pid;
    }
    if (j) {
        j->bg = true;
        if (opt[O_interactive] && !in_subshell && toplevel_interactive) {
            outf(out2, "[%d] %d\n", j->id, lastbgpid);
            flush_out(out2);
        }
    }
    if (!opt[O_interactive])
        jobs_trim();
    return 0;
}

/* ── [[ ... ]] ─────────────────────────────────────────────────────── */

static char *expand_regex(word_t *w);
static int   test_unary_op(const char *op, const char *arg);
static int   test_binary_op(const char *l, const char *op, const char *r);

static int db_eval(dbx_t *x)
{
    switch (x->kind) {
    case DB_AND: {
        int l = db_eval(x->l);
        if (l != 0) return l;
        return db_eval(x->r);
    }
    case DB_OR: {
        int l = db_eval(x->l);
        if (l == 0 || l == 2) return l;
        return db_eval(x->r);
    }
    case DB_NOT: {
        int l = db_eval(x->l);
        return l == 2 ? 2 : !l;
    }
    case DB_WORD: {
        char *s = expand_str(x->w1, X_TILDE);
        if (!s) return 2;
        int r = *s ? 0 : 1;
        xfree(s);
        return r;
    }
    case DB_UNARY: {
        char *s = expand_str(x->w1, X_TILDE);
        if (!s) return 2;
        int r;
        if (strcmp(x->op, "-v") == 0) {
            /* -v name, or -v name[subscript] */
            char *br = strchr(s, '[');
            size_t sl = strlen(s);
            if (br && sl > 2 && s[sl - 1] == ']') {
                *br = '\0';
                s[sl - 1] = '\0';
                long long idx = 0;
                if (!arith_eval(br + 1, &idx))
                    idx = -1000000000;
                r = var_elem(var_lookup(s), idx) ? 0 : 1;
            } else {
                r = var_get(s) ? 0 : 1;
            }
        } else if (strcmp(x->op, "-o") == 0) {
            int o = opt_index(s);
            r = o >= 0 && opt[o] ? 0 : 1;
        } else {
            r = test_unary_op(x->op, s);
        }
        xfree(s);
        return r;
    }
    case DB_BINARY: {
        char *l = expand_str(x->w1, X_TILDE);
        if (!l) return 2;
        int r;
        const char *op = x->op;
        if (strcmp(op, "==") == 0 || strcmp(op, "=") == 0 ||
            strcmp(op, "!=") == 0) {
            char *pat = expand_str(x->w2, X_PATTERN | X_TILDE);
            if (!pat) { xfree(l); return 2; }
            bool m = pmatch(pat, l, false);
            r = (op[0] == '!') ? (m ? 1 : 0) : (m ? 0 : 1);
            xfree(pat);
        } else if (strcmp(op, "=~") == 0) {
            char *re = expand_regex(x->w2);
            if (!re) { xfree(l); return 2; }
            const char *err = NULL;
            lpre *rx = re_compile(re, true, false, &err);
            if (!rx) {
                r = 2;
            } else {
                int caps[RE_MAX_CAPS];
                for (int i = 0; i < RE_MAX_CAPS; i++) caps[i] = -1;
                bool m = re_search(rx, l, 0, false, caps);
                r = m ? 0 : 1;
                strvec_t groups = {0};
                if (m) {
                    int ng = re_ngroups(rx);
                    for (int i = 0; i <= ng && i < RE_MAX_GROUPS; i++) {
                        int s0 = caps[2 * i], s1 = caps[2 * i + 1];
                        if (s0 >= 0 && s1 >= s0)
                            sv_push(&groups, xstrndup(l + s0, (size_t)(s1 - s0)));
                        else
                            sv_push(&groups, xstrdup(""));
                    }
                }
                var_t *bv = var_lookup("BASH_REMATCH");
                if (!bv || !(bv->flags & V_READONLY))
                    var_set_array("BASH_REMATCH", &groups, false);
                sv_free(&groups);
                re_free(rx);
            }
            xfree(re);
        } else if (strcmp(op, "<") == 0 || strcmp(op, ">") == 0) {
            char *rs = expand_str(x->w2, X_TILDE);
            if (!rs) { xfree(l); return 2; }
            int c = strcmp(l, rs);
            r = (op[0] == '<' ? c < 0 : c > 0) ? 0 : 1;
            xfree(rs);
        } else if (op[0] == '-' && (strcmp(op, "-nt") && strcmp(op, "-ot") &&
                                    strcmp(op, "-ef"))) {
            /* -eq and friends: both sides are arithmetic in [[ ]] */
            char *rs = expand_str(x->w2, X_TILDE);
            if (!rs) { xfree(l); return 2; }
            long long a = 0, b = 0;
            if (!arith_eval(*l ? l : "0", &a) || !arith_eval(*rs ? rs : "0", &b)) {
                xfree(rs); xfree(l);
                return 2;
            }
            bool t = strcmp(op, "-eq") == 0 ? a == b
                   : strcmp(op, "-ne") == 0 ? a != b
                   : strcmp(op, "-lt") == 0 ? a < b
                   : strcmp(op, "-le") == 0 ? a <= b
                   : strcmp(op, "-gt") == 0 ? a > b : a >= b;
            r = t ? 0 : 1;
            xfree(rs);
        } else {
            char *rs = expand_str(x->w2, X_TILDE);
            if (!rs) { xfree(l); return 2; }
            r = test_binary_op(l, op, rs);
            xfree(rs);
        }
        xfree(l);
        return r;
    }
    }
    return 2;
}

static int evaldbrack(node_t *n)
{
    cmd_lineno = n->lineno;
    if (opt[O_xtrace]) {
        strbuf_t b = {0};
        xtrace_begin(&b);
        sb_puts(&b, "[[ ... ]]");
        xtrace_end(&b);
    }
    expand_error = false;
    int r = db_eval(n->dbx);
    if (expand_error)
        return command_error(2);
    return r == 2 ? 2 : r;
}

static int evalarith(node_t *n)
{
    cmd_lineno = n->lineno;
    long long v = 0;
    if (opt[O_xtrace]) {
        strbuf_t b = {0};
        xtrace_begin(&b);
        char *s = expand_str(n->words, 0);
        sb_puts(&b, "(( ");
        sb_puts(&b, s ? s : "");
        sb_puts(&b, " ))");
        xfree(s);
        xtrace_end(&b);
    }
    if (!arith_word(n->words, &v, false))
        return 1;
    return v ? 0 : 1;
}

static int evaltime(node_t *n, int flags)
{
    s64 t0 = now_ms();
    long tb[4] = {0}, ta[4] = {0};
    sys_times(tb);
    int st = evaltree(n->a, flags & ~EV_EXIT);
    s64 t1 = now_ms();
    sys_times(ta);
    long hz = 100;
    long u = (ta[0] + ta[2]) - (tb[0] + tb[2]);
    long s = (ta[1] + ta[3]) - (tb[1] + tb[3]);
    s64 real = t1 - t0;
    outf(out2, "\nreal\t%dm%d.%03ds\nuser\t%ldm%ld.%03lds\nsys\t%ldm%ld.%03lds\n",
         (int)(real / 60000), (int)((real / 1000) % 60), (int)(real % 1000),
         u / hz / 60, (u / hz) % 60, (u % hz) * (1000 / hz),
         s / hz / 60, (s / hz) % 60, (s % hz) * (1000 / hz));
    flush_out(out2);
    exitstatus = st;
    return st;
}

/* ── evaltree ──────────────────────────────────────────────────────── */

static void check_errexit(int status, int flags)
{
    if (!status || (flags & EV_TESTED) || evalskip)
        return;
    if (trap_err && !in_trap) {
        run_trap_cmd(trap_err);
        exitstatus = status;
    }
    if (opt[O_errexit])
        exitshell(status);
}

static int evaltree(node_t *n, int flags)
{
    if (pending_signals)
        run_pending_traps();
    if (evalskip)
        return exitstatus;
    if (!n) {
        exitstatus = 0;
        if (flags & EV_EXIT)
            exitshell(0);
        return 0;
    }
    int status = 0;
    bool pushed = false;
    switch (n->type) {
    case N_SEQ:
        evaltree(n->a, flags & ~EV_EXIT);
        if (evalskip)
            return exitstatus;
        status = evaltree(n->b, flags);
        break;
    case N_AND:
        status = evaltree(n->a, (flags & ~EV_EXIT) | EV_TESTED);
        if (evalskip)
            return exitstatus;
        if (status == 0)
            status = evaltree(n->b, flags);
        break;
    case N_OR:
        status = evaltree(n->a, (flags & ~EV_EXIT) | EV_TESTED);
        if (evalskip)
            return exitstatus;
        if (status != 0)
            status = evaltree(n->b, flags);
        break;
    case N_NOT:
        status = evaltree(n->a, (flags & ~EV_EXIT) | EV_TESTED);
        if (evalskip)
            return exitstatus;
        status = !status;
        break;
    case N_CMD:
        pipestatus_done = false;
        status = evalcommand(n, flags);
        if (!pipestatus_done)
            pipestatus_set(&status, 1);
        exitstatus = status;
        check_errexit(status, flags);
        break;
    case N_PIPE:
        status = evalpipe(n, flags & ~EV_EXIT);
        exitstatus = status;
        check_errexit(status, flags);
        break;
    case N_BG:
        status = evalbg(n, flags);
        break;
    case N_SUBSHELL:
        status = evalsubshell(n, flags);
        exitstatus = status;
        check_errexit(status, flags);
        break;
    case N_FUNC:
        func_define(n);
        status = 0;
        break;
    case N_DBRACK:
        status = evaldbrack(n);
        exitstatus = status;
        check_errexit(status, flags);
        break;
    case N_ARITH:
        status = evalarith(n);
        exitstatus = status;
        check_errexit(status, flags);
        break;
    case N_TIME:
        status = evaltime(n, flags);
        break;
    default:
        /* the compound commands that run in this process and may carry
         * redirections of their own */
        if (n->redirs) {
            if (!redirect_push(n->redirs, true)) {
                status = 2;
                exitstatus = status;
                if (!toplevel_interactive || in_subshell)
                    exitshell(status);
                return status;
            }
            pushed = true;
        }
        switch (n->type) {
        case N_GROUP:
            status = evaltree(n->a, flags & ~EV_EXIT);
            break;
        case N_IF: {
            int c = evaltree(n->a, EV_TESTED);
            if (evalskip) {
                status = exitstatus;
                break;
            }
            if (c == 0)
                status = evaltree(n->b, flags & ~EV_EXIT);
            else if (n->c)
                status = evaltree(n->c, flags & ~EV_EXIT);
            else
                status = 0;
            break;
        }
        case N_WHILE:
        case N_UNTIL:
            status = evalloop(n, flags);
            break;
        case N_FOR:
            status = evalfor(n, flags);
            break;
        case N_FORARITH:
            status = evalforarith(n, flags);
            break;
        case N_CASE:
            status = evalcase(n, flags);
            break;
        default:
            status = 0;
            break;
        }
        if (pushed)
            redirect_pop();
        break;
    }
    if (!(evalskip && (evalskip == SKIP_RETURN || evalskip == SKIP_ABORT)))
        exitstatus = status;
    if (pending_signals)
        run_pending_traps();
    if (flags & EV_EXIT)
        exitshell(exitstatus);
    return exitstatus;
}

/* ── command substitution ──────────────────────────────────────────── */

static char *cmdsub_capture(node_t *tree, int *status)
{
    *status = 0;
    if (!tree)
        return xstrdup("");
    /* $(< file): read the file without a fork, as bash does */
    if (tree->type == N_CMD && !tree->words && !tree->assigns &&
        tree->redirs && !tree->redirs->next && tree->redirs->type == R_IN &&
        tree->redirs->fd == 0) {
        char *path = expand_str(tree->redirs->target, X_TILDE);
        if (!path)
            return NULL;
        long fd = lp_open(path, O_RDONLY, 0);
        if (fd < 0) {
            sh_warn("%s: %s", path, lp_strerror((int)-fd));
            xfree(path);
            *status = 1;
            return xstrdup("");
        }
        xfree(path);
        strbuf_t b = {0};
        char buf[4096];
        for (;;) {
            long n = xread((int)fd, buf, sizeof buf);
            if (n <= 0) break;
            for (long i = 0; i < n; i++)
                if (buf[i]) sb_putc(&b, buf[i]);
        }
        lp_close((int)fd);
        return sb_take(&b);
    }
    int p[2];
    long r = lp_pipe(p);
    if (r < 0) {
        sh_perror("pipe", r);
        *status = 2;
        return xstrdup("");
    }
    flush_all();
    long pid = lp_fork();
    if (pid < 0) {
        lp_close(p[0]);
        lp_close(p[1]);
        sh_perror("fork", pid);
        *status = 2;
        return xstrdup("");
    }
    if (pid == 0) {
        child_init(NULL, FORK_NOJOB);
        lp_close(p[0]);
        if (p[1] != 1) {
            lp_dup2(p[1], 1);
            lp_close(p[1]);
        }
        evaltree(tree, EV_EXIT);
        exitshell(exitstatus);
    }
    lp_close(p[1]);
    strbuf_t b = {0};
    char buf[4096];
    for (;;) {
        long n = lp_read(p[0], buf, sizeof buf);
        if (n == -E_INTR)
            continue;
        if (n <= 0)
            break;
        for (long i = 0; i < n; i++)
            if (buf[i])
                sb_putc(&b, buf[i]);
    }
    lp_close(p[0]);
    int ws = 0;
    for (;;) {
        long w = lp_waitpid((int)pid, &ws, 0);
        if (w == -E_INTR)
            continue;
        break;
    }
    *status = status_of(ws);
    return sb_take(&b);
}

/* PS4 and the prompts are subject to parameter expansion. The parse
 * goes into an arena of its own, thrown away at once. */
static char *expand_prompt_var(const char *val)
{
    if (!strchr(val, '$') && !strchr(val, '`') && !strchr(val, '\\'))
        return xstrdup(val);
    arena_t *save = cur_arena;
    arena_t *tmp = arena_new();
    cur_arena = tmp;
    bool err = false;
    bool save_xe = expand_error;
    int save_status = exitstatus;
    word_t *w = parse_string_word(val, true, &err);
    char *r = NULL;
    if (!err && w) {
        r = expand_word_str(w, 0);
    }
    expand_error = save_xe;
    exitstatus = save_status;
    cur_arena = save;
    arena_unref(tmp);
    return r ? r : xstrdup(val);
}

/* Regular expression text for =~: what was quoted matches literally. */
static char *expand_regex(word_t *w)
{
    fb_t f;
    fb_init(&f, 0);
    f.ifs = "";
    expand_parts(&f, w, 0);
    fb_endfield(&f);
    strbuf_t b = {0};
    for (int i = 0; i < f.fields.n; i++) {
        const char *t = f.fields.v[i], *m = f.masks.v[i];
        size_t n = strlen(t);
        for (size_t k = 0; k < n; k++) {
            if (m[k] && strchr(".[]()*+?{}|^$\\", t[k]))
                sb_putc(&b, '\\');
            sb_putc(&b, t[k]);
        }
    }
    sv_free(&f.fields);
    sv_free(&f.masks);
    if (expand_error) {
        sb_free(&b);
        return NULL;
    }
    return sb_take(&b);
}

/* ── running strings and files ─────────────────────────────────────── */

static bool input_string_done(void);

static int evalstring(const char *s, int flags)
{
    arena_t *save_arena = cur_arena;
    insrc_t *below = in;
    input_push_string(s, strlen(s), NULL);
    bool ran = false;
    for (;;) {
        arena_t *a = arena_new();
        cur_arena = a;
        bool err = false;
        node_t *nd = parse_command(&err);
        if (nd == PARSE_EOF) {
            cur_arena = save_arena;
            arena_unref(a);
            break;
        }
        if (err) {
            cur_arena = save_arena;
            arena_unref(a);
            exitstatus = 2;
            while (in && in != below)
                input_pop();
            if (!toplevel_interactive || in_subshell)
                exitshell(2);
            evalskip = SKIP_ABORT;
            return 2;
        }
        if (nd) {
            bool last = input_string_done();
            evaltree(nd, last ? flags : (flags & ~EV_EXIT));
            ran = true;
        }
        cur_arena = save_arena;
        arena_unref(a);
        if (evalskip)
            break;
    }
    while (in && in != below)
        input_pop();
    if (!ran)
        exitstatus = 0;
    if (flags & EV_EXIT)
        exitshell(exitstatus);
    return exitstatus;
}

/* Read and run commands from the current input until it ends. top is
 * the shell's own main loop, as against a file being sourced. */
static void interactive_prelude(void);

static int cmdloop(bool top)
{
    insrc_t *mine = in;
    int eofs = 0;
    for (;;) {
        if (top && toplevel_interactive)
            interactive_prelude();
        else if (pending_signals)
            run_pending_traps();
        arena_t *save = cur_arena;
        arena_t *a = arena_new();
        cur_arena = a;
        bool err = false;
        node_t *nd = parse_command(&err);
        if (nd == PARSE_EOF) {
            cur_arena = save;
            arena_unref(a);
            if (top && toplevel_interactive && in == mine) {
                if (jobs_any_stopped() && !warned_stopped) {
                    warned_stopped = true;
                    outs(out2, "\nThere are stopped jobs.\n");
                    flush_out(out2);
                    in->eof = false;
                    continue;
                }
                if (opt[O_ignoreeof] && ++eofs < 10) {
                    outs(out2, "\nUse \"exit\" to leave the shell.\n");
                    flush_out(out2);
                    in->eof = false;
                    continue;
                }
                outs(out2, "exit\n");
                flush_out(out2);
            }
            break;
        }
        eofs = 0;
        if (err) {
            exitstatus = 2;
            if (!toplevel_interactive || !top) {
                cur_arena = save;
                arena_unref(a);
                if (!toplevel_interactive)
                    exitshell(2);
                break;
            }
        } else if (nd && !(opt[O_noexec] && !toplevel_interactive)) {
            warned_stopped = false;
            evaltree(nd, 0);
        }
        cur_arena = save;
        arena_unref(a);
        if (evalskip) {
            if (evalskip == SKIP_ABORT && top && toplevel_interactive) {
                evalskip = SKIP_NONE;
                input_discard_line();
                continue;
            }
            if (evalskip == SKIP_BREAK || evalskip == SKIP_CONT) {
                evalskip = SKIP_NONE;
                continue;
            }
            break;
        }
    }
    return exitstatus;
}

static int run_file(const char *path, bool must_exist)
{
    long fd = lp_open(path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
        if (must_exist) {
            sh_warn("%s: %s", path, lp_strerror((int)-fd));
            return -1;
        }
        return 0;
    }
    int hfd = fd_move_high((int)fd);
    if (hfd < 0)
        hfd = (int)fd;
    insrc_t *below = in;
    input_push_file(hfd, false);
    char *save_name = script_name;
    script_name = xstrdup(path);
    int save_line = cmd_lineno;
    sourcenest++;
    int st = cmdloop(false);
    sourcenest--;
    xfree(script_name);
    script_name = save_name;
    cmd_lineno = save_line;
    while (in && in != below) {
        int f = in->fd;
        bool file = in->kind == IS_FILE;
        input_pop();
        if (file && f >= 0)
            lp_close(f);
    }
    if (evalskip == SKIP_RETURN)
        evalskip = SKIP_NONE;
    return st;
}

static void exitshell(int status)
{
    static bool exiting;
    if (!exiting) {
        exiting = true;
        if (trap_cmd[0] && trap_cmd[0][0]) {
            char *t = trap_cmd[0];
            trap_cmd[0] = NULL;
            exitstatus = status;
            evalskip = SKIP_NONE;
            in_trap = true;
            evalstring(t, 0);
            xfree(t);
        }
    }
    flush_all();
    if (toplevel_interactive && !in_subshell) {
        history_write();
        jobs_release_terminal();
    }
    lp_exit(status & 0xff);
}
