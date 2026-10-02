/* crontab - put the table cron reads in place, or take it away.
 *
 *   crontab -l          print the table
 *   crontab -e          edit it, install it only if it parses
 *   crontab -r          remove it
 *   crontab file        install this file (- reads standard input)
 *
 * ── The one job this program has ──
 * cron reads /data/crontab, falling back to /etc/crontab, and its parser
 * is small: a field is a star, a star-slash-step, a comma list, or a
 * number, and that is all. Anything else is not rejected by the daemon -
 * it simply never matches, so the job sits in the table looking correct
 * and never runs. `0 9 * * 1-5` is the one everybody writes: legal on
 * Ubuntu, and here `1-5` reads as the number 1, so the nightly job runs
 * on Mondays and nobody is told.
 *
 * So the validator in this file is the daemon's grammar, deliberately,
 * field for field, and a file with one bad line is refused whole with
 * the line number. A crontab that installs and then quietly does nothing
 * is the worst outcome this program can produce, worse than refusing a
 * file that would have worked.
 *
 * The same reasoning covers the limits: cron holds 32 jobs, reads at
 * most 599 bytes of a line and keeps 511 of a command. Past any of those
 * it truncates rather than complains, so those are checked here too.
 *
 * ── One table, not one per user ──
 * This cron has a single table and runs every line as root through
 * /bin/sh. There is no per-user crontab to edit, so `-u` is accepted
 * only for root and only for root, and a user who is not root is told
 * that rather than being allowed to write a file that would run as root.
 * On Ubuntu every user has their own; that difference is real and this
 * says so instead of pretending.
 *
 * ── Writing it ──
 * The install is a write to a temporary file in the same directory
 * followed by a rename. cron re-reads the table every minute, and a
 * rename is the only way to change a file it is reading without it ever
 * seeing half of one.
 *
 * Not here: Vixie's "Do you want to retry the same edit?" prompt after a
 * failed -e. The edits are left in a named file and the path is printed,
 * which is the same information without a question in the way.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define TABLE_DIR  "/data"
#define TABLE_A    "/data/crontab"
#define TABLE_B    "/etc/crontab"

/* cron's own limits. Past these it truncates silently, so they are
 * checked here where somebody is still watching. */
#define CRON_MAX_JOBS  32
#define CRON_MAX_LINE  599
#define CRON_MAX_CMD   511
#define CRON_MAX_FIELD 79

#define FILE_MAX  65536

static const char *prog = "crontab";
static char  text[FILE_MAX + 2];
static long  textlen;

static const struct { const char *what; int lo, hi; } FIELD[5] = {
    { "minute",       0, 59 },
    { "hour",         0, 23 },
    { "day-of-month", 1, 31 },
    { "month",        1, 12 },
    /* cron compares against lp_tm_t.wday, which is 0 for Sunday and
     * never 7 - so 7 is a value that can never come up, unlike Vixie
     * where it is a second spelling of Sunday. */
    { "day-of-week",  0,  6 },
};

static void usage(void)
{
    dprintf(STDERR_FILENO,
        "usage:\tcrontab [-u user] file\n"
        "\tcrontab [-u user] [-l | -r | -e]\n"
        "\n"
        "  the table is %s, or %s\n"
        "  MIN HOUR DAY MONTH WEEKDAY  command\n"
        "  fields take *, a number, a list (1,15), or */5\n",
        TABLE_A, TABLE_B);
}

/* The file cron would read right now, or NULL when it has none. */
static const char *current_table(void)
{
    if (lp_exists(TABLE_A))
        return TABLE_A;
    if (lp_exists(TABLE_B))
        return TABLE_B;
    return NULL;
}

/* Where a new table goes. /data is the writable half; a table written
 * to /etc on a machine whose root is in RAM lasts until the reboot, and
 * nobody expects that of a crontab. */
static const char *install_table(void)
{
    return lp_is_dir(TABLE_DIR) ? TABLE_A : TABLE_B;
}

