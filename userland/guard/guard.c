/* guard - the safety net.
 *
 * One daemon watching the five ways a board this small actually falls
 * over: it runs out of memory, it overheats, it browns out, one runaway
 * process takes the CPU and nothing else gets a turn, or the disk fills
 * up. Each of these is a policy the kernel does not have on its own.
 *
 *   memory   Linux has an OOM killer, but it acts only once an
 *            allocation has already failed - the machine has been
 *            crawling for a while by then - and it picks the victim
 *            itself, which may well be the SSH server. So we step in
 *            first, on our own thresholds, and we choose:
 *              free < WARN_MB     -> warn
 *              free < RESERVE_MB  -> kill the largest unprotected process
 *
 *   heat     The chip throttles itself at 80C - that is the hardware
 *            protecting the hardware. What it will not do is tell anyone
 *            or stop asking for the work. We watch the temperature and
 *            hold the CPU at its lowest frequency while it is hot, so it
 *            gets a chance to come back down.
 *
 *   power    Undervoltage - a thin cable, a phone charger - is the most
 *            common cause of a corrupted SD card on this board, and it
 *            is completely silent. The GPU firmware records it; we read
 *            that and cap the frequency, which cuts the current draw
 *            that caused it in the first place.
 *
 *   CPU      A process spinning at 100% must not make the machine
 *            unreachable. After half a minute of it we push it to the
 *            back of the run queue and keep the important processes at
 *            the front. Nothing is ever killed for using the CPU - that
 *            may well be the job you asked for. It only has to yield,
 *            and it gets its place back after the same half minute of
 *            behaving itself.
 *
 *   disk     A full data partition means no logs, no saved clock, no
 *            writes at all. We warn early and drop the old rotated log
 *            when it gets critical.
 *
 * ── What is protected, and how that is decided ──
 * Protected means: never killed to reclaim memory, never demoted for
 * holding a core, and marked oom_score_adj=-1000 so the kernel's own
 * killer will not take it either. These have to survive for anyone to
 * see the state and act on it.
 *
 * This used to be decided by the process NAME, read from
 * /proc/<pid>/comm, against a fixed list that included "sh". A name is
 * not an identity:
 *
 *   - "sh" was on the list, so EVERY process a shell started was
 *     protected. A fork bomb written in shell was immune to the memory
 *     killer, and four shell loops pinning all four cores were immune to
 *     the CPU-hog demotion - the two attacks this daemon exists to stop
 *     were the two it could not see.
 *   - comm is whatever the process says it is. prctl(PR_SET_NAME), or
 *     just a copied binary called "dropbear", bought immunity.
 *
 * So identity now comes from two sources that a process cannot write:
 *
 *   1) /var/service.pids, written by init - the pids of the services it
 *      supervises. init is the only process that knows them, /var is on
 *      the root filesystem which is RAM inside the kernel image, and it
 *      is writable by root alone.
 *   2) A shell is protected only when it is a SESSION LEADER with a
 *      controlling terminal - pid == sid and tty_nr != 0. That is the
 *      login shell on the console and the shell of each SSH session,
 *      which are the ones you need to fix anything. A process forked off
 *      by one of them is neither, so a runaway started from a shell is
 *      fair game while the shell you are typing into is not.
 *
 * pid 1 and guard itself are protected by pid, as they always were.
 *
 * Two layers keep them out of the memory killer's way:
 *   1) We step in first, at 32MB free.
 *   2) If it still reaches the kernel's OOM killer, the kernel knows
 *      nothing of our list - so we write each process's
 *      /proc/<pid>/oom_score_adj to tell it:
 *        -1000  never kill this (protected)
 *          500  kill this first (everything else)
 *      The value is inherited across fork, so the children dropbear
 *      spawns per connection are protected automatically.
 *
 * Everything here is best-effort by design: on a virtual machine there
 * is no temperature sensor, no firmware word and no cpufreq, and every
 * one of those reads simply fails and is skipped.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"

#define RESERVE_MB    32      /* the reserve. Below this we start killing */
#define WARN_MB       64      /* start warning here */
#define POLL_MS      1000     /* how often we look, normally */
#define POLL_BUSY_MS  200     /* how often we look under pressure */
/* Room for a storm. This was 256, which a fork bomb of 1600 processes
 * overran in a second: scan_processes stopped at 256 entries and guard
 * spent its time choosing the largest of an arbitrary sixteenth of the
 * problem. The table is static and one entry is 56 bytes, so 2048 costs
 * 112KB of a 512MB board - the price of seeing the whole system. */
#define MAX_PROCS    2048
#define TERM_GRACE_MS 500     /* grace between SIGTERM and SIGKILL */

/* ── Storm mode ────────────────────────────────────────────────────
 * A fork bomb is an exponential attack. Answering it one process at a
 * time, each with a SIGTERM, a 500ms wait and a SIGKILL, is a linear
 * response: measured against 1600 processes it had not finished after
 * forty seconds, and guard's whole loop was blocked in those waits, so
 * nothing else was being watched either.
 *
 * Above this many processes guard stops being polite. It groups what it
 * can see by process group and kills the largest group with a single
 * kill(-pgid, SIGKILL) - one syscall for the whole tree, because that is
 * what a fork bomb is: one tree. No SIGTERM, no grace; a process that
 * has forked 1600 children is not going to clean up after itself. */
#define STORM_PROCS    300    /* more than this and it is a storm */
#define STORM_ROUNDS     8    /* group kills per pass before looking again */
#define PGID_BUCKETS MAX_PROCS  /* a group per process, worst case */

/* The slow pass. Everything except memory is checked on this cadence.
 * A temperature does not change in a second, and walking /proc every
 * second for nothing is exactly the idle cost we are trying not to pay. */
#define SLOW_MS      5000

/* ── Heat ──────────────────────────────────────────────────────────
 * No fan, no heatsink. Raspberry Pi's firmware starts throttling at 80C
 * and clamps hard at 85C; 70C is where we start saying something. We
 * only let go again below COOL_C - the gap is what stops it flapping
 * between the two governors every few seconds. */
#define TEMP_WARN_C     70
#define TEMP_HOT_C      80
#define TEMP_COOL_C     65

/* ── CPU ───────────────────────────────────────────────────────────
 * A process is a hog once it has held most of a core for this long.
 * /proc always counts in 100ths of a second regardless of the kernel's
 * own tick rate, so this figure is fixed. */
#define TICKS_PER_SEC   100
#define HOG_PERCENT      90
#define HOG_SECONDS      30   /* act after this long */
#define HOG_NOTICE_SECONDS 10 /* but say so well before that */

/* ── The whole machine being saturated ──
 *
 * Per-process accounting answers "who is spinning". It does not answer
 * "is this board keeping up", and those are different questions: four
 * processes at 70% each saturate a four-core board while not one of them
 * crosses a 90% line. Load average is the number that describes it, and
 * nothing here was reading it.
 *
 * Load is in runnable processes, so the interesting figure is per core.
 * At 0.90 per core every core has something on it and the board has
 * nothing left to give - which is the state to report. A higher figure
 * would miss the case this exists for: a four-core board pinned by four
 * processes sits at about 4.0, which is 1.0 per core, and each of those
 * processes is only using one core.
 *
 * It is only ever a warning. A board asked to compute something is
 * supposed to be busy, and the whole point of the CPU policy here is
 * that nothing is killed for using the CPU it was given. */
#define LOAD_WARN_PER_CORE   90   /* hundredths: 0.90 runnable per core */
#define LOAD_QUIET_PASSES    12   /* slow passes between repeats (60s) */
#define NICE_PROTECT    (-5)
#define NICE_WATCHDOG  (-10)
#define NICE_HOG         10

/* ── Disk ──────────────────────────────────────────────────────────*/
#define DISK_WARN_MB    32
#define DISK_CRIT_MB     8
#define RAMFS_WARN_MB   32    /* the root filesystem, which is memory */
#define DISK_EVERY       6    /* slow passes between disk checks (30s) */
#define OLD_LOG      "/data/log/messages.1"

/* ── Durability ────────────────────────────────────────────────────
 * ext4 commits its journal every 5s by itself; this is the belt to that
 * pair of braces, and costs nothing when nothing is dirty. */
#define SYNC_EVERY      12    /* slow passes between syncs (60s) */

/* ── Boot loop ─────────────────────────────────────────────────────
 * bootcount counts boots on the data partition and /etc/rc stops
 * trusting /data/rc.local once too many in a row have failed. Staying up
 * this long is what "a boot that worked" means, so this is where we
 * clear the count. Five minutes is past everything that starts at boot
 * and well past the watchdog's patience. */
#define BOOT_OK_PASSES  60    /* slow passes before the boot counts (5min) */

