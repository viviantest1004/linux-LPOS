/* date - show and set the clock.
 *
 *   date                            Sat Sep 26 23:47:01 KST 2026
 *   date +%Y-%m-%d                  any format GNU date knows, via lp_strftime
 *   date -d "next monday 9am" +%F   a time other than now
 *   date -f FILE                    each line of FILE, as -d would read it
 *   date -r FILE                    when FILE was last changed
 *   date -u / -R / -I[FMT] / --rfc-3339=FMT
 *   date -s "2026-09-01 12:34:56"   set the clock; MMDDhhmm[[CC]YY][.ss] too
 *   date -e                         unix seconds only (ours, not GNU's)
 *   date -z [ZONE|list]             the zone (ours; timedatectl does the work)
 *
 * ── The -d language ──
 *
 * GNU's -d is a small language - "last friday", "2 hours ago", "tomorrow
 * 5pm", "22-Nov-2025", "12:00 UTC+9", "@1790000000", TZ="Asia/Seoul"
 * prefixes - and scripts written on Ubuntu use all of it. The parser
 * below follows gnulib's parse-datetime.y rule for rule rather than
 * approximating it, because an approximation is worse than a refusal:
 * a date that is silently a day out is found when the backup did not
 * run, not when the script was written.
 *
 * That includes GNU's surprises, kept on purpose. A signed number after
 * a time is a zone offset, so "12:00 +1 month" is noon at UTC+1 plus a
 * month; "EST" in September is refused in New York, because it asks for
 * winter time on a summer date; and a relative month keeps the summer or
 * winter flag of the day it started from, so "8 months ago" in September
 * lands an hour earlier on the clock. Each of those is what Ubuntu says,
 * and a script tested there has to mean the same thing here. The
 * differential test (scratchpad cli/test/datediff.py) runs several
 * hundred strings through both.
 *
 * ── The zone ──
 *
 * All of it comes from the libc (tz.h): TZ, /etc/localtime, the older
 * /data/timezone line. -u is TZ=UTC0, exactly as GNU does it, so a -d
 * string is read in UTC too. `date -z NAME` used to write the zone itself
 * from a table of forty; setting a zone is timedatectl's job now, and -z
 * hands over to it so there is one writer of those files.
 *
 * The clock itself: `date -s` sets it, then writes it to the hardware
 * clock when there is one and to /data/.clock when there is not, because
 * a Pi Zero has no battery and would otherwise wake up in 1970.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define CLOCK_NAME  ".clock"
/* Anything before 2020 means the clock was never set. */
#define SANE_MIN    1577836800LL

static const char *prog = "date";

#include "parse-date.h"

/* ══ Output ══════════════════════════════════════════════════════════ */

static lp_lang_t lang;

static bool show(const char *fmt, s64 t, long ns)
{
    lp_tm_t tm;
    lp_localtime(t, &tm);
    char small[512];
    size_t need = lp_strftime_lang(small, sizeof small, fmt, &tm, ns, lang);
    if (need < sizeof small) {
        small[need] = '\n';
        lp_write(STDOUT_FILENO, small, need + 1);
        return true;
    }
    char *big = malloc(need + 2);
    if (!big) { dprintf(STDERR_FILENO, "%s: memory exhausted\n", prog); return false; }
    lp_strftime_lang(big, need + 1, fmt, &tm, ns, lang);
    big[need] = '\n';
    lp_write(STDOUT_FILENO, big, need + 1);
    free(big);
    return true;
}

static const char *default_format(void)
{
    /* glibc's ko_KR date_fmt, which is what Ubuntu prints in Korean. */
    return lang == LP_LANG_KO ? "%Y. %m. %d. (%a) %H:%M:%S %Z"
                              : "%a %b %e %H:%M:%S %Z %Y";
}

/* ══ Setting the clock ═══════════════════════════════════════════════ */

/* Remember the time so the next boot can pick up where this one left
 * off: the hardware clock when there is one, and /data/.clock, which
 * ntp -r reads on a board without one. */
static void save_clock(s64 t)
{
    bool rtc = lp_rtc_write(t);
    char buf[32];
    int  len = snprintf(buf, sizeof(buf), "%lld\n", (long long)t);
    char path[256];
    lp_setting_path(CLOCK_NAME, path, sizeof path);
    if (!lp_write_file_atomic(path, buf, (size_t)len) && !rtc)
        dprintf(STDERR_FILENO, "%s: no hardware clock and nothing writable that"
                " survives a reboot - this time will be gone at the next boot\n", prog);
}

