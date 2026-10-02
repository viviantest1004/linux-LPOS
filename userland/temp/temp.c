/* temp - how hot this board is, and whether that is a problem.
 *
 * The number was already on the machine three times over: `info` prints
 * it, `sysinfo` prints it and `guard` acts on it. What was missing was
 * the question people actually ask. "Is it too hot?" meant running
 * `info`, reading forty other lines to find the one, and then knowing
 * off the top of your head that 82C is bad and 58C is not. So: one
 * number, one bar, and one word saying whether to care.
 *
 * Two kinds of sensor are read, because two kinds of machine run this
 * system.
 *
 * A Pi has /sys/class/thermal: one zone, the SoC inside it, numbered
 * from zero with no gaps, and the zone's own trip points as the limit
 * the kernel will act on. A PC or an EC2 instance usually has nothing
 * useful there and puts everything under /sys/class/hwmon instead - a
 * package sensor, one per core, and the NVMe drive alongside. Those
 * numbers are handed out as drivers load, so hwmon0 and hwmon2 with
 * nothing in between is ordinary. Stopping at the first missing hwmon
 * therefore finds no sensors at all on a machine that has five, which
 * is why the thermal loop below stops at its first gap and the hwmon
 * loop keeps going.
 *
 * One chip can show up in both places: the thermal core registers each
 * of its zones under hwmon as well, so the same reading arrives twice
 * by two paths. Nothing here tries to spot that and hide one of them.
 * Deciding that two numbers are "really" the same sensor means guessing,
 * and the guess is wrong on some machine somewhere; a duplicate costs
 * nothing, because the headline takes the hottest either way and -a
 * prints the path each number came from, which makes it obvious.
 *
 * The GPU firmware's throttle word is the part of this worth having on
 * a Pi, and it is not about heat. Undervoltage leaves no other trace at
 * all: the board does not crash, nothing is logged, it simply corrupts
 * the card quietly some weeks later - and by then the power supply is
 * the last thing anyone suspects. If that bit is set this says so in as
 * many words, and says what to do about it.
 *
 * A machine with no sensor says so. It used to be tempting to print
 * 0.0 C for a missing zone, and 0.0 C reads as a measurement of a very
 * cold board rather than as the absence of a thermometer.
 *
 * There is no floating point anywhere in this system, so the tenth of a
 * degree is cut out of the millidegrees by hand.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

/* The same width as the bars in `usage`, so two commands on one screen
 * do not disagree about where the right hand edge is. */
#define BAR_WIDTH   24

/* Where the bar starts, and where it ends when the kernel does not say.
 *
 * It does not start at zero. A board with the power on is never below
 * about 25C, so a bar counting up from absolute zero sits a third full
 * when idle and then moves barely a finger's width across the whole
 * range anyone actually cares about. Starting at 30C and ending where
 * the chip protects itself means a full bar means exactly one thing. */
#define BAR_COLD_MC 30000
#define BAR_HOT_MC  85000

/* The verdict, in whole degrees. Under 60 the board is fine. 80 is
 * where the firmware starts capping the clock to cool the chip down,
 * and 70 is far enough ahead of that to be worth a word. These are the
 * same two numbers guard acts on, deliberately: a command that says
 * "hot" while guard is still calling it normal teaches nothing. */
#define WARM_C      60
#define HOT_C       70
#define SLOW_C      80

/* A sensor reading below -40C or above 200C is not a temperature, it is
 * a driver returning a sentinel. Trip points are the usual source:
 * a disabled one reads as INT_MAX, which would print as a limit of
 * 2147483.6 C and make the bar useless. */
#define SANE_LOW_MC  (-40000)
#define SANE_HIGH_MC 200000

#define MAX_SENSORS 48
#define MAX_ZONES   16   /* thermal zones; the loop stops at the first gap */
#define MAX_HWMON   32   /* hwmon numbers have gaps, so this is a ceiling */
#define MAX_TEMPS   16   /* tempK_input within one hwmon, K counts from 1 */
#define MAX_TRIPS   12   /* trip_point_M_temp within one zone */

#define CPUFREQ "/sys/devices/system/cpu/cpu0/cpufreq/"

typedef struct {
    char label[48];      /* what the sensor is attached to */
    char path[144];      /* the file the number was read out of */
    long mc;             /* millidegrees C */
    long limit_mc;       /* what the kernel itself calls too hot, or -1 */
} sensor_t;

static sensor_t sensors[MAX_SENSORS];
static int      nsensors = 0;