/* ── When there is nothing left to do ──
 * Memory below the reserve, and every process that remains is one we
 * must not kill. Nothing changes from here: the watchdog is healthy so
 * it keeps petting the timer, and no amount of looking again will free a
 * page. POLL_BUSY_MS apart, this is a minute of it before rebooting. */
#define STUCK_PASSES   300
#define UPDATE_TRIAL_FILE  "/data/.update-trial"
#define BOOT_COUNT_FILE "/data/boot_count"

/* /proc/<pid>/oom_score_adj values.
 * At -1000 the kernel's OOM killer drops the process from its candidates. */
#define OOM_PROTECT   (-1000)
#define OOM_SACRIFICE    500

typedef struct {
    pid_t pid;
    pid_t pgid;         /* process group - what a fork bomb shares */
    pid_t sid;          /* session - a login shell leads its own */
    int   tty;          /* controlling terminal, 0 for none */
    long  rss_kb;
    u64   ticks;        /* CPU time used so far, in 100ths of a second */
    char  name[32];
} proc_t;

typedef struct {
    const char *name;
    int         nice;
} protected_t;

/* Never killed, and kept at the front of the run queue. Without these
 * you can neither see nor fix anything.
 *
 * The watchdog sits highest of all: if it does not get a turn every few
 * seconds the board resets itself, and a machine that is merely busy
 * must never reset. */
/* The nice value each supervised service should run at, looked up by
 * name once its pid has been confirmed against /var/service.pids.
 *
 * "sh" is deliberately NOT here. It was, and it made every process
 * started from a shell unkillable and undemotable - see the header. A
 * shell is protected by being a session leader with a terminal, which is
 * a fact about the process, not a string it chose. */
static const protected_t PROTECTED[] = {
    { "init",           NICE_PROTECT  },
    { "guard",          NICE_PROTECT  },
    { "watchdog",       NICE_WATCHDOG },
    { "dropbear",       NICE_PROTECT  },
    { "wpa_supplicant", NICE_PROTECT  },
    { "logd",           NICE_PROTECT  },
    { "dhcp",           NICE_PROTECT  },
    { NULL,             0             }
};

/* ── The pids init says it is supervising ──
 *
 * Re-read on every slow pass, because a service that died and was
 * restarted has a new pid and the old one may since belong to something
 * else entirely. A stale entry here is exactly the mistake that would
 * make a stranger's process unkillable, so this is never cached across
 * passes. */
#define MAX_SVC_PIDS 32
#define SERVICE_PIDS "/var/service.pids"

typedef struct {
    pid_t pid;
    char  name[32];
} svc_pid_t;

static svc_pid_t svc_pids[MAX_SVC_PIDS];

/* How long each service has been in D, in milliseconds. Indexed the
 * same as svc_pids and reset whenever the pid in a slot changes. */
static u32   svc_d_ms[MAX_SVC_PIDS];
static pid_t svc_d_pid[MAX_SVC_PIDS];
static bool  svc_d_said[MAX_SVC_PIDS];
static int       nsvc_pids;
static bool      svc_pids_read;      /* did the file exist at all */

static void read_service_pids(void)
{
    nsvc_pids     = 0;
    svc_pids_read = false;

    char buf[2048];
    if (proc_read(SERVICE_PIDS, buf, sizeof buf) <= 0)
        return;
    svc_pids_read = true;

    char *p = buf;
    while (*p && nsvc_pids < MAX_SVC_PIDS) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = '\0';

        char *sp = strchr(p, ' ');
        if (sp) {
            *sp = '\0';
            pid_t pid = (pid_t)strtol(p, NULL, 10);
            if (pid > 0) {
                svc_pids[nsvc_pids].pid = pid;
                strlcpy(svc_pids[nsvc_pids].name, sp + 1,
                        sizeof svc_pids[nsvc_pids].name);
                nsvc_pids++;
            }
        }

        if (!eol) break;
        p = eol + 1;
    }
}

/* The name init supervises this pid under, or NULL. */
static const char *service_name_of(pid_t pid)
{
    for (int i = 0; i < nsvc_pids; i++)
        if (svc_pids[i].pid == pid)
            return svc_pids[i].name;
    return NULL;
}

#define NOT_PROTECTED  100    /* not a valid nice value, so unambiguous */
#define NICE_KEEP       99    /* protected from killing; priority untouched */

static bool cpu_is_unboosted(pid_t pid);

/* The effective uid of a process, from the owner of its /proc entry.
 * One stat instead of reading and parsing /proc/<pid>/status. Returns
 * (uid_t)-1 when the process has gone, which never compares equal to 0
 * and so is treated as "not root". */
static uid_t proc_uid(pid_t pid)
{
    char path[32];
    snprintf(path, sizeof path, "/proc/%d", (int)pid);

    lp_stat_t st;
    if (lp_stat(path, &st, true) < 0)
        return (uid_t)-1;
    return st.uid;
}

/* The priority a process should run at, or NOT_PROTECTED.
 *
 * `p` carries the pid, the session id and the terminal, all read from
 * /proc/<pid>/stat, because none of those can be set by the process to
 * something it is not. The name is used only to look up which nice value
 * a confirmed service wants, never to decide whether it is one. */
static int protected_nice(const proc_t *p, pid_t self)
{
    /* pid 1 is init and there is only one of it. Ourselves likewise. */
    if (p->pid == 1 || p->pid == self)
        return NICE_PROTECT;

    /* A service init says it is supervising, by pid. */
    const char *svc = service_name_of(p->pid);
    if (svc) {
        for (int i = 0; PROTECTED[i].name; i++)
            if (strcmp(svc, PROTECTED[i].name) == 0)
                return PROTECTED[i].nice;
        return NICE_PROTECT;        /* supervised, so worth keeping */
    }

    /* An interactive shell: session leader, with a terminal. The console
     * login shell and each SSH session's shell are the two things a
     * person needs in order to fix anything, and both are session
     * leaders with a tty. A process forked off by one is not, which is
     * the whole point - the runaway is not protected by the shell that
     * started it. */
    /* A session leader with a terminal: a console or SSH login shell.
     *
     * TWO limits on this, both learned the hard way.
     *
     * It has to be root's. /dev/ptmx is mode 0666 - that is how devpts
     * works everywhere - so four syscalls with no privilege at all
     * (fork, setsid, openpty, TIOCSCTTY) make any process a session
     * leader with a terminal. Without the uid check, a program run by
     * `dropprivs 1000 python3 /data/app.py`, or a user added with
     * `useradd`, could hand itself everything below. The owner of
     * /proc/<pid> is the process's effective uid, which costs one stat
     * instead of parsing /proc/<pid>/status.
     *
     * And it buys protection from being killed, NOT a place at the
     * front of the queue. That distinction was missing: this returned
     * NICE_PROTECT, which is -5, so anyone holding a terminal was moved
     * ahead of every service on the board - a priority an unprivileged
     * process cannot ask for itself and was being given one anyway. A
     * login shell needs to survive so somebody can log in and fix
     * things. It does not need to outrun ntp. */
    if (p->pid == p->sid && p->tty != 0 && proc_uid(p->pid) == 0)
        return NICE_KEEP;

    /* Last resort: if init never wrote the pid file - an old kernel
     * image, or /var unwritable - fall back to matching names, because
     * an imperfect protection list beats none at all. It is announced
     * once at startup so this is never a silent downgrade. */
    if (!svc_pids_read) {
        for (int i = 0; PROTECTED[i].name; i++)
            if (strcmp(p->name, PROTECTED[i].name) == 0)
                return PROTECTED[i].nice;
    }

    return NOT_PROTECTED;
}

static bool is_protected(const proc_t *p, pid_t self)
{
    return protected_nice(p, self) != NOT_PROTECTED;
}

/* Tell the kernel how this process should be ranked.
 * Skip the write when it already holds the value we want, so we are not
 * writing every second for nothing. */
static void set_oom_score(pid_t pid, int score)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", (int)pid);

    char cur[32];
    if (proc_read(path, cur, sizeof(cur)) > 0) {
        long v = strtol(cur, NULL, 10);
        if (v == score)
            return;                 /* already right */
    }

    long fd = lp_open(path, O_WRONLY, 0);
    if (fd < 0)
        return;                     /* the process exited meanwhile */

    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d\n", score);
    lp_write((int)fd, buf, (size_t)n);
    lp_close((int)fd);
}

/* Read the process name from /proc/<pid>/comm, without the newline. */
static bool read_comm(pid_t pid, char *out, size_t size)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);

    char buf[64];
    if (proc_read(path, buf, sizeof(buf)) <= 0)
        return false;

    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    strlcpy(out, buf, size);
    return true;
}

/* The second field of /proc/<pid>/statm is the resident page count.
 *   size resident shared text lib data dt   (all in pages) */
