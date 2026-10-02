/* info - every fact about this machine, in one place.
 *
 *   info              everything, grouped and paged
 *   info -s           the dozen lines that answer most questions
 *   info <section>    one of: os cpu mem disk net kernel project
 *
 * ── Why this exists ──
 * The facts were all there and all scattered. `uname` had the kernel,
 * `free` the memory, `disk` the hardware, `storage` the partitions,
 * `net` the interfaces, `sysinfo` a summary of about half of it. Six
 * commands, six layouts, and nobody filing a bug report ran all six.
 * This is the one word to type before asking a question, and the one
 * output to paste under it.
 *
 * ── Where the facts come from ──
 * The kernel has already written every one of them down under /proc and
 * /sys. Nothing here runs another program: shelling out to `df` and
 * `ifconfig` would mean this command reporting on a machine where those
 * binaries might be the thing that is broken, and it would make the
 * output depend on their formatting rather than on the files. The one
 * exception is the firewall, which nftables publishes nowhere in /proc -
 * so that is one netlink question, asked directly.
 *
 * ── Absent is a fact too ──
 * A virtual machine has no temperature sensor, no cpufreq and no Pi
 * firmware to ask about undervoltage. Printing 0 C, 0 MHz and "power ok"
 * there is worse than printing nothing, because all three read as
 * measurements. Every section says "no temperature sensor here" in the
 * shape of a sentence instead, so a person reading a pasted output can
 * tell "we did not look" from "we looked and it was zero".
 *
 * ── Paging ──
 * The whole thing is about seventy lines and a framebuffer console has
 * no scrollback: what scrolls off the top is gone, not "further up". So
 * the long form goes through page_line and stops at the bottom of each
 * screen. A single section and -s are short enough to print straight,
 * and go through unpaged so they can be piped without a pager in the
 * way. (page_line already passes everything straight through when the
 * output is redirected; not starting the pager at all just keeps the
 * short forms honest about it.)
 */
#include "types.h"
#include "osname.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"
#include "disk.h"

/* The version of the userland, as opposed to the kernel's release.
 *
 * /etc/osversion wins when it is there, so an image can be stamped at
 * build time without recompiling this; the Makefile can define
 * LP_OS_VERSION; and failing both there is a constant here, because a
 * bug report that says which OS but not which version of it is a bug
 * report nobody can act on. */
#ifndef LP_OS_VERSION
#  define LP_OS_VERSION "1.0"
#endif

/* Where the source is: given when building (make LP_PROJECT_URL=...),
 * and left out of the output when it was not. */
#ifndef LP_PROJECT_URL
#  define LP_PROJECT_URL ""
#endif

/* The nftables table firewall(1) creates. Named here rather than
 * guessed: if that name ever changes, this reports "off" for a firewall
 * that is on, which is the one wrong answer that matters. */
#define FW_TABLE "lpzero"

/* ── output ──────────────────────────────────────────────────────────
 *
 * Every line is formatted into one buffer and handed to emit(), which
 * either prints it or gives it to the pager. Once the reader has
 * pressed q, emit() drops everything: the sections after it still run
 * their reads, which costs a few file opens and keeps the control flow
 * to a single flag instead of an error return threaded through twenty
 * functions. */

static bool paged;
static bool quit;
static char lb[256];

/* Two buffers, not one per file.
 *
 * /proc/cpuinfo is the big one - a couple of kilobytes per core - and
 * giving cpuinfo, meminfo, /proc/stat, /proc/cmdline and /proc/mounts a
 * static buffer each cost 63KB of memory for the life of every run of
 * this command, on a board that has 512MB for everything. So there is
 * one buffer for whatever file is being read at the moment: each
 * section reads its file, takes what it wants out of it, and is
 * finished with the buffer before the next section starts.
 *
 * /proc/mounts gets its own, because root_fs() is called from inside
 * sections that are holding another file in `scratch` at the time. */
static char scratch[12288];
static char mountbuf[6144];

static void emit(const char *s)
{
    if (quit)
        return;
    if (!paged) {
        printf("%s\n", s);
        return;
    }
    if (!page_line(s))
        quit = true;
}

#define P(...) do { snprintf(lb, sizeof lb, __VA_ARGS__); emit(lb); } while (0)

/* The label column, wide enough for the longest label there is
 * ("architecture"). One place, so a label added later that overflows it
 * shifts every value in the file rather than one line in one section. */
#define L "  %-12s "

/* ── reading files ───────────────────────────────────────────────── */

/* A whole /proc or /sys file with the trailing whitespace taken off.
 * false when it is not there, which is how "this machine has no such
 * sensor" arrives. */
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
 * The absence of the file is returned separately rather than as a
 * sentinel value. It was -1 at first, and -1 is a perfectly ordinary
 * number for one of these files to hold: with a missing thermal zone
 * the temperature then printed as "-0.0 C", which reads as a
 * measurement of a cold board rather than as no sensor at all. */
static bool readnum(const char *path, long *out)
{
    char b[64];
    if (!slurp(path, b, sizeof b))
        return false;
    *out = strtol(b, NULL, 10);
    return true;
}

/* The value of a "key : value" line, as /proc/cpuinfo writes them.
 *
 * The key must start the line and the next non-blank after it must be
 * the colon. Both halves matter: without the line anchor "model" would
 * match inside "cpu model", and without the colon check "model" would
 * match the line "model name" and return the wrong string. x86 cpuinfo
 * has both of those keys, so this is not hypothetical. */
