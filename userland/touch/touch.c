/* touch - create a file, or set its times.
 *
 *   touch [-acmh] [-d DATE | -t STAMP | -r FILE] <file>...
 *
 * As GNU's: a file that is not there is created, and every file named
 * gets its access and modification times set - to now, or to the time
 * -d, -t or -r give. -a and -m set only one of the two.
 *
 * This used to create files and nothing else. `touch existing-file`
 * succeeded and changed nothing, and -d was an invalid option, because
 * libc had no wrapper for utimensat(2). It is called directly here, the
 * way lp_umask calls umask: the number is in each architecture's
 * syscall header, and on 32-bit ARM it is utimensat_time64, which takes
 * the same 64-bit timespec the other two do.
 *
 * -d reads a date the way `date -d` does - the parser is date's own
 * (date/parse-date.h) - so "2026-09-28 14:00", "@1790000000",
 * "yesterday" and "3 days ago" all mean what date says they mean.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"
#include "../date/parse-date.h"

#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define UTIME_NOW           ((1L << 30) - 1L)
#define UTIME_OMIT          ((1L << 30) - 2L)

/* The kernel's __kernel_timespec: 64-bit seconds and nanoseconds on all
 * three architectures (x86-64 and arm64 natively, 32-bit ARM through
 * utimensat_time64). */
typedef struct { s64 sec; s64 nsec; } kts_t;

static long set_times(const char *path, const kts_t t[2], int flags)
{
    return sys_call4(SYS_utimensat, AT_FDCWD, (long)path, (long)t, flags);
}

static void usage(int fd)
{
    dprintf(fd, "Usage: touch [OPTION]... FILE...\n"
                "Update the access and modification times of each FILE to the current time.\n\n"
                "A FILE argument that does not exist is created empty, unless -c or -h\n"
                "is supplied.\n\n"
                "  -a                     change only the access time\n"
                "  -c, --no-create        do not create any files\n"
                "  -d, --date=STRING      parse STRING and use it instead of current time\n"
                "  -h, --no-dereference   affect each symbolic link instead of any referenced\n"
                "                         file (never creates a file)\n"
                "  -m                     change only the modification time\n"
                "  -r, --reference=FILE   use this file's times instead of current time\n"
                "  -t STAMP               use [[CC]YY]MMDDhhmm[.ss] instead of current time\n"
                "      --time=WORD        change the specified time:\n"
                "                           WORD is access, atime, or use: equivalent to -a\n"
                "                           WORD is modify or mtime: equivalent to -m\n"
                "      --help     display this help and exit\n");
}

static int two(const char *s) { return (s[0] - '0') * 10 + (s[1] - '0'); }

/* -t [[CC]YY]MMDDhhmm[.ss], in local time. A two-digit year is 1969-2068,
 * as POSIX says. */
static bool parse_stamp(const char *s, s64 *out)
{
    size_t n = strlen(s), digits = n;
    const char *dot = strchr(s, '.');
    int sec = 0;
    if (dot) {
        if (strlen(dot + 1) != 2) return false;
        digits = (size_t)(dot - s);
        for (int k = 1; k <= 2; k++) if (dot[k] < '0' || dot[k] > '9') return false;
        sec = two(dot + 1);
    }
    if (digits != 8 && digits != 10 && digits != 12) return false;
    for (size_t k = 0; k < digits; k++) if (s[k] < '0' || s[k] > '9') return false;

    lp_tm_t tm;
    memset(&tm, 0, sizeof tm);
    long nowns;
    lp_localtime(lp_time_ns(&nowns), &tm);          /* the year, when none is given */
    const char *p = s;
    if (digits == 12) { tm.year = two(p) * 100 + two(p + 2); p += 4; }
    else if (digits == 10) { int yy = two(p); tm.year = yy < 69 ? 2000 + yy : 1900 + yy; p += 2; }
    tm.mon = two(p); tm.day = two(p + 2); tm.hour = two(p + 4); tm.min = two(p + 6);
    tm.sec = sec;
    if (tm.mon < 1 || tm.mon > 12 || tm.day < 1 || tm.day > 31 ||
        tm.hour > 23 || tm.min > 59 || tm.sec > 60)
        return false;
    tm.isdst = -1;
    *out = lp_mktime(&tm);
    return true;
}