static long read_rss_kb(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/statm", (int)pid);

    char buf[128];
    if (proc_read(path, buf, sizeof(buf)) <= 0)
        return -1;

    char *p = buf;
    while (*p && *p != ' ') p++;      /* skip the first field */
    while (*p == ' ') p++;

    long pages = strtol(p, NULL, 10);
    if (pages <= 0)
        return -1;

    return pages * 4;                  /* 4KB pages -> KB */
}

/* utime + stime out of /proc/<pid>/stat, in 100ths of a second.
 *
 * The fields are positional, but field 2 is the process name in
 * parentheses and a name is allowed to contain ')' itself - so we find
 * the LAST one and count from there. What follows the paren is field 3,
 * and utime is field 14, so eleven fields get skipped. */
/* ── A service stuck inside the kernel ────────────────────────────────
 *
 * The failure this exists for, seen on the board: brcmfmac loses its
 * firmware a few minutes after boot, and the thread that notices is
 * holding RTNL when it wedges. RTNL is one lock for all of networking,
 * so wpa_supplicant blocks in brcmf_fil_cmd_data_set, dhcp blocks in
 * devinet_ioctl waiting for the same lock, and six kworkers sit in D.
 *
 * Everything guard already watches says the machine is fine. Memory is
 * free, no process is burning CPU - they are all asleep - the
 * temperature is normal, and the watchdog daemon is still being
 * scheduled, so it keeps petting and the hardware never fires. The
 * board stays up for ever in a state where nothing about the network
 * can be changed. That was the gap: "the kernel is alive but nothing
 * can talk" had no detector.
 *
 * D is "uninterruptible sleep": waiting on something in the kernel that
 * will not take a signal. A second or two of it is ordinary - that is
 * what a disk read looks like. A service in it for a minute and a half
 * is not waiting, it is stuck, and nothing in user space can unstick
 * it: the process cannot be killed, and every other route to the
 * network wants the lock it is holding.
 */
#define STUCK_D_REPORT_MS   30000     /* say something */
#define STUCK_D_REBOOT_MS   90000     /* and then do something */

/* Field 3 of /proc/<pid>/stat: R, S, D, Z and the rest. It comes right
 * after the command name, which is in brackets and may itself contain
 * spaces and brackets - so the last ')' is the anchor, as read_cpu_ticks
 * does for the same reason. */
static char read_state(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);

    char buf[512];
    if (proc_read(path, buf, sizeof(buf)) <= 0)
        return '?';

    char *p = strrchr(buf, ')');
    if (!p)
        return '?';
    p++;
    while (*p == ' ') p++;
    return *p ? *p : '?';
}

static u64 read_cpu_ticks(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);

    char buf[512];
    if (proc_read(path, buf, sizeof(buf)) <= 0)
        return 0;

    char *p = strrchr(buf, ')');
    if (!p)
        return 0;
    p++;

    for (int field = 0; field < 11; field++) {
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
        if (!*p)
            return 0;
    }

    while (*p == ' ') p++;
    u64 utime = (u64)strtol(p, &p, 10);
    while (*p == ' ') p++;
    u64 stime = (u64)strtol(p, &p, 10);

    /* And the time its finished children used: cutime and cstime, the
     * next two fields.
     *
     * Without these, a loop that forks once per iteration is invisible.
     * A shell running `while true; do something; done` spends its own
     * time in fork and wait - well under the threshold - while all the
     * work happens in children that live a few milliseconds each. guard
     * samples every five seconds, so it never sees the same child
     * twice: every one is a first sighting with nothing to compare
     * against, and gets skipped. Measured on a board with two cores
     * fully saturated this way, guard reported nothing for seventy
     * seconds, because by its own accounting nobody was busy.
     *
     * The kernel already adds a reaped child's time to its parent's
     * cutime/cstime. Counting them attributes the work to the process
     * that is actually causing it, which is the one you would want to
     * slow down. */
    while (*p == ' ') p++;
    u64 cutime = (u64)strtol(p, &p, 10);
    while (*p == ' ') p++;
    u64 cstime = (u64)strtol(p, NULL, 10);

    return utime + stime + cutime + cstime;
}

/* ── Heat ─────────────────────────────────────────────────────────────
 * Milli-degrees C, or -1 where there is no sensor at all (which is what
 * a virtual machine looks like). */
static long read_temp_mc(void)
{
    char buf[32];
    if (proc_read("/sys/class/thermal/thermal_zone0/temp",
                  buf, sizeof(buf)) <= 0)
        return -1;
    return strtol(buf, NULL, 10);
}

/* ── Power ────────────────────────────────────────────────────────────
 * The GPU firmware keeps a word describing what it has had to do to
 * keep the board alive. It is the only place undervoltage is visible -
 * there is no voltage rail we can measure ourselves.
 *
 *   bit 0   under-voltage right now
 *   bit 1   the ARM frequency is capped right now
 *   bit 2   throttled right now
 *   bit 3   the soft temperature limit has been reached
 *   bits 16-19  the same four again, "has happened at some point"
 *
 * The sysfs name has moved between kernel versions, so we look through
 * the places it has lived rather than betting on one. */
#define THR_UNDERVOLT   0x1
#define THR_CAPPED      0x2
#define THR_THROTTLED   0x4
#define THR_SOFT_TEMP   0x8

static const char *THROTTLE_PATHS[] = {
    "/sys/devices/platform/soc/soc:firmware/get_throttled",
    "/sys/devices/platform/soc:firmware/get_throttled",
    "/sys/firmware/raspberrypi/get_throttled",
    NULL
};

static long read_throttled(void)
{
    static const char *found    = NULL;
    static bool        searched = false;

    if (!searched) {
        searched = true;
        for (int i = 0; THROTTLE_PATHS[i]; i++)
            if (lp_exists(THROTTLE_PATHS[i])) {
                found = THROTTLE_PATHS[i];
                break;
            }
    }
    if (!found)
        return -1;

    char buf[32];
    if (proc_read(found, buf, sizeof(buf)) <= 0)
        return -1;
    return strtol(buf, NULL, 16);       /* printed as 0x... */
}

/* ── The frequency lever ──────────────────────────────────────────────
 * The one response to heat and to undervoltage that costs nobody their
 * work: the job still finishes, it just takes longer. Killing a process
 * because the room is warm would be a worse answer.
 *
 * The four cores share one policy on this chip, so writing cpu0 moves
 * all of them; the rest of the writes are harmless. On a virtual machine
 * there is no cpufreq directory and every write fails, which is fine. */
static char gov_normal[32] = "ondemand";

static void read_normal_governor(void)
{
    char buf[32];
    if (proc_read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
                  buf, sizeof(buf)) <= 0)
        return;
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    if (buf[0] && strcmp(buf, "powersave") != 0)
        strlcpy(gov_normal, buf, sizeof(gov_normal));
}

static void set_governor(const char *gov)
{
    for (int cpu = 0; cpu < 4; cpu++) {
        char path[96];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
        long fd = lp_open(path, O_WRONLY, 0);
        if (fd < 0)
            continue;
        lp_write((int)fd, gov, strlen(gov));
        lp_close((int)fd);
    }
}

/* Walk /proc and build the process list. Returns the count. */
static int scan_processes(proc_t *list, int max, pid_t self)
{
    long fd = lp_open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 0;

    static char buf[8192];
    int n = 0;

    for (;;) {
        long got = sys_getdents(fd, buf, sizeof(buf));
        if (got <= 0)
            break;

        for (long off = 0; off < got && n < max; ) {
            char *rec  = buf + off;
            u16   len  = *(u16 *)(rec + 16);
            char *name = rec + 19;
            if (len == 0) break;
            off += len;

            /* An all-digit name is a process directory. */
            if (name[0] < '1' || name[0] > '9')
                continue;
            bool numeric = true;
            for (char *c = name; *c; c++)
                if (*c < '0' || *c > '9') { numeric = false; break; }
            if (!numeric)
                continue;

            pid_t pid = (pid_t)strtol(name, NULL, 10);
            /* Two quite different things are dropped by the test
             * below, and only one of them is obvious.
             *
             * A process that exited between the getdents and the read -
             * the ordinary race, and the reason the test was written.
             *
             * And every kernel thread. A kthread has no address space,
             * so its statm reads "0 0 0 0 0 0 0" and read_rss_kb says
             * -1. That is the right answer for all of them - a kworker
             * cannot be OOM-killed, has no business being niced by us,
             * and is not what anybody means by a process - but it is
             * worth writing down, because the count this list produces
             * is what check_storm compares against STORM_PROCS. On this
             * board /proc holds about seventy entries of which about ten
             * are processes, and on a multi-core amd64 machine - which
             * this system also runs on - kernel threads alone can run to
             * several hundred. If they were counted, guard would declare
             * a fork storm on an idle machine and start killing process
             * groups. They are not, and n means what the storm code
             * thinks it means. */
            long rss = read_rss_kb(pid);
            if (rss < 0)
                continue;

            list[n].pid    = pid;
            list[n].rss_kb = rss;
            list[n].ticks  = read_cpu_ticks(pid);
            if (!read_comm(pid, list[n].name, sizeof(list[n].name)))
                strlcpy(list[n].name, "?", sizeof(list[n].name));

            /* The group, the session and the terminal - what protection
             * is decided by now, and what a group kill needs. */
            list[n].pgid = 0;
            list[n].sid  = 0;
            list[n].tty  = 0;
            lp_proc_ids(pid, NULL, &list[n].pgid, &list[n].sid,
                        &list[n].tty);

            (void)self;
            n++;
        }
    }

    lp_close((int)fd);
    return n;
}