static bool cpu_field(const char *text, const char *key, char *out, size_t n)
{
    size_t klen = strlen(key);
    for (const char *p = text; *p; ) {
        if (strncmp(p, key, klen) == 0) {
            const char *c = p + klen;
            while (*c == ' ' || *c == '\t') c++;
            if (*c == ':') {
                c++;
                while (*c == ' ' || *c == '\t') c++;
                const char *e = strchr(c, '\n');
                size_t len = e ? (size_t)(e - c) : strlen(c);
                if (len >= n) len = n - 1;
                memcpy(out, c, len);
                out[len] = '\0';
                return out[0] != '\0';
            }
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return false;
}

/* ── small formatters ────────────────────────────────────────────── */

/* "3d 4h 12m" - the units that are zero at the front are left out, so a
 * board that has been up eleven minutes does not read "0d 0h 11m". */
static void duration(long secs, char *out, size_t n)
{
    long d = secs / 86400, h = (secs % 86400) / 3600;
    long m = (secs % 3600) / 60, s = secs % 60;

    if (d)      snprintf(out, n, "%ldd %ldh %ldm", d, h, m);
    else if (h) snprintf(out, n, "%ldh %ldm", h, m);
    else if (m) snprintf(out, n, "%ldm %lds", m, s);
    else        snprintf(out, n, "%lds", s);
}

/* Kilobytes as /proc/meminfo counts them, in megabytes. */
static long mb(long kb) { return kb / 1024; }

/* A percentage without floating point, which does not exist here.
 * Guards the divide: an empty filesystem is 0% used, not a crash. */
static long pct(u64 part, u64 whole)
{
    return whole ? (long)((part * 100) / whole) : 0;
}

/* ── facts several sections want ─────────────────────────────────── */

#define UTS_FIELD 65
#define UTS_SYSNAME  (0 * UTS_FIELD)
#define UTS_NODENAME (1 * UTS_FIELD)
#define UTS_RELEASE  (2 * UTS_FIELD)
#define UTS_VERSION  (3 * UTS_FIELD)
#define UTS_MACHINE  (4 * UTS_FIELD)

static char uts[UTS_FIELD * 6];

static const char *uts_at(int off)
{
    return uts[0] ? uts + off : "unknown";
}

/* aarch64 -> arm64, x86_64 -> amd64: the names the images are built and
 * distributed under, which is what somebody downloading one has in
 * front of them. */
static const char *arch_name(void)
{
    const char *m = uts_at(UTS_MACHINE);
    if (strcmp(m, "aarch64") == 0) return "arm64";
    if (strcmp(m, "x86_64")  == 0) return "amd64";
    return m;
}

/* The board, from the device tree. Only a real Pi has one; a PC and a
 * virtual machine do not, and get an empty string. */
static bool board_model(char *out, size_t n)
{
    return slurp("/proc/device-tree/model", out, n) ||
           slurp("/sys/firmware/devicetree/base/model", out, n);
}

/* One line of /proc/mounts, found either by its device or by where it
 * is mounted, with any of the three fields handed back.
 *
 * Three questions here need this file - what the root filesystem is,
 * what kind of filesystem a partition holds, and what /data is on - and
 * they were three separate parsers before. A mount point never contains
 * a space: the kernel writes one as \040, which is exactly so that
 * splitting on spaces is safe. */
static bool mounts_lookup(const char *key, bool by_point,
                          char *dev, size_t dn,
                          char *point, size_t pn,
                          char *type, size_t tn)
{
    if (dev)   dev[0]   = '\0';
    if (point) point[0] = '\0';
    if (type)  type[0]  = '\0';

    if (proc_read("/proc/mounts", mountbuf, sizeof mountbuf) <= 0)
        return false;

    for (char *line = mountbuf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char *f[3];
        int nf = 0;
        for (char *q = line; *q && nf < 3; ) {
            while (*q == ' ') q++;
            if (!*q) break;
            f[nf++] = q;
            while (*q && *q != ' ') q++;
            if (*q) *q++ = '\0';
        }
        if (nf == 3 && strcmp(by_point ? f[1] : f[0], key) == 0) {
            if (dev)   strlcpy(dev,   f[0], dn);
            if (point) strlcpy(point, f[1], pn);
            if (type)  strlcpy(type,  f[2], tn);
            return true;
        }
        line = nl ? nl + 1 : NULL;
    }
    return false;
}

/* "Is the root the RAM image" is the question behind several answers on
 * this system - nothing written to / survives - so it is worth being
 * exact. The initramfs is unpacked into rootfs, which reports itself as
 * rootfs or tmpfs with no device behind it; anything else is a real
 * partition somebody could have booted from instead. */
static bool root_fs(char *dev, size_t dn, char *type, size_t tn)
{
    return mounts_lookup("/", true, dev, dn, NULL, 0, type, tn);
}

static bool root_is_ram(const char *dev, const char *type)
{
    return strcmp(type, "rootfs") == 0 || strcmp(type, "tmpfs") == 0 ||
           strcmp(dev, "rootfs") == 0  || strcmp(dev, "none") == 0;
}

/* ── operating system ────────────────────────────────────────────── */

static void os_name(char *out, size_t n)
{
    if (!slurp("/etc/osname", out, n))
        strlcpy(out, LP_OS_NAME, n);
}

static void os_version(char *out, size_t n)
{
    if (!slurp("/etc/osversion", out, n))
        strlcpy(out, LP_OS_VERSION, n);
}

/* Which image this is. Decided when it was compiled, not guessed now:
 * the arm64 build is the Pi image and the amd64 build is the PC one,
 * and a Pi image running under emulation is still the Pi image. The
 * board underneath is reported separately, on its own line, so the two
 * cannot be confused when they disagree. */
static const char *image_kind(void)
{
#if defined(__aarch64__)
    return "the Pi image (arm64)";
#elif defined(__x86_64__)
    return "the PC image (amd64)";
#else
    return "an image for an architecture this command does not know";
#endif
}

static void show_os(void)
{
    char name[64], ver[32];
    os_name(name, sizeof name);
    os_version(ver, sizeof ver);

    P(L "%s %s", "name", name, ver);
    P(L "%s", "built as", image_kind());
    P(L "%s   (uname calls it %s)", "architecture", arch_name(),
      uts_at(UTS_MACHINE));

    char model[96];
    if (board_model(model, sizeof model))
        P(L "%s", "running on", model);
    else
        P(L "%s", "running on", "no device tree here - a PC or a virtual"
                                " machine, not a Pi");

    char host[64];
    if (slurp("/proc/sys/kernel/hostname", host, sizeof host))
        P(L "%s", "hostname", host);

    char buf[256];
    if (slurp("/proc/uptime", buf, sizeof buf)) {
        char d[32];
        duration(strtol(buf, NULL, 10), d, sizeof d);
        P(L "%s", "uptime", d);
    }

    if (slurp("/proc/loadavg", buf, sizeof buf)) {
        /* The first three numbers are the averages; what follows is the
         * running/total process count and the last pid, which belong to
         * `ps`, not here. */
        int spaces = 0;
        for (char *p = buf; *p; p++)
            if (*p == ' ' && ++spaces == 3) { *p = '\0'; break; }
        P(L "%s   (1, 5 and 15 minutes)", "load", buf);
    }

    /* When the kernel started, from /proc/stat's btime. Printed in UTC
     * because that is what the clock here keeps; date(1) owns the local
     * offset and this command does not read it. */
    if (proc_read("/proc/stat", scratch, sizeof scratch) > 0) {
        const char *b = strstr(scratch, "btime ");
        if (b) {
            lp_tm_t tm;
            lp_gmtime(strtol(b + 6, NULL, 10), &tm);
            P(L "%d-%02d-%02d %02d:%02d:%02d UTC", "booted at",
              tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec);
        }
    }

    /* How long the boot took, as far as /proc can say: field 22 of
     * /proc/1/stat is when init was started, in 100ths of a second since
     * the kernel began. That is the kernel's own share of the boot -
     * decompressing itself, finding the hardware, unpacking the RAM
     * image. What /etc/rc then spent is not recorded anywhere, so it is
     * not claimed here.
     *
     * Parsing starts after the LAST ')': field 2 is the process name in
     * parentheses and a process may put anything inside them. */
    char pstat[512];
    if (slurp("/proc/1/stat", pstat, sizeof pstat)) {
        char *p = strrchr(pstat, ')');
        if (p) {
            long v = 0;
            int  field = 2;      /* field 2 is the name, in the brackets */

            /* Stepping over whole words rather than calling strtol on
             * each. Field 3 is the state letter, and strtol on "S"
             * returns 0 without moving the pointer - which is not a
             * parse error you can see, it is a loop that never ends. */
            for (p++; *p && field < 22; ) {
                while (*p == ' ') p++;
                if (!*p) break;
                char *tok = p;
                while (*p && *p != ' ') p++;
                if (++field == 22)
                    v = strtol(tok, NULL, 10);
            }
            if (field == 22)
                P(L "%ld.%02lds to reach init"
                    "   (what /etc/rc then took is not recorded)",
                  "boot took", v / 100, v % 100);
        }
    }
}

/* ── kernel ──────────────────────────────────────────────────────── */

static void show_kernel(void)
{
    P(L "%s %s", "kernel", uts_at(UTS_SYSNAME), uts_at(UTS_RELEASE));
    P(L "%s", "build", uts_at(UTS_VERSION));
    P(L "%s", "machine", uts_at(UTS_MACHINE));

    char dev[64], type[32];
    if (root_fs(dev, sizeof dev, type, sizeof type)) {
        if (root_is_ram(dev, type))
            P(L "%s", "root", "the RAM image - unpacked from the kernel at"
                              " boot, nothing written to / survives");
        else
            P(L "%s on %s - NOT the RAM image", "root", type, dev);
    } else {
        P(L "%s", "root", "cannot tell - /proc/mounts is unreadable");
    }

    if (proc_read("/proc/cmdline", scratch, sizeof scratch) > 0) {
        char *nl = strchr(scratch, '\n');
        if (nl) *nl = '\0';

        /* Nearly always wider than a console. Wrapped at a space rather
         * than truncated, because the argument that explains a boot
         * that went wrong is as likely to be the last one as the first.
         * A single word longer than the width is printed whole and
         * allowed to wrap itself - cutting a kernel argument in half
         * would make it unreadable and unsearchable. */
        char *p = scratch;
        const char *label = "cmdline";
        while (*p) {
            if (strlen(p) <= 62) {
                P(L "%s", label, p);
                break;
            }
            char *cut = NULL;
            for (char *q = p; (size_t)(q - p) < 62 && *q; q++)
                if (*q == ' ') cut = q;
            if (!cut) {
                /* One word wider than the column. Let it wrap itself
                 * and carry on after it - breaking out here instead
                 * dropped every argument that followed. */
                cut = strchr(p, ' ');
                if (!cut) {
                    P(L "%s", label, p);
                    break;
                }
            }
            *cut = '\0';
            P(L "%s", label, p);
            label = "";
            p = cut + 1;
            while (*p == ' ') p++;
        }
    }
}

/* ── cpu ─────────────────────────────────────────────────────────── */

#define CPUFREQ "/sys/devices/system/cpu/cpu0/cpufreq/"

/* The ARM cores this system is ever built for. /proc/cpuinfo on arm64
 * gives a part number and no name at all, and "0xd03" answers nobody's
 * question. */
static const char *arm_part(const char *part)
{
    if (strcmp(part, "0xd03") == 0) return "Cortex-A53";
    if (strcmp(part, "0xd08") == 0) return "Cortex-A72";
    if (strcmp(part, "0xd0b") == 0) return "Cortex-A76";
    if (strcmp(part, "0xb76") == 0) return "ARM1176";
    return part;
}

/* Is `word` one of the space-separated words in `list`? Comparing the
 * whole word matters: a plain strstr for "avx" is also true of "avx2"
 * and "avx512f", so every machine with AVX2 would claim plain AVX. */
static bool word_in(const char *list, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = list; *p; ) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        if ((size_t)(p - start) == n && strncmp(start, word, n) == 0)
            return true;
    }
    return false;
}