int main(int argc, char **argv)
{
    enum { OPT_TIME = 300 };
    static const lp_lopt_t lo[] = {
        { "no-create", 0, 'c' }, { "date", 1, 'd' }, { "no-dereference", 0, 'h' },
        { "reference", 1, 'r' }, { "time", 1, OPT_TIME }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    bool no_create = false, only_a = false, only_m = false, nofollow = false;
    const char *date = NULL, *stamp = NULL, *ref = NULL;

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "acd:fhmr:t:", lo);
    for (int c; (c = lp_getopt(&g)) != -1; )
        switch (c) {
        case 'a': only_a = true; break;
        case 'c': no_create = true; break;
        case 'd': date = g.arg; break;
        case 'f': break;                        /* ignored, as in GNU */
        case 'h': nofollow = true; break;
        case 'm': only_m = true; break;
        case 'r': ref = g.arg; break;
        case 't': stamp = g.arg; break;
        case OPT_TIME:
            if (!strcmp(g.arg, "access") || !strcmp(g.arg, "atime") || !strcmp(g.arg, "use"))
                only_a = true;
            else if (!strcmp(g.arg, "modify") || !strcmp(g.arg, "mtime"))
                only_m = true;
            else {
                dprintf(STDERR_FILENO, "touch: invalid argument '%s' for '--time'\n"
                        "Valid arguments are:\n  - 'atime', 'access', 'use'\n"
                        "  - 'mtime', 'modify'\n"
                        "Try 'touch --help' for more information.\n", g.arg);
                return 1;
            }
            break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default:  lp_getopt_err("touch", &g); return 1;
        }

    if ((date != NULL) + (stamp != NULL) > 1 || (stamp && ref)) {
        dprintf(STDERR_FILENO, "touch: cannot specify times from more than one source\n"
                               "Try 'touch --help' for more information.\n");
        return 1;
    }
    if (g.ind >= argc) {
        dprintf(STDERR_FILENO, "touch: missing file operand\n"
                               "Try 'touch --help' for more information.\n");
        return 1;
    }

    /* The two times to set; UTIME_NOW unless something said otherwise. */
    kts_t t[2] = { { 0, UTIME_NOW }, { 0, UTIME_NOW } };
    bool given = false;
    if (ref) {
        lp_stat_t st;
        long r = lp_stat(ref, &st, !nofollow);
        if (r < 0) {
            lp_diag("touch", "failed to get attributes of", NULL, "cannot read", ref, (int)-r);
            return 1;
        }
        t[0].sec = st.atime; t[0].nsec = st.atime_ns;
        t[1].sec = st.mtime; t[1].nsec = st.mtime_ns;
        given = true;
    }
    if (date) {
        /* With -r as well, GNU reads the date relative to the reference's
         * modification time ("-r f -d '+1 hour'"). */
        long nowns = 0;
        s64 now = ref ? t[1].sec : lp_time_ns(&nowns);
        if (ref) nowns = (long)t[1].nsec;
        s64 when; long ns;
        if (!parse_date(date, now, nowns, &when, &ns)) {
            dprintf(STDERR_FILENO, "touch: invalid date format '%s'\n", date);
            return 1;
        }
        t[0].sec = t[1].sec = when;
        t[0].nsec = t[1].nsec = ns;
        given = true;
    }
    if (stamp) {
        s64 when;
        if (!parse_stamp(stamp, &when)) {
            dprintf(STDERR_FILENO, "touch: invalid date format '%s'\n", stamp);
            return 1;
        }
        t[0].sec = t[1].sec = when;
        t[0].nsec = t[1].nsec = 0;
        given = true;
    }
    (void)given;
    /* -a alone leaves the modification time, -m alone the access time;
     * both, or neither, set both. */
    if (only_a && !only_m) t[1].nsec = UTIME_OMIT;
    if (only_m && !only_a) t[0].nsec = UTIME_OMIT;

    int rc = 0;
    for (int i = g.ind; i < argc; i++) {
        const char *f = argv[i];
        lp_stat_t st;
        bool there = lp_stat(f, &st, !nofollow) == 0;
        if (!there) {
            if (no_create || nofollow)
                continue;
            /* No O_EXCL. If someone else created it between the check and
             * here, the file exists, which is what we wanted anyway. */
            long fd = lp_open(f, O_WRONLY | O_CREAT, 0666);
            if (fd < 0) {
                lp_diag("touch", "cannot touch", NULL, "cannot create", f, (int)-fd);
                rc = 1;
                continue;
            }
            lp_close((int)fd);
        }
        long r = set_times(f, t, nofollow ? AT_SYMLINK_NOFOLLOW : 0);
        if (r < 0) {
            lp_diag("touch", "setting times of", NULL, "cannot set the times", f, (int)-r);
            rc = 1;
        }
    }
    return rc;
}
