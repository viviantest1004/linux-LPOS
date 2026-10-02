/* integrity - has anything that survives a reboot been changed.
 *
 *   integrity            check against what was recorded, then record
 *   integrity -c         check only, change nothing
 *   integrity -u         accept what is there now as correct
 *   integrity -a <path>  accept just these paths, leave the rest alone
 *   integrity -l         list what is watched and its current hash
 *
 * ── Why this is small enough to be worth doing ──
 * On an ordinary Linux there are dozens of places a program can arrange
 * to be run again after a reboot: cron, systemd units, .bashrc, profile
 * scripts, service configuration, kernel modules. Watching all of them
 * is a job in itself, which is why almost nobody does it.
 *
 * Here the root filesystem is inside the kernel image and unpacked into
 * RAM at every boot, so nothing written to it survives. That leaves the
 * data partition, and on it a handful of files decide whether something
 * runs again or someone gets back in:
 *
 *   /data/rc.local                  runs as root at every boot
 *   /root/.ssh/authorized_keys      who may log in
 *   /data/users, /data/groups       who exists, and with which uid
 *   /data/services                  init starts these as root at boot
 *   /data/services.disabled         and this is what stops one starting
 *   /data/crontab                   cron runs these as root, on a clock
 *
 * A short list, short enough to hash on every boot and compare, which
 * turns "we would never know" into "it says so at boot and in sysinfo".
 * Three directories are watched by their listing as well, because a new
 * file appearing in them matters too.
 *
 * The list is the whole value of this program: a persistence path that
 * is not on it is a persistence path nobody is looking at. Anything new
 * that /etc/rc reads off /data at boot belongs here on the same commit
 * that creates it.
 *
 * This detects; it does not prevent. Whoever changed the file could
 * change the record beside it - so this is aimed at the ordinary case
 * of automated malware and mistakes, not at somebody who has read this
 * comment. It costs almost nothing, and the alternative was nothing.
 *
 * ── Why -a exists, and why it is not -u ──
 * Three of the watched paths are directories our own commands write to
 * in the course of doing exactly what they were asked. `pkg install jq`
 * puts a file in /data/bin and two in /data/pkg/db; `useradd bob` adds a
 * line to /data/users; authkey rewrites authorized_keys every time it
 * recovers a key from the boot partition. Every one of those made the
 * next run of this program announce that something which survives a
 * reboot had changed, and defend(1) turned that into a security finding.
 * A check that cries wolf after an ordinary install is a check people
 * learn to skip, and then it is worth nothing on the day it is right.
 *
 * So the record follows the commands that are ALLOWED to change it: each
 * of them runs `integrity -a <the path it just wrote>` when it succeeds.
 *
 * -a and not -u, and this distinction is the whole point. -u accepts
 * EVERYTHING as it stands now. A package installed at 03:00 while
 * somebody was editing /data/rc.local would, with -u, adopt the new
 * rc.local as correct in the same breath - the one change we exist to
 * catch, swallowed by the one we do not care about. -a rewrites only the
 * lines it is given and copies the rest of the record through untouched,
 * so a change to anything the installer did not write is still sitting
 * there to be reported at the next run.
 *
 * Two more rules make -a safe to call from a program:
 *
 *   - an argument that is not one of the watched paths below is a
 *     mistake in the caller, not something to guess at, so it is an
 *     error and it says which path it did not recognise. Silently
 *     ignoring it is how a caller ends up accepting nothing at all for
 *     a year without anybody noticing.
 *   - with no record on disk it accepts nothing, quietly. Creating the
 *     record here would mean that deleting /data/.integrity and waiting
 *     for the next `pkg install` is enough to have this machine adopt
 *     whatever state it is in. Only a plain `integrity` run - the one
 *     /etc/rc does at boot - creates the record, and defend reports a
 *     record that has gone missing.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define RECORD "/data/.integrity"

/* linux_dirent64 offsets */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

typedef enum { WATCH_FILE, WATCH_DIR } kind_t;

typedef struct {
    const char *path;
    kind_t      kind;
    const char *why;
} watch_t;

