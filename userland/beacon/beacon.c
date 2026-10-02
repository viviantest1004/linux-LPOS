/* beacon - report how the machine is doing, to somewhere that is not it.
 *
 *   beacon              send one report and print it
 *   beacon -d           daemon: report every interval, forever
 *   beacon -n           print the report, send nothing
 *
 * ── Why this exists ──
 * The claim this board is built around is that it stays up for months
 * with nobody near it. There is no way to check that claim from here:
 * finding out whether it is alive means logging in, and a machine you
 * cannot log into is exactly the one you wanted to know about. Silence
 * reads the same as health right up until you need it.
 *
 * So the board says how it is, on a schedule, to a URL. If the reports
 * stop, something is wrong - and that is the only signal that works when
 * the failure is "it went away". Anything that pushes the other way -
 * you polling it - cannot tell "down" from "unreachable from where you
 * happen to be standing".
 *
 * ── Where the reports go ──
 * /boot/beacon.conf names a URL. Any service that expects a periodic
 * ping and complains when one is missed will do - that is a common
 * enough shape that several free ones exist, and none of this is
 * specific to any of them. The report is JSON, sent as the POST body,
 * which those services keep as the ping's note.
 *
 * With no URL configured it still runs and still writes
 * /data/log/status.json, because half the value is having the numbers
 * at all. That file is also what makes the long-run claim checkable
 * later: it is the same record, kept locally, whether or not anything
 * was listening.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

#define CONF_BOOT  "/boot/beacon.conf"
#define CONF_DATA  "/data/beacon.conf"
#define STATUS     "/data/log/status.json"
#define HISTORY    "/data/log/status.log"

#define DEFAULT_INTERVAL 300       /* five minutes */

static char g_url[512];
static char g_name[64];
static long g_interval = DEFAULT_INTERVAL;

/* ── settings ────────────────────────────────────────────────────── */

static void read_conf(const char *path)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return;

    char line[600];
    /* >= 0, not > 0. readline returns 0 for an empty line and -1 only
     * at the end, so "> 0" stopped at the first blank line - and a
     * blank line between sections is the obvious way to write beacon.conf.
     * Everything after it was dropped in silence, with the rules that
     * did apply reported as a success. */
    while (readline((int)fd, line, sizeof line) >= 0) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char *key = line;
        while (*key == ' ' || *key == '\t') key++;
        if (!*key) continue;

        char *val = key;
        while (*val && *val != ' ' && *val != '\t') val++;
        if (!*val) continue;
        *val++ = '\0';
        while (*val == ' ' || *val == '\t') val++;

        char *end = val + strlen(val);
        while (end > val && (end[-1] == ' ' || end[-1] == '\r' ||
                             end[-1] == '\t')) *--end = '\0';
        if (!*val) continue;

        if (strcmp(key, "url") == 0)
            strlcpy(g_url, val, sizeof g_url);
        else if (strcmp(key, "name") == 0)
            strlcpy(g_name, val, sizeof g_name);
        else if (strcmp(key, "interval") == 0) {
            g_interval = atoi(val);
            /* A report every few seconds is not monitoring, it is a
             * denial of service against whoever agreed to receive it. */
            if (g_interval < 60) g_interval = 60;
        } else {
            dprintf(STDERR_FILENO,
                    "beacon: %s: ignoring \"%s\"\n", path, key);
        }
    }
    lp_close((int)fd);
}

/* ── the numbers ─────────────────────────────────────────────────── */

static long meminfo(const char *key)
{
    static char buf[4096];
    if (proc_read("/proc/meminfo", buf, sizeof buf) <= 0)
        return -1;
    return proc_find_kv(buf, key);
}

/* Milli-degrees from the thermal zone, or -1 where there is no sensor -
 * which is every virtual machine, and is not a fault. */
static long temperature(void)
{
    static const char *paths[] = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/devices/virtual/thermal/thermal_zone0/temp",
        0
    };
    char buf[32];
    for (int i = 0; paths[i]; i++)
        if (proc_read(paths[i], buf, sizeof buf) > 0)
            return strtol(buf, NULL, 10);
    return -1;
}

/* The firmware's throttling word. Bit 0 is under-voltage now, bit 16 is
 * under-voltage since boot - the second one is the interesting half,
 * because a supply that dipped once at 3am leaves no other trace. */