/* x86 publishes about ninety flags. Listing them all costs six lines of
 * console for facts nobody reads; these are the ones that change what
 * will run. ARM's Features line is short enough to print whole. */
static const char *X86_FLAGS[] = {
    "sse2", "sse4_2", "avx", "avx2", "avx512f", "aes", "sha_ni",
    "rdrand", "hypervisor", NULL
};

static void show_cpu(void)
{
    if (proc_read("/proc/cpuinfo", scratch, sizeof scratch) <= 0) {
        P("  /proc/cpuinfo cannot be read - this kernel has no procfs?");
        return;
    }
    const char *ci = scratch;

    int cores = 0;
    for (const char *p = ci; *p; ) {
        if (strncmp(p, "processor", 9) == 0) cores++;
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }

    /* Big enough for the x86 flags line, which is a little over a
     * thousand characters and holds the interesting names near its end -
     * truncate it at 512 and this machine loses aes, avx512f and
     * sha_ni and quietly reports that it has none of them. */
    char v[2048];
    if (cpu_field(ci, "model name", v, sizeof v))
        P(L "%s", "model", v);
    else if (cpu_field(ci, "CPU part", v, sizeof v))
        P(L "%s%s", "model", arm_part(v),
          strcmp(v, arm_part(v)) == 0 ? "  (unrecognised ARM part number)" : "");
    else
        P(L "%s", "model", "not named in /proc/cpuinfo");

    P(L "%d   (%s)", "cores", cores, arch_name());

    if (cpu_field(ci, "Features", v, sizeof v)) {
        P(L "%s", "features", v);
    } else if (cpu_field(ci, "flags", v, sizeof v)) {
        char list[128];
        list[0] = '\0';
        for (int i = 0; X86_FLAGS[i]; i++)
            if (word_in(v, X86_FLAGS[i])) {
                if (list[0]) strlcat(list, " ", sizeof list);
                strlcat(list, X86_FLAGS[i], sizeof list);
            }
        /* On its own line, because saying that the list is filtered
         * takes more room than fits beside it on an 80-column console -
         * and leaving it unsaid would make a filtered list look like
         * the whole one. */
        P(L "%s", "features", list[0] ? list : "none of the usual ones");
        P(L "%s", "", "(only the ones that change what will run)");
    }

    /* What it is running at now, what it may run at, and who decides.
     * "powersave" on this board normally means guard has stepped in
     * because the supply or the temperature could not keep up. */
    long cur = 0, max = 0;
    bool has_cur = readnum(CPUFREQ "scaling_cur_freq", &cur);
    bool has_max = readnum(CPUFREQ "cpuinfo_max_freq", &max);
    char gov[32];
    if (has_cur || has_max) {
        if (has_cur && has_max)
            P(L "%ld MHz now, %ld MHz maximum", "speed", cur / 1000, max / 1000);
        else if (has_cur)
            P(L "%ld MHz now", "speed", cur / 1000);
        else
            P(L "%ld MHz maximum", "speed", max / 1000);
        if (slurp(CPUFREQ "scaling_governor", gov, sizeof gov))
            P(L "%s", "governor", gov);
    } else if (cpu_field(ci, "cpu MHz", v, sizeof v)) {
        /* No cpufreq driver, which is normal on a virtual machine. The
         * number cpuinfo prints there is the nominal clock, not a
         * measurement of anything, and saying so costs one line.
         *
         * It is printed as "2100.000" and there is no floating point
         * anywhere in this system, so the fraction is cut off rather
         * than passed through as the only decimal point in the
         * output. */
        char *dot = strchr(v, '.');
        if (dot) *dot = '\0';
        P(L "%s MHz nominal - no cpufreq driver, so this is not measured",
          "speed", v);
    } else {
        P(L "%s", "speed", "no cpufreq here and cpuinfo does not say");
    }

    /* Millidegrees C. thermal_zone0 is the SoC on a Pi; its "type" file
     * says what the sensor is actually attached to, which is worth
     * printing rather than assuming. */
    long mc = 0;
    if (readnum("/sys/class/thermal/thermal_zone0/temp", &mc)) {
        char kind[32];
        if (!slurp("/sys/class/thermal/thermal_zone0/type", kind, sizeof kind))
            strlcpy(kind, "thermal_zone0", sizeof kind);
        P(L "%ld.%ld C   (%s)", "temperature", mc / 1000, (mc % 1000) / 100, kind);
    } else {
        P(L "%s", "temperature", "no temperature sensor here");
    }

    /* The GPU firmware's record of every time it has had to step in.
     * Undervoltage leaves no other trace: the board does not crash, it
     * corrupts the card quietly some weeks later. Bits 0-3 are "right
     * now", bits 16-19 the same four as "has happened since power on". */
    static const char *THROTTLE[] = {
        "/sys/devices/platform/soc/soc:firmware/get_throttled",
        "/sys/devices/platform/soc:firmware/get_throttled",
        "/sys/firmware/raspberrypi/get_throttled",
        NULL
    };
    char tb[32];
    long thr = -1;
    for (int i = 0; THROTTLE[i]; i++)
        if (slurp(THROTTLE[i], tb, sizeof tb)) {
            thr = strtol(tb, NULL, 16);
            break;
        }

    if (thr < 0) {
        P(L "%s", "throttling", "no Pi firmware to ask - this board keeps"
                                " no undervoltage record");
    } else {
        P(L "%s", "power",
          (thr & 0x1)     ? "UNDERVOLTAGE NOW - fix the supply; nothing"
                            " else here is reliable"
        : (thr & 0x10000) ? "undervoltage has happened since this board"
                            " was powered on"
                          : "ok");
        P(L "%s", "throttling",
          (thr & 0x4)     ? "throttled right now"
        : (thr & 0x2)     ? "frequency capped right now"
        : (thr & 0x40000) ? "has been throttled since power on"
                          : "none");
        if (thr & 0x8)
            P(L "%s", "heat", "at the soft temperature limit");
    }
}