/* POSIX's operand: MMDDhhmm[[CC]YY][.ss], in local time. */
static bool parse_posix_set(const char *s, s64 now, s64 *out)
{
    char d[16];
    size_t k = 0;
    const char *dot = strchr(s, '.');
    for (const char *p = s; *p && p != dot; p++) {
        if (!is_digit(*p) || k >= sizeof d - 1) return false;
        d[k++] = *p;
    }
    d[k] = '\0';
    if (k != 8 && k != 10 && k != 12) return false;
    int sec = 0;
    if (dot) {
        if (!is_digit(dot[1]) || !is_digit(dot[2]) || dot[3]) return false;
        sec = (dot[1] - '0') * 10 + (dot[2] - '0');
    }
#define TWO(i) ((d[i] - '0') * 10 + (d[i + 1] - '0'))
    lp_tm_t tm, now_tm;
    lp_localtime(now, &now_tm);
    memset(&tm, 0, sizeof tm);
    tm.mon = TWO(0); tm.day = TWO(2); tm.hour = TWO(4); tm.min = TWO(6);
    tm.sec = sec;
    if (k == 8) tm.year = now_tm.year;
    else if (k == 10) { int y = TWO(8); tm.year = y + (y < 69 ? 2000 : 1900); }
    else tm.year = TWO(8) * 100 + TWO(10);
#undef TWO
    tm.isdst = -1;
    lp_tm_t tm0 = tm;
    *out = lp_mktime(&tm);
    return same_fields(&tm0, &tm);
}

static int set_clock(s64 t, long ns, const char *fmt)
{
    int rc = 0;
    if (lp_settime(t) < 0) {
        dprintf(STDERR_FILENO, "%s: cannot set date: Operation not permitted\n", prog);
        rc = 1;
    } else {
        save_clock(t);
    }
    /* GNU prints the time even when setting it failed. */
    show(fmt, t, ns);
    return rc;
}

/* ══ -z: the zone, handed to timedatectl ═════════════════════════════ */

static int do_zone(int argc, char **argv, int at)
{
    if (at >= argc) {
        s64 now = lp_time();
        lp_tm_t tm;
        lp_localtime(now, &tm);
        char off[16];
        lp_strftime(off, sizeof off, "%:z", &tm);
        const char *name = lp_tz_name();
        printf("%s (%s, %s)\n", name[0] ? name : tm.zone, tm.zone, off);
        printf("run 'timedatectl list-timezones' to see the choices,\n"
               "'timedatectl set-timezone Area/City' to change it\n");
        return 0;
    }
    char *args[4] = { (char *)"timedatectl", NULL, NULL, NULL };
    if (strcmp(argv[at], "list") == 0) args[1] = (char *)"list-timezones";
    else { args[1] = (char *)"set-timezone"; args[2] = argv[at]; }
    lp_execve("/bin/timedatectl", args, environ);
    dprintf(STDERR_FILENO, "%s: cannot run timedatectl\n", prog);
    return 1;
}

/* ══ main ════════════════════════════════════════════════════════════ */

