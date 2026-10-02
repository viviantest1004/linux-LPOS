/* service - what init is keeping alive, and turning it on or off.
 *
 *   service                  everything, and whether it is running
 *   service status <name>
 *   service restart <name>
 *   service stop <name>
 *   service start <name>
 *
 * ── How this works, which is not how systemd works ──
 * init reads /etc/services at boot and restarts anything in it that
 * dies. That is the whole supervisor: no units, no dependencies, no
 * targets, no sockets. This command talks to it through two things -
 * signals, and a file.
 *
 *   restart   kill it. init notices and starts it again. That is not a
 *             workaround; it is what "restart" means here.
 *   stop      write the name into /data/services.disabled, then kill
 *             it. init finds it in that file and leaves it alone.
 *   start     take the name out of the file. init looks about once a
 *             second when anything is down, so it comes back.
 *
 * The file is on the data partition, so a service turned off stays off
 * across a reboot. It is also readable, which is the answer to "why is
 * this not running" that a signal could never be.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"

/* /etc/lp/services on the desktop, where /etc/services is glibc's
 * port-name table; /etc/services on the boards. Same choice init makes. */
static const char *services_path(void)
{
    return lp_exists("/etc/lp/services") ? "/etc/lp/services"
                                          : "/etc/services";
}
#define SERVICES  services_path()
#define DISABLED  "/data/services.disabled"

/* Services added on this machine.
 *
 * /etc/services is inside the kernel image and is rebuilt from it at
 * every boot, so there was no way at all to have init supervise a
 * program of your own. The only writable thing that ran at boot was
 * /data/rc.local, which starts something once and never looks at it
 * again - so a web server started that way stays dead the first time it
 * crashes, on a board nobody is watching. That is precisely what init
 * exists to prevent.
 *
 * init reads this file alongside the built-in one, and looks at it again
 * every few seconds, so `service add` takes effect without a reboot. */
#define USER_SVC  "/data/services"

/* dirent, as getdents64 lays it out */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

/* ── which services there are ────────────────────────────────────── */

#define MAX_SERVICES 24
static char names[MAX_SERVICES][32];
static char lines[MAX_SERVICES][160];
static int  nservices = 0;

static void load_from(const char *path)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return;

    char line[256];
    while (nservices < MAX_SERVICES &&
           readline((int)fd, line, sizeof line) >= 0) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#')
            continue;

        /* A line may open with "?<path>", meaning init only starts it
         * where that path exists. Skip the condition before taking the
         * name, or the service is called "?/sys/class/net/wlan0" and
         * every command that takes a name stops working for it. That is
         * how `service restart wpa_supplicant` came to answer "not in
         * /etc/services" - on a board where WiFi is the only way in,
         * and restarting it is the first thing you reach for. */
        if (*p == '?') {
            while (*p && *p != ' ' && *p != '\t') p++;
            while (*p == ' ' || *p == '\t') p++;
            if (!*p)
                continue;              /* a condition and nothing else */
        }

        strlcpy(lines[nservices], p, sizeof lines[0]);

        /* "=name" as the first word says what the service is called,
         * and init drops it before running the rest. It exists for the
         * services that run through a wrapper: without it,
         *
         *     ?/sys/class/net/wlan0 =wpa_supplicant sh /etc/wpa-start
         *
         * would be called "sh" here and by init, and `service restart
         * wpa_supplicant` would answer that there is no such service on
         * a board that is running it. Read it the same way init does,
         * or the two disagree about what things are called - which is
         * worse than either answer on its own. */
        const char *name_at = p;
        if (*p == '=' && p[1] && p[1] != ' ' && p[1] != '\t')
            name_at = p + 1;

        const char *end = name_at;
        while (*end && *end != ' ' && *end != '\t') end++;
        size_t len = (size_t)(end - name_at);
        if (len >= sizeof names[0]) len = sizeof names[0] - 1;
        memcpy(names[nservices], name_at, len);
        names[nservices][len] = '\0';
        nservices++;
    }
    lp_close((int)fd);
}

/* The system's own list, then this machine's. Same order init uses, so
 * what `service` shows is what init is actually holding. */
static void load(void)
{
    load_from(SERVICES);
    load_from(USER_SVC);
}

static int index_of(const char *name)
{
    for (int i = 0; i < nservices; i++)
        if (strcmp(names[i], name) == 0)
            return i;
    return -1;
}