/* ── memory ──────────────────────────────────────────────────────── */

static long kv(const char *text, const char *key)
{
    long v = proc_find_kv(text, key);
    return v < 0 ? 0 : v;
}

static void show_memory(void)
{
    if (proc_read("/proc/meminfo", scratch, sizeof scratch) <= 0) {
        P("  /proc/meminfo cannot be read");
        return;
    }
    const char *mi = scratch;

    long total = kv(mi, "MemTotal");
    long avail = kv(mi, "MemAvailable");
    long freem = kv(mi, "MemFree");
    long buf   = kv(mi, "Buffers");
    long cache = kv(mi, "Cached");
    long stot  = kv(mi, "SwapTotal");
    long sfree = kv(mi, "SwapFree");

    P(L "%ld MB", "total", mb(total));
    /* "used" against MemAvailable rather than MemFree. The page cache
     * takes whatever is going and gives it back on demand, so MemFree on
     * a healthy machine is near zero and reads as a machine about to
     * die. Available is what a new program could actually have. */
    long used = total - avail;
    if (used < 0) used = 0;               /* never seen, but the cast below
                                           * would turn it into 16 exabytes */
    P(L "%ld MB   (%ld%%)", "used", mb(used), pct((u64)used, (u64)total));
    P(L "%ld MB   what a new program can have without swapping",
      "available", mb(avail));
    P(L "%ld MB   nothing at all is using it", "free", mb(freem));
    P(L "%ld MB", "buffers", mb(buf));
    P(L "%ld MB   given back the moment something needs it", "cached", mb(cache));

    if (stot > 0) {
        char algo[32];
        long disksize = 0;
        if (readnum("/sys/block/zram0/disksize", &disksize) && disksize > 0) {
            /* comp_algorithm lists every algorithm this kernel has and
             * puts the one actually in use in [brackets] - printing
             * the line whole would claim the swap is compressed with
             * four algorithms at once. */
            if (!slurp("/sys/block/zram0/comp_algorithm", algo, sizeof algo)) {
                strlcpy(algo, "zram", sizeof algo);
            } else {
                char *open  = strchr(algo, '[');
                char *close = open ? strchr(open, ']') : NULL;
                if (open && close) {
                    char active[32];
                    *close = '\0';
                    strlcpy(active, open + 1, sizeof active);
                    strlcpy(algo, active, sizeof algo);
                }
            }
            P(L "%ld MB total, %ld MB used   (zram0 in RAM)",
              "swap", mb(stot), mb(stot - sfree));

            /* mm_stat is one line of numbers: original bytes stored,
             * compressed bytes, then memory actually taken. The old
             * orig_data_size and compr_data_size files that sysinfo(1)
             * reads were removed from the kernel years ago, so on 6.x
             * this is the only place the ratio still exists. */
            char mm[128];
            if (slurp("/sys/block/zram0/mm_stat", mm, sizeof mm)) {
                char *p = mm;
                long orig = strtol(p, &p, 10);
                long comp = strtol(p, &p, 10);
                if (orig > 0 && comp > 0)
                    P(L "%ld KB stored in %ld KB - %ld%% of its size, using %s",
                      "compressed", orig / 1024, comp / 1024,
                      pct((u64)comp, (u64)orig), algo);
                else
                    P(L "%s", "compressed", "nothing swapped out yet");
            }
        } else {
            P(L "%ld MB total, %ld MB used   (not zram)",
              "swap", mb(stot), mb(stot - sfree));
        }
    } else {
        P(L "%s", "swap", "none - `zram on` makes some, compressed, in RAM");
    }

    /* What the root filesystem itself costs. It is unpacked into RAM at
     * boot and never given back, so it is subtracted from every number
     * above whether or not anyone has noticed. */
    char dev[64], type[32];
    if (root_fs(dev, sizeof dev, type, sizeof type) && root_is_ram(dev, type)) {
        u64 fre = 0, tot = 0;
        if (lp_fs_space("/", &fre, &tot) >= 0 && tot > 0)
            P(L "%lu MB   the / filesystem lives in RAM and this is its cost",
              "ram root", (unsigned long)((tot - fre) / 1048576));
        else
            P(L "%s", "ram root", "/ is in RAM but its size cannot be read");
    } else {
        P(L "/ is %s on %s - not a RAM filesystem, so it costs none",
          "ram root", type, dev);
    }
}