/* Apply our policy to the processes we scanned: who the kernel may kill
 * when memory runs out, and who gets the CPU first when it runs short.
 *
 * New processes keep appearing - dropbear forks one per connection, the
 * shell one per command - so setting this once is not enough. We revisit
 * it on every slow pass.
 *
 * Only the protected set has its priority forced. Everything else is
 * left where it is: a nice value someone set deliberately is a decision,
 * and overwriting it every five seconds would be rude. The one exception
 * is a hog, handled separately below. */
static void apply_priorities(const proc_t *list, int n, pid_t self)
{
    for (int i = 0; i < n; i++) {
        int want = protected_nice(&list[i], self);
        bool prot = (want != NOT_PROTECTED);

        set_oom_score(list[i].pid, prot ? OOM_PROTECT : OOM_SACRIFICE);

        if (!prot || want == NICE_KEEP)
            continue;

        /* Do not undo a demotion check_cpu_hogs made. It runs later in
         * the same pass and drops a spinning protected process to 0;
         * without this the next pass put it straight back to -5 and the
         * two of them fought forever, five seconds apart, with the
         * process winning. */
        if (cpu_is_unboosted(list[i].pid))
            continue;

        if (lp_getpriority(list[i].pid) != want)
            lp_setpriority(list[i].pid, want);
    }
}

/* ── CPU hogs ─────────────────────────────────────────────────────────
 * One entry per process we are keeping an eye on. The table is a fixed
 * array rather than a list because there is no memory to spare and a
 * linear walk of 256 entries every five seconds is nothing. */
typedef struct cpu_state {
    pid_t pid;
    u64   ticks;       /* the reading from the previous pass */
    int   secs_hot;    /* how long it has been over the line */
    int   secs_calm;   /* how long a demoted one has behaved itself */
    int   orig_nice;   /* what it was running at before we touched it */
    bool  demoted;
    bool  announced;   /* we have already said this one is spinning */
    bool  unboosted;   /* protected, so not demoted - just not favoured */
    bool  seen;
} cpu_state_t;

static cpu_state_t cpu_state[MAX_PROCS];

static cpu_state_t *cpu_slot(pid_t pid)
{
    cpu_state_t *free_slot = NULL;

    for (int i = 0; i < MAX_PROCS; i++) {
        if (cpu_state[i].pid == pid)
            return &cpu_state[i];
        if (!free_slot && cpu_state[i].pid == 0)
            free_slot = &cpu_state[i];
    }
    if (!free_slot)
        return NULL;               /* full - this process waits its turn */

    /* Clear the WHOLE slot. announced and unboosted were left alone
     * here, and slots are freed by zeroing only the pid, so a new
     * process landing on a recycled slot inherited them: announced=true
     * meant guard never said a word about a real hog, and
     * unboosted=true meant a spinning protected shell kept its priority
     * boost for ever, which is the exact case that flag exists to end. */
    free_slot->pid       = pid;
    free_slot->ticks     = 0;
    free_slot->secs_hot  = 0;
    free_slot->secs_calm = 0;
    free_slot->orig_nice = 0;
    free_slot->demoted   = false;
    free_slot->announced = false;
    free_slot->unboosted = false;
    free_slot->seen      = false;
    return free_slot;
}

/* Has this pid had its priority boost taken away for spinning?
 * Read-only - it creates no state. */
static bool cpu_is_unboosted(pid_t pid)
{
    for (int i = 0; i < MAX_PROCS; i++)
        if (cpu_state[i].pid == pid)
            return cpu_state[i].unboosted;
    return false;
}

/* Find the processes holding a core to themselves and, after
 * HOG_SECONDS of it, move them to the back of the queue.
 *
 * Half a minute is deliberate, and it is a compromise. Anything much
 * shorter and an ordinary build or a Python script that happens to be
 * busy gets moved for doing its job; anything longer and a board with
 * every core pinned stays unreachable while the timer runs. It was 60
 * seconds and is now 30, with a line printed at HOG_NOTICE_SECONDS so
 * that the answer to "is guard even running" arrives long before the
 * action does.
 *
 * What makes 30 seconds acceptable for legitimate work is that the
 * punishment is only a nice value: the work still runs, at the same
 * speed on an idle board, and it stops deciding whether SSH answers.
 * The same half minute has to pass in the other direction before the
 * priority is given back, so a build that pauses is not let off. */
static void check_cpu_hogs(const proc_t *list, int n, pid_t self,
                           long elapsed_ms)
{
    if (elapsed_ms <= 0)
        return;

    for (int i = 0; i < MAX_PROCS; i++)
        cpu_state[i].seen = false;

    for (int i = 0; i < n; i++) {
        cpu_state_t *st = cpu_slot(list[i].pid);
        if (!st)
            continue;
        st->seen = true;

        u64  prev  = st->ticks;
        u64  now   = list[i].ticks;
        st->ticks  = now;

        /* First sighting, or the counter went backwards because the pid
         * was reused. Either way there is nothing to compare against. */
        if (prev == 0 || now < prev) {
            st->secs_hot = 0;
            continue;
        }

        /* Percent of one core: ticks used, over ticks available. */
        long used_ticks = (long)(now - prev);
        long have_ticks = elapsed_ms * TICKS_PER_SEC / 1000;
        long percent    = have_ticks > 0 ? used_ticks * 100 / have_ticks : 0;

        if (percent < HOG_PERCENT) {
            st->secs_hot  = 0;
            st->announced = false;

            /* A protected process that has stopped spinning gets its
             * boost back at once - it was never at fault, it was busy. */
            if (st->unboosted) {
                lp_setpriority(list[i].pid, st->orig_nice);
                st->unboosted = false;
            }

            /* It has stopped. Give it back what it was running at - the
             * demotion was for what it was doing, not for what it is.
             * The same minute has to pass in the other direction, so a
             * process that pauses for a moment does not get let off. */
            if (st->demoted) {
                st->secs_calm += (int)(elapsed_ms / 1000);
                if (st->secs_calm >= HOG_SECONDS) {
                    lp_setpriority(list[i].pid, st->orig_nice);
                    st->demoted   = false;
                    st->secs_calm = 0;
                    dprintf(STDERR_FILENO,
                            "guard: %s (pid %d) has settled down"
                            " - priority restored\n",
                            list[i].name, (int)list[i].pid);
                }
            }
            continue;
        }
        st->secs_calm = 0;

        st->secs_hot += (int)(elapsed_ms / 1000);

        /* Say something as soon as it is clear, not only once we act.
         * The whole complaint about this check was that a board with all
         * four cores pinned printed nothing at all for over a minute -
         * so from the outside there was no way to tell guard from a
         * guard that was not running. Silence is the one thing a watchdog
         * must never do. */
        if (!st->announced && st->secs_hot >= HOG_NOTICE_SECONDS) {
            dprintf(STDERR_FILENO,
                    "guard: %s (pid %d) has held a core for %ds"
                    " (%ld%% of one core)\n",
                    list[i].name, (int)list[i].pid, st->secs_hot, percent);

            /* The log, not just the console.
             *
             * This notice went to stderr alone, which means it was on
             * screen for whoever happened to be looking and nowhere
             * else. On a board left alone for months - which is the
             * only kind this system is for - nobody is looking, and
             * "something pinned a core at 3am" is exactly the sort of
             * thing you want to find afterwards.
             *
             * The demotion below was already logged. Logging only the
             * action and not the observation also left a gap: a
             * protected process never gets demoted, so a spinning shell
             * produced a console line and no record at all. */
            {
                char m[160];
                snprintf(m, sizeof m,
                         "%s (pid %d) held a core for %ds (%ld%% of one core)",
                         list[i].name, (int)list[i].pid, st->secs_hot, percent);
                lp_log("guard", m);
            }
            st->announced = true;
        }

        /* A protected process pinning a core does not get pushed to the
         * back of the queue - being answerable is the point of it. But
         * it does lose the boost we gave it: at nice -5 an SSH session
         * spinning in a shell loop outruns everything else on the board,
         * including the processes that would tell you about it. Back to
         * 0 is not a punishment, it is the absence of a favour.
         *
         * This used to be a bare `continue` under a comment saying it
         * was "worth saying out loud", which said nothing and did
         * nothing - and because "sh" was on the protected list, that
         * branch was where every shell CPU hog on the machine ended up. */
        if (is_protected(&list[i], self)) {
            if (st->secs_hot >= HOG_SECONDS && !st->unboosted) {
                int cur = lp_getpriority(list[i].pid);
                if (cur < 0 && lp_setpriority(list[i].pid, 0) == 0) {
                    st->unboosted = true;
                    st->orig_nice = cur;
                    dprintf(STDERR_FILENO,
                            "guard:   it is protected, so it is not"
                            " demoted - but its priority boost is off"
                            " while it spins\n");
                }
            }
            continue;
        }

        if (st->secs_hot < HOG_SECONDS || st->demoted)
            continue;

        /* Someone who already asked to run at a low priority does not
         * need us to arrange it, and moving them to 10 would be a
         * promotion. */
        int was = lp_getpriority(list[i].pid);
        if (was >= NICE_HOG)
            continue;

        if (lp_setpriority(list[i].pid, NICE_HOG) == 0) {
            st->orig_nice = was;
            st->demoted   = true;
            dprintf(STDERR_FILENO,
                    "guard: %s (pid %d) has held a core for %ds"
                    " - moved to the back of the queue\n",
                    list[i].name, (int)list[i].pid, st->secs_hot);
            {
                char m[160];
                snprintf(m, sizeof m,
                         "%s (pid %d) held a core for %ds - demoted",
                         list[i].name, (int)list[i].pid, st->secs_hot);
                lp_log("guard", m);
            }
        }
    }

    /* Forget the processes that have gone, so their slots come free and
     * a reused pid does not inherit someone else's history. */
    for (int i = 0; i < MAX_PROCS; i++)
        if (cpu_state[i].pid != 0 && !cpu_state[i].seen)
            cpu_state[i].pid = 0;
}