/* ── is it running ───────────────────────────────────────────────── */

/* The first pid running under this name, or 0.
 *
 * /proc/<pid>/comm is the program name and nothing else - no path, no
 * arguments - so a match cannot be some other process that merely
 * mentions the name on its command line. */
static int pid_of(const char *name)
{
    long fd = lp_open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 0;

    char buf[4096];
    int  found = 0;

    while (!found) {
        long n = sys_getdents((int)fd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (long off = 0; off < n && !found; ) {
            char *rec = buf + off;
            u16   len = *(u16 *)(rec + DIRENT_RECLEN);
            char *ent = rec + DIRENT_NAME;
            if (len == 0) break;
            off += len;

            int pid = atoi(ent);
            if (pid <= 1)
                continue;

            char path[64], comm[64];
            snprintf(path, sizeof path, "/proc/%d/comm", pid);
            if (proc_read(path, comm, sizeof comm) <= 0)
                continue;
            for (int j = 0; comm[j]; j++)
                if (comm[j] == '\n') { comm[j] = '\0'; break; }
            if (strcmp(comm, name) == 0)
                found = pid;
        }
    }
    lp_close((int)fd);
    return found;
}

/* ── the disabled list ───────────────────────────────────────────── */

static bool is_disabled(const char *name)
{
    long fd = lp_open(DISABLED, O_RDONLY, 0);
    if (fd < 0)
        return false;
    char line[128];
    bool found = false;
    while (readline((int)fd, line, sizeof line) >= 0)
        if (line[0] && strcmp(line, name) == 0) { found = true; break; }
    lp_close((int)fd);
    return found;
}

/* ── telling integrity(1) that this was us ───────────────────────────
 *
 * integrity(1) hashes the handful of files that decide whether
 * something runs again after a reboot, and /data/services is one of them: it is
 * read as root and what is in it starts as root. That is exactly why
 * it is watched, and exactly why the machine must not report its own
 * bookkeeping as an intrusion - `service add` is the intended way to put a
 * line there.
 *
 * `integrity -a` re-records only the paths named and copies the rest
 * of the record through untouched, so an edit to /data/rc.local or to
 * authorized_keys in the same window is still reported at the next
 * check. */
static void integrity_accept(const char *path)
{
    if (!lp_exists("/bin/integrity"))
        return;                 /* an image without it watches nothing */

    char *argv[] = { (char *)"integrity", (char *)"-a",
                     (char *)path, NULL };

    pid_t pid = lp_fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        lp_execve("/bin/integrity", argv, environ);
        lp_exit(127);
    }
    int st = 0;
    lp_waitpid(pid, &st, 0);
    /* integrity says on stderr why it refused, if it did. Saying it
     * twice helps nobody. */
}

static int set_disabled(const char *name, bool off)
{
    /* Read what is there, write it back with this name added or gone.
     * The list is a handful of short lines; there is nothing to stream. */
    static char kept[2048];
    size_t used = 0;
    bool  present = false;

    long fd = lp_open(DISABLED, O_RDONLY, 0);
    if (fd >= 0) {
        char line[128];
        while (readline((int)fd, line, sizeof line) >= 0) {
            if (!line[0])
                continue;
            if (strcmp(line, name) == 0) { present = true; continue; }
            size_t l = strlen(line);
            if (used + l + 2 < sizeof kept) {
                memcpy(kept + used, line, l);
                used += l;
                kept[used++] = '\n';
            }
        }
        lp_close((int)fd);
    }

    if (off == present && off)
        return 0;                       /* already off */

    if (off) {
        size_t l = strlen(name);
        if (used + l + 2 < sizeof kept) {
            memcpy(kept + used, name, l);
            used += l;
            kept[used++] = '\n';
        }
    }

    long out = lp_open(DISABLED, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        dprintf(STDERR_FILENO,
                "service: cannot write %s (%ld).\n"
                "service:   Without the data partition there is nowhere to\n"
                "service:   remember this, so it would come back anyway.\n",
                DISABLED, -out);
        return 1;
    }
    if (used)
        lp_write((int)out, kept, used);
    lp_close((int)out);
    lp_sync();
    integrity_accept(DISABLED);
    return 0;
}

/* ── the commands ────────────────────────────────────────────────── */

