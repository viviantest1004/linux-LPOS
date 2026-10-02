/* init.c - PID 1.
 *
 * Starting this program is the last thing the kernel does.
 * From here on it is our responsibility.
 *
 * The rules that only apply to PID 1:
 *   - It must never exit. If it does, the kernel panics.
 *   - Every orphaned process is reparented here. Left unreaped, zombies
 *     pile up until the process table is full.
 *   - It has to do the early setup itself: mount filesystems, open the
 *     console. What systemd would have done, we do. */
#include "types.h"
#include "syscall.h"
#include "osname.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

/* Our shell, by the name that is always ours.
 *
 * On the RAM-root images /bin/sh IS our shell, and /bin/lpsh is the same
 * file under a second name. On the desktop, /bin/sh belongs to Debian's
 * dash package - dash's own postinst manages that link, apt-key and every
 * maintainer script expect a POSIX shell there, and a package upgrade
 * would put it back if anything else took it. So the desktop's /bin/sh
 * is dash, and our shell lives at /bin/lpsh. /etc/rc is written for our
 * shell, and the console shell is ours, so init asks for it by the name
 * that cannot be taken away, and falls back to /bin/sh only on an image
 * built before /bin/lpsh existed. */
#define SHELL_OURS   "/bin/lpsh"
#define SHELL_POSIX  "/bin/sh"
static const char *shell_path(void)
{
    return lp_exists(SHELL_OURS) ? SHELL_OURS : SHELL_POSIX;
}
#define SHELL_PATH   shell_path()
#define RC_SCRIPT    "/etc/rc"
/* On the Debian-based desktop /etc/services belongs to netbase: it is
 * the port-name table glibc reads for getservbyname(). Our list lives
 * in /etc/lp/services there, and the boards keep the old path. */
#define SERVICES_OURS   "/etc/lp/services"
#define SERVICES_LEGACY "/etc/services"
/* A netbase port table has lines like "ssh 22/tcp". Started as a list
 * of programs, every one of those lines is a service that cannot run,
 * restarted every thirty seconds forever - which is what the first
 * desktop image did when it carried an init older than /etc/lp/services.
 * So the old path is only believed when it does not look like that. */
static bool is_port_table(const char *path)
{
    static char buf[4096];
    long n = proc_read(path, buf, sizeof buf - 1);
    if (n <= 0)
        return false;
    buf[n] = '\0';
    for (char *l = buf; l && *l; ) {
        char *next = strchr(l, '\n');
        if (next)
            *next++ = '\0';
        while (*l == ' ' || *l == '\t')
            l++;
        if (*l && *l != '#') {
            char *w = l;
            while (*w && *w != ' ' && *w != '\t')
                w++;                       /* skip the first word */
            while (*w == ' ' || *w == '\t')
                w++;
            char *d = w;
            while (*d >= '0' && *d <= '9')
                d++;
            if (d > w && (strncmp(d, "/tcp", 4) == 0 ||
                          strncmp(d, "/udp", 4) == 0))
                return true;
        }
        l = next;
    }
    return false;
}

static const char *services_path(void)
{
    static const char *chosen;
    if (!chosen) {
        if (lp_exists(SERVICES_OURS) || !lp_exists(SERVICES_LEGACY))
            chosen = SERVICES_OURS;
        else if (is_port_table(SERVICES_LEGACY)) {
            dprintf(STDERR_FILENO, "init: %s is a port table, not a service"
                    " list - put services in %s\n", SERVICES_LEGACY,
                    SERVICES_OURS);
            chosen = SERVICES_OURS;
        } else
            chosen = SERVICES_LEGACY;
    }
    return chosen;
}
#define SERVICES     services_path()

/* Services somebody added on this machine.
 *
 * /etc/services is in the kernel image, so it is read-only in every
 * sense that matters: editing it lasts until the next boot. That left
 * no way at all to have init supervise a program of your own - the only
 * writable thing that ran at boot was /data/rc.local, which starts
 * something once and never looks at it again. A web server started that
 * way stays dead the first time it crashes, on a board nobody is
 * watching, which is the whole reason init supervises anything.
 *
 * So this file is read too, right after the built-in one and after
 * /etc/rc has mounted /data. Same format, same supervision, same
 * protection from guard - a service init names is one guard will not
 * kill under memory pressure. `service add` writes it. */
#define USER_SERVICES "/data/services"
#define RESPAWN_MS   1000   /* pause before a respawn, to stop a busy loop */
/* Nine of these are the system's own. The rest is room for what
 * somebody adds with `service add`; past that init says so rather than
 * silently dropping the last line. */
#define MAX_SERVICES 24
#define SERVICE_PIDS "/var/service.pids"
#define MAX_SVC_ARGS 16

/* The virtual filesystems to mount.
 * Without these there is no ps and no /dev/null. */