/* ── Heat and power ───────────────────────────────────────────────────
 * Both end at the same lever, so they share one piece of state: while
 * either of them says the board is in trouble, the CPU is held at its
 * lowest frequency. It is let go again only once the temperature is
 * properly back down - not merely below the line it crossed. */
static bool capped = false;

static void check_heat_and_power(void)
{
    long temp_mc  = read_temp_mc();
    long thr      = read_throttled();
    long temp_c   = temp_mc >= 0 ? temp_mc / 1000 : -1;

    bool hot   = (temp_c >= 0 && temp_c >= TEMP_HOT_C);
    bool cool  = (temp_c < 0  || temp_c <= TEMP_COOL_C);
    bool power = (thr > 0 && (thr & (THR_UNDERVOLT | THR_CAPPED)) != 0);

    static bool warned_temp = false;
    static bool warned_power = false;

    if (temp_c >= TEMP_WARN_C && !warned_temp) {
        dprintf(STDERR_FILENO, "guard: %ldC - getting warm\n", temp_c);
        warned_temp = true;
    } else if (temp_c >= 0 && temp_c < TEMP_WARN_C) {
        warned_temp = false;
    }

    if (power && !warned_power) {
        dprintf(STDERR_FILENO,
                "guard: ** the power supply is not keeping up"
                " (firmware says 0x%x)\n"
                "guard:    this is what corrupts SD cards."
                " Use a better cable or charger.\n", (unsigned)thr);
        warned_power = true;
    } else if (!power) {
        warned_power = false;
    }

    if ((hot || power) && !capped) {
        set_governor("powersave");
        capped = true;
        dprintf(STDERR_FILENO,
                "guard: holding the CPU at its lowest speed (%s)\n",
                hot ? "too hot" : "power");
    } else if (capped && cool && !power) {
        set_governor(gov_normal);
        capped = false;
        dprintf(STDERR_FILENO, "guard: back to normal speed");
        if (temp_c >= 0)
            dprintf(STDERR_FILENO, " (%ldC)", temp_c);
        dprintf(STDERR_FILENO, "\n");
    }
}