static u32 throttled(void)
{
    static const char *paths[] = {
        "/sys/devices/platform/soc/soc:firmware/get_throttled",
        "/sys/devices/platform/soc/soc:firmware/rpi_firmware/get_throttled",
        "/sys/class/hwmon/hwmon0/device/get_throttled",
        0
    };
    char buf[32];
    for (int i = 0; paths[i]; i++)
        if (proc_read(paths[i], buf, sizeof buf) > 0)
            return (u32)strtol(buf, NULL, 0);
    return 0;
}

static void first_address(char *out, size_t n)
{
    strlcpy(out, "none", n);
    static const char *ifs[] = { "wlan0", "eth0", "usb0", 0 };
    for (int i = 0; ifs[i]; i++) {
        u32 a = 0;
        if (net_get_addr(ifs[i], &a) >= 0 && a != 0) {
            char buf[20];
            ipv4_format(a, buf);
            snprintf(out, n, "%s:%s", ifs[i], buf);
            return;
        }
    }
}

static long uptime_seconds(void)
{
    char buf[64];
    if (proc_read("/proc/uptime", buf, sizeof buf) <= 0)
        return -1;
    return strtol(buf, NULL, 10);
}

static void load_average(char *out, size_t n)
{
    char buf[64];
    strlcpy(out, "0", n);
    if (proc_read("/proc/loadavg", buf, sizeof buf) <= 0)
        return;
    char *sp = strchr(buf, ' ');
    if (sp) *sp = '\0';
    strlcpy(out, buf, n);
}

static long boot_count(void)
{
    char buf[32];
    if (proc_read("/data/boot_count", buf, sizeof buf) <= 0)
        return -1;
    return strtol(buf, NULL, 10);
}

/* ── the report ──────────────────────────────────────────────────── */

static void build(char *out, size_t n)
{
    long up      = uptime_seconds();
    long mem_tot = meminfo("MemTotal");
    long mem_av  = meminfo("MemAvailable");
    long swp_tot = meminfo("SwapTotal");
    long swp_fr  = meminfo("SwapFree");
    long temp    = temperature();
    u32  thr     = throttled();

    u64 data_free = 0, data_total = 0;
    lp_fs_space("/data", &data_free, &data_total);

    char addr[64], load[16];
    first_address(addr, sizeof addr);
    load_average(load, sizeof load);

    char host[64] = "";
    proc_read("/proc/sys/kernel/hostname", host, sizeof host);
    for (int i = 0; host[i]; i++)
        if (host[i] == '\n') { host[i] = '\0'; break; }

    /* Every field is one a person would look at when asking "is it all
     * right", and nothing else. A report nobody reads is a report that
     * gets turned off. */
    snprintf(out, n,
        "{"
        "\"name\":\"%s\","
        "\"uptime_s\":%ld,"
        "\"boots\":%ld,"
        "\"load\":%s,"
        "\"mem_total_kb\":%ld,"
        "\"mem_available_kb\":%ld,"
        "\"swap_used_kb\":%ld,"
        "\"data_free_mb\":%lu,"
        "\"data_total_mb\":%lu,"
        "\"temp_c\":%ld,"
        "\"throttled\":\"0x%x\","
        "\"undervolted_now\":%s,"
        "\"undervolted_ever\":%s,"
        "\"address\":\"%s\","
        "\"time\":%ld,"
        "\"clock_set\":%s"
        "}",
        g_name[0] ? g_name : (host[0] ? host : "lpzero"),
        up, boot_count(), load,
        mem_tot, mem_av,
        (swp_tot > 0 && swp_fr >= 0) ? swp_tot - swp_fr : 0,
        (unsigned long)(data_free / 1048576),
        (unsigned long)(data_total / 1048576),
        temp >= 0 ? temp / 1000 : -1,
        thr,
        (thr & 0x1)     ? "true" : "false",
        (thr & 0x10000) ? "true" : "false",
        addr,
        (long)lp_time(),
        /* This board has no battery-backed clock, so before ntp runs
         * the time is seconds-since-boot dressed up as 1970. Saying so
         * costs a field and stops the timestamp being read as real. */
        lp_time() > 1600000000 ? "true" : "false");
}