static const struct {
    const char *source;
    const char *target;
    const char *fstype;
    unsigned long flags;
} MOUNTS[] = {
    { "proc",     "/proc", "proc",     MS_NOSUID | MS_NODEV | MS_NOEXEC },
    { "sysfs",    "/sys",  "sysfs",    MS_NOSUID | MS_NODEV | MS_NOEXEC },
    /* With devtmpfs the kernel creates the device nodes itself.
     * It needs CONFIG_DEVTMPFS. */
    { "devtmpfs", "/dev",  "devtmpfs", MS_NOSUID },
    /* An SSH session needs a pseudo terminal. Without devpts, dropbear
     * cannot start a shell. This has to come after /dev. */
    { "devpts",   "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC },
    /* /run holds what is true only while the machine is running: udev's
     * device database, the D-Bus socket, the session's XDG_RUNTIME_DIR,
     * pid files.
     *
     * On the RAM root this was already a tmpfs by accident - the whole
     * root was one. On a disk root it is a real directory, and then a
     * socket from the last boot is still sitting there at the next one.
     * A stale /run/dbus/system_bus_socket is worse than a missing one:
     * a client connects, finds nothing at the other end, and reports a
     * connection error rather than "no bus here". */
    { "tmpfs",    "/run",  "tmpfs",    MS_NOSUID | MS_NODEV },
};

static void mount_filesystems(void)
{
    for (unsigned i = 0; i < sizeof(MOUNTS) / sizeof(MOUNTS[0]); i++) {
        /* Create the mount point if it is missing from the initramfs. */
        if (!lp_is_dir(MOUNTS[i].target))
            lp_mkdir(MOUNTS[i].target, 0755);

        long r = lp_mount(MOUNTS[i].source, MOUNTS[i].target,
                          MOUNTS[i].fstype, MOUNTS[i].flags, NULL);
        if (r < 0)
            dprintf(STDERR_FILENO, "init: cannot mount %s (%ld)\n",
                    MOUNTS[i].target, -r);
    }
}

/* Attach the console to stdin, stdout and stderr.
 *
 * The kernel opens /dev/console as fd 0,1,2 before running init. But if
 * the initramfs has no /dev/console node that fails ("unable to open an
 * initial console") and init starts with no file descriptors at all - in
 * which case not even an error message can get out, and the cause is
 *
 * impossible to find. Once devtmpfs is mounted /dev/console exists, so we
 * open it ourselves. */
static void setup_console(void)
{
    long fd = lp_open("/dev/console", O_RDWR, 0);
    if (fd < 0)
        return;             /* if this fails there is no way to say so */

    lp_dup2((int)fd, STDIN_FILENO);
    lp_dup2((int)fd, STDOUT_FILENO);
    lp_dup2((int)fd, STDERR_FILENO);

    /* Tell the kernel's line editor that input is UTF-8.
     *
     * Without this, backspace erases a single byte. A Hangul character is
     * three bytes, so one press would leave a broken fragment instead of
     * removing the character. IUTF8 makes the kernel erase the whole thing. */
    lp_term_set_utf8((int)fd);

    if (fd > STDERR_FILENO)
        lp_close((int)fd);
}

/* Put the boot screen up.
 *
 * It runs as a separate program rather than as code in here: drawing
 * needs a font and some arithmetic, and none of that should be resident
 * in PID 1 for the life of the machine when it is wanted once, for a
 * second, at boot. Forking it costs a few milliseconds and it exits.
 *
 * Failure is normal and silent - a board with nothing plugged into HDMI
 * has no framebuffer, and there is nothing to say about that. */
static pid_t splash_pid = -1;

static void show_splash(void)
{
    pid_t pid = lp_fork();
    if (pid < 0)
        return;

    if (pid == 0) {
        char *argv[] = { (char *)"splash", NULL };
        char *envp[] = { NULL };
        lp_execve("/bin/splash", argv, envp);
        lp_exit(127);
    }

    /* Deliberately not waited for. splash sits watching for /dev/fb0 to
     * turn up, which can take a second or two while the graphics device
     * is probed - and the boot has better things to do meanwhile. */
    splash_pid = pid;
}

/* Let the splash finish before anything else draws on the screen.
 *
 * They are drawing to the same pixels by two different routes - splash
 * straight into the framebuffer, the shell through the console driver -
 * and if the splash lands second it covers a prompt that is still live,
 * which is worse than no splash at all. By the time this is called the
 * boot script has already run, so normally there is nothing to wait for.
 * The three seconds is a ceiling, not a delay. */
static void wait_for_splash(void)
{
    if (splash_pid <= 0)
        return;

    for (int waited = 0; waited < 3000; waited += 100) {
        int   status = 0;
        pid_t r = lp_waitpid(splash_pid, &status, WNOHANG);
        if (r == splash_pid) {
            splash_pid = -1;
            return;
        }
        lp_sleep_ms(100);
    }
}

/* What the screen says when a shell starts on it.
 *
 * The serial console has seen every line of the boot; the screen has
 * seen the splash and whatever the kernel decided was important. Someone
 * sitting in front of it needs to be told where they are and what to
 * type, or a bare prompt is all they get. */
static void screen_greeting(int fd)
{
    dprintf(fd, "\x1b[H\x1b[2J");        /* clear, and go to the top */
    dprintf(fd, "  " LP_OS_TITLE "\n");
    dprintf(fd, "  " LP_OS_MACHINE "\n\n");
    dprintf(fd, "  help     every command there is\n");
    dprintf(fd, "  sysinfo  what this machine is and how it is doing\n");
    dprintf(fd, "  top      what is running\n\n");
    dprintf(fd, "  Tab completes. The arrow keys go back through what you\n");
    dprintf(fd, "  have typed. Files under /data and /root survive a reboot.\n\n");
}

static void banner(void)
{
    printf("\n");
    printf("  " LP_OS_NAME " OS\n");
    printf("  init (pid %d)\n", lp_getpid());
    printf("\n");
}

/* ── Supervising services ─────────────────────────────────────────
 *
 * This is what PID 1 is really for. Every line of /etc/services is started
 * as a program, and restarted when it dies.
 *
 * Why here and not in /etc/rc: rc runs once and finishes. Nothing revives
 * what it started. If the SSH server died that way, there would be no way
 * left to reach the machine. init already receives every child's exit, so
 * watching them here is the natural place. */

typedef struct {
    char   line[256];               /* split in place; argv points into it */
    char   orig[256];               /* the same line before splitting, so a
                                       rescan can tell "already running" from
                                       "new" without rebuilding the argv */
    char  *argv[MAX_SVC_ARGS + 1];
    /* What this service is CALLED, which is not always argv[0].
     *
     * `service status wpa_supplicant`, guard's protection list and
     * defend's "which supervised service holds this socket" all work by
     * name, and they all read it from here. A service that runs through
     * a wrapper - "sh /etc/wpa-start" - has argv[0] "sh", and calling it
     * that makes every one of those answer about the wrong thing, or
     * about nothing. So a line may say what it is called. */
    char   name[64];
    pid_t  pid;
    int    fails;                   /* consecutive failures */
    s64    retry_at;                /* monotonic ms to start it again, 0 = none */
    s64    started_at;              /* monotonic ms, to tell a crash loop from
                                       a service that ran for a week */
    bool   given_up;                /* failed too often; not tried again */
    bool   was_off;                 /* it was in the disabled list last look */
    bool   off_refused;             /* said once: critical, listed, started anyway */
} service_t;

static service_t services[MAX_SERVICES];
static int       nservices;

/* ── Services this machine cannot be left without ──
 *
 * Everything in /etc/services gets restarted when it dies. These three
 * are also never given up on, however often they fail, and their backoff
 * is capped short.
 *
 *   guard     is the whole self-defence of the machine: the memory
 *             killer, the disk-full check, the CPU-hog demotion. With
 *             guard gone the board looks fine and is defenceless, and
 *             the next thing that goes wrong takes it down with nothing
 *             to stop it or say so. It is the one process whose absence
 *             is invisible, which is exactly why it must not stay absent.
 *   dropbear  is the only way in.
 *   watchdog  is the only way back from a wedge.
 *
 * "Giving up" is right for a service that is broken - it stops a restart
 * loop from being the thing that kills the board. It is wrong for these
 * three, because a board with none of them running is not a board worth
 * protecting from a restart loop. The short backoff is what makes the
 * loop affordable. */
static const char *CRITICAL[] = { "guard", "dropbear", "watchdog", NULL };

static bool is_critical(const char *name)
{
    for (int i = 0; CRITICAL[i]; i++)
        if (strcmp(name, CRITICAL[i]) == 0)
            return true;
    return false;
}

/* ── Telling guard which pids to protect ──
 *
 * guard decides what it may kill. It used to decide by process name, and
 * a name is whatever the process says it is: anything called "dropbear"
 * was safe from the memory killer, and anything called "sh" was safe
 * from both that and the CPU-hog demotion - which made every runaway
 * started from a shell immune to the only thing watching for runaways.
 *
 * A pid cannot be forged. init is the one process that knows the pids of
 * the things it supervises, so it writes them here and guard reads them.
 * /var is on the root filesystem, which is RAM inside the kernel image
 * and writable by root alone, so nothing unprivileged can plant a pid in
 * this file and become unkillable. */
static bool pids_dirty = true;

static void publish_service_pids(void)
{
    long fd = lp_open(SERVICE_PIDS, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;             /* guard falls back to its built-in list */
    for (int i = 0; i < nservices; i++)
        if (services[i].pid > 0)
            dprintf((int)fd, "%d %s\n",
                    (int)services[i].pid, services[i].name);
    lp_close((int)fd);
}

/* Split a line on whitespace into argv. Quotes are not handled - no
 * service command line has ever needed an argument with a space in it. */
/* ── '?<path>' : only if that path exists ──
 *
 * Some services only make sense on some machines. wpa_supplicant is the
 * case that forced this: on a real Pi it drives the WiFi, and in a
 * virtual machine there is no wlan0, so it fails instantly, and init
 * restarts it, backs off, restarts it, twelve times, printing a driver
 * error each round before giving up. A supervisor doing exactly what it
 * is meant to do, about hardware that is not there and never will be.
 *
 * "?/sys/class/net/wlan0 wpa_supplicant -i wlan0 ..." says the
 * condition out loud instead. No condition, no change: everything else
 * starts as it always did. */
static bool service_wanted_here(const char *line, const char **rest)
{
    *rest = line;
    if (*line != '?')
        return true;

    line++;
    char path[128];
    size_t i = 0;
    while (*line && *line != ' ' && *line != '\t' && i < sizeof path - 1)
        path[i++] = *line++;
    path[i] = '\0';

    while (*line == ' ' || *line == '\t')
        line++;
    *rest = line;

    if (lp_exists(path))
        return true;

    printf("init: not starting %s - there is no %s on this machine\n",
           line, path);
    return false;
}

static bool parse_service(const char *line, service_t *svc)
{
    strlcpy(svc->line, line, sizeof(svc->line));
    strlcpy(svc->orig, line, sizeof(svc->orig));

    int n = 0;
    char *p = svc->line;

    while (*p && n < MAX_SVC_ARGS) {
        while (*p == ' ' || *p == '\t') *p++ = '\0';
        if (*p == '\0') break;

        svc->argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    svc->argv[n] = NULL;
    if (n == 0)
        return false;

    /* "=name" as the first word says what this service is called, and
     * is then dropped from the command. Without it the name is argv[0],
     * which is what it has always been and is right for every service
     * that is simply its own program.
     *
     *   =wpa_supplicant sh /etc/wpa-start
     *
     * is "wpa_supplicant" to `service`, to guard and to defend, and
     * "sh /etc/wpa-start" to execve. Before this it was "sh" to all
     * three, so `service status wpa_supplicant` answered that there is
     * no such service on a machine that was running it. */
    if (svc->argv[0][0] == '=' && svc->argv[0][1] != '\0' && n > 1) {
        strlcpy(svc->name, svc->argv[0] + 1, sizeof svc->name);
        for (int i = 0; i < n; i++)
            svc->argv[i] = svc->argv[i + 1];
        n--;
    } else {
        strlcpy(svc->name, svc->argv[0], sizeof svc->name);
    }
    return n > 0;
}

static void start_service(service_t *svc)
{
    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "init: fork failed for %s\n", svc->name);
        svc->pid = -1;
        return;
    }

    if (pid == 0) {
        char *envp[] = {
            (char *)"PATH=/bin:/data/bin:/usr/local/bin:/sbin:/usr/bin:/usr/sbin",
            (char *)"HOME=/root",
            NULL
        };
        lp_execve(svc->argv[0], svc->argv, envp);

        /* Search PATH ourselves: execve does not look at it. */
        char path[256];
        snprintf(path, sizeof(path), "/bin/%s", svc->argv[0]);
        lp_execve(path, svc->argv, envp);

        dprintf(STDERR_FILENO, "init: cannot run %s\n", svc->argv[0]);
        lp_exit(127);
    }

    svc->pid = pid;
    pids_dirty = true;
}

/* Safe mode: lpzero.safe on the kernel command line.
 *
 * The boot partition is FAT32 and cmdline.txt is a text file on it, so
 * this is one word typed from any PC with a card reader - which is the
 * only tool available when a machine will not come up far enough to log
 * into. In safe mode init starts SSH and nothing else, and /etc/rc
 * skips the user's startup script.
 *
 * SSH rather than nothing at all, because a safe mode you cannot reach
 * is only a slower way of pulling the card out. */
static bool safe_mode(void)
{
    static bool checked = false;
    static bool safe    = false;

    if (checked)
        return safe;
    checked = true;

    char cmdline[1024];
    if (proc_read("/proc/cmdline", cmdline, sizeof(cmdline)) > 0)
        safe = (strstr(cmdline, "lpzero.safe") != NULL);
    return safe;
}

/* In safe mode, which services still start.
 *
 * Only the SSH server. Not ntp - a wrong clock does not stop anyone
 * logging in. Not logd - it writes to the data partition, which may be
 * the thing that is wrong. Not the watchdog - if the machine is being
 * rescued, a reset every fifteen seconds is in the way. */
static bool wanted_in_safe_mode(const char *program)
{
    return strcmp(program, "dropbear") == 0;
}

static void load_services_from(const char *path)
{
    /* Big enough for the file plus the comments explaining it. When this
     * was 2048 the file grew past it and the last service - the watchdog -
     * was silently cut off: it never started, and nothing said so. A
     * service quietly missing is the worst way for this to fail, so the
     * truncation is now reported. */
    /* 32K, not 8K. The file that ships is 8.4K - most of it comments
     * explaining what each service is for - and at 8K the last line of
     * it fell off the end. That line was `defend -d`, the thing that
     * watches what happens to a board left on the internet, and it had
     * simply never started on any image. The warning below is what
     * found it; the size is what fixes it. */
    static char buf[32768];
    long n = proc_read(path, buf, sizeof(buf));
    if (n <= 0)
        return;

    if (n >= (long)sizeof(buf) - 1)
        dprintf(STDERR_FILENO,
                "init: %s is larger than %d bytes - the end was cut off\n",
                path, (int)sizeof(buf));

    char *p = buf;
    while (*p && nservices < MAX_SERVICES) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = '\0';

        /* Skip blank lines and comments. */
        while (*p == ' ' || *p == '\t') p++;
        if (*p && *p != '#') {
            const char *cmd = p;
            if (!service_wanted_here(p, &cmd)) {
                if (!eol) break;
                p = eol + 1;
                continue;
            }
            if (parse_service(cmd, &services[nservices])) {
                if (safe_mode() &&
                    !wanted_in_safe_mode(services[nservices].name)) {
                    printf("init: safe mode - not starting %s\n",
                           services[nservices].name);
                    if (!eol) break;
                    p = eol + 1;
                    continue;
                }
                start_service(&services[nservices]);
                services[nservices].started_at = lp_monotonic_ms();
                printf("init: started service %s (pid %d)\n",
                       services[nservices].name,
                       (int)services[nservices].pid);
                nservices++;
            }
        }

        if (!eol) break;
        p = eol + 1;
    }

    if (pids_dirty) {
        publish_service_pids();
        pids_dirty = false;
    }

    /* One more thing that used to be silent: more lines than there is
     * room for meant the last services never started and nothing said
     * so. The file is read in full above; this is the table filling up. */
    if (nservices >= MAX_SERVICES)
        dprintf(STDERR_FILENO,
                "init: ** %s has more than %d services."
                " The rest were not started.\n", path, MAX_SERVICES);
}