static void usage(void)
{
    printf("Usage: date [OPTION]... [+FORMAT]\n"
           "  or:  date [-u|--utc|--universal] [MMDDhhmm[[CC]YY][.ss]]\n"
           "Display date and time in the given FORMAT.\n"
           "With -s, or with [MMDDhhmm[[CC]YY][.ss]], set the date and time.\n\n"
           "  -d, --date=STRING          display time described by STRING, not 'now'\n"
           "      --debug                annotate the parsed date to stderr\n"
           "  -f, --file=DATEFILE        like --date; once for each line of DATEFILE\n"
           "  -I[FMT], --iso-8601[=FMT]  output date/time in ISO 8601 format.\n"
           "                               FMT='date' for date only (the default),\n"
           "                               'hours', 'minutes', 'seconds', or 'ns'\n"
           "  -R, --rfc-email            output date and time in RFC 5322 format.\n"
           "                               Example: Mon, 14 Aug 2006 02:34:56 -0600\n"
           "      --rfc-3339=FMT         output date/time in RFC 3339 format.\n"
           "                               FMT='date', 'seconds', or 'ns'\n"
           "  -r, --reference=FILE       display the last modification time of FILE\n"
           "  -s, --set=STRING           set time described by STRING\n"
           "  -u, --utc, --universal     print or set Coordinated Universal Time (UTC)\n"
           "      --help        display this help and exit\n"
           "      --version     output version information and exit\n\n"
           "FORMAT controls the output, as in GNU date: %%a %%A %%b %%B %%c %%C %%d %%D\n"
           "%%e %%F %%g %%G %%h %%H %%I %%j %%k %%l %%m %%M %%n %%N %%p %%P %%q %%r %%R %%s %%S\n"
           "%%t %%T %%u %%U %%V %%w %%W %%x %%X %%y %%Y %%z %%:z %%::z %%:::z %%Z, with the\n"
           "flags - _ 0 ^ # + and a field width (%%-d, %%_H, %%^a, %%3N).\n\n"
           "STRING is GNU's date language: 'now', 'yesterday', 'next monday',\n"
           "'2 hours ago', 'last friday 5pm', '2026-09-26 12:00 UTC+9', '@1790000000',\n"
           "'22-Nov-2025', 'TZ=\"Asia/Seoul\" 2026-09-26 09:00'.\n\n"
           "Two options here are this system's own:\n"
           "  -e             the time as plain unix seconds\n"
           "  -z [ZONE|list] show the zone, list them, or set one (timedatectl)\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "date", 1, 'd' }, { "file", 1, 'f' }, { "iso-8601", 2, 'I' },
        { "rfc-email", 0, 'R' }, { "rfc-2822", 0, 'R' }, { "rfc-822", 0, 'R' },
        { "rfc-3339", 1, '3' }, { "reference", 1, 'r' }, { "set", 1, 's' },
        { "utc", 0, 'u' }, { "universal", 0, 'u' }, { "uct", 0, 'u' },
        { "debug", 0, 'D' }, { "help", 0, 'H' }, { "version", 0, 'V' },
        { 0, 0, 0 }
    };
    lang = lp_time_lang();

    const char *when = NULL, *setstr = NULL, *reffile = NULL, *datefile = NULL;
    const char *iso = NULL, *rfc3339 = NULL;
    bool rfc_email = false;
    int nopt = 0;              /* -I, -R and --rfc-3339, each time given */
    int real_argc = argc;

    /* -z is ours and takes the rest of the line, so it is taken off
     * before the GNU options are parsed. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) break;
        if (strcmp(argv[i], "-z") == 0) return do_zone(real_argc, argv, i + 1);
    }

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "d:f:I::Rr:s:ue", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'd': when = g.arg; break;
        case 'f': datefile = g.arg; break;
        case 'I': iso = g.arg ? g.arg : "date"; nopt++; break;
        case '3': rfc3339 = g.arg; nopt++; break;
        case 'R': rfc_email = true; nopt++; break;
        case 'r': reffile = g.arg; break;
        case 's': setstr = g.arg; break;
        case 'u': setenv("TZ", "UTC0", 1); break;
        case 'D': break;
        case 'e': printf("%lld\n", (long long)lp_time()); return 0;
        case 'H': usage(); return 0;
        case 'V': printf("date (LP) 2.0\n"); return 0;
        default: lp_getopt_err(prog, &g); return 1;
        }
    }

    if ((when != NULL) + (datefile != NULL) + (reffile != NULL) > 1) {
        dprintf(STDERR_FILENO, "%s: the options to specify dates for printing are"
                " mutually exclusive\nTry 'date --help' for more information.\n", prog);
        return 1;
    }
    if (setstr && (when || datefile || reffile)) {
        dprintf(STDERR_FILENO, "%s: the options to print and set the time may not be"
                " used together\nTry 'date --help' for more information.\n", prog);
        return 1;
    }

    /* The format: from an operand, or from the option that implies one. */
    const char *fmt = NULL, *posix_set = NULL;
    int nfmt = 0;
    if (rfc_email) { fmt = "%a, %d %b %Y %H:%M:%S %z"; nfmt++; }
    if (rfc3339) {
        size_t l = strlen(rfc3339);
        if (l && strncmp("date", rfc3339, l) == 0)         fmt = "%Y-%m-%d";
        else if (l && strncmp("seconds", rfc3339, l) == 0) fmt = "%Y-%m-%d %H:%M:%S%:z";
        else if (l && strncmp("ns", rfc3339, l) == 0)      fmt = "%Y-%m-%d %H:%M:%S.%N%:z";
        else {
            dprintf(STDERR_FILENO, "%s: invalid argument '%s' for '--rfc-3339'\n"
                    "Valid arguments are:\n  - 'date'\n  - 'seconds'\n  - 'ns'\n"
                    "Try 'date --help' for more information.\n", prog, rfc3339);
            return 1;
        }
        nfmt++;
    }
    if (iso) {
        static const struct { const char *name, *fmt; } ISO[] = {
            { "date", "%Y-%m-%d" }, { "hours", "%Y-%m-%dT%H%:z" },
            { "minutes", "%Y-%m-%dT%H:%M%:z" }, { "seconds", "%Y-%m-%dT%H:%M:%S%:z" },
            { "ns", "%Y-%m-%dT%H:%M:%S,%N%:z" }, { NULL, NULL }
        };
        size_t l = strlen(iso);
        const char *f = NULL;
        for (int i = 0; ISO[i].name; i++)
            if (l && strncmp(ISO[i].name, iso, l) == 0) { f = ISO[i].fmt; break; }
        if (!f) {
            dprintf(STDERR_FILENO, "%s: invalid argument '%s' for '--iso-8601'\n"
                    "Valid arguments are:\n  - 'hours'\n  - 'minutes'\n  - 'date'\n"
                    "  - 'seconds'\n  - 'ns'\nTry 'date --help' for more information.\n",
                    prog, iso);
            return 1;
        }
        fmt = f;
        nfmt++;
    }
    if (nfmt > 1 || nopt > 1) {
        dprintf(STDERR_FILENO, "%s: multiple output formats specified\n", prog);
        return 1;
    }
    /* coreutils' order, so the complaint is the same one: a second
     * operand is always "extra", then a format that clashes with -I, -R
     * or --rfc-3339, then a set-operand after an option that already
     * named a date. */
    if (g.ind < argc) {
        if (g.ind + 1 < argc) {
            dprintf(STDERR_FILENO, "%s: extra operand '%s'\n"
                    "Try 'date --help' for more information.\n", prog, argv[g.ind + 1]);
            return 1;
        }
        if (argv[g.ind][0] == '+') {
            if (fmt) {
                dprintf(STDERR_FILENO, "%s: multiple output formats specified\n", prog);
                return 1;
            }
            fmt = argv[g.ind] + 1;
        } else if (setstr || when || datefile || reffile) {
            dprintf(STDERR_FILENO, "%s: the argument '%s' lacks a leading '+';\n"
                    "when using an option to specify date(s), any non-option\n"
                    "argument must be a format string beginning with '+'\n"
                    "Try 'date --help' for more information.\n", prog, argv[g.ind]);
            return 1;
        } else {
            posix_set = argv[g.ind];
        }
    }
    if (!fmt) fmt = default_format();

    long now_ns;
    s64 now = lp_time_ns(&now_ns);

    if (posix_set) {
        s64 t;
        if (!parse_posix_set(posix_set, now, &t)) {
            dprintf(STDERR_FILENO, "%s: invalid date '%s'\n", prog, posix_set);
            return 1;
        }
        return set_clock(t, 0, fmt);
    }
    if (setstr) {
        s64 t; long ns;
        if (!parse_date(setstr, now, now_ns, &t, &ns)) {
            dprintf(STDERR_FILENO, "%s: invalid date '%s'\n", prog, setstr);
            return 1;
        }
        return set_clock(t, ns, fmt);
    }

    if (datefile) {
        long fd = strcmp(datefile, "-") == 0 ? STDIN_FILENO : lp_open(datefile, O_RDONLY, 0);
        if (fd < 0) {
            lp_diag(prog, NULL, NULL, "cannot open", datefile, (int)fd);
            return 1;
        }
        int rc = 0;
        char line[1024];
        while (readline((int)fd, line, sizeof line) >= 0) {
            s64 t; long ns;
            if (!parse_date(line, now, now_ns, &t, &ns)) {
                dprintf(STDERR_FILENO, "%s: invalid date '%s'\n", prog, line);
                rc = 1;
                continue;
            }
            show(fmt, t, ns);
        }
        if (fd != STDIN_FILENO) lp_close((int)fd);
        return rc;
    }

    s64 t = now;
    long ns = now_ns;
    if (reffile) {
        lp_stat_t st;
        long r = lp_stat(reffile, &st, true);
        if (r < 0) {
            lp_diag(prog, NULL, NULL, "cannot stat", reffile, (int)r);
            return 1;
        }
        t = st.mtime;
        ns = st.mtime_ns;
    }
    if (when && !parse_date(when, t, ns, &t, &ns)) {
        dprintf(STDERR_FILENO, "%s: invalid date '%s'\n", prog, when);
        return 1;
    }
    if (!show(fmt, t, ns)) return 1;
    return 0;
}