static void human(const char *json)
{
    /* The JSON is what goes over the wire; this is for a person at the
     * console, who wants the same facts without counting brackets. */
    long up      = uptime_seconds();
    long mem_tot = meminfo("MemTotal");
    long mem_av  = meminfo("MemAvailable");
    long temp    = temperature();
    u32  thr     = throttled();
    u64  df = 0, dt = 0;
    lp_fs_space("/data", &df, &dt);

    char addr[64], load[16];
    first_address(addr, sizeof addr);
    load_average(load, sizeof load);

    printf("  up          %ld days %ld hours %ld minutes\n",
           up / 86400, (up % 86400) / 3600, (up % 3600) / 60);
    printf("  boots       %ld\n", boot_count());
    printf("  load        %s\n", load);
    if (mem_tot > 0)
        printf("  memory      %ld of %ld MB free\n",
               mem_av / 1024, mem_tot / 1024);
    if (dt)
        printf("  /data       %lu of %lu MB free\n",
               (unsigned long)(df / 1048576), (unsigned long)(dt / 1048576));
    if (temp >= 0)
        printf("  temperature %ld C\n", temp / 1000);
    else
        printf("  temperature no sensor on this board\n");
    printf("  power       %s\n",
           (thr & 0x10000) ? "the supply has dipped since boot - check it"
                           : "no undervoltage recorded");
    printf("  address     %s\n", addr);
    (void)json;
}

/* ── sending and keeping ─────────────────────────────────────────── */

static void keep_locally(const char *json)
{
    lp_mkdir("/data/log", 0755);

    long fd = lp_open(STATUS, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        dprintf((int)fd, "%s\n", json);
        lp_close((int)fd);
    }

    /* One line per report, appended. This is the record that makes a
     * claim about months of uptime checkable afterwards, rather than
     * something to be taken on trust. guard drops this file first when
     * /data fills, which is the right thing to lose. */
    fd = lp_open(HISTORY, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dprintf((int)fd, "%s\n", json);
        lp_close((int)fd);
    }
}

static int send_report(bool quiet)
{
    char json[1024];
    build(json, sizeof json);
    keep_locally(json);

    if (!g_url[0]) {
        if (!quiet) {
            printf("beacon: no url set, so nothing was sent.\n");
            printf("        %s is up to date.\n", STATUS);
            printf("        Put a line in %s:\n", CONF_BOOT);
            printf("          url https://your.monitor/ping/xxxx\n");
        }
        return 1;
    }

    long rc = net_http_post(g_url, json, NULL);
    if (rc < 0) {
        if (!quiet)
            dprintf(STDERR_FILENO,
                    "beacon: could not reach %s\n"
                    "        'net test' says which step is failing.\n", g_url);
        return 1;
    }
    if (!quiet)
        printf("beacon: reported to %s\n", g_url);
    return 0;
}

static void usage(void)
{
    printf("beacon - report how this machine is doing, to somewhere else\n\n");
    printf("  beacon           send one report, and print it\n");
    printf("  beacon -n        print it, send nothing\n");
    printf("  beacon -d        keep reporting, every interval\n\n");
    printf("Settings, in %s (or %s):\n\n", CONF_BOOT, CONF_DATA);
    printf("  url       https://your.monitor/ping/xxxx\n");
    printf("  interval  300            seconds, minimum 60\n");
    printf("  name      kitchen-pi     what to call this board\n\n");
    printf("The report is JSON in the POST body. Any service that expects\n");
    printf("a periodic ping and complains when one is missed will do.\n\n");
    printf("With no url it still runs and still writes %s,\n", STATUS);
    printf("which is the record that makes a claim about months of uptime\n");
    printf("checkable later instead of something to take on trust.\n");
}

int main(int argc, char **argv)
{
    bool daemon = false, dry = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0)      daemon = true;
        else if (strcmp(argv[i], "-n") == 0) dry = true;
        else if (strcmp(argv[i], "-h") == 0 ||
                 strcmp(argv[i], "--help") == 0) { usage(); return 0; }
        else {
            dprintf(STDERR_FILENO, "beacon: no idea what \"%s\" means\n",
                    argv[i]);
            return 2;
        }
    }

    /* /boot first, so the URL can be changed with a card reader when the
     * machine is not reachable - which is the situation this exists for. */
    read_conf(CONF_BOOT);
    if (!g_url[0])
        read_conf(CONF_DATA);

    if (dry) {
        char json[1024];
        build(json, sizeof json);
        keep_locally(json);
        human(json);
        printf("\n%s\n", json);
        return 0;
    }

    if (!daemon) {
        char json[1024];
        build(json, sizeof json);
        human(json);
        printf("\n");
        return send_report(false);
    }

    if (!g_url[0]) {
        /* Still worth running: the local record is half the point, and a
         * URL added later is picked up on the next boot. */
        printf("beacon: no url set - keeping %s only, every %lds\n",
               STATUS, g_interval);
    } else {
        printf("beacon: reporting to %s every %lds\n", g_url, g_interval);
    }

    for (;;) {
        send_report(true);
        lp_sleep_ms(g_interval * 1000);
    }
}