/* The system's services first, then whatever this machine added.
 *
 * That order matters. The system's own - guard, dropbear, the watchdog -
 * are what get the machine back if something else goes wrong, so they
 * take the table's places before anything a person wrote can fill it. */
static void load_services(void)
{
    load_services_from(SERVICES);
    load_services_from(USER_SERVICES);
}

/* Pick up a service added since boot.
 *
 * `service add` writes /data/services, and the first question anybody
 * asks after that is whether they have to reboot. Answering no costs
 * one small file read every few seconds inside a loop that is otherwise
 * asleep, and it is the difference between a supervisor people use and
 * one they work around with rc.local - which starts a program once and
 * never looks at it again.
 *
 * Only additions are acted on. A line taken OUT of the file is left
 * running until it dies or `service stop` says so: init killing a
 * process because a file changed underneath it is not a surprise
 * anybody wants at three in the morning. */
static void rescan_user_services(void)
{
    static s64 next_look = 0;
    s64 now = lp_monotonic_ms();
    if (now < next_look)
        return;
    next_look = now + 5000;

    if (safe_mode())
        return;                     /* rescue mode runs SSH and nothing else */

    char buf[8192];
    long n = proc_read(USER_SERVICES, buf, sizeof buf);
    if (n <= 0)
        return;

    char *p = buf;
    while (*p && nservices < MAX_SERVICES) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = '\0';

        while (*p == ' ' || *p == '\t') p++;
        if (*p && *p != '#') {
            const char *cmd = p;
            if (service_wanted_here(p, &cmd)) {
                bool known = false;
                for (int i = 0; i < nservices; i++)
                    if (strcmp(services[i].orig, cmd) == 0) { known = true; break; }

                if (!known && parse_service(cmd, &services[nservices])) {
                    start_service(&services[nservices]);
                    services[nservices].started_at = lp_monotonic_ms();
                    printf("init: started service %s (pid %d) - added since"
                           " boot\n", services[nservices].name,
                           (int)services[nservices].pid);
                    nservices++;
                }
            }
        }

        if (!eol) break;
        p = eol + 1;
    }

    if (pids_dirty) {
        publish_service_pids();
        pids_dirty = false;
    }
}

