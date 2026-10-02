/* sysinfo - the machine's state at a glance.
 *
 * The kernel already publishes all of this under /proc and /sys. All we do
 * is gather the scattered numbers and lay them out readably.
 */
#include "types.h"
#include "osname.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"
#include "net.h"

/* struct utsname - 65 bytes per field, 390 in total */
#define UTS_LEN       65
#define UTS_SYSNAME    0
#define UTS_NODENAME  65
#define UTS_RELEASE  130
#define UTS_VERSION  195
#define UTS_MACHINE  260
#define UTS_SIZE     390

/* The filesystem figures come from lp_statfs(), which knows the layout
 * of every architecture. They used to be read out of a byte buffer at
 * fixed offsets with a comment saying "(arm64)" - and on the Pi Zero W,
 * where the 32-bit statfs is a different structure with 32-bit fields,
 * that is what it printed: whatever those bytes happened to mean. The
 * disk line was nonsense on every 32-bit board. */

#define CPUFREQ_DIR "/sys/devices/system/cpu/cpu0/cpufreq/"

static void hr(void) { printf("─────────────────────────────────────────────\n"); }

/* Value of the first occurrence of a key in /proc/cpuinfo. The format is
 * "key : value", so the meminfo parser does not fit. */
static bool cpuinfo_field(const char *text, const char *key, char *out, size_t n)
{
    size_t klen = strlen(key);
    for (const char *p = text; *p; ) {
        if (strncmp(p, key, klen) == 0) {
            const char *c = strchr(p, ':');
            const char *e = strchr(p, '\n');
            if (c && (!e || c < e)) {
                c++;
                while (*c == ' ' || *c == '\t') c++;
                size_t len = e ? (size_t)(e - c) : strlen(c);
                if (len >= n) len = n - 1;
                memcpy(out, c, len);
                out[len] = '\0';
                return true;
            }
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return false;
}

static int count_cpus(const char *cpuinfo)
{
    int n = 0;
    for (const char *p = cpuinfo; *p; ) {
        if (strncmp(p, "processor", 9) == 0) n++;
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return n;
}

static void show_system(void)
{
    u8 uts[UTS_SIZE];
    memset(uts, 0, sizeof(uts));

    printf("\n[system]\n");
    if (lp_uname(uts) == 0) {
        printf("  kernel     %s %s\n", (char *)uts + UTS_SYSNAME,
               (char *)uts + UTS_RELEASE);
        printf("  arch       %s\n", (char *)uts + UTS_MACHINE);
        printf("  hostname   %s\n", (char *)uts + UTS_NODENAME);
    }

    /* The time. This board has no battery-backed clock, so it is often
     * wrong - and a wrong clock breaks HTTPS entirely, so it goes up front. */
    {
        s64  now   = lp_time();
        int  tzmin = 0;
        char label[32] = "UTC";

        /* /data/timezone holds "<minutes> <label>", written by date(1).
         * The label is only for display, so a file with just a number
         * still works - we fall back to spelling the offset out. */
        char tzbuf[64];
        long fd = lp_open("/data/timezone", O_RDONLY, 0);
        if (fd >= 0) {
            long n = lp_read((int)fd, tzbuf, sizeof(tzbuf) - 1);
            lp_close((int)fd);
            if (n > 0) {
                tzbuf[n] = '\0';
                tzmin = atoi(tzbuf);

                char *p = tzbuf;
                while (*p && *p != ' ' && *p != '\t') p++;
                while (*p == ' ' || *p == '\t')       p++;
                char *e = p;
                while (*e && *e != '\n' && *e != ' ') e++;
                *e = '\0';
                if (*p) strlcpy(label, p, sizeof(label));
            }
        }
        if (strcmp(label, "UTC") == 0 && tzmin != 0) {
            int a = tzmin < 0 ? -tzmin : tzmin;
            snprintf(label, sizeof(label), "UTC%c%d:%02d",
                     tzmin < 0 ? '-' : '+', a / 60, a % 60);
        }

        lp_tm_t tm;
        lp_gmtime(now + (s64)tzmin * 60, &tm);
        printf("  time       %d-%02d-%02d %02d:%02d:%02d %s",
               tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec, label);
        if (tm.year < 2020)
            printf("   <- clock is not set. Run 'ntp', or 'date -s'");
        printf("\n");
    }

    char buf[256];
    if (proc_read("/proc/uptime", buf, sizeof(buf)) > 0) {
        long secs = strtol(buf, NULL, 10);
        printf("  uptime     %ldd %ldh %ldm %lds\n",
               secs / 86400, (secs % 86400) / 3600,
               (secs % 3600) / 60, secs % 60);
    }
    if (proc_read("/proc/loadavg", buf, sizeof(buf)) > 0) {
        char *nl = strchr(buf, '\n'); if (nl) *nl = '\0';
        printf("  load       %s\n", buf);
    }
}

static void show_cpu(void)
{
    static char cpuinfo[8192];
    if (proc_read("/proc/cpuinfo", cpuinfo, sizeof(cpuinfo)) <= 0)
        return;

    printf("\n[CPU]\n");
    printf("  cores      %d\n", count_cpus(cpuinfo));

    char v[128];
    if (cpuinfo_field(cpuinfo, "CPU implementer", v, sizeof(v)))
        printf("  vendor     %s%s\n", v,
               strcmp(v, "0x41") == 0 ? "  (ARM)" : "");
    if (cpuinfo_field(cpuinfo, "CPU part", v, sizeof(v)))
        printf("  part       %s%s\n", v,
               strcmp(v, "0xd03") == 0 ? "  (Cortex-A53)" : "");
    if (cpuinfo_field(cpuinfo, "Features", v, sizeof(v)))
        printf("  features   %s\n", v);

    /* SoC temperature. The Pi reports millidegrees C in thermal_zone0. */
    char t[32];
    if (proc_read("/sys/class/thermal/thermal_zone0/temp", t, sizeof(t)) > 0) {
        long mc = strtol(t, NULL, 10);
        printf("  temp       %ld.%ld C\n", mc / 1000, (mc % 1000) / 100);
    }

    /* What it is actually running at, and who is deciding.
     * "powersave" here normally means guard is holding it down because
     * the board is hot or the power supply is not keeping up. */
    char f[32], g[32];
    long khz = 0;
    if (proc_read(CPUFREQ_DIR "scaling_cur_freq", f, sizeof(f)) > 0)
        khz = strtol(f, NULL, 10);
    if (proc_read(CPUFREQ_DIR "scaling_governor", g, sizeof(g)) > 0) {
        char *nl = strchr(g, '\n');
        if (nl) *nl = '\0';
        if (khz > 0)
            printf("  speed      %ld MHz  (%s)\n", khz / 1000, g);
        else
            printf("  speed      %s\n", g);
    }
}

/* ── Health ───────────────────────────────────────────────────────────
 * The GPU firmware records every time it has had to step in to keep the
 * board running. Undervoltage in particular leaves no other trace: the
 * board does not crash, it corrupts the card quietly weeks later. If
 * anything here is not "ok", it is worth acting on.
 *
 * The file does not exist on a virtual machine, and this section is then
 * left out entirely rather than printing four reassuring lies. */
static const char *THROTTLE_PATHS[] = {
    "/sys/devices/platform/soc/soc:firmware/get_throttled",
    "/sys/devices/platform/soc:firmware/get_throttled",
    "/sys/firmware/raspberrypi/get_throttled",
    NULL
};

static void show_health(void)
{
    char buf[32];
    long thr = -1;

    for (int i = 0; THROTTLE_PATHS[i]; i++)
        if (proc_read(THROTTLE_PATHS[i], buf, sizeof(buf)) > 0) {
            thr = strtol(buf, NULL, 16);
            break;
        }

    long boots = 0;
    if (proc_read("/data/boot_count", buf, sizeof(buf)) > 0)
        boots = strtol(buf, NULL, 10);

    /* Nothing to say: no firmware to ask, and no failed boot behind us.
     * A count of 1 is this boot, which has simply not been up for five
     * minutes yet. */
    if (thr < 0 && boots <= 1)
        return;

    printf("\n[health]\n");

    if (thr >= 0) {
        /* Bits 0-3 are "right now", bits 16-19 the same four as "has
         * happened at some point since this board was powered on". */
        printf("  power      %s\n",
               (thr & 0x1)     ? "** UNDERVOLTAGE NOW - fix the supply"
             : (thr & 0x10000) ? "undervoltage has happened since boot"
                               : "ok");
        printf("  throttling %s\n",
               (thr & 0x4)     ? "** throttled right now"
             : (thr & 0x2)     ? "frequency capped right now"
             : (thr & 0x40000) ? "has been throttled since boot"
                               : "none");
        if (thr & 0x8)
            printf("  heat       ** at the soft temperature limit\n");
    }

    /* 1 is the current boot, which has simply not been up for five
     * minutes yet. From 2 up, a previous boot really did fail. */
    if (boots > 1)
        printf("  boots      %ld in a row did not last 5 minutes\n",
               boots - 1);
}

static void show_memory(void)
{
    static char mem[8192];
    if (proc_read("/proc/meminfo", mem, sizeof(mem)) <= 0)
        return;

    long total = proc_find_kv(mem, "MemTotal");
    long avail = proc_find_kv(mem, "MemAvailable");
    long swt   = proc_find_kv(mem, "SwapTotal");
    long swf   = proc_find_kv(mem, "SwapFree");

    printf("\n[memory]\n");
    if (total > 0) {
        long used = total - (avail > 0 ? avail : 0);
        printf("  total      %4ld MB\n", total / 1024);
        printf("  used       %4ld MB  (%ld%%)\n", used / 1024, used * 100 / total);
        printf("  free       %4ld MB\n", avail / 1024);
    }
    if (swt > 0) {
        printf("  zram swap  %4ld MB total, %ld MB used\n",
               swt / 1024, (swt - swf) / 1024);
        /* How well it is compressing */
        char n1[32], n2[32];
        if (proc_read("/sys/block/zram0/orig_data_size", n1, sizeof(n1)) > 0 &&
            proc_read("/sys/block/zram0/compr_data_size", n2, sizeof(n2)) > 0) {
            long orig = strtol(n1, NULL, 10), compr = strtol(n2, NULL, 10);
            if (orig > 0 && compr > 0)
                printf("  compressed %ld%%  (%ld KB -> %ld KB)\n",
                       compr * 100 / orig, orig / 1024, compr / 1024);
        }
    } else {
        printf("  zram swap  none\n");
    }
}

static void show_one_fs(const char *path, const char *label)
{
    lp_statfs_t fs;
    if (lp_statfs(path, &fs) < 0)
        return;

    u64 bs     = fs.bsize;
    u64 blocks = fs.blocks;
    u64 avail  = fs.bavail;
    if (bs == 0 || blocks == 0)
        return;

    u64 total_mb = blocks * bs / 1048576;
    u64 avail_mb = avail  * bs / 1048576;
    u64 used_mb  = total_mb - avail_mb;

    printf("  %-10s %5lu MB total, %lu MB used (%lu%%), %lu MB free\n",
           label, (unsigned long)total_mb, (unsigned long)used_mb,
           total_mb ? (unsigned long)(used_mb * 100 / total_mb) : 0UL,
           (unsigned long)avail_mb);
}

static void show_storage(void)
{
    printf("\n[storage]\n");
    show_one_fs("/",     "/ (RAM)");
    show_one_fs("/data", "/data");
}

static void show_network(void)
{
    static const char *IFACES[] = { "wlan0", "eth0", "usb0", "lo", NULL };

    printf("\n[network]\n");
    bool any = false;

    for (int i = 0; IFACES[i]; i++) {
        u32 addr = 0;
        if (net_get_addr(IFACES[i], &addr) < 0)
            continue;

        char ip[16];
        ipv4_format(addr, ip);

        u8 mac[6];
        bool has_mac = net_if_hwaddr(IFACES[i], mac) == 0;

        printf("  %-6s %-15s %s", IFACES[i], ip,
               net_if_is_up(IFACES[i]) ? "UP" : "DOWN");
        if (has_mac)
            printf("  %02x:%02x:%02x:%02x:%02x:%02x",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        printf("\n");
        any = true;
    }

    if (!any)
        printf("  no interface has an address\n");

    char dns[128];
    if (proc_read("/etc/resolv.conf", dns, sizeof(dns)) > 0) {
        char *p = strstr(dns, "nameserver ");
        if (p) {
            p += 11;
            char *nl = strchr(p, '\n'); if (nl) *nl = '\0';
            printf("  DNS    %s\n", p);
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("\n");
    hr();
    printf("  " LP_OS_NAME " OS  system information\n");
    hr();

    show_system();
    show_cpu();
    show_health();
    show_memory();
    show_storage();
    show_network();

    printf("\n");
    return 0;
}