static int list_all(void)
{
    printf("  %-16s %8s  %s\n", "service", "pid", "state");
    for (int i = 0; i < nservices; i++) {
        int pid = pid_of(names[i]);
        const char *state = pid ? "running"
                          : is_disabled(names[i]) ? "stopped (by hand)"
                          : "not running";
        char pidbuf[12] = "-";
        if (pid) snprintf(pidbuf, sizeof pidbuf, "%d", pid);
        printf("  %-16s %8s  %s\n", names[i], pidbuf, state);
    }
    printf("\n  init restarts anything here that dies. %s\n", SERVICES);
    printf("  is the list; a service turned off is remembered in\n");
    printf("  %s and stays off across a reboot.\n", DISABLED);
    return 0;
}

static int status_of(const char *name)
{
    int i = index_of(name);
    if (i < 0) {
        dprintf(STDERR_FILENO,
                "service: %s is not in %s.\n"
                "service:   'service' with no arguments lists what is.\n",
                name, SERVICES);
        return 1;
    }
    int pid = pid_of(name);
    printf("%s\n", names[i]);
    printf("  command  %s\n", lines[i]);
    if (pid) {
        char path[64], buf[64];
        snprintf(path, sizeof path, "/proc/%d/stat", pid);
        printf("  running  pid %d\n", pid);
        snprintf(path, sizeof path, "/proc/%d/status", pid);
        if (proc_read(path, buf, sizeof buf) > 0) { /* first lines only */ }
    } else if (is_disabled(name)) {
        printf("  stopped  by hand - it is in %s\n", DISABLED);
        printf("           'service start %s' brings it back\n", name);
    } else {
        printf("  stopped  and not on purpose. init gives up on a service\n");
        printf("           that keeps failing; 'dmesg' and the console say\n");
        printf("           why.\n");
    }
    return pid ? 0 : 1;
}

static int do_restart(const char *name)
{
    if (index_of(name) < 0) {
        dprintf(STDERR_FILENO, "service: %s is not in %s\n", name, SERVICES);
        return 1;
    }
    /* Make sure it is not disabled, or killing it would stop it for
     * good and "restart" would have meant "stop". */
    set_disabled(name, false);

    int pid = pid_of(name);
    if (!pid) {
        printf("service: %s was not running. init will start it.\n", name);
        return 0;
    }
    if (lp_kill(pid, SIGTERM) < 0) {
        dprintf(STDERR_FILENO, "service: cannot signal pid %d\n", pid);
        return 1;
    }
    printf("service: stopped %s (pid %d). init starts it again.\n", name, pid);
    return 0;
}

/* ── Stopping the things that keep the machine alive ──
 *
 * `service stop x` writes x to /data/services.disabled, which is on the
 * data partition and therefore survives a reboot. For these three that
 * is not "stop it", it is "disable it permanently, on a board that may
 * be somewhere else by the time it matters":
 *
 *   guard     every defence the machine has - the memory killer, the
 *             fork-storm response, the disk-full check. Without it the
 *             board looks healthy right up to the moment it is gone.
 *   dropbear  the only way in.
 *   watchdog  the only way back from a wedge.
 *
 * --force still does it, because somebody debugging guard needs to be
 * able to stop guard. It just cannot happen by typing four words. */
static bool is_critical_service(const char *name)
{
    return strcmp(name, "guard") == 0
        || strcmp(name, "dropbear") == 0
        || strcmp(name, "watchdog") == 0;
}

static int do_stop(const char *name, bool force)
{
    if (index_of(name) < 0) {
        dprintf(STDERR_FILENO, "service: %s is not in %s\n", name, SERVICES);
        return 1;
    }

    if (is_critical_service(name) && !force) {
        dprintf(STDERR_FILENO,
                "service: %s is not something to stop.\n", name);
        if (strcmp(name, "guard") == 0)
            dprintf(STDERR_FILENO,
                    "service:   It is the whole self-defence of this"
                    " machine: what kills a runaway\n"
                    "service:   before it takes the board down, what"
                    " notices /data filling up, what\n"
                    "service:   keeps SSH answering under load. Stopping"
                    " it leaves a board that\n"
                    "service:   looks fine and is defenceless, and"
                    " nothing to say so afterwards.\n");
        else if (strcmp(name, "dropbear") == 0)
            dprintf(STDERR_FILENO,
                    "service:   It is the only way into this machine.\n");
        else
            dprintf(STDERR_FILENO,
                    "service:   It is the only thing that can reboot a"
                    " board that has wedged.\n");
        dprintf(STDERR_FILENO,
                "service:\n"
                "service:   This would also outlast a reboot - the"
                " disabled list is on /data.\n"
                "service:   'service stop %s --force' if you mean it.\n",
                name);
        return 1;
    }

    /* Written before the kill, not after. The other order is a race:
     * init sees the death, finds nothing in the file, and starts it
     * again before this process gets to write. */
    if (set_disabled(name, true) != 0)
        return 1;

    int pid = pid_of(name);
    if (pid)
        lp_kill(pid, SIGTERM);

    printf("service: %s is stopped and will stay stopped, including after\n"
           "         a reboot. 'service start %s' undoes it.\n", name, name);

    if (strcmp(name, "dropbear") == 0)
        printf("\nservice:   ** that was the SSH server. If you are reading\n"
               "service:      this over SSH, this session is the last one.\n");
    return 0;
}