#define DISABLED_FILE  "/data/services.disabled"

/* ── Turning a service off ────────────────────────────────────────────
 *
 * /data/services.disabled is a list of names, one per line, that init
 * will not start. `service stop x` writes the name there and kills the
 * process; init sees it die, finds it in the list, and leaves it alone.
 * `service start x` takes the line out, and init starts it again on its
 * next look.
 *
 * A file rather than a signal, because there is no way to send init a
 * message here - no handler support in the libc, and pid 1 catching
 * signals is its own set of problems. A file is also the thing you can
 * read to find out why something is not running, which a signal is not.
 *
 * It is on /data, so a service turned off stays off across a reboot.
 * Delete the file and everything comes back. */
static bool service_disabled(const char *name)
{
    long fd = lp_open(DISABLED_FILE, O_RDONLY, 0);
    if (fd < 0)
        return false;

    char line[128];
    bool found = false;
    while (readline((int)fd, line, sizeof line) >= 0) {
        char *end = line + strlen(line);
        while (end > line && (end[-1] == ' ' || end[-1] == '\r'))
            *--end = '\0';
        if (line[0] && strcmp(line, name) == 0) { found = true; break; }
    }
    lp_close((int)fd);
    return found;
}

/* Note that a supervised service died, and when to try it again.
 * true if it was one of ours.
 *
 * The restart is scheduled, not slept through. init used to sleep here,
 * which meant that while one service was backing off, pid 1 was not
 * reaping anything or restarting the shell - the whole machine waited
 * on the one thing that was already broken.
 *
 * The delay doubles each time, up to half a minute. Without that, a
 * service that cannot possibly start - the watchdog daemon on a board
 * with no watchdog - takes the console with it: twenty restarts and
 * forty lines of the same message, in the second and a half it takes
 * anyone to read the first one. */
static bool respawn_service(pid_t dead, int status)
{
    for (int i = 0; i < nservices; i++) {
        if (services[i].pid != dead)
            continue;

        services[i].pid = -1;
        pids_dirty = true;

        if (service_disabled(services[i].name)) {
            printf("init: service %s stopped (it is in %s)\n",
                   services[i].name, DISABLED_FILE);
            services[i].retry_at = 0;
            services[i].fails    = 0;
            return true;
        }

        /* A service that stayed up for a minute was not failing to
         * start - it died of something else. Do not make it serve a
         * backoff earned months ago. */
        if (lp_monotonic_ms() - services[i].started_at > 60000)
            services[i].fails = 0;
        services[i].fails++;

        /* "Nothing for me to do on this machine." Not a failure, and
         * not something that changes while the machine is running, so
         * asking again - twelve times, with a growing backoff, printing
         * the same thing each time - is pure noise. The watchdog service
         * on a virtual machine is the case this exists for. */
        if (LP_WIFEXITED(status) &&
            LP_WEXITSTATUS(status) == LP_EXIT_NO_HARDWARE) {
            printf("init: %s has nothing to do on this machine"
                   " - not starting it again\n", services[i].name);
            services[i].retry_at = 0;
            services[i].fails    = 0;
            services[i].given_up = true;
            return true;
        }

        bool critical = is_critical(services[i].name);

        if (services[i].fails > 12 && !critical) {
            dprintf(STDERR_FILENO,
                    "init:   %s keeps failing. Giving up on it.\n"
                    "init:   'service start %s' tries again.\n",
                    services[i].name, services[i].name);
            services[i].retry_at = 0;
            services[i].given_up = true;
            return true;
        }

        long cap = critical ? 5000 : 30000;
        long wait_ms = RESPAWN_MS;
        for (int n = 1; n < services[i].fails && wait_ms < cap; n++)
            wait_ms *= 2;
        if (wait_ms > cap) wait_ms = cap;

        /* Say it plainly rather than quietly retrying forever: a
         * critical service that has failed a dozen times is a fault
         * somebody has to look at, even though init will keep trying. */
        if (critical && services[i].fails == 13)
            dprintf(STDERR_FILENO,
                    "init:   ** %s has failed 12 times and is critical,"
                    " so init keeps trying.\n"
                    "init:   ** Something is wrong with it. This machine"
                    " is running without it.\n",
                    services[i].name);

        int code = LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : -1;
        dprintf(STDERR_FILENO,
                "init: service %s exited (code %d) - again in %lds\n",
                services[i].name, code, wait_ms / 1000);
        {
            char m[160];
            snprintf(m, sizeof m, "service %s exited (code %d), restarting",
                     services[i].name, code);
            lp_log("init", m);
        }

        services[i].retry_at = lp_monotonic_ms() + wait_ms;
        return true;
    }
    return false;
}

/* Start whatever is due. Called every time round the main loop. */
/* Set the moment a shutdown starts, so services stop being put back.
 * Declared here rather than beside the shutdown code because
 * restart_due_services - the thing it has to stop - comes first. */
static bool shutting_down = false;

static void restart_due_services(void)
{
    if (shutting_down)
        return;                 /* the machine is going down; leave them dead */

    s64 now = lp_monotonic_ms();
    for (int i = 0; i < nservices; i++) {
        service_t *s = &services[i];

        /* ── Why is_critical() is checked HERE and not in `service` ──
         *
         * `service stop guard` refuses without --force. That check lives
         * in the service program, which is the wrong process for it:
         * the file it guards is an ordinary line of text in /data, and
         * anything that can write a file can append to it. Two commands
         * -   echo guard >> /data/services.disabled
         *     kill guard
         * - reproduced exactly the failure the supervision was added to
         * stop, and left guard down for the rest of the uptime.
         *
         * A refusal is only real where the decision is made. init is the
         * only reader that acts on this file, so the answer belongs
         * here: the three services the board cannot defend itself
         * without are restarted whatever the file says. `service` still
         * asks for --force, but that is now a courtesy to the person
         * typing, not the enforcement. */
        if (service_disabled(s->name)) {
            if (!is_critical(s->name)) {
                s->was_off = true;
                continue;               /* somebody turned it off */
            }
            if (!s->off_refused) {
                s->off_refused = true;
                printf("init: %s is listed in %s, but it is one of the"
                       " services this board cannot defend itself"
                       " without.\n"
                       "init:   Starting it anyway. Remove the line to"
                       " stop seeing this.\n",
                       s->name, DISABLED_FILE);
            }
        }

        /* Coming back from being turned off clears the failure history.
         * Whoever re-enabled it is asking for a fresh try, not for the
         * backoff it had earned before. */
        if (s->was_off) {
            s->was_off  = false;
            s->fails    = 0;
            s->given_up = false;
            s->retry_at = 0;
        }

        if (s->pid > 0)
            continue;
        if (s->given_up)
            continue;                   /* it said so already */
        if (s->retry_at != 0 && now < s->retry_at)
            continue;                   /* still serving its backoff */

        s->retry_at = 0;
        start_service(s);
        s->started_at = lp_monotonic_ms();
        printf("init: started service %s (pid %d)\n",
               s->name, (int)s->pid);
    }

    /* Only when a pid actually changed. This file is read by guard to
     * decide what it must not kill, so it has to be right, but rewriting
     * it every time round the loop would be a write a second forever. */
    if (pids_dirty) {
        publish_service_pids();
        pids_dirty = false;
    }
}

