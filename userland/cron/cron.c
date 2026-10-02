/* cron - run things on a schedule.
 *
 *   cron -d          the daemon: read the table, run what is due
 *   cron -l          list what is in the table and when each next runs
 *   cron -n          say what would run in the next hour, run nothing
 *
 * The table is /data/crontab, with /etc/crontab as a fallback so an
 * image can ship jobs. /data first because /data is the writable half:
 * on a machine whose root is in RAM, editing /etc/crontab lasts until
 * the next reboot and nobody expects that of a crontab.
 *
 *   MIN HOUR DOM MON DOW  command
 *
 * The times are UTC, not local. This machine keeps its clock in UTC and
 * `date` applies a zone only when it prints, so there is no local time
 * for a daemon to read - and a scheduler that guessed one would run the
 * nightly job at the wrong hour twice a year without saying anything.
 *
 *   *       every value
 *   5       exactly five
 *   1,15,30 any of these
 *   星/5    every fifth (written as asterisk-slash-5)
 *
 * Ranges (1-5) are deliberately not here. Everything above is one line
 * of parsing each; ranges need a second syntax in the same field and
 * they are the one form that is always writable as a list.
 *
 * ── The minute is the unit, and it is checked once ──
 *
 * The loop wakes at the top of each minute and asks each job whether
 * this minute is its. A job that takes ten minutes does not delay the
 * next check, because it is run in a child - but the same job is not
 * started again while the first is still going. A backup that starts
 * every five minutes and takes twenty would otherwise end up with four
 * copies of itself writing the same file, which is how a scheduler
 * turns a slow job into a broken one.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "stdlib.h"

#define MAX_JOBS 32
#define TABLE_A  "/data/crontab"
#define TABLE_B  "/etc/crontab"

typedef struct {
    char  min[80], hour[80], dom[80], mon[80], dow[80];
    char  cmd[512];
    pid_t running;          /* 0 when it is not */
} job_t;

static job_t jobs[MAX_JOBS];
static int   njobs = 0;

/* Does `field` match `value`? The four spellings, in the order they
 * cost: a star, a step, a list, a number. */
static bool field_matches(const char *field, int value)
{
    if (field[0] == '*' && field[1] == '\0')
        return true;

    if (field[0] == '*' && field[1] == '/') {
        int step = atoi(field + 2);
        return step > 0 && (value % step) == 0;
    }

    const char *p = field;
    while (*p) {
        int v = atoi(p);
        if (v == value && (p[0] >= '0' && p[0] <= '9'))
            return true;
        const char *comma = strchr(p, ',');
        if (!comma) break;
        p = comma + 1;
    }
    return false;
}

static bool due(const job_t *j, const lp_tm_t *t)
{
    return field_matches(j->min,  t->min)
        && field_matches(j->hour, t->hour)
        && field_matches(j->dom,  t->day)
        && field_matches(j->mon,  t->mon)
        && field_matches(j->dow,  t->wday);
}

/* One line into a job. Returns false for a blank or a comment. */
static bool parse_line(const char *line, job_t *j)
{
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#')
        return false;

    char *dst[5] = { j->min, j->hour, j->dom, j->mon, j->dow };
    for (int f = 0; f < 5; f++) {
        size_t n = 0;
        while (*line && *line != ' ' && *line != '\t' && n < 79)
            dst[f][n++] = *line++;
        dst[f][n] = '\0';
        if (!n)
            return false;
        while (*line == ' ' || *line == '\t') line++;
    }
    if (!*line)
        return false;
    strlcpy(j->cmd, line, sizeof j->cmd);
    j->running = 0;
    return true;
}

static const char *load(void)
{
    njobs = 0;
    const char *path = TABLE_A;
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0) {
        path = TABLE_B;
        fd = lp_open(path, O_RDONLY, 0);
    }
    if (fd < 0)
        return NULL;

    char line[600];
    while (readline((int)fd, line, sizeof line) >= 0 && njobs < MAX_JOBS) {
        if (parse_line(line, &jobs[njobs]))
            njobs++;
    }
    lp_close((int)fd);
    return path;
}