/* ── storage ─────────────────────────────────────────────────────── */

/* What kind of thing this disk is. The kernel does not say in one
 * place, so it is three cheap tests: the device name settles SD cards,
 * NVMe and virtio, and for everything else the sysfs path a disk hangs
 * off names the bus it is on. */
static const char *disk_kind(const blk_t *d)
{
    if (strncmp(d->name, "mmcblk", 6) == 0) return "SD card";
    if (strncmp(d->name, "nvme", 4)   == 0) return "NVMe";
    if (strncmp(d->name, "vd", 2)     == 0) return "virtual disk";

    char link[256], path[64];
    snprintf(path, sizeof path, "/sys/block/%s", d->name);
    long n = lp_readlink(path, link, sizeof link - 1);
    if (n > 0) {
        link[n] = '\0';
        if (strstr(link, "/usb")) return "USB";
        if (strstr(link, "virtio")) return "virtual disk";
    }
    return d->removable ? "removable" : "disk";
}

static void volume_line(const blk_t *b)
{
    char size[12];
    disk_human(b->bytes, size, sizeof size);

    /* The filesystem type. /proc/mounts is asked first and the
     * superblock second, not the other way round: disk_identify only
     * knows the handful of superblock magics this system writes, so a
     * mounted squashfs or an XFS somebody plugged in reads as "no
     * filesystem" there while the kernel has already named it. */
    char fs[16];
    if (!b->mount[0] ||
        !mounts_lookup(b->path, false, NULL, 0, NULL, 0, fs, sizeof fs))
        strlcpy(fs, b->fs, sizeof fs);

    char full[40];
    if (b->mount[0]) {
        u64 fre = 0, tot = 0;
        if (lp_fs_space(b->mount, &fre, &tot) >= 0 && tot > 0) {
            char fh[12];
            disk_human(fre, fh, sizeof fh);
            snprintf(full, sizeof full, "%ld%% used, %s free",
                     pct(tot - fre, tot), fh);
        } else {
            strlcpy(full, "mounted, size unreadable", sizeof full);
        }
    } else if (!b->fs[0]) {
        /* Not "empty": disk_identify only knows the superblock magics
         * this system writes, so this covers a blank partition and an
         * XFS one alike. `storage format` makes an ext4 one either way. */
        strlcpy(full, "blank, or a filesystem we do not know", sizeof full);
    } else {
        strlcpy(full, "not mounted", sizeof full);
    }

    P("  %-16s %5s %-8s %-8s %-14s %s",
      b->path, size,
      fs[0]       ? fs       : "-",
      b->label[0] ? b->label : "-",
      b->mount[0] ? b->mount : "-",
      full);
}

static void show_storage(void)
{
    blk_t disks[DISK_MAX];
    int nd = disk_list(disks, DISK_MAX);

    if (nd <= 0) {
        P("  no block device at all - everything here is in RAM");
        return;
    }

    /* The hardware first and the filesystems second, rather than the
     * partitions of each disk indented under it. The two have different
     * columns, so interleaving them meant a heading above every disk or
     * a table whose headings did not line up with half its rows. The
     * names carry the nesting anyway: mmcblk0p2 is plainly part of
     * mmcblk0. */
    P("  %-16s %5s  %-13s %s", "disk", "size", "kind", "model");
    for (int i = 0; i < nd; i++) {
        char size[12];
        disk_human(disks[i].bytes, size, sizeof size);
        if (disks[i].model[0])
            P("  %-16s %5s  %-13s %s", disks[i].path, size,
              disk_kind(&disks[i]), disks[i].model);
        else
            P("  %-16s %5s  %s", disks[i].path, size, disk_kind(&disks[i]));
    }

    emit("");
    P("  %-16s %5s %-8s %-8s %-14s %s",
      "volume", "size", "fs", "label", "mount", "how full");
    for (int i = 0; i < nd; i++) {
        blk_t parts[DISK_PARTS];
        int np = disk_parts(disks[i].path, parts, DISK_PARTS);
        if (np <= 0)
            volume_line(&disks[i]);        /* no table: the disk is the volume */
        else
            for (int j = 0; j < np; j++)
                volume_line(&parts[j]);
    }
    emit("");

    /* /data is the only thing on this machine meant to survive a
     * reboot, so which device is behind it is the fact worth stating
     * outright rather than leaving to be read out of the table above.
     *
     * The device name is matched back against the disks so the kind can
     * be named too: "SD card" is the difference between a partition
     * that will wear out in two years of logging and one that will not. */
    char ddev[64], dtype[16];
    if (mounts_lookup("/data", true, ddev, sizeof ddev, NULL, 0,
                      dtype, sizeof dtype)) {
        const char *kind = "";
        for (int i = 0; i < nd; i++) {
            char whole[48];
            if (strcmp(ddev, disks[i].path) == 0 ||
                (disk_whole(ddev, whole, sizeof whole) &&
                 strcmp(whole, disks[i].path) == 0))
                kind = disk_kind(&disks[i]);
        }
        P("  /data is %s on %s%s%s", dtype, ddev,
          kind[0] ? ", " : "", kind);
    } else {
        char rdev[64], rtype[16];
        bool ram = root_fs(rdev, sizeof rdev, rtype, sizeof rtype) &&
                   root_is_ram(rdev, rtype);
        P("  /data is not a filesystem of its own - it is part of /%s",
          ram ? ", which is in RAM, so nothing written there survives"
                " a reboot"
              : ", which is on disk");
    }
}

/* ── network ─────────────────────────────────────────────────────── */

/* The prefix length of this interface's network - the 24 in
 * 192.168.0.42/24 - from the on-link route it has in /proc/net/route:
 * the line with no gateway and a destination that is not the default.
 * The kernel publishes a netmask nowhere else short of opening a socket
 * and asking by ioctl.
 *
 * A prefix length rather than the mask spelled out. "192.168.0.42 /
 * 255.255.255.0" is thirty characters and pushed the columns beside it
 * off an 80-column console; the same fact written /24 is fifteen.
 *
 * -1 when the interface has no such route, which is normal for loopback
 * and for an interface that has an address and no network yet. */