/* Is anything waiting to be restarted? Only then does the main loop have
 * to poll instead of blocking in wait(). */
static bool services_pending(void)
{
    /* A service waiting out a backoff needs looking at again, and so
     * does one that somebody may be about to re-enable - blocking in
     * wait() would mean `service start` did nothing until the next time
     * some other process happened to exit.
     *
     * One that init has given up on does not: it will not be started
     * again until somebody says so, and that somebody takes it out of
     * the disabled list, which is the was_off path. Polling for it
     * forever would be a wakeup a second for the life of the machine. */
    for (int i = 0; i < nservices; i++) {
        if (services[i].pid > 0)
            continue;
        if (services[i].retry_at != 0)
            return true;
        if (services[i].was_off)
            return true;
    }
    return false;
}

/* Run the boot script and wait for it to finish.
 *
 * This lives in a script rather than in init because things like joining
 * WiFi or starting SSH change with the setup. Editing /etc/rc beats
 * rebuilding init every time. */
static void run_rc(void)
{
    if (!lp_exists(RC_SCRIPT))
        return;

    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "init: fork failed for rc\n");
        return;
    }

    if (pid == 0) {
        char *argv[] = { (char *)SHELL_PATH, (char *)RC_SCRIPT, NULL };
        char *envp[] = {
            (char *)"PATH=/bin:/data/bin:/usr/local/bin:/sbin:/usr/bin:/usr/sbin",
            (char *)"HOME=/root",
            NULL
        };
        lp_execve(SHELL_PATH, argv, envp);
        lp_exit(127);
    }

    /* Wait for rc. Reap any other child that dies meanwhile. */
    for (;;) {
        int status = 0;
        pid_t done = lp_wait(&status);
        if (done < 0)
            break;
        if (done == pid) {
            if (!LP_WIFEXITED(status) || LP_WEXITSTATUS(status) != 0)
                dprintf(STDERR_FILENO, "init: %s exited with an error\n",
                        RC_SCRIPT);
            break;
        }
    }
}

/* Start one shell. Returns the child's pid. */
/* Defined below, next to the screen-shell decision that also uses it. */
static bool console_device(char *out, size_t size);

static pid_t spawn_shell_on(const char *tty, bool greet)
{
    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "init: fork failed (%d)\n", (int)pid);
        return -1;
    }

    if (pid == 0) {
        /* Become the leader of a new session. This is half of getting a
         * controlling terminal; the other half is asking for one, below,
         * and both halves are needed. With neither, the terminal has no
         * foreground process group, the line discipline has nobody to
         * send a signal to, and Ctrl-C is silently nothing - you type it
         * at a `cat` waiting on input and the 0x03 goes in as data. */
        lp_setsid();

        /* A NULL tty means "inherit whatever init has". Otherwise open
         * the named device and make it this shell's stdin, stdout and
         * stderr. */
        if (tty) {
            long fd = lp_open(tty, O_RDWR, 0);
            if (fd < 0)
                lp_exit(127);
            lp_dup2((int)fd, STDIN_FILENO);
            lp_dup2((int)fd, STDOUT_FILENO);
            lp_dup2((int)fd, STDERR_FILENO);
            if (fd > STDERR_FILENO)
                lp_close((int)fd);
            lp_term_set_utf8(STDIN_FILENO);
        }

        /* Claim it. Without this a shell on the console has no
         * controlling terminal, and every key that is supposed to be a
         * signal - Ctrl-C, Ctrl-\, Ctrl-Z - is just a byte.
         *
         * It has to be the real device: /dev/console can never be a
         * controlling terminal, because the kernel forces O_NOCTTY for
         * it. That is why the caller resolves the device name first
         * rather than letting the shell inherit init's own fds.
         *
         * Not fatal if it fails - a shell with no signals still beats no
         * shell - but it is worth saying, because everything downstream
         * of it looks like the shell ignoring the keyboard. */
        if (lp_term_make_controlling(STDIN_FILENO) < 0)
            dprintf(STDERR_FILENO,
                    "init: could not claim %s as the controlling"
                    " terminal - Ctrl-C will not work there\n",
                    tty ? tty : "the console");

        if (greet)
            screen_greeting(STDOUT_FILENO);

        char *argv[] = { (char *)SHELL_PATH, NULL };
        char *envp[] = {
            (char *)"PATH=/bin:/data/bin:/usr/local/bin:/sbin:/usr/bin:/usr/sbin",
            (char *)"HOME=/root",
            (char *)"TERM=linux",
            NULL
        };

        /* The screen of an installed desktop asks who you are.
         *
         * A root shell on tty1 with no password is right for a board on
         * a desk, and wrong for a laptop: Ctrl+Alt+F1 at the lock screen
         * gave whoever held the machine root. The desktop image says so
         * with /etc/lp/console-login, and there a virtual terminal gets
         * login (every account there has a password, root's is locked).
         * The serial console keeps its shell: nobody reaches that
         * without the case open or the VM's host, and it is the way back
         * in when everything else is broken. */
        if (tty && strncmp(tty, "/dev/tty", 8) == 0 &&
            tty[8] >= '0' && tty[8] <= '9' &&
            lp_exists("/etc/lp/console-login") && lp_exists("/bin/login")) {
            char *largv[] = { (char *)"/bin/login", NULL };
            lp_execve("/bin/login", largv, envp);
            dprintf(STDERR_FILENO, "init: cannot run /bin/login\n");
            lp_exit(127);
        }

        lp_execve(SHELL_PATH, argv, envp);

        dprintf(STDERR_FILENO, "init: cannot run %s\n", SHELL_PATH);
        lp_exit(127);
    }

    return pid;
}

/* The shell on whatever the console is.
 *
 * It used to be spawn_shell_on(NULL) - inherit init's own fds, which are
 * /dev/console. That is why Ctrl-C did nothing on this machine: a shell
 * whose terminal is /dev/console cannot have a controlling terminal, so
 * no key ever became a signal. Resolving the device by name and opening
 * that instead is the whole fix. */
static pid_t spawn_shell(void)
{
    char name[64], path[80];
    if (console_device(name, sizeof name)) {
        snprintf(path, sizeof path, "/dev/%s", name);
        if (lp_exists(path))
            return spawn_shell_on(path, false);
    }

    /* No /sys yet, or a console with no device node. The shell still
     * runs; it just has no signals, which is what it always had. */
    return spawn_shell_on(NULL, false);
}