static long slurp(int fd, char *buf, long max)
{
    long used = 0;
    for (;;) {
        long n = lp_read(fd, buf + used, (size_t)(max - used));
        if (n < 0)
            return n;
        if (n == 0)
            break;
        used += n;
        if (used >= max)
            break;
    }
    buf[used] = '\0';
    return used;
}

static bool read_path(const char *path, const char *gnu_before)
{
    long fd = strcmp(path, "-") == 0
            ? STDIN_FILENO
            : lp_open(path, O_RDONLY, 0);
    if (fd < 0) {
        lp_diag(prog, gnu_before, NULL, "cannot read", path, (int)-fd);
        return false;
    }
    textlen = slurp((int)fd, text, FILE_MAX);
    if (fd != STDIN_FILENO)
        lp_close((int)fd);
    if (textlen < 0) {
        lp_diag(prog, gnu_before, NULL, "cannot read", path, (int)-textlen);
        return false;
    }
    return true;
}

/* ── The validator ──
 *
 * This is cron's field_matches() read backwards: whatever that function
 * can act on is accepted, and everything else is refused here rather
 * than accepted and then ignored at three in the morning. */

static bool all_digits(const char *s, const char *end)
{
    if (s == end)
        return false;
    for (const char *p = s; p < end; p++)
        if (*p < '0' || *p > '9')
            return false;
    return true;
}

static int number(const char *s, const char *end)
{
    int v = 0;
    for (const char *p = s; p < end && v < 1000000; p++)
        v = v * 10 + (*p - '0');
    return v;
}

static bool saw_range, saw_dow_seven;

static bool field_ok(const char *s, const char *end, int which)
{
    if (end - s > CRON_MAX_FIELD)
        return false;                      /* cron keeps only 79 bytes */

    for (const char *p = s; p < end; p++)
        if (*p == '-')
            saw_range = true;

    if (end - s == 1 && *s == '*')
        return true;

    if (end - s > 2 && s[0] == '*' && s[1] == '/') {
        if (!all_digits(s + 2, end))
            return false;
        /* A step of zero matches nothing at all, in cron and here. */
        return number(s + 2, end) > 0;
    }

    /* A comma list, or a single number, which is a list of one. */
    const char *p = s;
    for (;;) {
        const char *comma = p;
        while (comma < end && *comma != ',')
            comma++;
        if (!all_digits(p, comma))
            return false;
        int v = number(p, comma);
        if (which == 4 && v == 7)
            saw_dow_seven = true;
        if (v < FIELD[which].lo || v > FIELD[which].hi)
            return false;
        if (comma == end)
            return true;
        p = comma + 1;
    }
}

/* Check the whole thing, saying what is wrong with each line it refuses.
 * Returns the number of bad lines. */