/* Colour, but only when somebody is looking at it. Redirected into a
 * file, escape codes are noise wrapped around the number a script came
 * for. Same rule as `usage`. */
static bool colour = false;

/* ── reading /sys ─────────────────────────────────────────────────── */

/* One line out of a /sys file, with the trailing newline taken off.
 * Same shape as the one in info.c; these files are all one short line. */
static bool slurp(const char *path, char *buf, size_t n)
{
    long got = proc_read(path, buf, n);
    if (got <= 0) {
        buf[0] = '\0';
        return false;
    }
    while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r' ||
                       buf[got - 1] == ' '  || buf[got - 1] == '\t'))
        buf[--got] = '\0';
    return buf[0] != '\0';
}

/* A single number out of a one-line /sys file.
 *
 * "Not there" comes back separately rather than as a value, because
 * every value one of these files can hold is also a real reading. */
static bool readnum(const char *path, long *out)
{
    char b[64];
    if (!slurp(path, b, sizeof b))
        return false;
    *out = strtol(b, NULL, 10);
    return true;
}

static bool sane(long mc)
{
    return mc > SANE_LOW_MC && mc < SANE_HIGH_MC;
}

static void add(const char *label, const char *path, long mc, long limit_mc)
{
    if (nsensors >= MAX_SENSORS)
        return;
    sensor_t *s = &sensors[nsensors++];
    strlcpy(s->label, label, sizeof s->label);
    strlcpy(s->path,  path,  sizeof s->path);
    s->mc       = mc;
    s->limit_mc = limit_mc;
}

/* The kernel's own idea of too hot for a zone: the hottest trip point
 * it has. The cooler ones are where it starts a fan or drops a clock;
 * the hottest is where it stops asking nicely. */
static long zone_limit(int z)
{
    char path[144];
    long limit = -1;

    for (int t = 0; t < MAX_TRIPS; t++) {
        long v = 0;
        snprintf(path, sizeof path,
                 "/sys/class/thermal/thermal_zone%d/trip_point_%d_temp", z, t);
        if (!readnum(path, &v) || !sane(v))
            continue;
        if (v > limit)
            limit = v;
    }
    return limit;
}

/* /sys/class/thermal - what a Pi has. The zones are numbered from zero
 * with no gaps, so the first one missing is the end of the list. */
static void scan_thermal(void)
{
    char path[144], type[48];

    for (int z = 0; z < MAX_ZONES; z++) {
        long mc = 0;
        snprintf(path, sizeof path,
                 "/sys/class/thermal/thermal_zone%d/temp", z);
        if (!readnum(path, &mc))
            break;
        if (!sane(mc))
            continue;

        char tpath[144];
        snprintf(tpath, sizeof tpath,
                 "/sys/class/thermal/thermal_zone%d/type", z);
        if (!slurp(tpath, type, sizeof type))
            snprintf(type, sizeof type, "thermal_zone%d", z);

        add(type, path, mc, zone_limit(z));
    }
}

/* /sys/class/hwmon - what a PC or an EC2 instance has.
 *
 * Both numbers here have holes in them. hwmonN is numbered in the order
 * drivers registered, so a machine can have hwmon0 and hwmon3 and
 * nothing between; tempK within one chip skips as well, because K is
 * the driver's own channel number and not every channel is populated.
 * Neither loop may stop at the first miss.
 *
 * The directory is the existence test rather than the name file: a few
 * drivers do not publish a name, and skipping those would lose real
 * sensors to save nothing. */
static void scan_hwmon(void)
{
    char path[144], name[48], label[48], shown[64];

    for (int h = 0; h < MAX_HWMON; h++) {
        snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d", h);
        if (!lp_exists(path))
            continue;

        snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/name", h);
        if (!slurp(path, name, sizeof name))
            snprintf(name, sizeof name, "hwmon%d", h);

        for (int k = 1; k <= MAX_TEMPS; k++) {
            long mc = 0;
            snprintf(path, sizeof path,
                     "/sys/class/hwmon/hwmon%d/temp%d_input", h, k);
            if (!readnum(path, &mc) || !sane(mc))
                continue;

            /* The label is what the chip calls that channel - "Package
             * id 0", "Core 3", "Composite". Without it, four readings
             * from one coretemp are four identical lines. */
            char aux[144];
            snprintf(aux, sizeof aux,
                     "/sys/class/hwmon/hwmon%d/temp%d_label", h, k);
            if (slurp(aux, label, sizeof label))
                snprintf(shown, sizeof shown, "%s %s", name, label);
            else
                snprintf(shown, sizeof shown, "%s temp%d", name, k);

            /* crit is where the hardware acts, max is where the driver
             * would like it to stop. crit first, because that is the
             * one with consequences. */
            long limit = -1, v = 0;
            snprintf(aux, sizeof aux,
                     "/sys/class/hwmon/hwmon%d/temp%d_crit", h, k);
            if (readnum(aux, &v) && sane(v)) {
                limit = v;
            } else {
                snprintf(aux, sizeof aux,
                         "/sys/class/hwmon/hwmon%d/temp%d_max", h, k);
                if (readnum(aux, &v) && sane(v))
                    limit = v;
            }

            add(shown, path, mc, limit);
        }
    }
}