/* What is worth watching, and why it is on the list. */
static const watch_t WATCHED[] = {
    { "/data/rc.local", WATCH_FILE,
      "runs as root at every boot" },
    { "/root/.ssh/authorized_keys", WATCH_FILE,
      "decides who can log in" },
    { "/data/users", WATCH_FILE,
      "appended to /etc/passwd at boot - a uid 0 line here is root" },
    { "/data/groups", WATCH_FILE,
      "appended to /etc/group at boot - membership of any group" },
    { "/data/services", WATCH_FILE,
      "init starts what is listed here, as root, at every boot" },
    { "/data/services.disabled", WATCH_FILE,
      "a name here stops that service starting - guard and defend included" },
    { "/data/crontab", WATCH_FILE,
      "cron runs these as root, on a schedule" },
    /* The three directories below are also the three that pkg(1) writes
     * to when somebody installs a package, which is why `integrity -a`
     * exists - see the header. Their entries change during ordinary use
     * of this machine; the four files above it do not, and a program of
     * ours asking to accept one of those is a much bigger claim. */
    { "/data/bin", WATCH_DIR,
      "on PATH - anything here can be run by name" },
    { "/data/pkg/db", WATCH_DIR,
      "what packages are installed" },
    { "/data/python/lib/python3.12/site-packages", WATCH_DIR,
      "loaded whenever python runs" },
    { NULL, WATCH_FILE, NULL }
};

#define MAX_WATCH 12
static char now_hash[MAX_WATCH][72];
static char was_hash[MAX_WATCH][72];

/* A directory is hashed by its listing rather than its contents: a file
 * appearing or disappearing is the thing worth noticing, and hashing
 * every byte of site-packages at each boot would cost seconds. */
static bool hash_dir(const char *path, char *hex)
{
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return false;

    /* Collect the names, sorted, so that the order the filesystem
     * happens to return them in does not look like a change. */
    static char names[512][64];
    int n = 0;

    char buf[8192];
    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof(buf));
        if (got <= 0)
            break;
        for (long off = 0; off < got && n < 512; ) {
            char       *rec  = buf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            const char *name = rec + DIRENT_NAME;
            off += len;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
            strlcpy(names[n++], name, 64);
        }
    }
    lp_close((int)fd);

    for (int i = 1; i < n; i++) {
        char tmp[64];
        strlcpy(tmp, names[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], tmp) > 0) {
            strlcpy(names[j + 1], names[j], 64);
            j--;
        }
        strlcpy(names[j + 1], tmp, 64);
    }

    /* Write the listing to a temporary file and hash that - the hash
     * helper works on files, and this keeps one implementation. */
    /* O_EXCL and a fresh unlink, because /tmp is 1777 and this runs as
     * root. A fixed name opened O_CREAT|O_TRUNC with no O_EXCL is a
     * standing invitation: anyone can pre-create that path as a symlink
     * and have root truncate whatever it points at. Aim it at
     * /root/.ssh/authorized_keys and the next boot locks everyone out
     * of a board with no other way in. O_EXCL refuses to follow a
     * symlink at all, so the unlink-then-create pair cannot be turned
     * into a write somewhere else. */
    const char *tmpf = "/tmp/.integrity.list";
    lp_unlink(tmpf);
    long out = lp_open(tmpf, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (out < 0) {
        dprintf(STDERR_FILENO,
                "integrity: cannot create %s (%ld) - not checking\n",
                tmpf, -out);
        return false;
    }
    for (int i = 0; i < n; i++) {
        lp_write((int)out, names[i], strlen(names[i]));
        lp_write((int)out, "\n", 1);
    }
    lp_close((int)out);

    bool ok = lp_sha256_file(tmpf, hex);
    lp_unlink(tmpf);
    return ok;
}

static void measure(void)
{
    for (int i = 0; WATCHED[i].path; i++) {
        bool ok = (WATCHED[i].kind == WATCH_DIR)
                  ? hash_dir(WATCHED[i].path, now_hash[i])
                  : lp_sha256_file(WATCHED[i].path, now_hash[i]);
        if (!ok)
            strlcpy(now_hash[i], "-", sizeof(now_hash[i]));
    }
}