static int check(const char *label)
{
    int bad = 0, jobs = 0, lineno = 0;
    saw_range = saw_dow_seven = false;

    for (long i = 0; i < textlen; ) {
        long j = i;
        while (j < textlen && text[j] != '\n')
            j++;
        lineno++;

        const char *line = text + i, *eol = text + j;
        long len = j - i;
        i = j + 1;

        const char *p = line;
        while (p < eol && (*p == ' ' || *p == '\t'))
            p++;
        if (p == eol || *p == '#')
            continue;                       /* blank or comment, as cron does */

        if (len > CRON_MAX_LINE) {
            dprintf(STDERR_FILENO,
                    "\"%s\":%d: line is longer than %d bytes and cron would"
                    " cut it\n", label, lineno, CRON_MAX_LINE);
            bad++;
            continue;
        }

        bool line_bad = false;
        for (int f = 0; f < 5 && !line_bad; f++) {
            const char *start = p;
            while (p < eol && *p != ' ' && *p != '\t')
                p++;
            if (start == p || !field_ok(start, p, f)) {
                dprintf(STDERR_FILENO, "\"%s\":%d: bad %s\n",
                        label, lineno, FIELD[f].what);
                bad++;
                line_bad = true;
                break;
            }
            while (p < eol && (*p == ' ' || *p == '\t'))
                p++;
        }
        if (line_bad)
            continue;

        if (p == eol) {
            dprintf(STDERR_FILENO, "\"%s\":%d: bad command\n", label, lineno);
            bad++;
            continue;
        }
        if (eol - p > CRON_MAX_CMD) {
            dprintf(STDERR_FILENO,
                    "\"%s\":%d: command is longer than %d characters and"
                    " cron would cut it\n", label, lineno, CRON_MAX_CMD);
            bad++;
            continue;
        }

        if (++jobs > CRON_MAX_JOBS) {
            dprintf(STDERR_FILENO,
                    "\"%s\":%d: this cron holds %d jobs and from here on"
                    " they would not run\n", label, lineno, CRON_MAX_JOBS);
            bad++;
        }
    }

    if (bad) {
        dprintf(STDERR_FILENO, "errors in crontab file, can't install.\n");
        /* The two mistakes that are correct on Ubuntu and wrong here.
         * Without these lines "bad day-of-week" is true and useless. */
        if (saw_range)
            dprintf(STDERR_FILENO,
                    "%s: this cron has no ranges - write 1,2,3,4,5 rather"
                    " than 1-5\n", prog);
        if (saw_dow_seven)
            dprintf(STDERR_FILENO,
                    "%s: Sunday is 0 here, never 7\n", prog);
    }
    return bad;
}

/* ── telling integrity(1) that this was us ───────────────────────────
 *
 * integrity(1) hashes the handful of files that decide whether
 * something runs again after a reboot, and /data/crontab is one of
 * them: cron reads it and runs what is in it as root, on a clock. That
 * is exactly why it is watched, and exactly why the machine must not
 * report its own bookkeeping as an intrusion - `crontab -e` is the
 * intended way to change it.
 *
 * `integrity -a` re-records only the path named and copies the rest of
 * the record through untouched, so an edit to /data/rc.local or to
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

/* ── Putting it in place ──
 *
 * Into a temporary file beside the real one, then rename. cron re-reads
 * the table every minute; a rename swaps it in one step, so the daemon
 * either sees all of the old table or all of the new one and never a
 * half-written line. */
static bool install(const char *target)
{
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.%d", target, (int)lp_getpid());

    long fd = lp_open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        lp_diag(prog, "cannot create", NULL, "cannot create", tmp, (int)-fd);
        return false;
    }
    long off = 0;
    while (off < textlen) {
        long w = lp_write((int)fd, text + off, (size_t)(textlen - off));
        if (w <= 0) {
            lp_close((int)fd);
            lp_unlink(tmp);
            dprintf(STDERR_FILENO,
                    "%s: error while writing new crontab to %s\n", prog, tmp);
            return false;
        }
        off += w;
    }
    lp_close((int)fd);

    lp_chmod(tmp, 0644);
    if (lp_rename(tmp, target) < 0) {
        lp_unlink(tmp);
        dprintf(STDERR_FILENO,
                "%s: error while writing new crontab to %s\n", prog, target);
        return false;
    }
    lp_sync();
    integrity_accept(target);
    return true;
}

/* ── The editor ──
 * $VISUAL then $EDITOR then this system's own, which is what a person
 * who has not set either will have. */
