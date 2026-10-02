/* hwclock - the clock that keeps running while the machine is off.
 *
 *   hwclock              read it
 *   hwclock -w           copy the system clock into it
 *   hwclock -s           copy it into the system clock
 *
 * There are two clocks. The system clock lives in RAM and dies with the
 * power. The hardware clock has a battery and does not - which is why a
 * PC switched on after a week knows a week has passed, and a Raspberry
 * Pi Zero 2 W, which has no such clock, does not.
 *
 * The kernel reads the hardware clock once at boot. Nothing writes it
 * back on its own, so a time set by hand or fetched by ntp is gone at
 * the next power cut unless something puts it there: that is what -w is
 * for, and `date -s` and `ntp` now do it themselves.
 *
 * The hardware clock is kept in UTC here, which is what Linux assumes
 * and what avoids an hour of confusion twice a year. --localtime is
 * accepted and refused rather than silently doing something else.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static const char *prog = "hwclock";

/* "2026-09-07 12:34:56.000000+09:00" - the shape util-linux prints. */
static void show(s64 t)
{
    lp_tm_t tm;
    lp_localtime(t, &tm);
    int off = lp_tz_offset(t);
    char sign = off < 0 ? '-' : '+';
    int  a = off < 0 ? -off : off;
    static const char *DAY[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    static const char *MON[13] = { "", "Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec" };
    printf("%s %s %2d %02d:%02d:%02d %04d %c%02d:%02d\n",
           DAY[tm.wday % 7], MON[tm.mon], tm.day,
           tm.hour, tm.min, tm.sec, tm.year, sign, a / 60, a % 60);
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "show", 0, 'r' }, { "systohc", 0, 'w' }, { "hctosys", 0, 's' },
        { "utc", 0, 'u' }, { "localtime", 0, 'l' },
        { "verbose", 0, 'v' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    enum { SHOW, TO_RTC, TO_SYS } what = SHOW;
    bool verbose = false;

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "rwsulv", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'r': what = SHOW; break;
        case 'w': what = TO_RTC; break;
        case 's': what = TO_SYS; break;
        case 'u': break;
        case 'l':
            dprintf(STDERR_FILENO,
                    "%s: the hardware clock here is kept in UTC, always.\n"
                    "%s: a local-time clock is an hour wrong twice a year "
                    "until somebody notices.\n", prog, prog);
            return 1;
        case 'v': verbose = true; break;
        case 'H':
            printf("Usage: hwclock [FUNCTION] [OPTION]...\n"
                   "Query or set the hardware clock.\n\n"
                   "Functions:\n"
                   "  -r, --show       read the hardware clock and print the result\n"
                   "  -w, --systohc    write the system time to the hardware clock\n"
                   "  -s, --hctosys    set the system time from the hardware clock\n\n"
                   "Options:\n"
                   "  -u, --utc        the hardware clock is kept in UTC (it always is here)\n"
                   "  -v, --verbose    say what is happening\n"
                   "      --help       display this help and exit\n");
            return 0;
        default: lp_getopt_err(prog, &g); return 1;
        }
    }

    s64 t;
    switch (what) {
    case SHOW:
        if (!lp_rtc_read(&t)) {
            dprintf(STDERR_FILENO,
                    "%s: cannot access the hardware clock\n"
                    "%s:   this board may not have one - a Raspberry Pi does not.\n"
                    "%s:   `date` still works; the time comes from ntp and from\n"
                    "%s:   what was saved at the last shutdown.\n",
                    prog, prog, prog, prog);
            return 1;
        }
        show(t);
        return 0;

    case TO_RTC:
        t = lp_time();
        if (!lp_rtc_write(t)) {
            dprintf(STDERR_FILENO, "%s: cannot write the hardware clock\n", prog);
            return 1;
        }
        if (verbose) { printf("%s: hardware clock set to ", prog); show(t); }
        return 0;

    case TO_SYS:
        if (!lp_rtc_read(&t)) {
            dprintf(STDERR_FILENO, "%s: cannot access the hardware clock\n", prog);
            return 1;
        }
        if (lp_settime(t) < 0) {
            dprintf(STDERR_FILENO, "%s: cannot set the system clock: "
                                   "must be root\n", prog);
            return 1;
        }
        if (verbose) { printf("%s: system clock set to ", prog); show(t); }
        return 0;
    }
    return 0;
}