static int prefix_of(const char *iface)
{
    long fd = lp_open("/proc/net/route", O_RDONLY, 0);
    if (fd < 0)
        return -1;

    char line[512];
    int  bits = -1;
    readline((int)fd, line, sizeof line);          /* the header */

    while (bits < 0 && readline((int)fd, line, sizeof line) >= 0) {
        char *f[9];
        int nf = 0;
        for (char *p = line; *p && nf < 9; ) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            f[nf++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
        if (nf < 8 || strcmp(f[0], iface) != 0)
            continue;
        if (strcmp(f[2], "00000000") != 0)         /* has a gateway: not on-link */
            continue;
        if (strcmp(f[1], "00000000") == 0)         /* the default route */
            continue;

        /* Counting the bits that are set, not the leading ones. A mask
         * is contiguous in every network anyone will attach this to,
         * and a count is right for both while a scan from the top has
         * to know which end the bytes are at. */
        u32 mask = (u32)strtol(f[7], NULL, 16);
        bits = 0;
        while (mask) { bits += (int)(mask & 1); mask >>= 1; }
    }
    lp_close((int)fd);
    return bits;
}

static u32 default_gw(char *iface, size_t n)
{
    long fd = lp_open("/proc/net/route", O_RDONLY, 0);
    if (fd < 0)
        return 0;

    char line[512];
    u32 gw = 0;
    if (n) iface[0] = '\0';
    readline((int)fd, line, sizeof line);

    while (!gw && readline((int)fd, line, sizeof line) >= 0) {
        char *f[4];
        int nf = 0;
        for (char *p = line; *p && nf < 4; ) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            f[nf++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
        if (nf < 3 || strcmp(f[1], "00000000") != 0)
            continue;
        gw = (u32)strtol(f[2], NULL, 16);
        if (n) strlcpy(iface, f[0], n);
    }
    lp_close((int)fd);
    return gw;
}

/* The network wpa_supplicant was told to join. There is no way to ask
 * the driver what it actually associated with: this kernel is built
 * without the wireless extensions, so SIOCGIWESSID does not exist, and
 * the control socket needs the wpa_cli protocol. So this is the
 * configured name, and the carrier flag beside it says whether the link
 * really came up - which together answer the question. */
static bool wifi_ssid(char *out, size_t n)
{
    static const char *CONF[] = {
        "/etc/wpa.conf", "/boot/wpa_supplicant.conf",
        "/data/wpa_supplicant.conf", NULL
    };
    char buf[2048];
    for (int i = 0; CONF[i]; i++) {
        if (proc_read(CONF[i], buf, sizeof buf) <= 0)
            continue;
        char *s = strstr(buf, "ssid=\"");
        if (!s)
            continue;
        s += 6;
        char *e = strchr(s, '"');
        if (!e)
            continue;
        *e = '\0';
        strlcpy(out, s, n);
        return out[0] != '\0';
    }
    return false;
}

/* ── is the firewall on ──────────────────────────────────────────────
 *
 * nftables publishes nothing under /proc, so this is the one fact that
 * cannot be read from a file. Asking is one netlink message: does the
 * table firewall(1) creates exist. Building it by hand rather than
 * linking anything - nft is a parser and two libraries, megabytes to
 * ask a yes/no question.
 *
 *   nlmsghdr   16 bytes   len, type, flags, seq, pid
 *   nfgenmsg    4 bytes   family, version, res_id
 *   attribute  12 bytes   len, type, "lpzero\0" padded to 4
 *
 * Returns 1 on, 0 off, -1 could not tell.
 */
#define NL_FAMILY        16      /* AF_NETLINK */
#define NL_NETFILTER     12
#define NFT_GETTABLE     ((u16)((10 << 8) | 1))
#define NFT_NEWTABLE     ((u16)((10 << 8) | 0))
#define NLM_REQUEST      0x001
#define NLM_ACK          0x004
#define NLMSG_IS_ERROR   2
#define NFPROTO_INET     1
#define NFTA_TABLE_NAME  1

static int firewall_on(void)
{
    struct { u16 family; u16 pad; u32 pid; u32 groups; } sa;
    memset(&sa, 0, sizeof sa);
    sa.family = NL_FAMILY;

    long fd = lp_socket(NL_FAMILY, SOCK_RAW, NL_NETFILTER);
    if (fd < 0)
        return -1;
    if (lp_bind((int)fd, &sa, sizeof sa) < 0) {
        lp_close((int)fd);
        return -1;
    }
    /* Without a receive timeout a kernel that never answers - one built
     * without nftables - would hang this command for ever. */
    s64 tv[2] = { 2, 0 };
    lp_setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof tv);

    /* Declared as u32 arrays, not u8 ones. Netlink is a stream of
     * 4-byte-aligned records read back through u32 and u16 pointers,
     * and a char array carries no such alignment guarantee. */
    u32 words[8];
    u8 *msg = (u8 *)words;
    memset(msg, 0, sizeof words);
    *(u32 *)(msg + 0)  = (u32)sizeof words;
    *(u16 *)(msg + 4)  = NFT_GETTABLE;
    *(u16 *)(msg + 6)  = NLM_REQUEST | NLM_ACK;
    *(u32 *)(msg + 8)  = 1;                       /* sequence */
    msg[16] = NFPROTO_INET;
    msg[17] = 0;                                  /* nfnetlink v0 */
    *(u16 *)(msg + 20) = (u16)(4 + sizeof FW_TABLE);
    *(u16 *)(msg + 22) = NFTA_TABLE_NAME;
    memcpy(msg + 24, FW_TABLE, sizeof FW_TABLE);

    if (lp_sendto((int)fd, msg, sizeof words, 0, &sa, sizeof sa) < 0) {
        lp_close((int)fd);
        return -1;
    }

    u32 reply[1024];
    u8 *buf = (u8 *)reply;
    long n = lp_recvfrom((int)fd, buf, sizeof reply, 0, 0, 0);
    lp_close((int)fd);
    if (n <= 0)
        return -1;                                /* the two-second timeout */

    /* The kernel answers a successful GETTABLE with the table itself and
     * then an acknowledgement; a missing table is a plain error record.
     * Anything else - EPERM without root, ENOPROTOOPT on a kernel built
     * without nftables - is "cannot tell", not "off". Reporting a
     * firewall that is on as off is the one wrong answer here that
     * would get somebody to open a port they did not need to. */
    u32 off = 0;
    while (off + 16 <= (u32)n) {
        u32 len  = *(u32 *)(buf + off);
        u16 type = *(u16 *)(buf + off + 4);
        if (len < 16 || off + len > (u32)n)
            break;
        if (type == NFT_NEWTABLE)
            return 1;
        if (type == NLMSG_IS_ERROR) {
            if (off + 20 > (u32)n)
                break;
            s32 err = *(s32 *)(buf + off + 16);
            if (err == 0)  return 1;              /* a plain ack: it exists */
            if (err == -2) return 0;              /* ENOENT: no such table */
            return -1;
        }
        off += (len + 3) & ~3u;
    }
    return -1;
}

static void show_network(void)
{
    long fd = lp_open("/proc/net/dev", O_RDONLY, 0);
    if (fd < 0) {
        P("  /proc/net/dev cannot be read - this kernel has no networking");
        return;
    }

    char line[512];
    int  lineno = 0;
    int  seen = 0;

    while (readline((int)fd, line, sizeof line) >= 0) {
        if (++lineno <= 2)
            continue;                              /* two heading lines */

        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = '\0';
        char *name = line;
        while (*name == ' ') name++;
        seen++;

        char addr[24] = "no address";
        u32 a = 0;
        if (net_get_addr(name, &a) >= 0 && a != 0) {
            char ip[16];
            ipv4_format(a, ip);
            int bits = prefix_of(name);
            if (bits >= 0)
                snprintf(addr, sizeof addr, "%s/%d", ip, bits);
            else
                strlcpy(addr, ip, sizeof addr);
        }

        /* operstate is the driver's own word for the link. carrier says
         * whether there is anything on the other end of it, and reading
         * it fails outright on an interface that is down - which is not
         * an error, it is the answer. */
        char oper[24];
        char path[80];
        snprintf(path, sizeof path, "/sys/class/net/%s/operstate", name);
        if (!slurp(path, oper, sizeof oper))
            strlcpy(oper, net_if_is_up(name) ? "up" : "down", sizeof oper);
        snprintf(path, sizeof path, "/sys/class/net/%s/carrier", name);
        long carrier = 0;
        bool has_carrier = readnum(path, &carrier);

        char mac[24] = "no hardware address";
        snprintf(path, sizeof path, "/sys/class/net/%s/address", name);
        slurp(path, mac, sizeof mac);

        P("  %-8s %-19s %-8s %s", name, addr, oper, mac);
        /* Reading carrier fails outright on an interface that is down.
         * That is not an error to report, it is the answer. */
        P("  %-8s link %s", "",
          !has_carrier ? "unknown - the interface is down"
        : carrier == 1 ? "up"
                       : "down - nothing on the other end of it");

        /* /proc/net/dev: eight receive counters then eight transmit
         * ones. bytes, packets, errors and dropped are the first four
         * of each. */
        u64 c[16];
        int nc = 0;
        for (char *p = colon + 1; *p && nc < 16; ) {
            while (*p == ' ') p++;
            if (!*p) break;
            c[nc++] = (u64)strtol(p, &p, 10);
        }
        if (nc >= 12) {
            char rb[12], tb[12];
            disk_human(c[0], rb, sizeof rb);
            disk_human(c[8], tb, sizeof tb);
            P("  %-8s received %6s in %lu packets, %lu errors, %lu dropped",
              "", rb, (unsigned long)c[1], (unsigned long)c[2],
              (unsigned long)c[3]);
            P("  %-8s sent     %6s in %lu packets, %lu errors, %lu dropped",
              "", tb, (unsigned long)c[9], (unsigned long)c[10],
              (unsigned long)c[11]);
        }
    }
    lp_close((int)fd);

    if (!seen)
        P("  the kernel found no network interface at all");

    char gwif[IFNAMSIZ];
    u32 gw = default_gw(gwif, sizeof gwif);
    if (gw) {
        char g[16];
        ipv4_format(gw, g);
        P(L "%s via %s", "gateway", g, gwif);
    } else {
        P(L "%s", "gateway", "none - nothing beyond this network can be"
                             " reached");
    }

    char resolv[512];
    if (proc_read("/etc/resolv.conf", resolv, sizeof resolv) > 0) {
        char list[128];
        list[0] = '\0';
        for (char *p = resolv; p && *p; ) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            if (strncmp(p, "nameserver ", 11) == 0) {
                if (list[0]) strlcat(list, ", ", sizeof list);
                strlcat(list, p + 11, sizeof list);
            }
            p = nl ? nl + 1 : NULL;
        }
        P(L "%s", "nameserver",
          list[0] ? list : "none in /etc/resolv.conf - names cannot be"
                           " looked up");
    } else {
        P(L "%s", "nameserver", "no /etc/resolv.conf - names cannot be"
                                " looked up");
    }

    char ssid[64];
    if (wifi_ssid(ssid, sizeof ssid)) {
        long carrier = 0;
        bool has = readnum("/sys/class/net/wlan0/carrier", &carrier);
        P(L "set up for \"%s\", %s", "wifi", ssid,
          !has          ? "but wlan0 is down or not present"
        : carrier == 1  ? "and associated"
                        : "but not associated - wrong password, or out of range");
    } else {
        P(L "%s", "wifi", "no network configured - `net wifi <name> <password>`");
    }

    int fw = firewall_on();
    P(L "%s", "firewall",
      fw == 1 ? "on - `firewall` lists the rules and what each has turned away"
    : fw == 0 ? "off - every port is open; `firewall on` closes them"
              : "cannot tell - the kernel would not answer. Try `firewall`");
}