/* Is the screen a separate place from the console we already talk to?
 *
 * The kernel picks one device as /dev/console - the last console= on the
 * command line. Ours puts the serial port last, because that is what a
 * headless board and an SSH session need.
 *
 * But in a VM with a window (UTM, or QEMU with a display), the screen is
 * tty1 and it would then show the boot messages and nothing else: the
 * shell would be off on the serial port, and the window would look dead.
 *
 * /sys/class/tty/console/active lists the active consoles, and the LAST
 * one is where /dev/console actually points. Being in the list is not
 * enough: with "console=tty1 console=ttyAMA0" both are listed, yet the
 * shell only lands on ttyAMA0. So we compare against the last entry, and
 * put a second shell on tty1 when that is not it. Costs one idle
 * process, and it means an HDMI screen plugged in later already has a
 * shell waiting on it. */
/* The name of the device /dev/console actually points at - "ttyAMA0",
 * "tty1" - or false when it cannot be worked out.
 *
 * This is needed for more than deciding where to put a second shell.
 * A shell on /dev/console has no controlling terminal and can never
 * have one: the kernel refuses to make major 5 minor 1 a controlling
 * terminal, whatever you open it with. So the shell has to be given the
 * real device, by name, or Ctrl-C on the console does nothing at all -
 * no signal is generated, because a terminal with no foreground process
 * group has nobody to signal. */
static bool console_device(char *out, size_t size)
{
    char buf[128];
    long fd = lp_open("/sys/class/tty/console/active", O_RDONLY, 0);
    if (fd < 0)
        return false;
    long n = lp_read((int)fd, buf, sizeof(buf) - 1);
    lp_close((int)fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';

    /* Find the last whitespace-separated name. With
     * "console=tty1 console=ttyAMA0" both are listed and the last one
     * is where /dev/console goes. */
    const char *last = buf;
    for (char *p = buf; *p; p++) {
        if (*p == ' ' || *p == '\n' || *p == '\t') {
            *p = '\0';
            if (p[1] && p[1] != ' ' && p[1] != '\n' && p[1] != '\t')
                last = p + 1;
        }
    }
    if (!*last)
        return false;

    strlcpy(out, last, size);
    return true;
}

static bool screen_needs_its_own_shell(void)
{
    if (!lp_exists("/dev/tty1"))
        return false;

    char name[64];
    if (!console_device(name, sizeof name))
        return false;

    return strcmp(name, "tty1") != 0;
}

/* ── Turning the machine off ──────────────────────────────────────────
 *
 * `poweroff` and `reboot` send init a signal; init does the work. The
 * work is not "call reboot(2)" - it is getting /data to a state where
 * the next boot does not have to repair it.
 *
 * /data is ext4 with a journal, so pulling the power is survivable, but
 * survivable is not the same as clean: the journal replays, fsck may
 * decide it wants a full check, and anything a service had buffered is
 * gone. On a board that is meant to run unattended for months, the
 * shutdown people type is the one chance to write everything down.
 *
 * So: stop restarting things, ask every service to finish, wait, insist
 * if they do not, sync, and unmount. Only then reboot.
 *
 * The signal handler does nothing but set a flag. Everything else -
 * printing, killing, unmounting - happens in the main loop, where it is
 * ordinary code that can take its time. A handler that did the work
 * would be running in the middle of whatever init was doing, on init's
 * stack, with no way to wait for anything. */
static volatile int shutdown_wanted = 0;
static pid_t shell_pid  = -1;    /* the console shell */
static pid_t screen_pid = -1;    /* the one on tty1, if any */   /* 0 none, 1 poweroff, 2 reboot */

static void on_poweroff(int sig) { (void)sig; shutdown_wanted = 1; }
static void on_reboot(int sig)   { (void)sig; shutdown_wanted = 2; }

/* Ask everything to stop, then make sure it did.
 *
 * SIGTERM first and a few seconds to act on it: dropbear closes its
 * sessions, logd flushes, guard writes its last line. Then SIGKILL for
 * whatever is left, because a shutdown that can be refused is not a
 * shutdown. */
static void stop_everything(void)
{
    printf("init: asking services to stop\n");
    for (int i = 0; i < nservices; i++)
        if (services[i].pid > 0)
            lp_kill(services[i].pid, SIGTERM);
    if (shell_pid > 0)  lp_kill(shell_pid, SIGTERM);
    if (screen_pid > 0) lp_kill(screen_pid, SIGTERM);

    /* Up to five seconds, but no longer than it takes. */
    for (int waited = 0; waited < 5000; waited += 100) {
        bool any = false;
        for (int i = 0; i < nservices; i++) {
            if (services[i].pid <= 0)
                continue;
            int st = 0;
            if (lp_waitpid(services[i].pid, &st, WNOHANG) == services[i].pid)
                services[i].pid = -1;
            else
                any = true;
        }
        if (!any)
            break;
        lp_sleep_ms(100);
    }

    /* Everything else on the machine, including anything a person left
     * running from a shell. -1 is "every process we may signal", and
     * the kernel does not deliver it to us or to pid 1. */
    lp_kill(-1, SIGTERM);
    lp_sleep_ms(300);
    lp_kill(-1, SIGKILL);
}

/* Take off everything mounted on top of a filesystem, deepest first.
 *
 * /proc/mounts is in mount order, so walking it backwards unmounts the
 * most recently added first - which is what a stack needs. Anything
 * that will not come off is skipped and named; one stubborn mount is
 * not a reason to leave the rest attached. */
/* Unmount everything under /media and /mnt.
 *
 * These are drives that were plugged in, so they may be pulled at any
 * moment and they may be slow. Reverse order for the same reason as
 * /data - a drive mounted inside another has to come off first - and
 * MNT_DETACH as a fallback, because a shell sitting in a directory on a
 * USB stick must not be able to stop the machine from shutting down.
 */
static void unmount_removable_drives(void)
{
    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0)
        return;

    char buf[8192];
    long got = lp_read((int)fd, buf, sizeof buf - 1);
    lp_close((int)fd);
    if (got <= 0)
        return;
    buf[got] = '\0';

    /* Collect first, then unmount in reverse: unmounting while walking
     * /proc/mounts means walking a list that changes underneath. */
    char points[24][96];
    int n = 0;

    for (char *line = buf; line && *line && n < 24; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char *sp = strchr(line, ' ');
        if (sp) {
            char *point = sp + 1;
            char *sp2 = strchr(point, ' ');
            if (sp2) *sp2 = '\0';
            if (strncmp(point, "/media/", 7) == 0 ||
                strncmp(point, "/mnt/", 5) == 0)
                strlcpy(points[n++], point, sizeof points[0]);
        }
        line = nl ? nl + 1 : NULL;
    }

    for (int i = n - 1; i >= 0; i--) {
        if (sys_call2(SYS_umount2, (long)points[i], 0) == 0)
            continue;
        /* Busy. Detach it: the data is already flushed by the unmount
         * attempt, and a drive nobody can unmount must not become a
         * machine nobody can switch off. */
        if (sys_call2(SYS_umount2, (long)points[i], 2 /* MNT_DETACH */) == 0)
            printf("init:   %s was busy - detached\n", points[i]);
        else
            printf("init:   %s would not unmount\n", points[i]);
    }

    if (n)
        printf("init:   %d plugged-in drive%s released\n",
               n, n == 1 ? "" : "s");
}