static int do_start(const char *name)
{
    if (index_of(name) < 0) {
        dprintf(STDERR_FILENO, "service: %s is not in %s\n", name, SERVICES);
        return 1;
    }
    if (pid_of(name)) {
        printf("service: %s is already running.\n", name);
        return 0;
    }
    if (set_disabled(name, false) != 0)
        return 1;
    printf("service: %s enabled. init starts it within a second.\n", name);
    return 0;
}

/* ── adding one ───────────────────────────────────────────────────── */

static bool user_line_matches(const char *line, const char *name)
{
    /* Match on the program, not the whole line: somebody removing
     * "httpd" means the httpd service, arguments and all. */
    while (*line == ' ' || *line == '\t') line++;
    size_t n = strlen(name);
    if (strncmp(line, name, n) != 0)
        return false;
    return line[n] == '\0' || line[n] == ' ' || line[n] == '\t';
}

/* Rewrite /data/services with one line added or removed.
 *
 * Through a temporary and a rename, so a power cut leaves either the old
 * list or the new one. A truncated services file is a board that comes
 * back with half its services missing and nothing saying why. */
static int rewrite_user(const char *drop_name, const char *add_line)
{
    char buf[8192];
    long n = proc_read(USER_SVC, buf, sizeof buf);
    if (n < 0) n = 0;
    buf[n] = '\0';

    char out[8192];
    int  len = 0;
    bool removed = false;

    len += snprintf(out + len, sizeof out - len,
                    "# Services added on this machine, supervised by init\n"
                    "# exactly like the built-in ones. `service add` writes\n"
                    "# this; `service remove` takes a line out.\n");

    char *p = buf;
    while (*p) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = '\0';

        char *line = p;
        while (*line == ' ' || *line == '\t') line++;
        if (*line && *line != '#') {
            if (drop_name && user_line_matches(line, drop_name)) {
                removed = true;
            } else if (len < (int)sizeof out - 300) {
                len += snprintf(out + len, sizeof out - len, "%s\n", line);
            }
        }

        if (!eol) break;
        p = eol + 1;
    }

    if (add_line && len < (int)sizeof out - 300)
        len += snprintf(out + len, sizeof out - len, "%s\n", add_line);

    if (drop_name && !removed) {
        dprintf(STDERR_FILENO,
                "service: %s was not added here.\n"
                "service:   `service` lists everything; the ones from the\n"
                "service:   system image cannot be removed, only stopped.\n",
                drop_name);
        return 1;
    }

    char tmp[] = USER_SVC ".new";
    long fd = lp_open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "service: cannot write %s - is /data mounted and writable?\n",
                USER_SVC);
        return 1;
    }
    bool ok = lp_write((int)fd, out, (size_t)len) == len;
    lp_close((int)fd);
    if (!ok || lp_rename(tmp, USER_SVC) != 0) {
        lp_unlink(tmp);
        dprintf(STDERR_FILENO, "service: could not replace %s\n", USER_SVC);
        return 1;
    }
    lp_sync();
    integrity_accept(USER_SVC);
    return 0;
}