/* Mark this boot as one that worked. See BOOT_OK_PASSES above. */
static void clear_boot_count(void)
{
    long fd = lp_open(BOOT_COUNT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;                     /* no data partition - nothing counted */
    lp_write((int)fd, "0\n", 2);
    lp_close((int)fd);

    /* The same moment settles a system update.
     *
     * `update` leaves this file behind when it installs a new system,
     * and puts the old one back if the board starts three times without
     * reaching here. Reaching here is the definition of the new system
     * being all right: five minutes up, memory and temperature fine,
     * nothing restarting. So this is where it stops being on trial.
     *
     * Deleting the file rather than running `update --commit`: guard is
     * the process that must not die, and forking from it to say one
     * thing is a risk taken for nothing. */
    lp_unlink(UPDATE_TRIAL_FILE);
}

/* ── Disk ─────────────────────────────────────────────────────────────
 * A full data partition is quiet until everything stops at once: the log
 * cannot be written, the clock cannot be saved, and a new SSH session
 * cannot write its own history. The one thing we can free without asking
 * is the previous rotated log - it has already been superseded. */
static void check_disk(void)
{
    u64 freeb = 0, totalb = 0;
    if (lp_fs_space("/data", &freeb, &totalb) < 0 || totalb == 0)
        return;                     /* no data partition - all in RAM */

    long free_mb = (long)(freeb / (1024 * 1024));
    static bool warned = false;

    if (free_mb <= DISK_CRIT_MB) {
        dprintf(STDERR_FILENO,
                "guard: ** /data has %ldMB left\n", free_mb);
        if (lp_exists(OLD_LOG) && lp_unlink(OLD_LOG) == 0)
            dprintf(STDERR_FILENO,
                    "guard:    dropped the old log (%s)\n", OLD_LOG);
        warned = true;
    } else if (free_mb <= DISK_WARN_MB) {
        if (!warned) {
            dprintf(STDERR_FILENO,
                    "guard: /data is filling up - %ldMB left\n", free_mb);
            warned = true;
        }
    } else {
        warned = false;
    }

    /* The root filesystem is RAM. Space used there is memory that no
     * process owns, so the memory killer cannot win it back: it would
     * kill process after process while the pages stay exactly where they
     * are. Worth saying plainly, because nothing about the free memory
     * figure points at a file as the cause. */
    static bool warned_root = false;
    if (lp_fs_space("/", &freeb, &totalb) == 0 && totalb > 0) {
        long root_free_mb = (long)(freeb / (1024 * 1024));
        if (root_free_mb <= RAMFS_WARN_MB && !warned_root) {
            dprintf(STDERR_FILENO,
                    "guard: ** the root filesystem is RAM and has %ldMB"
                    " left\n"
                    "guard:    files outside /data and /tmp are held in"
                    " memory. Delete some.\n", root_free_mb);
            warned_root = true;
        } else if (root_free_mb > RAMFS_WARN_MB) {
            warned_root = false;
        }
    }
}

/* ── Answering a storm ─────────────────────────────────────────────
 *
 * A fork bomb is one tree of processes, and a tree shares a process
 * group. kill(-pgid, SIGKILL) ends the whole group in a single syscall,
 * which is the only response that keeps up with something doubling.
 *
 * The care needed: the group of an SSH session includes the session's
 * own shell, and a bomb started from that shell is in it. Killing the
 * group would take the shell too. So when the group's leader is
 * protected we kill the group's members individually instead - still
 * with no SIGTERM and no waiting, just not the leader.
 *
 * Returns the number of processes signalled, or 0 when there was nothing
 * left that may be killed. */
static int kill_biggest_group(const proc_t *list, int n, pid_t self)
{
    /* Count unprotected members per group. A fixed histogram rather than
     * a map: there is no memory to spare, and the group we want is the
     * one with hundreds of members, which no amount of collision hides. */
    static pid_t pg[PGID_BUCKETS];
    static int   count[PGID_BUCKETS];
    int ngroups = 0;

    for (int i = 0; i < n; i++) {
        if (list[i].pgid <= 0)
            continue;
        if (is_protected(&list[i], self))
            continue;

        int at = -1;
        for (int g = 0; g < ngroups; g++)
            if (pg[g] == list[i].pgid) { at = g; break; }
        if (at < 0) {
            if (ngroups >= PGID_BUCKETS)
                continue;
            at = ngroups++;
            pg[at]    = list[i].pgid;
            count[at] = 0;
        }
        count[at]++;
    }

    /* Never our own group, and never group 0 or 1 - those would take
     * init, or us, or the whole session tree of the console.
     *
     * The important word is "next". This used to return 0 when the
     * biggest group was one of those, and 0 means "nothing left that
     * may be killed" to both callers: they printed that the storm was
     * entirely protected - which was false - and stopped, and
     * kill_largest returned early without even trying the ordinary
     * one-process memory killer below it. Every pass then counted
     * towards the reboot.
     *
     * Group 1 is not a rare case. init does not put services in their
     * own groups, so everything /data/rc.local starts and every child a
     * service forks is in group 1. A runaway there made group 1 the
     * biggest, and guard did nothing at all and rebooted. */
    pid_t my_pgid = 0;
    lp_proc_ids(self, NULL, &my_pgid, NULL, NULL);

    pid_t target = 0;
    int   target_count = 0;
    for (;;) {
        int best = -1;
        for (int g = 0; g < ngroups; g++)
            if (count[g] > 0 && (best < 0 || count[g] > count[best]))
                best = g;
        if (best < 0)
            return 0;                  /* now this really is nothing left */

        if (pg[best] > 1 && pg[best] != my_pgid) {
            target       = pg[best];
            target_count = count[best];
            break;
        }
        dprintf(STDERR_FILENO,
                "guard:   group %d is init's own or ours - taking the"
                " next largest instead\n", (int)pg[best]);
        count[best] = 0;               /* out of the running, try again */
    }

    /* Is the group leader something we must not kill?
     *
     * The old version concluded "not in the list, so it has already
     * gone - safe to take the group whole". That reasoning is wrong in
     * exactly the situation this function is for. The scan stops at
     * MAX_PROCS and /proc hands out pids in ascending order, so a
     * storm's truncated scan holds the LOWEST pids; a login shell
     * whose pid sorts above the cut is missing from the list while
     * being very much alive. kill(-pgid) then took the shell - the one
     * thing the whole individual-kill branch below exists to protect,
     * on a board whose only other way in is a serial cable.
     *
     * So when the leader is not listed, ask /proc about it directly
     * rather than assuming. */
    bool leader_protected = false;
    bool leader_found     = false;
    for (int i = 0; i < n; i++)
        if (list[i].pid == target) {
            leader_found     = true;
            leader_protected = is_protected(&list[i], self);
            break;
        }

    if (!leader_found) {
        proc_t ld;
        memset(&ld, 0, sizeof ld);
        ld.pid = target;
        if (lp_proc_ids(target, NULL, &ld.pgid, &ld.sid, &ld.tty) == 0) {
            if (!read_comm(target, ld.name, sizeof ld.name))
                strlcpy(ld.name, "?", sizeof ld.name);
            leader_protected = is_protected(&ld, self);
        }
        /* lp_proc_ids failing means /proc/<target> really is gone, and
         * the group is orphans. Taking it whole is right then. */
    }

    if (!leader_protected) {
        dprintf(STDERR_FILENO,
                "guard:   killing process group %d (%d processes) at once\n",
                (int)target, target_count);
        if (lp_kill(-target, SIGKILL) == 0)
            return target_count;
        /* The group went away between the scan and the signal, which is
         * the normal race. Fall through and kill what is still listed. */
    } else {
        dprintf(STDERR_FILENO,
                "guard:   group %d has a protected leader"
                " - stopping the group and killing its %d children\n",
                (int)target, target_count);
    }

    /* Freeze the group before killing anything in it.
     *
     * Killing the children one at a time while the bomb keeps forking is
     * a race the bomb wins: measured here, guard killed 9835 processes
     * in one pass and 1002 were left, then 9398 and 378 were left, over
     * and over, for as long as it was allowed to run. The board stayed
     * up - init, SSH and the log survived, which is what matters - but
     * it never got quiet again, and the person who had to notice and
     * type `kill` was exactly the person who could not, because the
     * shell was crawling.
     *
     * SIGSTOP to the whole group stops every member, including the ones
     * created a moment ago, in one syscall. A stopped process cannot
     * fork, so the sweep that follows converges: what is listed is what
     * exists. Then the leader - the login shell, the one thing we are
     * not allowed to kill - is continued, and it comes back to a prompt
     * with its children gone.
     *
     * SIGSTOP is not blockable, which is the property this relies on. */
    if (leader_protected)
        lp_kill(-target, SIGSTOP);

    int hit = 0;
    for (int i = 0; i < n; i++) {
        if (list[i].pgid != target)
            continue;
        if (is_protected(&list[i], self))
            continue;
        if (lp_kill(list[i].pid, SIGKILL) == 0)
            hit++;
    }

    /* Anything that appeared between the scan and the stop is stopped
     * now and not in the list. Sweep /proc once more for members of this
     * group while they are all frozen - this is the pass that actually
     * finishes the job. */
    if (leader_protected) {
        proc_t again[MAX_PROCS];
        int m = scan_processes(again, MAX_PROCS, self);
        for (int i = 0; i < m; i++) {
            if (again[i].pgid != target)
                continue;
            if (is_protected(&again[i], self))
                continue;
            if (lp_kill(again[i].pid, SIGKILL) == 0)
                hit++;
        }

        /* Let the shell run again, and make sure the killed ones are not
         * left stopped-and-dead: SIGCONT to the group reaches any that
         * survived being protected. */
        lp_kill(-target, SIGCONT);
    }

    return hit;
}

/* How many cores this machine has, counted once from /proc/cpuinfo. */
static int core_count(void)
{
    static int cached = 0;
    if (cached)
        return cached;

    cached = 1;

    /* Anchored to the start of a line, and a buffer big enough to reach
     * the last core. The first version counted the substring anywhere
     * in a 4096-byte read: on an x86_64 VM - which this system supports
     * running on - /proc/cpuinfo is 5.5KB and the fourth core's line
     * sits at byte 4218, so it counted 3 cores out of 4 and warned
     * about saturation on a board that was fine. "Common KVM
     * processor" in a model name would have counted too. */
    static char buf[16384];
    if (proc_read("/proc/cpuinfo", buf, sizeof buf) > 0) {
        int n = 0;
        for (const char *p = buf; *p; ) {
            if (strncmp(p, "processor", 9) == 0 &&
                (p[9] == ' ' || p[9] == '\t' || p[9] == ':'))
                n++;
            while (*p && *p != '\n') p++;
            if (*p) p++;
        }
        if (n > 0)
            cached = n;
    }
    return cached;
}

/* The 1-minute load average, in hundredths (150 means 1.50).
 * -1 when it cannot be read. */
static long read_load_x100(void)
{
    char buf[64];
    if (proc_read("/proc/loadavg", buf, sizeof buf) <= 0)
        return -1;

    /* "0.52 0.31 0.12 1/48 1234" - the first field, as hundredths,
     * without floating point: read the integer part, then two digits
     * after the dot. */
    const char *p = buf;
    long whole = strtol(p, (char **)&p, 10);
    long frac  = 0;
    if (*p == '.') {
        p++;
        for (int i = 0; i < 2; i++) {
            frac *= 10;
            if (*p >= '0' && *p <= '9')
                frac += *p++ - '0';
        }
    }
    return whole * 100 + frac;
}

/* Is the board keeping up at all? Says so when it is not, and names
 * what is using the most CPU, which per-process thresholds miss when
 * the load is spread across several processes.
 *
 * A warning only. Nothing is killed or demoted from here. */
static void check_load(const proc_t *list, int n, pid_t self)
{
    static int quiet_for = 0;

    if (quiet_for > 0) {
        quiet_for--;
        return;
    }

    long load  = read_load_x100();
    int  cores = core_count();
    if (load < 0)
        return;

    if (load < (long)LOAD_WARN_PER_CORE * cores)
        return;

    /* Name the three biggest CPU users so the message is actionable
     * rather than just alarming. Sorting the whole list would cost more
     * than picking three out of it. */
    dprintf(STDERR_FILENO,
            "guard: the board is saturated - load %ld.%02ld on %d core%s\n",
            load / 100, load % 100, cores, cores == 1 ? "" : "s");

    for (int shown = 0; shown < 3; shown++) {
        int  best = -1;
        u64  best_ticks = 0;
        static pid_t already[3];

        for (int i = 0; i < n; i++) {
            bool seen = false;
            for (int j = 0; j < shown; j++)
                if (already[j] == list[i].pid) { seen = true; break; }
            if (seen || list[i].pid == self)
                continue;
            if (list[i].ticks > best_ticks) {
                best_ticks = list[i].ticks;
                best = i;
            }
        }
        if (best < 0)
            break;

        already[shown] = list[best].pid;
        dprintf(STDERR_FILENO,
                "guard:   %-16s pid %-6d %lus of CPU so far%s\n",
                list[best].name, (int)list[best].pid,
                (unsigned long)(best_ticks / TICKS_PER_SEC),
                is_protected(&list[best], self) ? "  (protected)" : "");
    }

    dprintf(STDERR_FILENO,
            "guard:   Nothing is killed for using the CPU - that may be"
            " the job you asked for.\n"
            "guard:   'top' shows the rest. 'kill <name>' if it is not.\n");

    quiet_for = LOAD_QUIET_PASSES;
}

/* ── A storm is its own emergency ──
 *
 * The memory killer above only runs when free memory drops below the
 * reserve. A fork bomb of shells costs almost no memory - a few hundred
 * KB each, and they share their pages - so it can take every pid on the
 * board, make every fork by anything else fail, and leave the machine
 * unusable without the memory threshold ever being crossed. Nothing was
 * watching the one number that describes it: how many processes there
 * are.
 *
 * Returns true when it acted, so the caller can look again sooner. */
static bool check_storm(proc_t *list, int n, pid_t self)
{
    static bool  in_storm = false;
    bool truncated = (n >= MAX_PROCS);

    if (n <= STORM_PROCS && !truncated) {
        if (in_storm) {
            dprintf(STDERR_FILENO,
                    "guard: the storm is over - %d processes\n", n);
            in_storm = false;
        }
        return false;
    }

    if (!in_storm) {
        dprintf(STDERR_FILENO,
                "\nguard: ** %d processes%s. A board this size runs about"
                " twenty.\n"
                "guard:    Something is forking without stopping. Killing"
                " it by group.\n",
                n, truncated ? " (more than we can list)" : "");
        in_storm = true;
    }

    int killed = 0;
    for (int round = 0; round < STORM_ROUNDS; round++) {
        int hit = kill_biggest_group(list, n, self);
        if (hit == 0)
            break;
        killed += hit;
        n = scan_processes(list, MAX_PROCS, self);
        if (n <= STORM_PROCS)
            break;
    }

    if (killed == 0) {
        dprintf(STDERR_FILENO,
                "guard:   nothing in it may be killed - it is all"
                " protected. Not touching it.\n");
        return false;
    }

    dprintf(STDERR_FILENO,
            "guard:   killed %d, %d processes left\n", killed, n);
    {
        char m[128];
        snprintf(m, sizeof m,
                 "process storm: killed %d, %d left", killed, n);
        lp_log("guard", m);
    }
    return true;
}

/* Kill the largest unprotected process.
 * Returns true if we killed something. */
static bool kill_largest(pid_t self, long need_kb)
{
    static proc_t list[MAX_PROCS];
    int n = scan_processes(list, MAX_PROCS, self);

    /* A storm is either more processes than a board this size ever has a
     * reason to run, or a scan that filled the table - in which case
     * there are more than we can even see, which is worse.
     *
     * Under a storm the polite path is the wrong path: one victim per
     * pass, each with half a second of waiting, against something that
     * doubles. Kill by group instead, several groups per pass, and do
     * not wait for anything. */
    bool truncated = (n >= MAX_PROCS);
    if (n > STORM_PROCS || truncated) {
        dprintf(STDERR_FILENO,
                "guard: ** %d processes%s - this is a fork storm,"
                " killing by group\n",
                n, truncated ? " (and more than we can list)" : "");

        int killed = 0;
        for (int round = 0; round < STORM_ROUNDS; round++) {
            int hit = kill_biggest_group(list, n, self);
            if (hit == 0)
                break;
            killed += hit;
            /* Re-scan: the groups we just ended are gone, and the ones
             * still forking have grown. Cheap next to what it replaces. */
            n = scan_processes(list, MAX_PROCS, self);
            if (n <= STORM_PROCS)
                break;
        }

        if (killed > 0) {
            dprintf(STDERR_FILENO,
                    "guard:   killed %d processes, %d left\n", killed, n);
            return true;
        }
        dprintf(STDERR_FILENO,
                "guard:   nothing in the storm may be killed."
                " Everything left is protected.\n");
        return false;
    }

    int  best = -1;
    long best_rss = 0;

    for (int i = 0; i < n; i++) {
        if (is_protected(&list[i], self))
            continue;
        if (list[i].rss_kb > best_rss) {
            best_rss = list[i].rss_kb;
            best = i;
        }
    }

    if (best < 0) {
        dprintf(STDERR_FILENO,
                "guard: nothing left to reclaim."
                " Only protected processes remain (%ldKB short)\n", need_kb);
        return false;
    }

    dprintf(STDERR_FILENO,
            "guard: out of memory - killing %s (pid %d, %ldKB)\n",
            list[best].name, (int)list[best].pid, list[best].rss_kb);

    /* Give it a chance to clean up first. */
    lp_kill(list[best].pid, SIGTERM);
    lp_sleep_ms(TERM_GRACE_MS);

    /* Still there? Force it. kill(pid, 0) only tests for existence. */
    if (lp_kill(list[best].pid, 0) == 0) {
        lp_kill(list[best].pid, SIGKILL);
        dprintf(STDERR_FILENO, "guard:   no response, killed it\n");
    }
    return true;
}

/* Nothing in user space can unwedge this, so the only move left is to
 * start again. Two mechanisms, in that order, because the clean one can
 * hang on exactly the lock that is stuck.
 *
 * The watchdog daemon has to stop first. It is a separate process, it
 * is still being scheduled, and it will go on petting the hardware
 * through a reboot that never completes - which is why the board sat
 * there for ever instead of resetting. With it gone, the timer that
 * /etc/rc armed runs out and the hardware resets the board whatever
 * the kernel is doing.
 *
 * Then ask for a clean reboot. If it works, it works; if it hangs
 * inside a reboot notifier waiting for RTNL, the hardware has it. */
static void reboot_wedged(const char *what, u32 ms)
{
    dprintf(STDERR_FILENO,
            "guard: ** %s has been stuck in the kernel for %us\n",
            what, ms / 1000);
    dprintf(STDERR_FILENO,
            "guard:    this is the state where the machine is up and no\n");
    dprintf(STDERR_FILENO,
            "guard:    part of the network can be touched. Rebooting.\n");

    for (int i = 0; i < nsvc_pids; i++) {
        if (strcmp(svc_pids[i].name, "watchdog") == 0) {
            dprintf(STDERR_FILENO,
                    "guard:    stopping the watchdog daemon first, so the"
                    " hardware timer is the backstop\n");
            lp_kill(svc_pids[i].pid, 9);
        }
    }

    lp_sync();
    lp_reboot(LINUX_REBOOT_CMD_RESTART);
    dprintf(STDERR_FILENO,
            "guard: ** the reboot was refused - waiting for the watchdog\n");
}

/* Called once per pass. Cheap: one small read per service. */
static void check_stuck_services(u32 elapsed_ms)
{
    for (int i = 0; i < nsvc_pids; i++) {
        if (svc_d_pid[i] != svc_pids[i].pid) {
            svc_d_pid[i]  = svc_pids[i].pid;   /* a restart is a fresh start */
            svc_d_ms[i]   = 0;
            svc_d_said[i] = false;
        }

        if (read_state(svc_pids[i].pid) != 'D') {
            svc_d_ms[i]   = 0;
            svc_d_said[i] = false;
            continue;
        }

        svc_d_ms[i] += elapsed_ms;

        if (svc_d_ms[i] >= STUCK_D_REBOOT_MS) {
            reboot_wedged(svc_pids[i].name, svc_d_ms[i]);
            svc_d_ms[i] = 0;            /* do not ask twice a second */
        } else if (svc_d_ms[i] >= STUCK_D_REPORT_MS && !svc_d_said[i]) {
            svc_d_said[i] = true;
            dprintf(STDERR_FILENO,
                    "guard: %s has been in uninterruptible sleep for %us -"
                    " watching it\n",
                    svc_pids[i].name, svc_d_ms[i] / 1000);
        }
    }
}

int main(int argc, char **argv)
{
    long reserve_kb = RESERVE_MB * 1024;
    long warn_kb    = WARN_MB * 1024;
    bool daemonize  = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            daemonize = true;
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            reserve_kb = strtol(argv[++i], NULL, 10) * 1024;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            warn_kb = strtol(argv[++i], NULL, 10) * 1024;
        } else {
            printf("usage: guard [-d] [-r reserveMB] [-w warnMB]\n");
            printf("  -d  run in the background\n");
            printf("  -r  the memory reserve to keep free (default %d MB)\n",
                   RESERVE_MB);
            printf("  -w  where memory warnings start (default %d MB)\n",
                   WARN_MB);
            printf("\nWatches memory, temperature, the power supply, CPU"
                   " hogs and disk space.\n");
            return 2;
        }
    }

    if (warn_kb < reserve_kb)
        warn_kb = reserve_kb;

    if (daemonize) {
        pid_t pid = lp_fork();
        if (pid < 0) {
            dprintf(STDERR_FILENO, "guard: fork failed\n");
            return 1;
        }
        if (pid > 0)
            return 0;               /* the parent leaves at once */
        lp_setsid();
    }

    char mem[4096];
    long total = -1;
    if (proc_read("/proc/meminfo", mem, sizeof(mem)) > 0)
        total = proc_find_kv(mem, "MemTotal");

    if (total < 0) {
        dprintf(STDERR_FILENO,
                "guard: cannot read /proc/meminfo (is /proc mounted?)\n");
        return 1;
    }

    /* Remember what the CPU was set to before we touch it, so that
     * whatever the board came up with is what it goes back to. */
    read_normal_governor();

    printf("guard: %ldMB total, reserve %ldMB, warning at %ldMB\n",
           total / 1024, reserve_kb / 1024, warn_kb / 1024);

    {
        long t = read_temp_mc();
        if (t >= 0)
            printf("guard: %ldC now, slowing down at %dC\n",
                   t / 1000, TEMP_HOT_C);
        else
            printf("guard: no temperature sensor here\n");
    }

    pid_t self = lp_getpid();
    bool  warned = false;

    /* Apply once immediately. This is what keeps SSH and the shell alive
     * if the kernel's OOM killer runs before guard gets a chance. */
    {
        read_service_pids();
        static proc_t boot_list[MAX_PROCS];
        int n = scan_processes(boot_list, MAX_PROCS, self);
        apply_priorities(boot_list, n, self);
        printf("guard: priorities applied to %d processes\n", n);
        if (svc_pids_read)
            printf("guard:   protecting pid 1, myself, %d service(s) init"
                   " named, and login shells\n", nsvc_pids);
        else
            /* Not a fault at this point, and it used to be reported as
             * one. guard is the first line of /etc/services, so it is
             * running before init has finished starting the rest and
             * written their pids down. The file turns up within a few
             * seconds and the list is re-read every pass; only if it is
             * still missing well after boot is anything wrong, and that
             * is said below instead of here. */
            printf("guard:   protecting pid 1, myself and login shells"
                   " - waiting for init's service list\n");
    }

    /* Time is counted in the sleeps we ourselves ask for, not from the
     * clock. The clock jumps the moment ntp gets an answer, and a jump
     * would turn one CPU sample into nonsense. */
    long ms_since_slow = 0;
    long ms_since_cpu  = 0;   /* real time since the last CPU sample */
    int  slow_passes   = 0;
    int  storm_recent  = 0;     /* passes left of watching closely */
    int  stuck         = 0;     /* passes with nothing left to reclaim */

    for (;;) {
        long avail = -1, swap_total = -1, swap_free = -1;
        long sleep_ms = POLL_MS;

        /* ── the slow pass ── */
        if (ms_since_slow >= SLOW_MS) {
            /* Fresh every pass. A service that died and came back has a
             * new pid, and the old one may already belong to somebody
             * else - protecting a stale pid is how a stranger's process
             * would become unkillable. */
            read_service_pids();

            /* If init never wrote the list, protection falls back to
             * matching process names - which a process can choose for
             * itself. That is a real downgrade and has to be said, but
             * only once, and only after boot has had time to finish. */
            static int no_list_passes = 0;
            if (!svc_pids_read) {
                no_list_passes++;
                if (no_list_passes == 6)
                    dprintf(STDERR_FILENO,
                            "guard: ** %s still is not there %ds after"
                            " starting.\n"
                            "guard: ** Protection has fallen back to"
                            " matching process names, and a process can\n"
                            "guard: ** choose its own name. Is /var"
                            " writable?\n",
                            SERVICE_PIDS, no_list_passes * SLOW_MS / 1000);
            } else {
                no_list_passes = 0;
            }

            static proc_t all[MAX_PROCS];
            int n = scan_processes(all, MAX_PROCS, self);

            apply_priorities(all, n, self);

            /* Before anything else: is the machine still made of the
             * number of processes it is supposed to be? This does not
             * wait for memory to run out, because a fork bomb of shells
             * never makes it run out. */
            if (check_storm(all, n, self)) {
                n = scan_processes(all, MAX_PROCS, self);
                apply_priorities(all, n, self);
                storm_recent = 4;   /* look again quickly for a while */
            }

            /* ms_since_cpu, not ms_since_slow.
             *
             * After a storm the loop forces ms_since_slow to SLOW_MS to
             * make the next pass a full one, while actually sleeping
             * 200ms. check_cpu_hogs then believed 5200ms had passed
             * when 200 had - a 26x understatement - so a process
             * pinning a core measured 3%, every hog's evidence was
             * wiped, unboosted shells got their priority back, and
             * demoted ones earned 20 seconds of calm credit per fifth
             * of a second. The CPU policy came apart precisely when a
             * runaway was provably active. This counter is never
             * forced. */
            if (ms_since_cpu >= SLOW_MS) {
                check_cpu_hogs(all, n, self, ms_since_cpu);
                ms_since_cpu = 0;
            }
            check_load(all, n, self);
            check_heat_and_power();

            slow_passes++;
            if (slow_passes % DISK_EVERY == 0)
                check_disk();
            if (slow_passes % SYNC_EVERY == 0)
                lp_sync();
            if (slow_passes == BOOT_OK_PASSES)
                clear_boot_count();

            /* A service stuck inside the kernel. Everything else here
             * would call this machine healthy. */
            check_stuck_services(ms_since_slow);

            ms_since_slow = 0;
        }

        /* ── memory ── */
        if (proc_read("/proc/meminfo", mem, sizeof(mem)) > 0) {
            avail      = proc_find_kv(mem, "MemAvailable");
            swap_total = proc_find_kv(mem, "SwapTotal");
            swap_free  = proc_find_kv(mem, "SwapFree");
        }

        if (avail < 0) {
            /* nothing to go on this time round */
        } else if (avail < reserve_kb) {
            long swap_used = (swap_total > 0 && swap_free >= 0)
                             ? swap_total - swap_free : 0;
            dprintf(STDERR_FILENO,
                    "\nguard: ** memory limit - %ldMB free (reserve %ldMB)"
                    "%s\n", avail / 1024, reserve_kb / 1024,
                    swap_used > 0 ? ", swap in use" : "");

            /* Reclaim and look again. Under normal pressure that is
             * one process at a time - killing several at once would
             * take more than necessary. Under a fork storm it is a
             * whole process group per round; kill_largest decides. */
            if (kill_largest(self, reserve_kb - avail)) {
                stuck = 0;
            } else {
                /* Nothing may be killed and memory is still below the
                 * reserve. This is the state the old code sat in
                 * forever: printing the same line five times a second,
                 * reclaiming nothing, on a board nobody could reach.
                 *
                 * The watchdog does not help here - it is running fine,
                 * so it keeps petting the timer. Nothing else is going
                 * to intervene. After a minute of it, a reboot is the
                 * only move left, and a board that reboots is worth
                 * more than one that is wedged and silent: bootcount
                 * will put it into safe mode if it happens again.
                 */
                stuck++;
                if (stuck == 1)
                    dprintf(STDERR_FILENO,
                            "guard: ** nothing left that may be killed."
                            " Watching for a minute before rebooting.\n");
                if (stuck >= STUCK_PASSES) {
                    dprintf(STDERR_FILENO,
                            "guard: ** out of memory for %d seconds with"
                            " nothing to reclaim.\n"
                            "guard: ** Rebooting. This is the last move"
                            " left; a wedged board helps nobody.\n",
                            (int)(STUCK_PASSES * POLL_BUSY_MS / 1000));
                    /* Into the log before the reboot, or the only
                     * record of why the board restarted goes out the
                     * serial port and is gone. */
                    lp_log("guard", "out of memory with nothing left to"
                                    " reclaim - rebooting");
                    lp_sync();
                    lp_reboot(LINUX_REBOOT_CMD_RESTART);
                    /* If that did not work there is nothing else. */
                    dprintf(STDERR_FILENO,
                            "guard: ** the reboot was refused\n");
                    stuck = 0;
                }
            }
            warned = true;
            sleep_ms = POLL_BUSY_MS;
        } else if (avail < warn_kb) {
            /* Above the reserve. Whatever this is, it is not the wedge
             * the reboot exists for - so the counter starts again.
             * Without this, dips that each recovered on their own added
             * up across hours until the 300th one rebooted a healthy
             * board, and the message blamed "60 seconds" of something
             * that had never lasted more than a few. */
            stuck = 0;
            if (!warned) {
                dprintf(STDERR_FILENO,
                        "guard: warning - %ldMB free\n", avail / 1024);
                warned = true;
            }
            sleep_ms = POLL_BUSY_MS;
        } else if (warned) {
            /* Once it recovers, re-arm the warning. */
            dprintf(STDERR_FILENO,
                    "guard: recovered - %ldMB free\n", avail / 1024);
            warned = false;
            stuck  = 0;
        }

        /* After a storm, look again in a second rather than five: the
         * thing that forked 1600 times may still be forking. */
        if (storm_recent > 0) {
            storm_recent--;
            if (sleep_ms > POLL_BUSY_MS)
                sleep_ms = POLL_BUSY_MS;
            ms_since_slow = SLOW_MS;    /* force a full pass next time */
        }

        lp_sleep_ms(sleep_ms);
        ms_since_slow += sleep_ms;
        ms_since_cpu  += sleep_ms;
    }
}