/* ── project ─────────────────────────────────────────────────────── */

static void show_project(void)
{
    char name[64], ver[32];
    os_name(name, sizeof name);
    os_version(ver, sizeof ver);

    P("  %s %s is a Linux distribution written from nothing: its own C", name, ver);
    P("  library, its own init, its own shell and about 120 commands. No");
    P("  busybox, no glibc, no code from another userland.");
    P("");
    P(L "%s", "written by", "Claude Code (Anthropic)");
    P(L "%s", "licence", "MIT - do what you like with it, keep the notice");
    if (LP_PROJECT_URL[0])
        P(L "%s", "source", LP_PROJECT_URL);
    P(L "%s", "bug report", "paste the output of `info` under it");
}

/* ── the short form ──────────────────────────────────────────────── */

/* A dozen lines: what somebody wants when they have just booted the
 * image and are working out whether it came up right. Every number here
 * appears in the long form too; nothing is computed twice differently. */
static void show_short(void)
{
    char name[64], ver[32], buf[256];

    os_name(name, sizeof name);
    os_version(ver, sizeof ver);
    P("%s %s   %s", name, ver, image_kind());

    char dev[64], type[32];
    bool ram = root_fs(dev, sizeof dev, type, sizeof type) &&
               root_is_ram(dev, type);
    P(L "%s %s   root %s", "kernel", uts_at(UTS_SYSNAME), uts_at(UTS_RELEASE),
      ram ? "is the RAM image" : "is on disk");

    if (slurp("/proc/uptime", buf, sizeof buf)) {
        char d[32], load[64] = "";
        duration(strtol(buf, NULL, 10), d, sizeof d);
        if (slurp("/proc/loadavg", load, sizeof load)) {
            int spaces = 0;
            for (char *p = load; *p; p++)
                if (*p == ' ' && ++spaces == 3) { *p = '\0'; break; }
        }
        P(L "%s,  load %s", "uptime", d, load);
    }

    if (proc_read("/proc/cpuinfo", scratch, sizeof scratch) > 0) {
        const char *ci = scratch;
        int cores = 0;
        for (const char *p = ci; *p; ) {
            if (strncmp(p, "processor", 9) == 0) cores++;
            while (*p && *p != '\n') p++;
            if (*p) p++;
        }
        char model[128];
        if (!cpu_field(ci, "model name", model, sizeof model)) {
            char part[32];
            strlcpy(model, cpu_field(ci, "CPU part", part, sizeof part)
                           ? arm_part(part) : "unknown", sizeof model);
        }
        long cur = 0, mc = 0;
        bool has_cur = readnum(CPUFREQ "scaling_cur_freq", &cur);
        bool has_mc  = readnum("/sys/class/thermal/thermal_zone0/temp", &mc);

        char extra[48];
        if (has_cur)
            snprintf(extra, sizeof extra, "%ld MHz", cur / 1000);
        else if (has_mc)
            strlcpy(extra, "no cpufreq driver", sizeof extra);
        else
            strlcpy(extra, "no cpufreq or sensor here", sizeof extra);

        P(L "%s x%d, %s", "cpu", model, cores, extra);

        /* Its own line when there is a sensor, rather than tacked onto
         * the end of the cpu line. On a board in a case, in a cupboard,
         * under load, this is the number that explains why it got slow -
         * and it was the easiest thing on the screen to miss.
         *
         * No line at all when there is no sensor: the cpu line above
         * already says so, and a summary is a dozen lines or it is not
         * a summary. */
        if (has_mc) {
            char kind[64];
            if (!slurp("/sys/class/thermal/thermal_zone0/type",
                       kind, sizeof kind))
                strlcpy(kind, "thermal_zone0", sizeof kind);
            P(L "%ld.%ld C   (%s)", "temperature",
              mc / 1000, (mc % 1000) / 100, kind);
        }
    }

    if (proc_read("/proc/meminfo", scratch, sizeof scratch) > 0) {
        const char *mi = scratch;
        long total = kv(mi, "MemTotal"), avail = kv(mi, "MemAvailable");
        long stot = kv(mi, "SwapTotal"), sfree = kv(mi, "SwapFree");
        P(L "%ld MB total, %ld MB used, %ld MB available",
          "memory", mb(total), mb(total - avail), mb(avail));
        if (stot > 0)
            P(L "%ld MB, %ld MB used", "swap", mb(stot), mb(stot - sfree));
        else
            P(L "%s", "swap", "none");
    }

    u64 fre = 0, tot = 0;
    if (lp_fs_space("/", &fre, &tot) >= 0 && tot > 0) {
        char t[12];
        disk_human(tot, t, sizeof t);
        P(L "%ld%% of %s used%s", "/", pct(tot - fre, tot), t,
          ram ? "   (in RAM)" : "");
    }
    /* lp_fs_space answers for whatever filesystem holds the path, so on
     * a machine where /data is an ordinary directory it would happily
     * report the root filesystem's free space as /data's. Asking
     * /proc/mounts whether /data is a mount point of its own comes
     * first, because "your data partition has 2GB free" is exactly the
     * wrong thing to tell somebody who does not have one. */
    if (!mounts_lookup("/data", true, NULL, 0, NULL, 0, NULL, 0)) {
        P(L "%s", "/data", "not a filesystem of its own - see `storage`");
    } else if (lp_fs_space("/data", &fre, &tot) >= 0 && tot > 0) {
        char t[12], f[12];
        disk_human(tot, t, sizeof t);
        disk_human(fre, f, sizeof f);
        P(L "%s free of %s, %ld%% used", "/data", f, t, pct(tot - fre, tot));
    } else {
        P(L "%s", "/data", "mounted, but its size cannot be read");
    }

    char gwif[IFNAMSIZ], g[16] = "none";
    u32 gw = default_gw(gwif, sizeof gwif);
    if (gw) ipv4_format(gw, g);

    /* The interface with the default route is the one that matters; if
     * there is no route at all, say which interfaces have an address so
     * the next question has somewhere to start. */
    if (gw) {
        u32 a = 0;
        char ip[16] = "no address";
        if (net_get_addr(gwif, &a) >= 0 && a != 0)
            ipv4_format(a, ip);
        P(L "%s %s, gateway %s", "network", gwif, ip, g);
    } else {
        P(L "%s", "network", "no default route - `net test` finds where it"
                             " stops");
    }

    int fw = firewall_on();
    P(L "%s", "firewall",
      fw == 1 ? "on" : fw == 0 ? "off - every port is open"
                               : "cannot tell");
    if (LP_PROJECT_URL[0])
        P(L "%s   MIT", "project", LP_PROJECT_URL);
    else
        P(L "%s", "project", "MIT");
}