static void unmount_stack_above(const char *keep)
{
    char buf[8192];
    if (proc_read("/proc/mounts", buf, sizeof buf) <= 0)
        return;

    /* Collect the mount points, in order. */
    static char point[64][128];
    static char fstype[64][32];
    int n = 0;

    for (char *p = buf; *p && n < 64; ) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = '\0';

        /* "<source> <point> <type> <options> 0 0" */
        char *sp1 = strchr(p, ' ');
        if (sp1) {
            char *pt = sp1 + 1;
            char *sp2 = strchr(pt, ' ');
            if (sp2) {
                *sp2 = '\0';
                char *ty = sp2 + 1;
                char *sp3 = strchr(ty, ' ');
                if (sp3) *sp3 = '\0';
                strlcpy(point[n], pt, sizeof point[0]);
                strlcpy(fstype[n], ty, sizeof fstype[0]);
                n++;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }

    for (int i = n - 1; i >= 0; i--) {
        /* Leave the filesystem itself, and the kernel's own. */
        if (strcmp(point[i], keep) == 0)  continue;
        if (strcmp(point[i], "/") == 0)   continue;
        if (strcmp(point[i], "/proc") == 0 || strcmp(point[i], "/sys") == 0 ||
            strcmp(point[i], "/dev") == 0)
            continue;

        /* An overlay whose upper is on /data, or a bind from it. Both
         * hold /data open; a tmpfs does not, so leave it alone. */
        bool holds_data = (strcmp(fstype[i], "overlay") == 0) ||
                          (strcmp(point[i], "/root") == 0);
        if (!holds_data)
            continue;

        if (sys_call2(SYS_umount2, (long)point[i], 0) < 0)
            printf("init:   %s would not come off\n", point[i]);
    }
}

/* The type of the filesystem mounted exactly at point, or "" when
 * nothing is. The last line for it wins: that is the one on top. */
static void mount_type(const char *point, char *out, size_t size)
{
    char buf[8192];
    out[0] = '\0';
    if (proc_read("/proc/mounts", buf, sizeof buf) <= 0)
        return;
    for (char *p = buf; *p; ) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = '\0';
        /* "<source> <point> <type> <options> 0 0" */
        char *pt = strchr(p, ' ');
        char *ty = pt ? strchr(pt + 1, ' ') : NULL;
        char *end = ty ? strchr(ty + 1, ' ') : NULL;
        if (end) {
            *ty = '\0';
            *end = '\0';
            if (strcmp(pt + 1, point) == 0)
                strlcpy(out, ty + 1, size);
        }
        if (!eol) break;
        p = eol + 1;
    }
}

static void do_shutdown(int what)
{
    const char *word = (what == 2) ? "restarting" : "powering off";

    /* Before anything else: stop putting services back.
     *
     * Without this the console fills with "service guard exited - again
     * in 1s" underneath "stopping services", because restarting them is
     * exactly what init is for and nothing had told it to stop. Every
     * service killed here came straight back, and some of them were
     * writing to /data while it was being unmounted. */
    shutting_down = true;

    printf("\ninit: %s\n", word);

    lp_log("init", (what == 2) ? "reboot requested" : "poweroff requested");

    stop_everything();

    /* Write the time down before the disk goes away. This board has no
     * battery-backed clock, so without it the next boot starts in 1970
     * and every certificate check fails. ntp saves every 30 seconds;
     * this fills in that last window. Anything before 2020 means the
     * clock was never set, and saving that is worse than saving
     * nothing. (This used to live in the shell's poweroff, and was lost
     * when anything else took the machine down.) */
    s64 now = lp_time();
    if (now >= 1577836800LL) {
        char b[32];
        int  n = snprintf(b, sizeof b, "%lld\n", (long long)now);
        long fd = lp_open("/data/.clock", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { lp_write((int)fd, b, (size_t)n); lp_close((int)fd); }
    }

    /* Write everything down, twice: sync() returns before the disk has
     * necessarily finished, and the second call is cheap insurance. */
    printf("init: writing everything to disk\n");
    lp_sync();
    lp_sleep_ms(200);
    lp_sync();

    /* Unmount, so the next boot finds a clean filesystem rather than a
     * journal to replay. /data first - it is the one that matters and
     * the one things were writing to. A failure here is worth saying
     * out loud: it means something still had a file open, and the next
     * boot will spend time on fsck. */
    /* Everything stacked on top of /data has to come off first, or the
     * unmount below fails with EBUSY every single time and the "clean
     * shutdown" writes nothing.
     *
     * There are two kinds and both are easy to forget: /root is a bind
     * mount from /data/root, and `persist` puts an overlay on /bin,
     * /sbin, /lib, /usr, /opt and /srv whose upper layer is also on
     * /data. Rather than keep a list here that would go stale the next
     * time something is added, read /proc/mounts and take them off in
     * reverse order - last mounted, first removed, which is the only
     * order that works when they are stacked. */
    /* Drives somebody plugged in come off first.
     *
     * automount mounts them under /media, and `storage adopt` under
     * /mnt. Neither existed when this shutdown path was written, so
     * neither was in it - a USB drive stayed mounted right up to the
     * moment the power stopped, and everything still in the page cache
     * was lost. Nothing complained: the drive was fine on the next boot
     * because ext4 replayed its journal, so the only sign was a file
     * that came back empty.
     *
     * The same reverse-order walk /data uses works here, and it does
     * the sync as part of unmounting rather than hoping the later
     * lp_sync catches everything. */
    unmount_removable_drives();

    unmount_stack_above("/data");

    /* On the desktop /data is a directory on the root, not a partition:
     * there is nothing to unmount, and "would not unmount" at every
     * shutdown was a false alarm. The root below covers it. */
    char fs[32];
    mount_type("/data", fs, sizeof fs);
    if (!fs[0]) {
        /* nothing mounted there */
    } else if (sys_call2(SYS_umount2, (long)"/data", 0) < 0) {
        printf("init:   /data would not unmount - remounting it"
               " read-only instead\n");
        /* Second best, and much better than nothing: nothing can write
         * during the reset, so the filesystem is at least consistent.
         * The next boot replays the journal rather than starting clean. */
        lp_mount(NULL, "/data", NULL, MS_REMOUNT | MS_RDONLY, NULL);
    }
    else
        printf("init:   /data unmounted cleanly\n");
    sys_call2(SYS_umount2, (long)"/boot", 0);

    /* A root on a disk - the amd64 desktop, where the home directory and
     * every setting live on it - goes read-only last. Nothing here ever
     * unmounted or remounted it, so the start after a clean restart
     * began with ext4 replaying its journal ("recovery required") as if
     * the power had been cut: the sync above had put the data down, but
     * the filesystem was never marked clean. Everything that could write
     * has been killed by now; should something still hold a file open
     * for writing, the remount fails and the sync stays what there is.
     * A root in RAM has nothing to keep and is left alone. */
    mount_type("/", fs, sizeof fs);
    if (strcmp(fs, "ext4") == 0 &&
        lp_mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL) < 0)
        printf("init:   / would not go read-only - the sync above is all"
               " the disk gets\n");
    lp_sync();

    printf("init: %s now\n\n", (what == 2) ? "restarting" : "off");
    lp_reboot(what == 2 ? LINUX_REBOOT_CMD_RESTART
                        : LINUX_REBOOT_CMD_POWER_OFF);

    /* Only reached if the kernel refused, which on a machine with no
     * power control means "there is no way to switch this off". */
    printf("init: the kernel would not %s. It is safe to cut the"
           " power now.\n", (what == 2) ? "restart" : "power off");
    for (;;)
        lp_sleep_ms(60000);
}