static void scan(void)
{
    nsensors = 0;
    scan_thermal();
    scan_hwmon();
}

static const sensor_t *hottest(void)
{
    const sensor_t *best = NULL;
    for (int i = 0; i < nsensors; i++)
        if (!best || sensors[i].mc > best->mc)
            best = &sensors[i];
    return best;
}

/* ── printing a temperature ───────────────────────────────────────── */

/* Millidegrees to "47.6". No floating point exists here, so the whole
 * part and the tenth are two integer divisions.
 *
 * The sign needs saying out loud. C truncates towards zero, so -500
 * millidegrees gives a whole part of 0 and a remainder of -5, and the
 * obvious two-division version prints that as "0.-5". Below freezing is
 * not something a Pi does, but an ambient sensor on a PC in a cold room
 * is exactly the case that would produce it.
 *
 * The tenth is cut, not rounded: the last digit one of these sensors
 * gives is noise, and rounding 47.99 up to 48.0 would make the number
 * look steadier than the thing it measures. */
static void degrees(long mc, char *out, size_t n)
{
    long whole = mc / 1000;
    long tenth = (mc % 1000) / 100;
    if (tenth < 0)
        tenth = -tenth;
    snprintf(out, n, "%s%ld.%ld",
             (mc < 0 && whole == 0) ? "-" : "", whole, tenth);
}

static const char *verdict(long mc)
{
    long c = mc / 1000;
    if (c >= SLOW_C) return "throttling";
    if (c >= HOT_C)  return "hot";
    if (c >= WARM_C) return "warm";
    return "fine";
}

static const char *shade(long mc)
{
    if (!colour) return "";
    long c = mc / 1000;
    if (c >= HOT_C)  return "\x1b[31m";      /* red */
    if (c >= WARM_C) return "\x1b[33m";      /* yellow */
    return "\x1b[32m";                       /* green */
}

static const char *plain(void) { return colour ? "\x1b[0m" : ""; }

/* '#' and '.', not block-drawing characters, for the reason `usage`
 * gives: the console font on this board carries whatever glyphs were
 * built into it, and a bar that comes out as a row of question marks on
 * the one display the machine has is worse than a plain one. */
static void bar(long mc, long limit_mc)
{
    long hot = sane(limit_mc) && limit_mc > BAR_COLD_MC ? limit_mc : BAR_HOT_MC;

    long pct = (mc - BAR_COLD_MC) * 100 / (hot - BAR_COLD_MC);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    int filled = (int)(pct * BAR_WIDTH / 100);
    /* Anything above the cold end gets at least one mark, so a nearly
     * empty bar reads as "barely warm" rather than as a broken sensor. */
    if (filled == 0 && pct > 0)
        filled = 1;

    char buf[BAR_WIDTH + 1];
    for (int i = 0; i < BAR_WIDTH; i++)
        buf[i] = (i < filled) ? '#' : '.';
    buf[BAR_WIDTH] = '\0';

    printf("[%s%s%s]", shade(mc), buf, plain());
}

/* ── what else is going on next to the temperature ────────────────── */

/* What the CPU is running at against what it may run at. This is the
 * evidence for the verdict rather than decoration: a board that is
 * throttling is a board whose current frequency has dropped well below
 * its maximum, and seeing the two together is what makes "hot" mean
 * something. Absent on most virtual machines, which have no cpufreq
 * driver at all - then nothing is printed rather than a made-up number. */