/* ── sections ────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *title;
    void (*fn)(void);
} section_t;

static const section_t SECTIONS[] = {
    { "os",      "operating system", show_os      },
    { "kernel",  "kernel",           show_kernel  },
    { "cpu",     "cpu",              show_cpu     },
    { "mem",     "memory",           show_memory  },
    { "disk",    "storage",          show_storage },
    { "net",     "network",          show_network },
    { "project", "project",          show_project },
};
#define NSECTIONS ((int)(sizeof SECTIONS / sizeof SECTIONS[0]))

/* The names people type are not always the ones in the table. Someone
 * who has read the headings will type "memory" and "storage"; someone
 * who has read the usage will type "mem" and "disk". Both work. */
static const char *canonical(const char *s)
{
    if (strcmp(s, "memory")  == 0) return "mem";
    if (strcmp(s, "ram")     == 0) return "mem";
    if (strcmp(s, "storage") == 0) return "disk";
    if (strcmp(s, "network") == 0) return "net";
    if (strcmp(s, "system")  == 0) return "os";
    return s;
}

static void usage(void)
{
    printf("usage:\n");
    printf("  info              everything about this machine, paged\n");
    printf("  info -s           the short version\n");
    printf("  info <section>    one of: os kernel cpu mem disk net project\n");
    printf("\n");
    printf("Everything is read from /proc and /sys as it is now. Where a\n");
    printf("fact is not available on this hardware it says so rather than\n");
    printf("printing a zero.\n");
    printf("\n");
    printf("Filing a bug: paste `info` under it.\n");
    if (LP_PROJECT_URL[0])
        printf("  %s\n", LP_PROJECT_URL);
}

int main(int argc, char **argv)
{
    memset(uts, 0, sizeof uts);
    lp_uname(uts);

    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0) {
            usage();
            return 0;
        }
        if (strcmp(argv[1], "-s") == 0) {
            show_short();
            return 0;
        }

        const char *want = canonical(argv[1]);
        for (int i = 0; i < NSECTIONS; i++)
            if (strcmp(SECTIONS[i].name, want) == 0) {
                printf("%s\n", SECTIONS[i].title);
                SECTIONS[i].fn();
                return 0;
            }

        dprintf(STDERR_FILENO, "info: there is no \"%s\" section\n", argv[1]);
        dprintf(STDERR_FILENO,
                "  try: os kernel cpu mem disk net project, or `info` for"
                " all of them\n");
        return 2;
    }

    paged = true;
    page_begin();
    for (int i = 0; i < NSECTIONS && !quit; i++) {
        if (i) emit("");
        emit(SECTIONS[i].title);
        SECTIONS[i].fn();
    }
    page_end();
    return 0;
}