static void run_job(job_t *j)
{
    /* Still going from last time? Then this minute is skipped and said
     * so, because a job quietly not running is the failure a scheduler
     * must never have. */
    if (j->running) {
        int st = 0;
        if (lp_waitpid(j->running, &st, WNOHANG) == 0) {
            dprintf(STDERR_FILENO,
                    "cron: %s is still running from last time - skipped\n",
                    j->cmd);
            return;
        }
        j->running = 0;
    }

    pid_t pid = lp_fork();
    if (pid == 0) {
        /* Through the shell, so a crontab line can hold a pipe or a
         * redirection - which is most of what they hold. */
        char *argv[4];
        argv[0] = (char *)"/bin/sh";
        argv[1] = (char *)"-c";
        argv[2] = j->cmd;
        argv[3] = NULL;
        lp_execve("/bin/sh", argv, environ);
        lp_exit(127);
    }
    if (pid > 0)
        j->running = pid;
    else
        dprintf(STDERR_FILENO, "cron: cannot fork for %s\n", j->cmd);
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "-l";

    if (strcmp(mode, "-h") == 0) {
        printf("usage:\n");
        printf("  cron -d     run the table, forever\n");
        printf("  cron -l     what is in the table\n");
        printf("  cron -n     what would run in the next hour\n");
        printf("\n");
        printf("  the table is %s, or %s\n", TABLE_A, TABLE_B);
        printf("  MIN HOUR DAY MONTH WEEKDAY  command\n");
        printf("  fields take *, a number, a list (1,15), or */5\n");
        printf("  times are UTC\n");
        return 0;
    }

    const char *path = load();
    if (!path) {
        if (strcmp(mode, "-d") == 0) {
            /* No table is not an error - most machines have none. Sleep
             * instead of exiting, or the supervisor restarts this every
             * second forever. */
            for (;;) lp_sleep_ms(3600000);
        }
        printf("cron: no table (%s or %s)\n", TABLE_A, TABLE_B);
        return 1;
    }

    if (strcmp(mode, "-l") == 0) {
        printf("%s - %d job%s\n", path, njobs, njobs == 1 ? "" : "s");
        for (int i = 0; i < njobs; i++)
            printf("  %-6s %-5s %-4s %-4s %-4s  %s\n",
                   jobs[i].min, jobs[i].hour, jobs[i].dom,
                   jobs[i].mon, jobs[i].dow, jobs[i].cmd);
        return 0;
    }

    if (strcmp(mode, "-n") == 0) {
        lp_tm_t t;
        lp_gmtime(lp_time(), &t);
        printf("the next hour, from %02d:%02d\n", t.hour, t.min);
        int shown = 0;
        for (int step = 1; step <= 60; step++) {
            lp_tm_t f = t;
            f.min += step;
            while (f.min >= 60) { f.min -= 60; f.hour = (f.hour + 1) % 24; }
            for (int i = 0; i < njobs; i++)
                if (due(&jobs[i], &f)) {
                    printf("  %02d:%02d  %s\n", f.hour, f.min, jobs[i].cmd);
                    shown++;
                }
        }
        if (!shown)
            printf("  nothing\n");
        return 0;
    }

    if (strcmp(mode, "-d") != 0) {
        dprintf(STDERR_FILENO, "cron: %s? try `cron -h`\n", mode);
        return 2;
    }

    /* ── the daemon ──
     *
     * Waking on the minute rather than every sixty seconds matters: a
     * loop that sleeps sixty seconds drifts, and after a day it is
     * running "every minute" jobs at :59 and :01 of the same minute or
     * skipping one entirely. Sleeping to the top of the next minute
     * keeps it on the clock the crontab is written against. */
    int last_min = -1;
    for (;;) {
        lp_tm_t t;
        lp_gmtime(lp_time(), &t);

        if (t.min != last_min) {
            last_min = t.min;
            /* Re-read every minute so an edit takes effect without a
             * restart. It is one small file. */
            load();
            for (int i = 0; i < njobs; i++)
                if (due(&jobs[i], &t))
                    run_job(&jobs[i]);
        }

        /* Reap anything that finished, so the table does not fill with
         * zombies on a machine that runs a job every minute. */
        for (int i = 0; i < njobs; i++)
            if (jobs[i].running) {
                int st = 0;
                if (lp_waitpid(jobs[i].running, &st, WNOHANG) > 0)
                    jobs[i].running = 0;
            }

        lp_sleep_ms(5000);
    }
}