static bool speed_text(char *out, size_t n)
{
    long cur = 0, max = 0;
    bool has_cur = readnum(CPUFREQ "scaling_cur_freq", &cur);
    bool has_max = readnum(CPUFREQ "cpuinfo_max_freq", &max);

    if (has_cur && has_max)
        snprintf(out, n, "%ld of %ld MHz", cur / 1000, max / 1000);
    else if (has_cur)
        snprintf(out, n, "%ld MHz", cur / 1000);
    else if (has_max)
        snprintf(out, n, "up to %ld MHz", max / 1000);
    else {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* The GPU firmware's record, as hex. Three paths because the kernel has
 * moved it twice; the first that exists wins. -1 means there is no Pi
 * firmware here to ask, which is not the same as "everything is fine". */
static long throttle_word(void)
{
    static const char *PATHS[] = {
        "/sys/devices/platform/soc/soc:firmware/get_throttled",
        "/sys/devices/platform/soc:firmware/get_throttled",
        "/sys/firmware/raspberrypi/get_throttled",
        NULL
    };
    char b[32];

    for (int i = 0; PATHS[i]; i++)
        if (slurp(PATHS[i], b, sizeof b))
            return strtol(b, NULL, 16);
    return -1;
}

/* Bits 0-3 are "right now", bits 16-19 the same four as "has happened
 * since this board was powered on". */
#define THR_UNDERVOLT_NOW  0x1
#define THR_CAPPED_NOW     0x2
#define THR_THROTTLED_NOW  0x4
#define THR_SOFTLIMIT_NOW  0x8
#define THR_UNDERVOLT_EVER 0x10000
#define THR_THROTTLED_EVER 0x40000

static void show_power(long thr)
{
    if (thr < 0)
        return;      /* no firmware to ask; a reassuring line would be
                      * an invention rather than a reading */

    if (thr & THR_UNDERVOLT_NOW) {
        printf("\n  UNDERVOLTAGE RIGHT NOW.\n");
        printf("  The 5V rail is sagging under load. Use a thicker, "
               "shorter cable\n");
        printf("  and a supply that can really give 2A. Nothing else on "
               "this board\n");
        printf("  reports this, and left alone it is the usual reason a "
               "card goes\n");
        printf("  bad some weeks later.\n");
    } else if (thr & THR_UNDERVOLT_EVER) {
        printf("\n  Undervoltage has happened since this board was "
               "powered on.\n");
        printf("  Not this second, but the supply is marginal: a thicker, "
               "shorter\n");
        printf("  cable and a 2A supply. This is the usual reason a card "
               "goes bad\n");
        printf("  some weeks later.\n");
    } else {
        printf("\n  power supply ok\n");
    }

    if (thr & THR_THROTTLED_NOW)
        printf("  throttled right now - the clock is being held down\n");
    else if (thr & THR_CAPPED_NOW)
        printf("  frequency capped right now\n");
    else if (thr & THR_THROTTLED_EVER)
        printf("  has been throttled at some point since power on\n");

    if (thr & THR_SOFTLIMIT_NOW)
        printf("  at the soft temperature limit\n");
}

/* ── the views ────────────────────────────────────────────────────── */

static void no_sensor(void)
{
    printf("\n  No temperature sensor on this machine.\n\n");
    printf("  That is normal in a virtual machine: the sensors belong to "
           "the\n");
    printf("  host and it does not pass them through. Nothing is wrong "
           "with the\n");
    printf("  machine - there is simply nothing here to read.\n\n");
}

static void headline(void)
{
    const sensor_t *s = hottest();
    char c[16], speed[64];

    degrees(s->mc, c, sizeof c);

    printf("\n  %6s C  ", c);
    bar(s->mc, s->limit_mc);
    printf("  %s\n", verdict(s->mc));

    /* Twelve spaces, which is where the bar above starts: two of indent,
     * six of number, " C" and two more. The label and the clock belong
     * under the bar rather than under the margin. */
    if (speed_text(speed, sizeof speed))
        printf("            %s, %s\n", s->label, speed);
    else
        printf("            %s\n", s->label);

    /* The one word above is the answer. This is the sentence behind it,
     * and only when there is something to be done about it. */
    long deg = s->mc / 1000;
    if (deg >= SLOW_C)
        printf("\n  The chip is slowing itself down to cool off. Give it "
               "air, or\n  give it less to do.\n");
    else if (deg >= HOT_C)
        printf("\n  Hot. It starts slowing itself down at %dC.\n", SLOW_C);
}

static void show_all(void)
{
    char c[16], lim[16];

    printf("\n  every sensor on this machine:\n\n");

    for (int i = 0; i < nsensors; i++) {
        const sensor_t *s = &sensors[i];
        degrees(s->mc, c, sizeof c);

        /* The path goes on its own line. Some of these are eighty
         * characters by themselves, and a table that wraps in the
         * middle of a column is harder to read than two short lines. */
        if (s->limit_mc >= 0) {
            degrees(s->limit_mc, lim, sizeof lim);
            printf("  %-28s %6s C   limit %s C\n", s->label, c, lim);
        } else {
            printf("  %-28s %6s C   no limit given\n", s->label, c);
        }
        printf("    %s\n", s->path);
    }
}

/* One line per reading, until Ctrl-C. No signal handler: the default
 * disposition already ends the program, and there is nothing to tidy up
 * on the way out.
 *
 * The clock is on each line because the only reason to watch this is to
 * see whether it climbs, and a column of numbers on its own does not
 * say over how long. -c is the exception, and it is asking for exactly
 * that column to put in a file. */
static void watch_loop(long every_ms, bool bare)
{
    for (;;) {
        scan();
        const sensor_t *s = hottest();
        if (!s) {
            /* Nothing is going to plug a thermometer into the machine
             * while this runs, so carrying on would print nothing at
             * all, forever, and read as a hang rather than an answer. */
            no_sensor();
            lp_exit(1);
        }

        char c[16], speed[64];
        degrees(s->mc, c, sizeof c);

        /* -w with -c is a column of numbers going into a file, which is
         * what you want when the question is whether it climbs over an
         * hour. Ignoring -c here instead would leave a flag that is
         * accepted and does nothing. */
        if (bare) {
            printf("%s\n", c);
            lp_sleep_ms(every_ms);
            continue;
        }

        lp_tm_t tm;
        lp_localtime(lp_time(), &tm);

        printf("  %02d:%02d:%02d  %6s C  ", tm.hour, tm.min, tm.sec, c);
        bar(s->mc, s->limit_mc);
        printf("  %-10s", verdict(s->mc));

        if (speed_text(speed, sizeof speed))
            printf(" %s", speed);

        long thr = throttle_word();
        if (thr > 0 && (thr & THR_UNDERVOLT_NOW))
            printf("  UNDERVOLTAGE");

        printf("\n");

        lp_sleep_ms(every_ms);
    }
}

static void usage(void)
{
    printf("usage: temp [-a] [-c] [-w [secs]]\n");
    printf("  how hot this board is, and whether that is a problem.\n");
    printf("\n");
    printf("  -a         every sensor, with its limit and where it "
           "was read\n");
    printf("  -c         just the number, for a script\n");
    printf("  -w [secs]  one line per reading until Ctrl-C "
           "(2 seconds by default)\n");
    printf("  -h         this\n");
}

int main(int argc, char **argv)
{
    bool all = false, bare = false, watching = false;
    long every_ms = 2000;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(a, "-a") == 0) {
            all = true;
        } else if (strcmp(a, "-c") == 0) {
            bare = true;
        } else if (strcmp(a, "-w") == 0) {
            watching = true;
            /* The interval is optional, so what follows -w is only the
             * interval when it looks like a number. Otherwise it is the
             * next option, and swallowing it would silently drop it. */
            if (i + 1 < argc && argv[i + 1][0] >= '0' &&
                                argv[i + 1][0] <= '9') {
                long secs = strtol(argv[++i], NULL, 10);
                /* "temp -w 0" would spin on the sensor as fast as the
                 * kernel can answer, which heats the board it is
                 * measuring. One second is the floor. */
                if (secs < 1)
                    secs = 1;
                every_ms = secs * 1000;
            }
        } else {
            dprintf(STDERR_FILENO, "temp: unknown option: %s\n", a);
            dprintf(STDERR_FILENO, "Try 'temp -h'.\n");
            return 1;
        }
    }

    /* Never on -c: that output goes into a script, and an escape code
     * wrapped round the number is the one thing that breaks it. */
    colour = !bare && lp_isatty(STDOUT_FILENO);

    if (watching) {
        watch_loop(every_ms, bare);
        return 0;                    /* not reached; Ctrl-C ends it */
    }

    scan();
    const sensor_t *s = hottest();

    if (!s) {
        /* Failing rather than exiting 0 so that a script can tell "the
         * board is cold" from "this machine cannot answer". */
        if (bare)
            dprintf(STDERR_FILENO,
                    "temp: no temperature sensor on this machine\n");
        else
            no_sensor();
        return 1;
    }

    if (bare) {
        char c[16];
        degrees(s->mc, c, sizeof c);
        printf("%s\n", c);
        return 0;
    }

    headline();
    if (all)
        show_all();
    show_power(throttle_word());
    printf("\n");
    return 0;
}