/* The record is "<hash> <path>" per line, same shape as sha256sum. */
static void load_record(void)
{
    for (int i = 0; WATCHED[i].path; i++)
        strlcpy(was_hash[i], "", sizeof(was_hash[i]));

    long fd = lp_open(RECORD, O_RDONLY, 0);
    if (fd < 0)
        return;

    char line[512];
    while (readline((int)fd, line, sizeof(line)) >= 0) {
        char *sp = strchr(line, ' ');
        if (!sp)
            continue;
        *sp = '\0';
        char *path = sp + 1;
        while (*path == ' ') path++;

        for (int i = 0; WATCHED[i].path; i++)
            if (strcmp(WATCHED[i].path, path) == 0)
                strlcpy(was_hash[i], line, sizeof(was_hash[i]));
    }
    lp_close((int)fd);
}

static bool save_record(void)
{
    long fd = lp_open(RECORD, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return false;
    for (int i = 0; WATCHED[i].path; i++)
        dprintf((int)fd, "%s  %s\n", now_hash[i], WATCHED[i].path);
    lp_close((int)fd);
    return true;
}

/* ── accepting a few paths and no others ─────────────────────────────
 *
 * Write the record back with the current hash for the entries marked
 * accepted and the RECORDED hash for every other one, so that a change
 * somewhere else in the list survives this and is still reported at the
 * next run. That copying-through is the whole difference between this
 * and -u.
 *
 * An entry that has no recorded hash at all - the list grew since the
 * record was written - gets the current one, which is what the next
 * plain run would have done with it anyway. */
static bool save_record_except(const bool *accepted)
{
    long fd = lp_open(RECORD, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return false;
    for (int i = 0; WATCHED[i].path; i++) {
        const char *keep = (accepted[i] || !was_hash[i][0])
                           ? now_hash[i] : was_hash[i];
        dprintf((int)fd, "%s  %s\n", keep, WATCHED[i].path);
    }
    lp_close((int)fd);
    return true;
}

/* `integrity -a <path>...`, called by pkg, useradd and authkey when one
 * of them has just written a watched path itself. Silent when it works:
 * this runs inside somebody else's command, and a line of ours after
 * every `pkg install` is the sort of noise that makes people stop
 * reading the output of the command they actually typed. */
static int accept_paths(char **paths, int npaths)
{
    static bool accepted[MAX_WATCH];

    measure();
    load_record();

    bool have_record = false;
    for (int i = 0; WATCHED[i].path; i++)
        if (was_hash[i][0]) { have_record = true; break; }

    if (!have_record) {
        /* Nothing recorded, so nothing to accept, and nothing to say
         * either. This is the ordinary state on a first boot: /etc/rc
         * merges the SSH keys - which calls this - some way before it
         * runs `integrity`, so a line here would put "nothing has been
         * recorded" on the console of every brand new card, about a
         * machine that is perfectly fine and is about to record it.
         *
         * A record that is missing at any other time is worth knowing
         * about, and it is already covered: defend's check_integrity
         * looks for /data/.integrity by name and reports its absence
         * with what it means. One report of it, in the place that
         * exists to report things. */
        return 0;
    }

    int rc = 0;
    for (int a = 0; a < npaths; a++) {
        int found = -1;
        for (int i = 0; WATCHED[i].path; i++)
            if (strcmp(WATCHED[i].path, paths[a]) == 0) { found = i; break; }

        if (found < 0) {
            dprintf(STDERR_FILENO,
                    "integrity: %s is not one of the watched paths, so"
                    " accepting it would mean nothing\n", paths[a]);
            dprintf(STDERR_FILENO,
                    "integrity:   `integrity -l` lists them\n");
            rc = 2;
            continue;
        }
        accepted[found] = true;
    }

    if (rc != 0)
        return rc;              /* change nothing when the caller is wrong */

    if (!save_record_except(accepted)) {
        dprintf(STDERR_FILENO, "integrity: cannot write %s\n", RECORD);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    bool check_only = false, update = false, list = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) check_only = true;
        else if (strcmp(argv[i], "-u") == 0) update = true;
        else if (strcmp(argv[i], "-l") == 0) list = true;
        else if (strcmp(argv[i], "-a") == 0) {
            /* Everything after -a is a path, and -a answers on its own -
             * so a caller building the argument list does not have to
             * think about what else is on the line. */
            if (i + 1 >= argc) {
                dprintf(STDERR_FILENO,
                        "integrity: -a wants the paths to accept, e.g."
                        " `integrity -a /data/bin`\n");
                return 2;
            }
            return accept_paths(&argv[i + 1], argc - i - 1);
        }
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: integrity [-c] [-u] [-l] [-a <path>...]\n");
            printf("  Watches the few things that survive a reboot and\n");
            printf("  can make something run again or let someone in.\n\n");
            printf("  (no option)  check, report, then record\n");
            printf("  -c           check only, record nothing\n");
            printf("  -u           accept what is there now as correct\n");
            printf("  -a <path>..  accept only these watched paths and\n");
            printf("               leave the record for the others alone.\n");
            printf("               pkg, useradd and authkey use this when\n");
            printf("               they have just written one themselves\n");
            printf("  -l           list what is watched\n\n");
            printf("  Exit 1 means something changed.\n");
            return 0;
        }
    }

    measure();

    if (list) {
        printf("watched:\n");
        for (int i = 0; WATCHED[i].path; i++) {
            /* Sixteen characters of the hash is plenty to eyeball. Our
             * printf has no %.Ns precision, so cut it here. */
            char shown[20];
            strlcpy(shown, now_hash[i], sizeof shown);

            printf("  %-46s %s\n     %s%s  %s\n",
                   WATCHED[i].path,
                   strcmp(now_hash[i], "-") == 0 ? "(not there)" : "",
                   shown,
                   strlen(now_hash[i]) > strlen(shown) ? "..." : "",
                   WATCHED[i].why);
        }
        return 0;
    }

    if (update) {
        if (!save_record()) {
            dprintf(STDERR_FILENO, "integrity: cannot write %s\n", RECORD);
            return 1;
        }
        printf("integrity: recorded\n");
        return 0;
    }

    load_record();

    /* Nothing recorded yet: the first run, or the record was deleted.
     * Both are worth saying, because a deleted record is what someone
     * covering their tracks would leave behind. */
    bool first = true;
    for (int i = 0; WATCHED[i].path; i++)
        if (was_hash[i][0]) { first = false; break; }

    if (first) {
        if (!check_only)
            save_record();
        printf("integrity: first check - recorded %d items\n",
               (int)(sizeof(WATCHED) / sizeof(WATCHED[0])) - 1);
        return 0;
    }

    int changed = 0;
    for (int i = 0; WATCHED[i].path; i++) {
        if (!was_hash[i][0])
            continue;                    /* not previously recorded */
        if (strcmp(was_hash[i], now_hash[i]) == 0)
            continue;

        if (changed == 0)
            dprintf(STDERR_FILENO,
                    "integrity: ** something that survives a reboot has"
                    " changed\n");

        const char *what =
            strcmp(now_hash[i], "-") == 0 ? "gone" :
            strcmp(was_hash[i], "-") == 0 ? "appeared" : "changed";

        dprintf(STDERR_FILENO, "integrity:    %s  %s\n",
                WATCHED[i].path, what);
        dprintf(STDERR_FILENO, "integrity:      %s\n", WATCHED[i].why);
        changed++;
    }

    if (changed == 0) {
        printf("integrity: unchanged\n");
        return 0;
    }

    dprintf(STDERR_FILENO,
            "integrity:  If that was you, run 'integrity -u' to accept it.\n"
            "integrity:  If it was not, this machine has been changed by\n"
            "integrity:  somebody else. A reboot clears everything except\n"
            "integrity:  these files.\n");

    if (!check_only)
        save_record();       /* so the next boot compares against now */
    return 1;
}