static int run_editor(const char *path)
{
    const char *ed = getenv("VISUAL");
    if (!ed || !*ed) ed = getenv("EDITOR");
    if (!ed || !*ed) ed = "edit";

    char resolved[256];
    if (strchr(ed, '/')) {
        strlcpy(resolved, ed, sizeof resolved);
    } else {
        snprintf(resolved, sizeof resolved, "/bin/%s", ed);
        if (!lp_exists(resolved))
            snprintf(resolved, sizeof resolved, "/data/bin/%s", ed);
    }
    if (!lp_exists(resolved)) {
        dprintf(STDERR_FILENO, "%s: cannot run editor \"%s\"\n", prog, ed);
        return -1;
    }

    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "%s: cannot fork\n", prog);
        return -1;
    }
    if (pid == 0) {
        char *argv[3] = { resolved, (char *)path, NULL };
        lp_execve(resolved, argv, environ);
        lp_exit(127);
    }
    int status = 0;
    lp_waitpid(pid, &status, 0);
    if (!LP_WIFEXITED(status) || LP_WEXITSTATUS(status) != 0) {
        dprintf(STDERR_FILENO, "%s: \"%s\" exited without saving\n", prog, ed);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "list", 0, 'l' }, { "remove", 0, 'r' }, { "edit", 0, 'e' },
        { "user", 1, 'u' }, { "help", 0, 'h' }, { 0, 0, 0 }
    };

    bool do_list = false, do_remove = false, do_edit = false;
    const char *as_user = NULL;

    /* Everything after "--" is held aside. lp_getopt shuffles argv but
     * does not shrink argc, so a line containing "--" leaves the last
     * operand sitting in argv twice and `crontab -- f` would read as
     * two file names. */
    char *args[64], *tail[64];
    int   nargs = 0, ntail = 0;
    bool  past_dashdash = false;
    for (int i = 0; i < argc && nargs < 64 && ntail < 64; i++) {
        if (i && past_dashdash)             { tail[ntail++] = argv[i]; continue; }
        if (i && strcmp(argv[i], "--") == 0) { past_dashdash = true;   continue; }
        args[nargs++] = argv[i];
    }
    argc = nargs;
    argv = args;

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "lreu:h", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'l': do_list = true; break;
        case 'r': do_remove = true; break;
        case 'e': do_edit = true; break;
        case 'u': as_user = g.arg; break;
        case 'h': usage(); return 0;
        default:  lp_getopt_err(prog, &g); return 1;
        }
    }

    if ((int)do_list + (int)do_remove + (int)do_edit > 1) {
        dprintf(STDERR_FILENO,
                "%s: usage error: only one of -l, -r, -e\n", prog);
        usage();
        return 1;
    }

    int me = lp_getuid();
    lp_user_t self;
    const char *myname = lp_user_by_uid((uid_t)me, &self) ? self.name : "root";

    /* -u names whose table to work on. There is one, cron runs it as
     * root, and only root may say so. */
    const char *who = myname;
    if (as_user) {
        if (me != 0) {
            dprintf(STDERR_FILENO, "%s: must be privileged to use -u\n", prog);
            return 1;
        }
        lp_user_t u;
        if (!lp_user_by_name(as_user, &u)) {
            dprintf(STDERR_FILENO, "%s: user '%s' unknown\n", prog, as_user);
            return 1;
        }
        if (u.uid != 0) {
            dprintf(STDERR_FILENO,
                    "%s: this cron keeps one table and runs every line in"
                    " it as root.\n"
                    "%s:   There is no separate table for %s to hold. On"
                    " Ubuntu there\n"
                    "%s:   would be; here `cron -l` shows the only one"
                    " there is.\n", prog, prog, as_user, prog);
            return 1;
        }
        who = as_user;
    }

    /* Anyone may read the table - it is a 0644 file either way. Changing
     * it means choosing what runs as root, so that needs to be root. */
    if (me != 0 && !do_list) {
        dprintf(STDERR_FILENO,
                "%s: you are %s, and every line of this table runs as"
                " root.\n"
                "%s:   Only root may change it. `crontab -l` shows what is"
                " in it.\n", prog, myname, prog);
        return 1;
    }

    if (do_list) {
        const char *path = current_table();
        if (!path) {
            dprintf(STDERR_FILENO, "no crontab for %s\n", who);
            return 1;
        }
        if (!read_path(path, NULL))
            return 1;
        long off = 0;
        while (off < textlen) {
            long w = lp_write(STDOUT_FILENO, text + off,
                              (size_t)(textlen - off));
            if (w <= 0)
                break;
            off += w;
        }
        return 0;
    }

    if (do_remove) {
        const char *path = current_table();
        if (!path) {
            dprintf(STDERR_FILENO, "no crontab for %s\n", who);
            return 1;
        }
        long rc = lp_unlink(path);
        if (rc < 0) {
            lp_diag(prog, NULL, NULL, "cannot remove", path, (int)-rc);
            return 1;
        }
        lp_sync();
        /* Removing the writable one uncovers the one in the image, and
         * cron will start running that instead. Saying so is the whole
         * difference between "removed" and "removed, and now this". */
        const char *left = current_table();
        if (left)
            dprintf(STDERR_FILENO,
                    "%s: %s is gone; cron now reads %s, which is still"
                    " there\n", prog, path, left);
        return 0;
    }

    if (do_edit) {
        char tmp[64];
        snprintf(tmp, sizeof tmp, "/tmp/crontab.%d", (int)lp_getpid());

        const char *path = current_table();
        textlen = 0;
        text[0] = '\0';
        if (path && !read_path(path, NULL))
            return 1;

        long fd = lp_open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            lp_diag(prog, "cannot create", NULL, "cannot create", tmp,
                    (int)-fd);
            return 1;
        }
        if (textlen)
            lp_write((int)fd, text, (size_t)textlen);
        lp_close((int)fd);

        long before = textlen;
        static char was[FILE_MAX + 2];
        memcpy(was, text, (size_t)textlen);

        if (run_editor(tmp) != 0) {
            lp_unlink(tmp);
            return 1;
        }
        if (!read_path(tmp, NULL)) {
            lp_unlink(tmp);
            return 1;
        }
        if (textlen >= FILE_MAX) {
            dprintf(STDERR_FILENO,
                    "%s: %s is larger than %d bytes, which is more table"
                    " than cron can hold\n", prog, tmp, FILE_MAX);
            dprintf(STDERR_FILENO, "%s: edits left in %s\n", prog, tmp);
            return 1;
        }
        if (textlen == before && memcmp(was, text, (size_t)textlen) == 0) {
            lp_unlink(tmp);
            dprintf(STDERR_FILENO, "%s: no changes made to crontab\n", prog);
            return 0;
        }
        if (check(tmp) != 0) {
            /* The edit is not thrown away. Retyping it because the
             * validator did its job would be the wrong lesson. */
            dprintf(STDERR_FILENO, "%s: edits left in %s\n", prog, tmp);
            return 1;
        }
        if (!install(install_table()))
            return 1;
        lp_unlink(tmp);
        dprintf(STDERR_FILENO, "%s: installing new crontab\n", prog);
        return 0;
    }

    /* What is left is `crontab file`. */
    const char *from = NULL;
    int operands = (argc - g.ind) + ntail;
    if (operands == 1)
        from = (argc - g.ind) ? argv[g.ind] : tail[0];
    if (operands == 0) {
        dprintf(STDERR_FILENO,
                "%s: usage error: file name must be specified for replace\n",
                prog);
        usage();
        return 1;
    }
    if (operands > 1) {
        dprintf(STDERR_FILENO,
                "%s: usage error: no arguments permitted after this option\n",
                prog);
        usage();
        return 1;
    }

    if (!read_path(from, NULL))
        return 1;
    /* The whole table is installed from this buffer, so one that did not
     * fit would be installed with its tail missing - jobs that vanish
     * without a word. */
    if (textlen >= FILE_MAX) {
        dprintf(STDERR_FILENO,
                "%s: %s is larger than %d bytes, which is more table than"
                " cron can hold\n", prog, from, FILE_MAX);
        return 1;
    }
    if (check(strcmp(from, "-") == 0 ? "-" : from) != 0)
        return 1;
    if (!install(install_table()))
        return 1;
    return 0;
}