/* ── Recovery mode (recovery/, the boot-recovery track) ───────────────
 *
 * lp.mode=recovery on the kernel command line is the boot menu's "LP
 * Recovery" (boot/efi/lpboot.c): the root is then the LP-RECOVERY
 * partition, and the one thing to run is the recovery menu, as root, on
 * tty1 - no rc, no services, no splash (the menu draws its own screen)
 * and no shell on any console: the menu's recovery shell asks for an
 * administrator's password first, and an open console would not. If the
 * menu exits it is started again. It restarts the machine by sending
 * SIGUSR2 like `reboot`; / is made read-only before the reboot so the
 * recovery partition comes back clean. */
#define RECOVERY_MENU "/recovery/bin/lp-recovery"

static bool recovery_mode(void)
{
    char cmdline[1024];
    return proc_read("/proc/cmdline", cmdline, sizeof(cmdline)) > 0 &&
           strstr(cmdline, "lp.mode=recovery") != NULL;
}

static void run_recovery(void)
{
    printf("init: recovery mode - starting " RECOVERY_MENU " on tty1\n");
    lp_signal_handler(SIGUSR1, on_poweroff);
    lp_signal_handler(SIGUSR2, on_reboot);
    for (;;) {
        pid_t pid = lp_fork();
        if (pid == 0) {
            lp_setsid();
            long fd = lp_open("/dev/tty1", O_RDWR, 0);
            if (fd >= 0) {
                lp_dup2((int)fd, STDIN_FILENO);
                lp_dup2((int)fd, STDOUT_FILENO);
                lp_dup2((int)fd, STDERR_FILENO);
                if (fd > STDERR_FILENO)
                    lp_close((int)fd);
                lp_term_make_controlling(STDIN_FILENO);
            }
            char *argv[] = { (char *)"lp-recovery", NULL };
            char *envp[] = { (char *)"PATH=/usr/sbin:/usr/bin:/sbin:/bin",
                             (char *)"HOME=/root", (char *)"TERM=linux", NULL };
            lp_execve(RECOVERY_MENU, argv, envp);
            lp_exit(127);
        }
        screen_pid = pid;
        /* Non-blocking, like the main loop: the reboot signal has to be
         * noticed while the menu is still running. */
        for (;;) {
            if (shutdown_wanted) {
                if (pid > 0)
                    lp_kill(pid, SIGTERM);
                lp_sleep_ms(500);
                lp_sync();
                lp_mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);
                do_shutdown(shutdown_wanted);
            }
            int status = 0;
            pid_t r = lp_waitpid(-1, &status, WNOHANG);
            if (r == pid || pid < 0) {
                printf("init: the recovery menu exited - starting it again\n");
                break;
            }
            lp_sleep_ms(r > 0 ? 0 : 200);
        }
        lp_sleep_ms(RESPAWN_MS);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    bool is_pid1 = (lp_getpid() == 1);

    /* When we are not PID 1 (run by hand during development) skip the
     * mounts - remounting over a live system would not end well. */
    if (is_pid1) {
        mount_filesystems();
        setup_console();
        if (recovery_mode())
            run_recovery();             /* never returns */
        show_splash();
    } else
        printf("init: not pid 1, skipping the mounts (test mode)\n");

    banner();

    if (is_pid1 && safe_mode()) {
        printf("  ** SAFE MODE **  (lpzero.safe on the kernel command line)\n");
        printf("  SSH only. /data/rc.local is skipped.\n");
        printf("  Take the word out of cmdline.txt to boot normally.\n\n");
    }

    if (is_pid1) {
        run_rc();
        load_services();
    }

    shell_pid = spawn_shell();

    /* A VM window, or an HDMI screen, is a console nobody is reading
     * unless we put a shell there too. */
    /* A shell on the screen with no login prompt.
     *
     * That is right for a board on a desk being worked on, and wrong for
     * one in a public place: plugging in a monitor and a keyboard gives
     * whoever did it root, immediately. There is no password to ask for -
     * password authentication is compiled out of this system entirely -
     * so the only choice is whether the shell is there at all.
     *
     * lpzero.noscreen on the kernel command line turns it off. The
     * serial console and SSH are unaffected. cmdline.txt is on the boot
     * partition, so this is one word typed from any PC. */
    bool no_screen = false;
    {
        char cmdline[1024];
        if (proc_read("/proc/cmdline", cmdline, sizeof(cmdline)) > 0)
            no_screen = (strstr(cmdline, "lpzero.noscreen") != NULL);
    }

    screen_pid = -1;
    if (is_pid1 && no_screen) {
        printf("init: no shell on the screen (lpzero.noscreen)\n");
    } else if (is_pid1 && screen_needs_its_own_shell()) {
        wait_for_splash();
        screen_pid = spawn_shell_on("/dev/tty1", true);
        if (screen_pid > 0)
            printf("init: a second shell is on the screen (tty1)\n");
    }

    /* Main loop: reap dead children, restart services whose backoff has
     * run out, and restart the shell when it exits.
     *
     * The wait does not block: a service waiting to be restarted has a
     * time to be restarted at, and nothing would notice it passing if
     * pid 1 were parked in wait(). */
    /* SIGUSR1 is poweroff, SIGUSR2 is reboot. Dedicated signals rather
     * than the traditional SIGINT/SIGTERM, because those arrive by
     * accident - a stray kill, a Ctrl-Alt-Del the console maps - and
     * turning the machine off by accident is worse than not being able
     * to. `poweroff` and `reboot` send these. */
    lp_signal_handler(SIGUSR1, on_poweroff);
    lp_signal_handler(SIGUSR2, on_reboot);

    for (;;) {
        if (shutdown_wanted)
            do_shutdown(shutdown_wanted);

        restart_due_services();
        rescan_user_services();

        int status = 0;

        /* Always a non-blocking wait, then a second's sleep.
         *
         * This used to park in wait() whenever every service was healthy,
         * which costs nothing at all - but it also meant pid 1 noticed
         * nothing until a child died. A service added with `service add`
         * would then not start until the next reboot, and `service start`
         * on a healthy machine felt broken. One wakeup a second on a
         * 1GHz core is not a cost worth that. */
        bool pending = services_pending();
        pid_t pid = lp_waitpid(-1, &status, WNOHANG);

        if (pid == 0) {
            /* Children, but none of them finished. A quarter second when
             * something is waiting to be restarted, so a service comes
             * back promptly; a second otherwise, which is soon enough
             * for `service add` to feel immediate. */
            lp_sleep_ms(pending ? 250 : 1000);
            continue;
        }

        if (shutdown_wanted)
            do_shutdown(shutdown_wanted);

        if (pid < 0) {
            /* No children to wait for (ECHILD = 10), or a signal cut the
             * wait short - which is exactly what poweroff does. PID 1
             * must not exit, so pause and look again. */
            if (!is_pid1)
                break;
            lp_sleep_ms(200);
            continue;
        }

        if (respawn_service(pid, status))
            continue;           /* it was a supervised service */

        if (pid == screen_pid) {
            lp_sleep_ms(RESPAWN_MS);
            screen_pid = spawn_shell_on("/dev/tty1", true);
            continue;
        }

        if (pid != shell_pid)
            continue;           /* just an orphan we reaped */

        int code = LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : -1;
        printf("\ninit: the shell exited (code %d). Starting it again.\n", code);

        if (!is_pid1)
            break;              /* in test mode, only once */

        lp_sleep_ms(RESPAWN_MS);
        shell_pid = spawn_shell();
    }

    printf("init: exiting\n");
    return 0;
}