static int do_add(int argc, char **argv)
{
    if (argc < 3) {
        dprintf(STDERR_FILENO,
                "usage: service add <command> [arguments]\n"
                "  e.g. service add httpd -d /data/www -p 8080\n");
        return 2;
    }

    /* Build the line, and check the program exists before writing it.
     * A service that cannot start is a line init retries twenty times
     * and then gives up on, with the reason buried in the boot log. */
    char line[512];
    line[0] = '\0';
    for (int i = 2; i < argc; i++) {
        if (i > 2) strlcat(line, " ", sizeof line);
        strlcat(line, argv[i], sizeof line);
    }

    const char *prog = argv[2];
    char full[512];
    bool found = false;
    if (strchr(prog, '/')) {
        found = lp_access(prog, X_OK) == 0;
        strlcpy(full, prog, sizeof full);
    } else {
        static const char *dirs[] = { "/bin", "/sbin", "/usr/bin",
                                      "/usr/sbin", "/data/bin", NULL };
        for (int i = 0; dirs[i] && !found; i++) {
            snprintf(full, sizeof full, "%s/%s", dirs[i], prog);
            found = lp_access(full, X_OK) == 0;
        }
    }
    if (!found) {
        dprintf(STDERR_FILENO,
                "service: there is no %s to run.\n"
                "service:   `which %s` says where it would come from;"
                " give a full path\n"
                "service:   if it is somewhere else.\n", prog, prog);
        return 1;
    }

    /* Refuse a name the system already supervises, rather than starting
     * a second copy of it. */
    for (int i = 0; i < nservices; i++)
        if (strcmp(names[i], prog) == 0) {
            dprintf(STDERR_FILENO,
                    "service: %s is already supervised.\n"
                    "service:   `service status %s` shows it.\n", prog, prog);
            return 1;
        }

    if (rewrite_user(NULL, line) != 0)
        return 1;

    printf("service: %s added. init starts it within a few seconds, and\n"
           "service:   restarts it whenever it dies - including after a\n"
           "service:   reboot. `service status %s` to watch.\n", prog, prog);
    printf("service:   guard will not kill it under memory pressure either;\n"
           "service:   that protection follows the list init is holding.\n");
    return 0;
}

static int do_remove(const char *name)
{
    if (rewrite_user(name, NULL) != 0)
        return 1;
    printf("service: %s removed from %s.\n", name, USER_SVC);
    printf("service:   it is left running until it stops on its own -"
           " init does not\n"
           "service:   kill a process because a file changed. `service stop"
           " %s`\n"
           "service:   ends it now.\n", name);
    return 0;
}

static void usage(void)
{
    printf("service - what init is keeping alive\n\n");
    printf("  service                    all of them, and their state\n");
    printf("  service status <name>\n");
    printf("  service restart <name>\n");
    printf("  service stop <name>        and keep it stopped\n");
    printf("  service start <name>\n");
    printf("  service add <command> [args]   supervise something of"
           " your own\n");
    printf("  service remove <name>          stop supervising it\n\n");
    printf("init reads %s at boot, and %s\n", SERVICES, USER_SVC);
    printf("as well - that second file is on the data partition, which is\n");
    printf("why anything you add there is still supervised tomorrow. It\n");
    printf("restarts whatever is in either one when it dies\n");
    printf("that dies. There are no units, dependencies or targets - it is\n");
    printf("a list of commands and a supervisor.\n\n");
    printf("'restart' kills it and lets init do its job. 'stop' writes the\n");
    printf("name into %s first, which is what stops\n", DISABLED);
    printf("init bringing it back, and is also the file to read when you\n");
    printf("want to know why something is not running.\n");
}

int main(int argc, char **argv)
{
    load();

    if (nservices == 0) {
        dprintf(STDERR_FILENO,
                "service: neither %s nor %s\n"
                "service:   has anything in it, so init is supervising"
                " nothing.\n", SERVICES, USER_SVC);
        return 1;
    }

    if (argc < 2)
        return list_all();

    const char *cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "help") == 0) { usage(); return 0; }
    if (strcmp(cmd, "list") == 0) return list_all();
    if (strcmp(cmd, "add") == 0)  return do_add(argc, argv);

    if (argc < 3) {
        dprintf(STDERR_FILENO, "usage: service %s <name>\n", cmd);
        return 2;
    }
    const char *name = argv[2];

    bool force = false;
    for (int i = 3; i < argc; i++)
        if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0)
            force = true;

    if (strcmp(cmd, "status") == 0)  return status_of(name);
    if (strcmp(cmd, "restart") == 0) return do_restart(name);
    if (strcmp(cmd, "stop") == 0)    return do_stop(name, force);
    if (strcmp(cmd, "start") == 0)   return do_start(name);
    if (strcmp(cmd, "remove") == 0)  return do_remove(name);

    dprintf(STDERR_FILENO, "service: no idea what \"%s\" means\n", cmd);
    usage();
    return 2;
}
