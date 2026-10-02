/* tasks - the system monitor: what is running, and what it is costing.
 *
 *   lp-tasks                 open on the page used last
 *   lp-tasks --page=cpu      apps, processes, cpu, memory, gpu, drives,
 *                            network, battery
 *
 * The pages are the spec's (apps-and-settings §2-2) and the layout is the
 * owner's mockup (design/reference/task-manager-settings-mockup.html): a
 * sidebar of Apps and Processes, then the resources; each resource a
 * 60-second graph with its number written large on it, cards of the
 * figures that go with it, and a sentence where a number alone would not
 * say whether it is good or bad (a disk under 15% free is amber and says
 * so; a battery says how long it will last at this rate).
 *
 * Where a machine has more than one of a thing, each is shown on its
 * own: every GPU (the XPS has two, one of them asleep most of the day),
 * every disk with the filesystems on it, and every CPU package on a
 * machine with more than one socket. A total of two unlike devices
 * describes neither.
 *
 * ── What it costs to watch ──
 *
 * A system monitor that shows up in its own CPU graph is a joke nobody
 * laughs at twice, and this machine's owner asked for heavy optimisation.
 * So the work is split by what is on screen:
 *
 *  - Once a second, always: /proc/stat, /proc/meminfo, /proc/diskstats,
 *    /proc/net/dev, one hwmon file, the battery, each GPU's power state
 *    and the fdinfo of the few open GPU files. A few small reads; the
 *    history behind every graph has to keep coming even while its page
 *    is not shown, or switching to it would show a blank minute.
 *  - Every 30 seconds, or 5 while the GPU page is showing: which
 *    processes have a GPU open (every fd link in /proc; see find_gpus).
 *  - Once a second, only while Apps, Processes or Memory is showing and
 *    the window is not minimised: the walk over /proc/<pid>. That is the
 *    expensive part (hundreds of small files), and nobody reads a
 *    process list through a minimised window.
 *  - Every frame, only while a graph is mapped and its window is not
 *    minimised: the graph's scroll. The tick callback is added on map and
 *    removed on unmap and on minimise, so a window left open on another
 *    workspace costs one timer a second and nothing per frame.
 *
 * ── How a graph moves ──
 *
 * The samples come once a second; the graph glides between them. It is
 * drawn one sample late on purpose: the newest point enters at the right
 * edge the moment it is known, and the whole line travels left at one
 * sample-width per second, so there is never a jump and never a gap to
 * fill. The line itself is a cairo drawing made ONCE per sample and kept
 * as a render node; each frame only translates that node under a clip.
 * With the GL renderer a node that has not changed is a cached texture,
 * so a frame of a scrolling graph is one textured quad, not a cairo
 * redraw of a 3300-pixel-wide path. The large number on the graph and
 * the per-core bars follow critically damped springs (no overshoot - a
 * CPU bar that bounces past the real value is a lie for 100ms), and they
 * are drawn by the widget, not set as label text, so easing them
 * re-lays nothing out.
 *
 * ── What it can and cannot do to a process ──
 *
 * End (SIGTERM) and Kill (SIGKILL), each after a confirmation. Only the
 * person's own processes: signalling another account's needs root, and
 * the one root helper this desktop has (lp-privd) deliberately has no
 * "kill any process" verb. The error says so when it happens.
 */
#define _GNU_SOURCE 1
#include "lp-kit.h"
#include "lp-fit.h"

#include <gio/gdesktopappinfo.h>
#include <ifaddrs.h>
#include <math.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "lp-apps.h"
#include "lp-toplevel.h"

#define APP_ID      "org.lpzero.Tasks"
#define STATE_NAME  "tasks"
#define WINDOW_W    1120
#define WINDOW_H    780
#define SIDEBAR_W   176

#define HIST        62          /* samples kept: 60 s on screen + margin */
#define WINDOW_S    60.0        /* seconds a graph spans */
#define BAT_HIST    360         /* 3 hours of battery at one per 30 s */
#define LOW_FREE    0.15        /* spec: amber under 15% free */

/* ═══════════════════════════════════════════════════════════════════
 * Reading /proc and /sys
 * ═══════════════════════════════════════════════════════════════════ */

/* Small files are read into a caller's buffer with one read(): these are
 * kernel-generated and never short-read at this size, and GLib's
 * allocate-and-grow reader is the wrong tool for four hundred of them a
 * second. */
static ssize_t read_small(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    /* /proc/net/dev and /proc/diskstats hand out about 4 KB a read;
     * past that a single read() was a cut-off table. */
    ssize_t r = 0, k = 0;
    while ((size_t)r < n - 1 && (k = read(fd, buf + r, n - 1 - (size_t)r)) > 0)
        r += k;
    close(fd);
    if (r == 0 && k < 0)
        return -1;
    buf[r] = '\0';
    return r;
}

static gint64 read_num(const char *path, gint64 fallback)
{
    char b[64];
    if (read_small(path, b, sizeof b) <= 0)
        return fallback;
    char *end = NULL;
    gint64 v = g_ascii_strtoll(b, &end, 10);
    return end == b ? fallback : v;
}

static char *read_word(const char *path)
{
    char b[256];
    if (read_small(path, b, sizeof b) <= 0)
        return NULL;
    return g_strstrip(g_strdup(b));
}

/* A history of one number: oldest first, with the monotonic time each
 * sample was taken, so the graph can place it exactly. */
typedef struct {
    double v[HIST];
    gint64 t[HIST];
    int    n;
    guint  serial;     /* bumps on every push: the graph's "rebuild" cue */
} Series;

static void series_push(Series *s, gint64 t, double v)
{
    if (s->n == HIST) {
        memmove(s->v, s->v + 1, sizeof s->v[0] * (HIST - 1));
        memmove(s->t, s->t + 1, sizeof s->t[0] * (HIST - 1));
        s->n--;
    }
    s->v[s->n] = v;
    s->t[s->n] = t;
    s->n++;
    s->serial++;
}

static double series_last(const Series *s)
{
    return s->n ? s->v[s->n - 1] : 0.0;
}

typedef struct {
    char    *name;
    char    *path;       /* mount point */
    char    *dev;
    char    *disk;       /* the whole disk it is on ("nvme0n1"), or NULL */
    guint64  size, avail;
} Fs;

/* One CPU package (socket). A laptop has one and the page is as it
 * was; a machine with two gets a graph for each, because "the CPU is at
 * 50%" there can mean one socket flat out and the other asleep. */
typedef struct {
    int      id;              /* topology/physical_package_id */
    int      threads, cores;
    char    *model;
    char    *temp_path;       /* coretemp's "Package id N", or NULL */
    guint64  busy_d, total_d; /* this second's jiffies, summed over its CPUs */
    double   pct;
    double   temp;            /* NAN without a sensor */
    Series   h;
} Pkg;

/* One whole disk from /proc/diskstats. The set can change while the
 * window is open (a USB stick), so the Drives page is rebuilt when it
 * does. */
typedef struct {
    char    *name;            /* "nvme0n1" */
    char    *model;
    const char *kind;         /* SSD, HDD, USB, SD */
    guint64  size;            /* bytes */
    guint64  prev_rd, prev_wr, prev_io_ms;
    double   rd, wr;          /* bytes/s */
    double   active;          /* % of the second it was busy */
    gboolean primed, seen;
    Series   rd_h, wr_h;
    gpointer ui;              /* DiskUi, the page's widgets for it */
} Disk;

/* One graphics device: a /sys/class/drm/cardN with a driver. How busy
 * it is comes from the best source it has - see sample_gpus(). */
#define GPU_ENGINES 10

typedef enum { SRC_NONE, SRC_FDINFO, SRC_RC6, SRC_AMD } GpuSource;

typedef struct {
    char     *node;           /* "card0" */
    char     *dev;            /* its device directory in sysfs, resolved */
    char     *pci;            /* "0000:00:02.0", or NULL */
    char     *driver;
    char     *name;           /* "Intel HD Graphics 530" */
    gboolean  boot_vga;       /* the one the firmware lit the screen with */
    gboolean  asleep;         /* runtime PM has it suspended */
    GpuSource src;
    double    busy;           /* %, NAN when nothing says */
    /* i915: busy is the share of time outside RC6, the sleep state */
    gboolean  has_rc6;
    gint64    prev_rc6, prev_rc6_t;
    double    rc6_busy;
    int       mhz, max_mhz;
    /* amdgpu: the driver's own figure for the whole device */
    double    amd_busy;
    gint64    vram_used, vram_total;
    /* fdinfo: engine time and memory the programs using it report */
    gboolean  fd_engines, fd_mem;
    int       neng;
    char      eng[GPU_ENGINES][24];
    double    eng_cap[GPU_ENGINES];
    double    eng_ns[GPU_ENGINES];    /* this second's busy ns, summed */
    double    eng_pct[GPU_ENGINES];
    guint64   mem_fd;                 /* bytes, summed over programs */
    Series    h;
    gpointer  ui;
} Gpu;

typedef struct {
    /* CPU */
    int      ncpu;
    guint64 *prev_total, *prev_idle;  /* [0] the whole machine, [1..] cores */
    double   cpu;                     /* % of the whole machine */
    double  *core;                    /* % per core */
    guint64  delta_total;             /* all-CPU jiffies in the last second */
    Series   cpu_h;
    char    *model;
    int      cores;                   /* physical */
    double   mhz, max_mhz, base_mhz;
    char    *l3;
    gboolean virt;
    double   temp;                    /* NAN when there is no sensor */
    int      nprocs, nthreads;
    double   uptime;

    /* Memory, bytes */
    guint64  mem_total, mem_avail, mem_free, mem_cache, swap_total, swap_free;
    Series   mem_h;

    /* CPU packages; only filled in when there is more than one */
    int      npkg;
    Pkg     *pkg;
    int     *cpu_pkg;                 /* CPU number -> index in pkg */

    /* Drives */
    GPtrArray *disks;                 /* Disk*, in /proc/diskstats order */
    GHashTable *not_disks;            /* names that are partitions etc. */
    gboolean disks_changed;
    GPtrArray *fs;                    /* Fs*, refreshed while shown */

    /* Network, bytes/s */
    guint64  prev_rx, prev_tx, rx_total, tx_total;
    double   rx, tx;
    Series   rx_h, tx_h;

    /* GPUs */
    GPtrArray *gpus;                  /* Gpu*, by card number */
    guint    ticks;

    /* Battery */
    gboolean bat;
    int      bat_pct;
    char    *bat_state;
    double   bat_w;
    int      bat_min;                 /* -1 unknown */
    double   bat_health;              /* -1 unknown */
    int      bat_cycles;              /* -1 unknown */
    char    *bat_path;
    double   bat_hist[BAT_HIST];
    int      bat_n;
    gint64   bat_last;

    gboolean primed;
    gint64   now;
} Sys;

static Sys S;

/* Which package each CPU is in, from sysfs topology (/proc/cpuinfo has
 * no "physical id" on ARM), and what each package is. */
static void read_packages(GHashTable *models)
{
    S.cpu_pkg = g_new0(int, S.ncpu);
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(int));
    GHashTable *cores = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (int i = 0; i < S.ncpu; i++) {
        char p[96];
        g_snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", i);
        int id = (int)MAX(0, read_num(p, 0));
        g_snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/core_id", i);
        g_hash_table_add(cores, g_strdup_printf("%d/%d", id, (int)read_num(p, i)));
        guint k;
        for (k = 0; k < ids->len && g_array_index(ids, int, k) != id; k++)
            ;
        if (k == ids->len)
            g_array_append_val(ids, id);
        S.cpu_pkg[i] = (int)k;
    }
    S.npkg = (int)ids->len;
    S.pkg = g_new0(Pkg, MAX(1, S.npkg));
    for (int k = 0; k < S.npkg; k++) {
        Pkg *pk = &S.pkg[k];
        pk->id = g_array_index(ids, int, k);
        pk->temp = NAN;
        const char *m = g_hash_table_lookup(models, GINT_TO_POINTER(pk->id + 1));
        pk->model = g_strdup(m ? m : S.model);
        GHashTableIter it;
        gpointer key;
        g_hash_table_iter_init(&it, cores);
        while (g_hash_table_iter_next(&it, &key, NULL))
            if (atoi(key) == pk->id)
                pk->cores++;
    }
    for (int i = 0; i < S.ncpu; i++)
        S.pkg[S.cpu_pkg[i]].threads++;
    g_hash_table_unref(cores);
    g_array_unref(ids);
}

static void read_cpuinfo(void)
{
    char *text = NULL;
    GHashTable *models = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    S.ncpu = 0;
    if (g_file_get_contents("/proc/cpuinfo", &text, NULL, NULL)) {
        GHashTable *phys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        char *pid = NULL;
        const char *cur_model = NULL;
        for (char *l = text, *n; l && *l; l = n) {
            n = strchr(l, '\n');
            if (n)
                *n++ = '\0';
            char *colon = strchr(l, ':');
            if (!colon)
                continue;
            char *val = g_strstrip(colon + 1);
            if (g_str_has_prefix(l, "processor"))
                S.ncpu++;
            else if (g_str_has_prefix(l, "model name")) {
                cur_model = val;
                if (!S.model)
                    S.model = g_strdup(val);
            } else if (g_str_has_prefix(l, "physical id")) {
                g_free(pid);
                pid = g_strdup(val);
                gpointer key = GINT_TO_POINTER(atoi(val) + 1);
                if (cur_model && !g_hash_table_contains(models, key))
                    g_hash_table_insert(models, key, g_strdup(cur_model));
            } else if (g_str_has_prefix(l, "core id")) {
                g_hash_table_add(phys, g_strdup_printf("%s/%s", pid ? pid : "0", val));
            } else if (g_str_has_prefix(l, "flags") && !S.virt) {
                S.virt = strstr(val, " vmx") || strstr(val, " svm");
            }
        }
        S.cores = (int)g_hash_table_size(phys);
        g_hash_table_unref(phys);
        g_free(pid);
        g_free(text);
    }
    if (S.ncpu <= 0)
        S.ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (S.ncpu <= 0)
        S.ncpu = 1;
    if (S.cores <= 0)
        S.cores = S.ncpu;
    if (!S.model)
        S.model = g_strdup(T("Processor", "프로세서"));
    S.max_mhz = read_num("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", 0) / 1000.0;
    S.base_mhz = read_num("/sys/devices/system/cpu/cpu0/cpufreq/base_frequency", 0) / 1000.0;
    for (int i = 0; i < 5 && !S.l3; i++) {
        char p[96];
        g_snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu0/cache/index%d/level", i);
        if (read_num(p, 0) == 3) {
            g_snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu0/cache/index%d/size", i);
            S.l3 = read_word(p);
        }
    }
    S.prev_total = g_new0(guint64, S.ncpu + 1);
    S.prev_idle = g_new0(guint64, S.ncpu + 1);
    S.core = g_new0(double, S.ncpu);
    read_packages(models);
    g_hash_table_unref(models);
}

static void sample_cpu(void)
{
    char buf[16384];
    if (read_small("/proc/stat", buf, sizeof buf) <= 0)
        return;
    for (char *l = buf, *n; l && *l; l = n) {
        n = strchr(l, '\n');
        if (n)
            *n++ = '\0';
        if (strncmp(l, "cpu", 3) != 0)
            break;
        int idx = 0;
        char *p = l + 3;
        if (*p != ' ') {
            idx = (int)strtol(p, &p, 10) + 1;
            if (idx > S.ncpu)
                continue;
        }
        guint64 f[10] = { 0 };
        for (int i = 0; i < 10; i++)
            f[i] = g_ascii_strtoull(p, &p, 10);
        /* user nice system idle iowait irq softirq steal (guest is
         * already inside user). iowait is idle: the CPU was free. */
        guint64 idle = f[3] + f[4];
        guint64 total = f[0] + f[1] + f[2] + f[3] + f[4] + f[5] + f[6] + f[7];
        guint64 dt = total - S.prev_total[idx], di = idle - S.prev_idle[idx];
        double pct = (S.primed && dt) ? 100.0 * (double)(dt - di) / (double)dt : 0.0;
        if (idx == 0) {
            S.cpu = pct;
            S.delta_total = dt;
        } else {
            S.core[idx - 1] = pct;
            if (S.primed) {
                Pkg *pk = &S.pkg[S.cpu_pkg[idx - 1]];
                pk->busy_d += dt - di;
                pk->total_d += dt;
            }
        }
        S.prev_total[idx] = total;
        S.prev_idle[idx] = idle;
    }
    /* A package's share is its CPUs' busy jiffies over their total, not
     * a mean of percentages - an offline CPU would drag a mean down. */
    for (int k = 0; k < S.npkg; k++) {
        Pkg *pk = &S.pkg[k];
        pk->pct = pk->total_d ? 100.0 * (double)pk->busy_d / (double)pk->total_d : 0.0;
        pk->busy_d = pk->total_d = 0;
    }
    /* Mean of the cores' current clocks: what "3.84 GHz" means. */
    double sum = 0;
    int got = 0;
    for (int i = 0; i < S.ncpu; i++) {
        char p[96];
        g_snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        gint64 k = read_num(p, -1);
        if (k > 0) {
            sum += k / 1000.0;
            got++;
        }
    }
    S.mhz = got ? sum / got : 0;
    char up[64];
    if (read_small("/proc/uptime", up, sizeof up) > 0)
        S.uptime = g_ascii_strtod(up, NULL);
}

static void sample_mem(void)
{
    char buf[4096];
    if (read_small("/proc/meminfo", buf, sizeof buf) <= 0)
        return;
    guint64 buffers = 0, cached = 0, srecl = 0, shmem = 0;
    for (char *l = buf, *n; l && *l; l = n) {
        n = strchr(l, '\n');
        if (n)
            *n++ = '\0';
        char *c = strchr(l, ':');
        if (!c)
            continue;
        *c = '\0';
        guint64 kib = g_ascii_strtoull(c + 1, NULL, 10) * 1024;
        if (!strcmp(l, "MemTotal")) S.mem_total = kib;
        else if (!strcmp(l, "MemFree")) S.mem_free = kib;
        else if (!strcmp(l, "MemAvailable")) S.mem_avail = kib;
        else if (!strcmp(l, "Buffers")) buffers = kib;
        else if (!strcmp(l, "Cached")) cached = kib;
        else if (!strcmp(l, "SReclaimable")) srecl = kib;
        else if (!strcmp(l, "Shmem")) shmem = kib;
        else if (!strcmp(l, "SwapTotal")) S.swap_total = kib;
        else if (!strcmp(l, "SwapFree")) S.swap_free = kib;
    }
    guint64 cache = buffers + cached + srecl;
    S.mem_cache = cache > shmem ? cache - shmem : 0;
}

/* Whole disks only: a partition's sectors are already in its disk's. */
static gboolean whole_disk(const char *name)
{
    if (g_str_has_prefix(name, "loop") || g_str_has_prefix(name, "ram") ||
        g_str_has_prefix(name, "zram") || g_str_has_prefix(name, "dm-") ||
        g_str_has_prefix(name, "sr"))
        return FALSE;
    char p[128];
    g_snprintf(p, sizeof p, "/sys/block/%s", name);
    return g_file_test(p, G_FILE_TEST_IS_DIR);
}

static char *trimmed(char *s)
{
    if (s && !*g_strstrip(s))
        g_clear_pointer(&s, g_free);
    return s;
}

static Disk *disk_new(const char *name, guint64 size)
{
    Disk *d = g_new0(Disk, 1);
    d->name = g_strdup(name);
    d->size = size;
    char p[160];
    g_snprintf(p, sizeof p, "/sys/block/%s/device/model", name);
    d->model = trimmed(read_word(p));
    if (!d->model) {
        g_snprintf(p, sizeof p, "/sys/block/%s/device/name", name);   /* SD cards */
        d->model = trimmed(read_word(p));
    }
    g_snprintf(p, sizeof p, "/sys/block/%s", name);
    char *real = realpath(p, NULL);
    g_snprintf(p, sizeof p, "/sys/block/%s/removable", name);
    gboolean removable = read_num(p, 0) == 1;
    g_snprintf(p, sizeof p, "/sys/block/%s/queue/rotational", name);
    if ((real && strstr(real, "/usb")) || removable)
        d->kind = "USB";
    else if (g_str_has_prefix(name, "mmcblk"))
        d->kind = "SD";
    else
        d->kind = read_num(p, 0) == 1 ? "HDD" : "SSD";
    free(real);
    if (!d->model)
        d->model = g_strdup(g_str_has_prefix(name, "vd") ? T("Virtual disk", "가상 디스크")
                                                         : T("Disk", "디스크"));
    return d;
}

static void disk_free(Disk *d)
{
    g_free(d->name);
    g_free(d->model);
    g_free(d);
}

/* Every whole disk's rates and busy time. A name seen once as not a
 * disk (a partition, a loop device) is remembered, so the second-by-
 * second cost is one read of /proc/diskstats and no sysfs lookups. */
static void sample_disks(double secs)
{
    char buf[16384];
    if (read_small("/proc/diskstats", buf, sizeof buf) <= 0)
        return;
    for (guint i = 0; i < S.disks->len; i++)
        ((Disk *)g_ptr_array_index(S.disks, i))->seen = FALSE;
    for (char *l = buf, *n; l && *l; l = n) {
        n = strchr(l, '\n');
        if (n)
            *n++ = '\0';
        unsigned maj, min;
        char name[64];
        unsigned long long f[10];
        /* reads, merged, sectors read, ms, writes, merged, sectors
         * written, ms, in flight, ms doing I/O */
        if (sscanf(l, " %u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &maj, &min, name, &f[0], &f[1], &f[2], &f[3], &f[4], &f[5],
                   &f[6], &f[7], &f[8], &f[9]) < 13)
            continue;
        if (g_hash_table_contains(S.not_disks, name))
            continue;
        Disk *d = NULL;
        for (guint i = 0; i < S.disks->len && !d; i++) {
            Disk *x = g_ptr_array_index(S.disks, i);
            if (!strcmp(x->name, name))
                d = x;
        }
        if (!d) {
            if (!whole_disk(name)) {
                g_hash_table_add(S.not_disks, g_strdup(name));
                continue;
            }
            /* An empty card reader is a disk of size 0: not shown, and
             * asked again next second, when a card may be in it. */
            char p[128];
            g_snprintf(p, sizeof p, "/sys/block/%s/size", name);
            gint64 sectors = read_num(p, 0);
            if (sectors <= 0)
                continue;
            d = disk_new(name, (guint64)sectors * 512);
            g_ptr_array_add(S.disks, d);
            S.disks_changed = TRUE;
        }
        d->seen = TRUE;
        guint64 rd = f[2] * 512, wr = f[6] * 512, io = f[9];
        if (d->primed && secs > 0) {
            d->rd = rd >= d->prev_rd ? (rd - d->prev_rd) / secs : 0;
            d->wr = wr >= d->prev_wr ? (wr - d->prev_wr) / secs : 0;
            d->active = CLAMP((double)(io - d->prev_io_ms) / (secs * 10.0), 0.0, 100.0);
        }
        d->prev_rd = rd;
        d->prev_wr = wr;
        d->prev_io_ms = io;
        d->primed = TRUE;
    }
    for (guint i = 0; i < S.disks->len; i++)
        if (!((Disk *)g_ptr_array_index(S.disks, i))->seen)
            S.disks_changed = TRUE;     /* unplugged; the page drops it */
}

static void sample_net(double secs)
{
    char buf[8192];
    if (read_small("/proc/net/dev", buf, sizeof buf) <= 0)
        return;
    guint64 rx = 0, tx = 0;
    for (char *l = buf, *n; l && *l; l = n) {
        n = strchr(l, '\n');
        if (n)
            *n++ = '\0';
        char *c = strchr(l, ':');
        if (!c)
            continue;
        *c = '\0';
        char *name = g_strstrip(l);
        if (!strcmp(name, "lo"))
            continue;
        unsigned long long f[9];
        if (sscanf(c + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8]) < 9)
            continue;
        rx += f[0];
        tx += f[8];
    }
    if (S.primed && secs > 0) {
        /* The sum drops when an interface goes (a USB adapter pulled
         * out): that second is not a negative, wrapped-around rate. */
        S.rx = rx >= S.prev_rx ? (rx - S.prev_rx) / secs : 0;
        S.tx = tx >= S.prev_tx ? (tx - S.prev_tx) / secs : 0;
    }
    S.prev_rx = rx;
    S.prev_tx = tx;
    S.rx_total = rx;
    S.tx_total = tx;
}

/* The hottest package or core sensor; the same search `temp` makes,
 * hwmon numbered with gaps, so it does not stop at the first missing. */
static char *temp_path;

static void find_temp(void)
{
    double best = -1;
    for (int i = 0; i < 32; i++) {
        char nm[96];
        g_snprintf(nm, sizeof nm, "/sys/class/hwmon/hwmon%d/name", i);
        char *name = read_word(nm);
        if (!name)
            continue;
        gboolean cpuish = !strcmp(name, "coretemp") || !strcmp(name, "k10temp") ||
                          !strcmp(name, "zenpower") || !strcmp(name, "cpu_thermal") ||
                          !strcmp(name, "acpitz");
        for (int k = 1; k < 16 && cpuish; k++) {
            char p[128];
            g_snprintf(p, sizeof p, "/sys/class/hwmon/hwmon%d/temp%d_input", i, k);
            gint64 v = read_num(p, -1);
            if (v < 0)
                continue;
            /* Prefer the package sensor (temp1 on coretemp); acpitz only
             * if nothing better turns up. */
            double score = (strcmp(name, "acpitz") ? 1000.0 : 0.0) + (k == 1 ? 100.0 : 0.0);
            if (score > best) {
                best = score;
                g_free(temp_path);
                temp_path = g_strdup(p);
            }
        }
        g_free(name);
    }
    if (!temp_path && g_file_test("/sys/class/thermal/thermal_zone0/temp", G_FILE_TEST_EXISTS))
        temp_path = g_strdup("/sys/class/thermal/thermal_zone0/temp");
    /* Each package's own sensor, where there is more than one package:
     * coretemp has one hwmon per package, labelled "Package id N". */
    for (int i = 0; i < 32 && S.npkg > 1; i++) {
        char p[128];
        g_snprintf(p, sizeof p, "/sys/class/hwmon/hwmon%d/name", i);
        char *name = read_word(p);
        gboolean core = name && !strcmp(name, "coretemp");
        g_free(name);
        for (int k = 1; k < 4 && core; k++) {
            g_snprintf(p, sizeof p, "/sys/class/hwmon/hwmon%d/temp%d_label", i, k);
            char *lab = read_word(p);
            int id;
            if (lab && sscanf(lab, "Package id %d", &id) == 1)
                for (int j = 0; j < S.npkg; j++)
                    if (S.pkg[j].id == id && !S.pkg[j].temp_path)
                        S.pkg[j].temp_path = g_strdup_printf(
                            "/sys/class/hwmon/hwmon%d/temp%d_input", i, k);
            g_free(lab);
        }
    }
}

/* ── GPUs ──
 *
 * Every /sys/class/drm/cardN with a driver is one device, and each is
 * shown on its own: the XPS has two (the Intel HD 530 that drives the
 * screen, and the GTX 960M that nouveau keeps powered off until a program
 * asks for it), and a sum or an average of the two would describe
 * neither.
 *
 * How busy a device is comes from the best source it has, in order:
 *
 *  - amdgpu: gpu_busy_percent, the driver's own figure for the whole
 *    device.
 *  - fdinfo: /proc/<pid>/fdinfo/<fd> of an open DRM device carries, per
 *    client, drm-engine-<name>: the nanoseconds that engine has spent on
 *    the client's work (i915, amdgpu, xe, v3d, panfrost, msm - the
 *    kernel's drm-usage-stats). The difference over a second, summed over
 *    the clients, is how busy each engine was; the device is as busy as
 *    its busiest engine. Only the person's own processes can be read,
 *    which on this desktop includes the compositor. Two fds can be one
 *    client (a dup, a fork), so clients are counted once by drm-client-id.
 *  - i915 without fdinfo: the share of time outside RC6, its sleep state.
 *  - anything else says nothing - virtio_gpu, which is what a VM has, and
 *    nouveau - and the page says so instead of drawing a flat line at 0%.
 *
 * A device that runtime PM has suspended is not read at all beyond that
 * state: several drivers wake the hardware to answer a sysfs read, and
 * a task manager that powers up the discrete GPU to report that it is
 * asleep would cost the battery what the sleeping saves. Reading fdinfo
 * does not touch the device, and reading the state is a flag in the
 * kernel.
 *
 * Finding which processes have a DRM device open means reading every fd
 * link in /proc, so that is done every 5 seconds while the GPU page is
 * showing and every 30 otherwise; each second only the few fdinfo files
 * already found are read. A program that starts using the GPU is counted
 * from the next of those scans. */

static GHashTable *gpu_nodes;   /* "card0", "renderD128" -> Gpu* */

/* The device's name from pci.ids: the part in brackets when there is
 * one ("GM107M [GeForce GTX 960M]"), after the vendor's everyday name
 * for the three a person knows by it. */
static char *pci_name(const char *ids, guint vendor, guint device)
{
    if (!ids)
        return NULL;
    char key[16];
    g_snprintf(key, sizeof key, "\n%04x  ", vendor);
    const char *v = strstr(ids, key);
    if (!v)
        return NULL;
    char dkey[16];
    g_snprintf(dkey, sizeof dkey, "\t%04x  ", device);
    for (const char *l = strchr(v + 1, '\n'); l && l[1]; l = strchr(l + 1, '\n')) {
        const char *line = l + 1;
        if (line[0] != '\t' && line[0] != '#')
            break;                              /* the next vendor */
        if (strncmp(line, dkey, 7) != 0)
            continue;
        const char *nm = line + 7, *e = strchr(nm, '\n');
        char *full = g_strndup(nm, e ? (gsize)(e - nm) : strlen(nm));
        char *lb = strchr(full, '['), *rb = lb ? strchr(lb, ']') : NULL;
        char *dev = lb && rb ? g_strndup(lb + 1, (gsize)(rb - lb - 1)) : g_strdup(full);
        g_free(full);
        const char *vn = vendor == 0x8086 ? "Intel" : vendor == 0x10de ? "NVIDIA"
                       : vendor == 0x1002 ? "AMD" : NULL;
        char *out = vn && !g_str_has_prefix(dev, vn) ? g_strdup_printf("%s %s", vn, dev)
                                                     : g_strdup(dev);
        g_free(dev);
        return out;
    }
    return NULL;
}

static gboolean is_pci_addr(const char *s)
{
    unsigned a, b, c, d;
    int n = 0;
    return sscanf(s, "%x:%x:%x.%x%n", &a, &b, &c, &d, &n) == 4 && s[n] == '\0';
}

static int card_number(const char *name)
{
    int n = -1, len = 0;
    if (sscanf(name, "card%d%n", &n, &len) == 1 && name[len] == '\0')
        return n;
    return -1;
}

static int gpu_cmp(gconstpointer a, gconstpointer b)
{
    const Gpu *x = *(Gpu *const *)a, *y = *(Gpu *const *)b;
    return card_number(x->node) - card_number(y->node);
}

static Gpu *gpu_new(const char *node, char *dev, char *driver, const char *ids)
{
    Gpu *g = g_new0(Gpu, 1);
    g->node = g_strdup(node);
    g->dev = dev;
    g->driver = driver;
    g->busy = NAN;
    g->rc6_busy = NAN;
    g->amd_busy = -1;
    g->vram_used = g->vram_total = -1;
    /* The PCI device is the DRM device's own directory, or (virtio_gpu,
     * whose DRM device hangs off a virtio one) its parent. */
    char *pdir = NULL;
    char *base = g_path_get_basename(dev);
    if (is_pci_addr(base)) {
        pdir = g_strdup(dev);
    } else {
        char *up = g_path_get_dirname(dev), *ub = g_path_get_basename(up);
        if (is_pci_addr(ub))
            pdir = g_strdup(up);
        g_free(up);
        g_free(ub);
    }
    g_free(base);
    char p[512];
    if (pdir) {
        g->pci = g_path_get_basename(pdir);
        char b[32];
        g_snprintf(p, sizeof p, "%s/vendor", pdir);
        guint ven = read_small(p, b, sizeof b) > 0 ? (guint)g_ascii_strtoull(b, NULL, 16) : 0;
        g_snprintf(p, sizeof p, "%s/device", pdir);
        guint id = read_small(p, b, sizeof b) > 0 ? (guint)g_ascii_strtoull(b, NULL, 16) : 0;
        g->name = pci_name(ids, ven, id);
        g_snprintf(p, sizeof p, "%s/boot_vga", pdir);
        g->boot_vga = read_num(p, 0) == 1;
        g_free(pdir);
    }
    if (!g->name)
        g->name = g_strdup_printf(T("Graphics device (%s)", "그래픽 장치 (%s)"), g->driver);
    g_snprintf(p, sizeof p, "/sys/class/drm/%s/power/rc6_residency_ms", node);
    g->has_rc6 = g_file_test(p, G_FILE_TEST_EXISTS);
    g_snprintf(p, sizeof p, "/sys/class/drm/%s/gt_max_freq_mhz", node);
    g->max_mhz = (int)read_num(p, 0);
    return g;
}

static void find_gpus(void)
{
    S.gpus = g_ptr_array_new();
    gpu_nodes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GDir *d = g_dir_open("/sys/class/drm", 0, NULL);
    if (!d)
        return;
    char *ids = NULL;
    if (!g_file_get_contents("/usr/share/misc/pci.ids", &ids, NULL, NULL))
        g_file_get_contents("/usr/share/hwdata/pci.ids", &ids, NULL, NULL);
    GPtrArray *render = g_ptr_array_new_with_free_func(g_free);
    const char *e;
    while ((e = g_dir_read_name(d))) {
        if (g_str_has_prefix(e, "renderD"))
            g_ptr_array_add(render, g_strdup(e));
        if (card_number(e) < 0)
            continue;
        char p[160];
        g_snprintf(p, sizeof p, "/sys/class/drm/%s/device", e);
        char *real = realpath(p, NULL);
        g_snprintf(p, sizeof p, "/sys/class/drm/%s/device/driver", e);
        char *link = g_file_read_link(p, NULL);
        /* virtio_gpu's DRM device is the PCI function, whose driver is
         * virtio-pci; the GPU driver is bound to the virtio device
         * under it. */
        if (link && g_str_has_suffix(link, "/virtio-pci") && real) {
            GDir *vd = g_dir_open(real, 0, NULL);
            const char *v;
            while (vd && (v = g_dir_read_name(vd))) {
                g_snprintf(p, sizeof p, "%s/%s/driver", real, v);
                char *l2 = g_str_has_prefix(v, "virtio") ? g_file_read_link(p, NULL) : NULL;
                if (l2) {
                    g_free(link);
                    link = l2;
                    break;
                }
            }
            if (vd)
                g_dir_close(vd);
        }
        if (real && link) {
            Gpu *g = gpu_new(e, g_strdup(real), g_path_get_basename(link), ids);
            g_ptr_array_add(S.gpus, g);
            g_hash_table_insert(gpu_nodes, g_strdup(e), g);
        }
        free(real);
        g_free(link);
    }
    g_dir_close(d);
    g_free(ids);
    /* renderD128 is the same device as card0: the same Gpu. */
    for (guint i = 0; i < render->len; i++) {
        const char *r = g_ptr_array_index(render, i);
        char p[160];
        g_snprintf(p, sizeof p, "/sys/class/drm/%s/device", r);
        char *real = realpath(p, NULL);
        for (guint k = 0; real && k < S.gpus->len; k++) {
            Gpu *g = g_ptr_array_index(S.gpus, k);
            if (!strcmp(g->dev, real))
                g_hash_table_insert(gpu_nodes, g_strdup(r), g);
        }
        free(real);
    }
    g_ptr_array_unref(render);
    g_ptr_array_sort(S.gpus, gpu_cmp);
}

typedef struct { int pid, fd; Gpu *gpu; } DrmFd;

typedef struct {
    guint   gen;             /* the sample that last saw it */
    int     n;
    char    eng[GPU_ENGINES][24];
    guint64 ns[GPU_ENGINES];
} DrmClient;

static GArray *drm_fds;             /* DrmFd */
static GHashTable *drm_clients;     /* "card0/<drm-client-id>" -> DrmClient */
static guint drm_gen;

/* Which of the person's processes have which DRM device open. Other
 * accounts' /proc/<pid>/fd cannot be opened and are skipped at once. */
static void scan_drm_fds(void)
{
    if (!drm_fds) {
        drm_fds = g_array_new(FALSE, FALSE, sizeof(DrmFd));
        drm_clients = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    }
    g_array_set_size(drm_fds, 0);
    if (!S.gpus->len)
        return;
    GDir *d = g_dir_open("/proc", 0, NULL);
    const char *e;
    while (d && (e = g_dir_read_name(d))) {
        if (!g_ascii_isdigit(e[0]))
            continue;
        char p[64];
        g_snprintf(p, sizeof p, "/proc/%s/fd", e);
        GDir *fds = g_dir_open(p, 0, NULL);
        const char *f;
        while (fds && (f = g_dir_read_name(fds))) {
            char lp[96], tgt[64];
            g_snprintf(lp, sizeof lp, "/proc/%s/fd/%s", e, f);
            ssize_t n = readlink(lp, tgt, sizeof tgt - 1);
            if (n <= 9 || strncmp(tgt, "/dev/dri/", 9) != 0)
                continue;
            tgt[n] = '\0';
            Gpu *g = g_hash_table_lookup(gpu_nodes, tgt + 9);
            if (g) {
                DrmFd x = { atoi(e), atoi(f), g };
                g_array_append_val(drm_fds, x);
            }
        }
        if (fds)
            g_dir_close(fds);
    }
    if (d)
        g_dir_close(d);
}

static int gpu_engine(Gpu *g, const char *name)
{
    for (int i = 0; i < g->neng; i++)
        if (!strcmp(g->eng[i], name))
            return i;
    if (g->neng == GPU_ENGINES)
        return -1;
    g_strlcpy(g->eng[g->neng], name, sizeof g->eng[0]);
    g->eng_cap[g->neng] = 1;
    return g->neng++;
}

/* "1234 KiB", "12 MiB" or plain bytes (drm-usage-stats). */
static guint64 fdinfo_bytes(const char *v)
{
    char *end = NULL;
    guint64 n = g_ascii_strtoull(v, &end, 10);
    while (end && (*end == ' ' || *end == '\t'))
        end++;
    if (end && g_str_has_prefix(end, "KiB"))
        n *= 1024;
    else if (end && g_str_has_prefix(end, "MiB"))
        n *= 1024 * 1024;
    return n;
}

static void sample_drm(double secs)
{
    for (guint i = 0; i < S.gpus->len; i++) {
        Gpu *g = g_ptr_array_index(S.gpus, i);
        memset(g->eng_ns, 0, sizeof g->eng_ns);
        g->mem_fd = 0;
    }
    if (!drm_fds)
        return;
    guint gen = ++drm_gen;
    for (guint i = drm_fds->len; i-- > 0;) {
        DrmFd *f = &g_array_index(drm_fds, DrmFd, i);
        char p[64], buf[4096];
        g_snprintf(p, sizeof p, "/proc/%d/fdinfo/%d", f->pid, f->fd);
        const char *cid = NULL;
        if (read_small(p, buf, sizeof buf) <= 0 || !(cid = strstr(buf, "drm-client-id:"))) {
            g_array_remove_index_fast(drm_fds, i);   /* closed, or the process ended */
            continue;
        }
        Gpu *g = f->gpu;
        char key[64];
        g_snprintf(key, sizeof key, "%s/%" G_GUINT64_FORMAT, g->node,
                   g_ascii_strtoull(cid + 14, NULL, 10));
        DrmClient *c = g_hash_table_lookup(drm_clients, key);
        if (c && c->gen == gen)
            continue;                                /* the same client, another fd */
        if (!c) {
            c = g_new0(DrmClient, 1);
            g_hash_table_insert(drm_clients, g_strdup(key), c);
        }
        c->gen = gen;
        guint64 resident = 0, legacy = 0;
        gboolean has_resident = FALSE, has_mem = FALSE;
        for (char *l = buf, *n; l && *l; l = n) {
            n = strchr(l, '\n');
            if (n)
                *n++ = '\0';
            char *colon = strchr(l, ':');
            if (!g_str_has_prefix(l, "drm-") || !colon)
                continue;
            *colon = '\0';
            const char *k = l + 4, *v = colon + 1;
            if (g_str_has_prefix(k, "engine-capacity-")) {
                int e = gpu_engine(g, k + 16);
                if (e >= 0)
                    g->eng_cap[e] = MAX(1.0, g_ascii_strtod(v, NULL));
            } else if (g_str_has_prefix(k, "engine-")) {
                const char *en = k + 7;
                guint64 ns = g_ascii_strtoull(v, NULL, 10);
                int e = gpu_engine(g, en);
                g->fd_engines = TRUE;
                int j = 0;
                while (j < c->n && strcmp(c->eng[j], en) != 0)
                    j++;
                if (j < c->n) {
                    /* Its time since the last sample; a client first seen
                     * now only sets where it starts from. */
                    if (e >= 0 && ns >= c->ns[j])
                        g->eng_ns[e] += (double)(ns - c->ns[j]);
                    c->ns[j] = ns;
                } else if (c->n < GPU_ENGINES) {
                    g_strlcpy(c->eng[c->n], en, sizeof c->eng[0]);
                    c->ns[c->n++] = ns;
                }
            } else if (g_str_has_prefix(k, "resident-")) {
                resident += fdinfo_bytes(v);
                has_resident = has_mem = TRUE;
            } else if (g_str_has_prefix(k, "memory-")) {   /* amdgpu's older name */
                legacy += fdinfo_bytes(v);
                has_mem = TRUE;
            }
        }
        g->fd_mem |= has_mem;
        g->mem_fd += has_resident ? resident : legacy;
    }
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, drm_clients);
    while (g_hash_table_iter_next(&it, &k, &v))
        if (((DrmClient *)v)->gen != gen)
            g_hash_table_iter_remove(&it);
    for (guint i = 0; i < S.gpus->len; i++) {
        Gpu *g = g_ptr_array_index(S.gpus, i);
        for (int e = 0; e < g->neng; e++)
            g->eng_pct[e] = secs > 0
                ? CLAMP(100.0 * g->eng_ns[e] / (secs * 1e9 * g->eng_cap[e]), 0.0, 100.0) : 0.0;
    }
}

static void sample_gpus(double secs)
{
    for (guint i = 0; i < S.gpus->len; i++) {
        Gpu *g = g_ptr_array_index(S.gpus, i);
        char p[512];
        g_snprintf(p, sizeof p, "%s/power/runtime_status", g->dev);
        char *st = read_word(p);
        g->asleep = st && !strcmp(st, "suspended");
        g_free(st);
        g->mhz = 0;
        g->amd_busy = -1;
        if (g->asleep) {
            g->prev_rc6_t = 0;   /* RC6 time across a suspend means nothing */
            continue;
        }
        g_snprintf(p, sizeof p, "/sys/class/drm/%s/gt_act_freq_mhz", g->node);
        g->mhz = (int)read_num(p, 0);
        if (g->has_rc6) {
            g_snprintf(p, sizeof p, "/sys/class/drm/%s/power/rc6_residency_ms", g->node);
            gint64 rc6 = read_num(p, -1), t = g_get_monotonic_time() / 1000;
            g->rc6_busy = NAN;
            if (rc6 >= 0 && g->prev_rc6_t) {
                double idle = (double)(rc6 - g->prev_rc6) / (double)MAX(1, t - g->prev_rc6_t);
                g->rc6_busy = CLAMP(100.0 * (1.0 - idle), 0.0, 100.0);
            }
            g->prev_rc6 = rc6;
            g->prev_rc6_t = t;
        }
        if (!strcmp(g->driver, "amdgpu")) {
            g_snprintf(p, sizeof p, "%s/gpu_busy_percent", g->dev);
            g->amd_busy = (double)read_num(p, -1);
            g_snprintf(p, sizeof p, "%s/mem_info_vram_used", g->dev);
            g->vram_used = read_num(p, -1);
            g_snprintf(p, sizeof p, "%s/mem_info_vram_total", g->dev);
            g->vram_total = read_num(p, -1);
        }
    }
    sample_drm(secs);
    for (guint i = 0; i < S.gpus->len; i++) {
        Gpu *g = g_ptr_array_index(S.gpus, i);
        double top = 0;
        for (int e = 0; e < g->neng; e++)
            top = MAX(top, g->eng_pct[e]);
        if (g->asleep) {
            g->busy = 0;         /* asleep is idle, and that much is known */
        } else if (g->amd_busy >= 0) {
            g->src = SRC_AMD;
            g->busy = g->amd_busy;
        } else if (g->fd_engines) {
            g->src = SRC_FDINFO;
            g->busy = top;
        } else if (g->has_rc6) {
            g->src = SRC_RC6;
            g->busy = isnan(g->rc6_busy) ? 0 : g->rc6_busy;
        } else {
            g->src = SRC_NONE;
            g->busy = NAN;
        }
    }
}

static void find_battery(void)
{
    GDir *d = g_dir_open("/sys/class/power_supply", 0, NULL);
    if (!d)
        return;
    const char *e;
    while ((e = g_dir_read_name(d))) {
        char p[160];
        g_snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e);
        char *t = read_word(p);
        if (t && !strcmp(t, "Battery") && !S.bat_path)
            S.bat_path = g_strdup_printf("/sys/class/power_supply/%s", e);
        g_free(t);
    }
    g_dir_close(d);
    S.bat = S.bat_path != NULL;
}

static gint64 bat_num(const char *f)
{
    char p[200];
    g_snprintf(p, sizeof p, "%s/%s", S.bat_path, f);
    return read_num(p, -1);
}

static void sample_battery(void)
{
    if (!S.bat)
        return;
    S.bat_pct = (int)bat_num("capacity");
    char p[200];
    g_snprintf(p, sizeof p, "%s/status", S.bat_path);
    g_free(S.bat_state);
    S.bat_state = read_word(p);
    /* Power in µW directly, or µA x µV. */
    gint64 pw = bat_num("power_now");
    if (pw < 0) {
        gint64 cu = bat_num("current_now"), vo = bat_num("voltage_now");
        pw = (cu >= 0 && vo >= 0) ? (gint64)((double)cu * (double)vo / 1e6) : -1;
    }
    S.bat_w = pw >= 0 ? pw / 1e6 : -1;
    gint64 en = bat_num("energy_now"), ef = bat_num("energy_full"),
           efd = bat_num("energy_full_design");
    if (en < 0) {
        gint64 cn = bat_num("charge_now"), vo = bat_num("voltage_min_design");
        if (cn >= 0 && vo > 0)
            en = (gint64)((double)cn * vo / 1e6);
        ef = bat_num("charge_full");
        efd = bat_num("charge_full_design");
    }
    S.bat_health = (ef > 0 && efd > 0) ? 100.0 * ef / efd : -1;
    S.bat_cycles = (int)bat_num("cycle_count");
    S.bat_min = -1;
    if (S.bat_state && !strcmp(S.bat_state, "Discharging") && pw > 0 && en > 0)
        S.bat_min = (int)(60.0 * en / pw);
    else if (S.bat_state && !strcmp(S.bat_state, "Charging") && pw > 0 && en >= 0 && ef > en)
        S.bat_min = (int)(60.0 * (ef - en) / pw);
    if (!S.bat_last || S.now - S.bat_last >= 30 * G_USEC_PER_SEC) {
        S.bat_last = S.now;
        if (S.bat_n == BAT_HIST) {
            memmove(S.bat_hist, S.bat_hist + 1, sizeof(double) * (BAT_HIST - 1));
            S.bat_n--;
        }
        S.bat_hist[S.bat_n++] = S.bat_pct;
    }
}

/* The whole disk a block device is on: /dev/nvme0n1p2 -> nvme0n1, and
 * through device-mapper (an encrypted root) to the partition under it. */
static char *disk_of(const char *devpath)
{
    char *real = realpath(devpath, NULL);
    char *name = g_path_get_basename(real ? real : devpath);
    free(real);
    char p[192];
    for (int depth = 0; depth < 4; depth++) {
        g_snprintf(p, sizeof p, "/sys/class/block/%s/slaves", name);
        GDir *d = g_dir_open(p, 0, NULL);
        const char *e = d ? g_dir_read_name(d) : NULL;
        char *next = e ? g_strdup(e) : NULL;
        if (d)
            g_dir_close(d);
        if (!next)
            break;
        g_free(name);
        name = next;
    }
    g_snprintf(p, sizeof p, "/sys/class/block/%s/partition", name);
    if (g_file_test(p, G_FILE_TEST_EXISTS)) {
        g_snprintf(p, sizeof p, "/sys/class/block/%s", name);
        char *r = realpath(p, NULL);
        g_clear_pointer(&name, g_free);
        if (r) {
            char *up = g_path_get_dirname(r);
            name = g_path_get_basename(up);
            g_free(up);
            free(r);
        }
    }
    return name;
}

/* Mounted filesystems that are real disks, for the Drives page. */
static void sample_fs(void)
{
    if (!S.fs)
        S.fs = g_ptr_array_new();
    for (guint i = 0; i < S.fs->len; i++) {
        Fs *f = g_ptr_array_index(S.fs, i);
        g_free(f->name);
        g_free(f->path);
        g_free(f->dev);
        g_free(f->disk);
        g_free(f);
    }
    g_ptr_array_set_size(S.fs, 0);
    char *text = NULL;
    if (!g_file_get_contents("/proc/self/mountinfo", &text, NULL, NULL))
        return;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (char *l = text, *n; l && *l; l = n) {
        n = strchr(l, '\n');
        if (n)
            *n++ = '\0';
        char **f = g_strsplit(l, " ", 0);
        int nf = (int)g_strv_length(f);
        int dash = -1;
        for (int i = 6; i < nf; i++)
            if (!strcmp(f[i], "-")) {
                dash = i;
                break;
            }
        if (dash > 0 && dash + 2 < nf && g_str_has_prefix(f[dash + 2], "/dev/") &&
            !g_str_has_prefix(f[dash + 2], "/dev/loop") &&
            !g_hash_table_contains(seen, f[dash + 2])) {
            const char *mp = f[4];
            if (!strcmp(mp, "/") || g_str_has_prefix(mp, "/home") ||
                g_str_has_prefix(mp, "/media/") || g_str_has_prefix(mp, "/mnt/") ||
                g_str_has_prefix(mp, "/boot") || !strcmp(mp, "/data")) {
                struct statvfs sv;
                char *path = g_strcompress(mp);
                if (statvfs(path, &sv) == 0 && sv.f_blocks) {
                    Fs *fs = g_new0(Fs, 1);
                    fs->path = path;
                    fs->dev = g_strdup(f[dash + 2]);
                    fs->disk = disk_of(fs->dev);
                    fs->size = (guint64)sv.f_blocks * sv.f_frsize;
                    fs->avail = (guint64)sv.f_bavail * sv.f_frsize;
                    fs->name = !strcmp(path, "/") ? g_strdup(T("System", "시스템"))
                                                  : g_path_get_basename(path);
                    g_ptr_array_add(S.fs, fs);
                    g_hash_table_add(seen, g_strdup(f[dash + 2]));
                } else {
                    g_free(path);
                }
            }
        }
        g_strfreev(f);
    }
    g_hash_table_unref(seen);
    g_free(text);
}

/* ═══════════════════════════════════════════════════════════════════
 * Processes
 * ═══════════════════════════════════════════════════════════════════ */

#define LPT_TYPE_PROC (lpt_proc_get_type())
G_DECLARE_FINAL_TYPE(LptProc, lpt_proc, LPT, PROC, GObject)

struct _LptProc {
    GObject  parent_instance;
    int      pid;
    int      ppid;
    guint    uid;
    char    *name;       /* argv[0]'s basename, or comm */
    char    *cmd;
    char    *user;
    double   cpu;        /* % of the whole machine */
    guint64  rss;
    double   io;         /* bytes/s, -1 when not readable */
    int      threads;
    guint64  prev_ticks;
    guint64  prev_io;
    gboolean io_ok;
    guint    gen;        /* the scan that last saw it */
    char    *app;        /* desktop id of the application it belongs to */
};

G_DEFINE_TYPE(LptProc, lpt_proc, G_TYPE_OBJECT)
static guint proc_changed;

static void lpt_proc_finalize(GObject *o)
{
    LptProc *p = LPT_PROC(o);
    g_free(p->name);
    g_free(p->cmd);
    g_free(p->user);
    g_free(p->app);
    G_OBJECT_CLASS(lpt_proc_parent_class)->finalize(o);
}

static void lpt_proc_class_init(LptProcClass *k)
{
    G_OBJECT_CLASS(k)->finalize = lpt_proc_finalize;
    proc_changed = g_signal_new("changed", LPT_TYPE_PROC, G_SIGNAL_RUN_LAST,
                                0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void lpt_proc_init(LptProc *p) { p->io = -1; }

static GHashTable *procs;       /* pid -> LptProc (a ref) */
static GListStore *proc_store;
static guint scan_gen;
static GHashTable *users;       /* uid -> name */
static GHashTable *exe_apps;    /* executable basename -> desktop id */
static long page_size;
static long clk_tck;

static const char *user_name(guint uid)
{
    const char *n = g_hash_table_lookup(users, GUINT_TO_POINTER(uid + 1));
    if (n)
        return n;
    struct passwd *pw = getpwuid(uid);
    char *s = pw ? g_strdup(pw->pw_name) : g_strdup_printf("%u", uid);
    g_hash_table_insert(users, GUINT_TO_POINTER(uid + 1), s);
    return s;
}

/* Which application each executable belongs to, from the .desktop
 * files: "Exec=firefox-esr %u" -> firefox-esr -> firefox-esr.desktop. */
static void load_exe_apps(void)
{
    exe_apps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GList *all = g_app_info_get_all();
    for (GList *l = all; l; l = l->next) {
        GAppInfo *ai = l->data;
        if (!g_app_info_should_show(ai) || !g_app_info_get_id(ai))
            continue;
        const char *exe = g_app_info_get_executable(ai);
        if (!exe || !*exe)
            continue;
        char *base = g_path_get_basename(exe);
        if (!g_hash_table_contains(exe_apps, base) && strcmp(base, "env") &&
            strcmp(base, "sh") && strcmp(base, "bash"))
            g_hash_table_insert(exe_apps, base, g_strdup(g_app_info_get_id(ai)));
        else
            g_free(base);
    }
    g_list_free_full(all, g_object_unref);
}

static gboolean scan_one(int pid, guint64 total_delta)
{
    char path[64], buf[2048];
    g_snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (read_small(path, buf, sizeof buf) <= 0)
        return FALSE;
    /* "pid (comm) state ..." - comm may hold spaces and parentheses, so
     * the fields start after the LAST ')'. */
    char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
    if (!lp || !rp)
        return FALSE;
    /* Valid UTF-8 always: the kernel cuts comm at 15 bytes, which can
     * split a Hangul letter, and a label given bytes that are not UTF-8
     * is a crash in GTK 4.8's accessibility code. */
    char *comm = g_utf8_make_valid(lp + 1, (gssize)(rp - lp - 1));
    char *p = rp + 2;
    unsigned long long f[20] = { 0 };
    /* state is field 3; we want ppid(4) utime(14) stime(15) threads(20). */
    char state = *p;
    (void)state;
    p += 2;
    for (int i = 4; i <= 20; i++)
        f[i - 4 < 20 ? i - 4 : 19] = g_ascii_strtoull(p, &p, 10);
    guint64 ticks = f[14 - 4] + f[15 - 4];
    int threads = (int)f[20 - 4];
    int ppid = (int)f[4 - 4];

    LptProc *pr = g_hash_table_lookup(procs, GINT_TO_POINTER(pid));
    gboolean fresh = pr == NULL;
    if (fresh) {
        pr = g_object_new(LPT_TYPE_PROC, NULL);
        pr->pid = pid;
        g_snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
        char cb[1024];
        ssize_t n = read_small(path, cb, sizeof cb);
        if (n > 0) {
            for (ssize_t i = 0; i < n - 1; i++)
                if (cb[i] == '\0')
                    cb[i] = ' ';
            pr->cmd = g_utf8_make_valid(cb, -1);
            char *first = g_strndup(pr->cmd, strcspn(pr->cmd, " "));
            pr->name = g_path_get_basename(first);
            g_free(first);
        }
        if (!pr->name || !*pr->name || strlen(pr->name) < strlen(comm) / 2) {
            g_free(pr->name);
            pr->name = g_strdup(comm);
        }
        if (!pr->cmd)
            pr->cmd = g_strdup_printf("[%s]", comm);   /* a kernel thread */
        struct stat st;
        g_snprintf(path, sizeof path, "/proc/%d", pid);
        if (stat(path, &st) == 0)
            pr->uid = st.st_uid;
        pr->user = g_strdup(user_name(pr->uid));
        const char *app = g_hash_table_lookup(exe_apps, pr->name);
        if (!app) {
            const char *app2 = g_hash_table_lookup(exe_apps, comm);
            app = app2;
        }
        pr->app = g_strdup(app);
        pr->prev_ticks = ticks;
        g_hash_table_insert(procs, GINT_TO_POINTER(pid), pr);
        g_list_store_append(proc_store, pr);
    }
    g_free(comm);
    /* Share of the whole machine: the process's jiffies over all CPUs'.
     * Not across a gap: the walk stops while no process page is showing,
     * and the first one back would divide minutes by one second. */
    gboolean stale = !fresh && pr->gen + 1 != scan_gen;
    pr->cpu = total_delta && !stale && ticks >= pr->prev_ticks
              ? 100.0 * (double)(ticks - pr->prev_ticks) / (double)total_delta : 0;
    pr->prev_ticks = ticks;
    pr->threads = threads;
    pr->ppid = ppid;
    g_snprintf(path, sizeof path, "/proc/%d/statm", pid);
    if (read_small(path, buf, sizeof buf) > 0) {
        char *q = buf;
        g_ascii_strtoull(q, &q, 10);
        pr->rss = g_ascii_strtoull(q, NULL, 10) * (guint64)page_size;
    }
    /* /proc/<pid>/io is readable for our own processes only. */
    if (pr->uid == getuid()) {
        g_snprintf(path, sizeof path, "/proc/%d/io", pid);
        if (read_small(path, buf, sizeof buf) > 0) {
            guint64 rb = 0, wb = 0;
            char *r = strstr(buf, "read_bytes:"), *w = strstr(buf, "\nwrite_bytes:");
            if (r)
                rb = g_ascii_strtoull(r + 11, NULL, 10);
            if (w)
                wb = g_ascii_strtoull(w + 13, NULL, 10);
            pr->io = pr->io_ok && !stale && rb + wb >= pr->prev_io
                     ? (double)(rb + wb - pr->prev_io) : 0;
            pr->prev_io = rb + wb;
            pr->io_ok = TRUE;
        }
    }
    pr->gen = scan_gen;
    if (!fresh)
        g_signal_emit(pr, proc_changed, 0);
    return TRUE;
}

static void scan_procs(void)
{
    scan_gen++;
    GDir *d = g_dir_open("/proc", 0, NULL);
    if (!d)
        return;
    const char *e;
    int count = 0, threads = 0;
    while ((e = g_dir_read_name(d))) {
        if (!g_ascii_isdigit(e[0]))
            continue;
        int pid = atoi(e);
        if (scan_one(pid, S.delta_total)) {
            count++;
            LptProc *pr = g_hash_table_lookup(procs, GINT_TO_POINTER(pid));
            threads += pr ? pr->threads : 1;
        }
    }
    g_dir_close(d);
    S.nprocs = count;
    S.nthreads = threads;
    /* The dead: out of the store and the table. */
    for (guint i = g_list_model_get_n_items(G_LIST_MODEL(proc_store)); i-- > 0;) {
        LptProc *p = g_list_model_get_item(G_LIST_MODEL(proc_store), i);
        if (p->gen != scan_gen) {
            g_list_store_remove(proc_store, i);
            g_hash_table_remove(procs, GINT_TO_POINTER(p->pid));
        }
        g_object_unref(p);
    }
}

/* Counting processes and threads without the full walk, for the CPU page
 * when the process pages are not showing. */
static void count_procs(void)
{
    char buf[4096];
    if (read_small("/proc/loadavg", buf, sizeof buf) > 0) {
        char *slash = strchr(buf, '/');
        if (slash)
            S.nthreads = atoi(slash + 1);
    }
    GDir *d = g_dir_open("/proc", 0, NULL);
    int n = 0;
    const char *e;
    while (d && (e = g_dir_read_name(d)))
        if (g_ascii_isdigit(e[0]))
            n++;
    if (d)
        g_dir_close(d);
    S.nprocs = n;
}

/* ═══════════════════════════════════════════════════════════════════
 * Formatting
 * ═══════════════════════════════════════════════════════════════════ */

static char *fmt_bytes(double b)
{
    if (b < 0)
        return g_strdup("—");
    return g_format_size((guint64)b);
}

static char *fmt_rate(double b)
{
    if (b < 1)
        return g_strdup(T("0 B/s", "0 B/s"));
    char *s = g_format_size((guint64)b);
    char *r = g_strdup_printf("%s/s", s);
    g_free(s);
    return r;
}

static char *fmt_duration(double secs)
{
    long m = (long)(secs / 60);
    long h = m / 60, d = h / 24;
    if (d > 0)
        return g_strdup_printf(T("%ld d %ld h", "%ld일 %ld시간"), d, h % 24);
    if (h > 0)
        return g_strdup_printf(T("%ld h %ld min", "%ld시간 %ld분"), h, m % 60);
    return g_strdup_printf(T("%ld min", "%ld분"), m);
}

/* ═══════════════════════════════════════════════════════════════════
 * The graph (see the file comment: a cached node, translated per frame)
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum { FMT_PERCENT, FMT_RATE } GraphFmt;

typedef struct {
    GtkWidget      parent;
    const Series  *a, *b;
    GraphFmt       fmt;
    double         fixed_max;       /* > 0: a fixed scale (percent) */
    int            nat_h;           /* natural height; 0 for the usual */
    LpSpring       value;           /* the headline number, eased */
    LpSpring       scale;           /* auto scale, eased */
    guint          tick;
    gint64         last_frame;
    GskRenderNode *line;
    guint          line_serial;
    int            line_w, line_h;
    double         line_max;
    gint64         line_t;          /* newest sample time in the node */
    double         drawn_off;
    char          *detail;
    char          *caption;
    char          *legend_a, *legend_b;
    PangoLayout   *big, *small, *foot;
    gulong         state_handler;
    GdkSurface    *surface;
} LptGraph;
typedef struct { GtkWidgetClass parent_class; } LptGraphClass;
G_DEFINE_TYPE(LptGraph, lpt_graph, GTK_TYPE_WIDGET)

#define G_PAD    16.0
#define G_HEAD   52.0
#define G_FOOT   26.0

static const GdkRGBA C_LINE_A = { 0.91f, 0.91f, 0.91f, 1.0f };
static const GdkRGBA C_LINE_B = { 0.50f, 0.72f, 0.63f, 1.0f };   /* teal */
static const GdkRGBA C_GRID   = { 1.0f, 1.0f, 1.0f, 0.06f };
static const GdkRGBA C_T1     = { 0.91f, 0.91f, 0.91f, 1.0f };
static const GdkRGBA C_T3     = { 0.42f, 0.42f, 0.42f, 1.0f };
static const GdkRGBA C_AMBER  = { 0.94f, 0.70f, 0.31f, 1.0f };

static double graph_top(LptGraph *g)
{
    if (g->fixed_max > 0)
        return g->fixed_max;
    return MAX(g->scale.x, 1.0);
}

/* A scale that grows to fit and shrinks back slowly: the next power of
 * two (in KiB/s steps) above the largest value on screen. */
static double nice_max(double v)
{
    double m = 64 * 1024;
    while (m < v * 1.15)
        m *= 2;
    return m;
}

static void build_line(LptGraph *g, float pw, float ph)
{
    g_clear_pointer(&g->line, gsk_render_node_unref);
    const Series *ss[2] = { g->a, g->b };
    if (!g->a || g->a->n < 1)
        return;
    double dx = pw / WINDOW_S;
    double top = graph_top(g);
    gint64 tn = g->a->t[g->a->n - 1];
    graphene_rect_t r = GRAPHENE_RECT_INIT(-dx * 2, -2, pw + dx * 4, ph + 4);
    GskRenderNode *node = gsk_cairo_node_new(&r);
    cairo_t *cr = gsk_cairo_node_get_draw_context(node);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (int s = 1; s >= 0; s--) {
        const Series *se = ss[s];
        if (!se || se->n < 1)
            continue;
        /* x of a sample: its age before the newest, one dx per second,
         * newest at the right edge (pw). */
        cairo_new_path(cr);
        int first = 1;
        double x0 = 0;
        for (int i = 0; i < se->n; i++) {
            double age = (tn - se->t[i]) / 1e6;
            double x = pw - age * dx;
            if (x < -dx * 2)
                continue;
            double y = ph - CLAMP(se->v[i] / top, 0.0, 1.0) * (ph - 2) - 1;
            if (first) {
                cairo_move_to(cr, x, y);
                x0 = x;
                first = 0;
            } else {
                cairo_line_to(cr, x, y);
            }
        }
        const GdkRGBA *c = s == 0 ? &C_LINE_A : &C_LINE_B;
        if (s == 0 && !first) {
            /* A faint fill under the main line, the mockup's weight. */
            cairo_path_t *path = cairo_copy_path(cr);
            cairo_line_to(cr, pw, ph);
            cairo_line_to(cr, x0, ph);
            cairo_close_path(cr);
            cairo_set_source_rgba(cr, c->red, c->green, c->blue, 0.07);
            cairo_fill(cr);
            cairo_append_path(cr, path);
            cairo_path_destroy(path);
        }
        cairo_set_source_rgba(cr, c->red, c->green, c->blue, c->alpha);
        cairo_set_line_width(cr, 1.6);
        cairo_stroke(cr);
    }
    cairo_destroy(cr);
    g->line = node;
    g->line_serial = g->a->serial + (g->b ? g->b->serial * 7919u : 0);
    g->line_w = (int)pw;
    g->line_h = (int)ph;
    g->line_max = top;
    g->line_t = tn;
}

static char *graph_value_text(LptGraph *g, double v)
{
    if (g->fmt == FMT_PERCENT)
        return g_strdup_printf("%.0f%%", CLAMP(v, 0, 100));
    return fmt_rate(v);
}

static void layout_font(PangoLayout *l, int px, int weight)
{
    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    pango_font_description_set_weight(fd, weight);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    /* Tabular digits: a number that changes must not make its
     * neighbours move. */
    PangoAttrList *al = pango_attr_list_new();
    pango_attr_list_insert(al, pango_attr_font_features_new("tnum 1"));
    pango_layout_set_attributes(l, al);
    pango_attr_list_unref(al);
}

static void graph_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
    LptGraph *g = (LptGraph *)w;
    float W = gtk_widget_get_width(w), H = gtk_widget_get_height(w);
    float px = G_PAD, py = G_HEAD, pw = W - 2 * G_PAD, ph = H - G_HEAD - G_FOOT;
    if (pw < 10 || ph < 10)
        return;
    if (!g->big) {
        g->big = gtk_widget_create_pango_layout(w, "");
        layout_font(g->big, 26, 650);
        g->small = gtk_widget_create_pango_layout(w, "");
        layout_font(g->small, 13, 400);
        g->foot = gtk_widget_create_pango_layout(w, "");
        layout_font(g->foot, 12, 400);
    }

    /* Header: the number, large, and what goes with it. */
    char *vt = graph_value_text(g, g->value.x);
    pango_layout_set_text(g->big, vt, -1);
    g_free(vt);
    int bw, bh;
    pango_layout_get_pixel_size(g->big, &bw, &bh);
    gtk_snapshot_save(snap);
    gtk_snapshot_translate(snap, &GRAPHENE_POINT_INIT(G_PAD, 12));
    gtk_snapshot_append_layout(snap, g->big, &C_T1);
    gtk_snapshot_restore(snap);
    if (g->detail) {
        pango_layout_set_text(g->small, g->detail, -1);
        int sw, sh;
        pango_layout_get_pixel_size(g->small, &sw, &sh);
        gtk_snapshot_save(snap);
        gtk_snapshot_translate(snap, &GRAPHENE_POINT_INIT(G_PAD + bw + 10, 12 + bh - sh - 4));
        gtk_snapshot_append_layout(snap, g->small, &C_T3);
        gtk_snapshot_restore(snap);
    }

    /* Three faint lines at quarters. */
    for (int i = 1; i <= 3; i++)
        gtk_snapshot_append_color(snap, &C_GRID,
            &GRAPHENE_RECT_INIT(px, py + ph * i / 4.0f, pw, 1));

    /* The line: rebuilt when a sample arrived, the size or the scale
     * changed; otherwise the same node, moved. */
    guint serial = g->a ? g->a->serial + (g->b ? g->b->serial * 7919u : 0) : 0;
    double top = graph_top(g);
    if (!g->line || g->line_serial != serial || g->line_w != (int)pw ||
        g->line_h != (int)ph || fabs(g->line_max - top) > top * 0.002)
        build_line(g, pw, ph);
    if (g->line) {
        double dx = pw / WINDOW_S;
        /* One sample late: the newest point is at the right edge one
         * second after it was taken, and slides in from beyond it. */
        gint64 now = g->last_frame ? g->last_frame : g_get_monotonic_time();
        double age = (now - g->line_t) / 1e6 - 1.0;
        double off = -CLAMP(age, -1.0, 3.0) * dx;
        g->drawn_off = off;
        gtk_snapshot_push_clip(snap, &GRAPHENE_RECT_INIT(px, py - 2, pw, ph + 4));
        gtk_snapshot_save(snap);
        gtk_snapshot_translate(snap, &GRAPHENE_POINT_INIT(px + (float)off, py));
        gtk_snapshot_append_node(snap, g->line);
        gtk_snapshot_restore(snap);
        gtk_snapshot_pop(snap);
    }

    /* Footer: what the graph spans, and the legend. */
    GString *ft = g_string_new(g->caption ? g->caption : T("Last 60 seconds", "최근 60초"));
    pango_layout_set_text(g->foot, ft->str, -1);
    gtk_snapshot_save(snap);
    gtk_snapshot_translate(snap, &GRAPHENE_POINT_INIT(G_PAD, H - G_FOOT + 4));
    gtk_snapshot_append_layout(snap, g->foot, &C_T3);
    gtk_snapshot_restore(snap);
    float lx = W - G_PAD;
    const char *leg[2] = { g->legend_b, g->legend_a };
    const GdkRGBA *lc[2] = { &C_LINE_B, &C_LINE_A };
    for (int i = 0; i < 2; i++) {
        if (!leg[i])
            continue;
        pango_layout_set_text(g->foot, leg[i], -1);
        int fw, fh;
        pango_layout_get_pixel_size(g->foot, &fw, &fh);
        lx -= fw;
        gtk_snapshot_save(snap);
        gtk_snapshot_translate(snap, &GRAPHENE_POINT_INIT(lx, H - G_FOOT + 4));
        gtk_snapshot_append_layout(snap, g->foot, &C_T3);
        gtk_snapshot_restore(snap);
        lx -= 16;
        gtk_snapshot_append_color(snap, lc[i],
            &GRAPHENE_RECT_INIT(lx, H - G_FOOT + 4 + fh / 2.0f - 1, 10, 2));
        lx -= 14;
    }
    g_string_free(ft, TRUE);
}

static gboolean graph_tick(GtkWidget *w, GdkFrameClock *clock, gpointer d)
{
    (void)d;
    LptGraph *g = (LptGraph *)w;
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    double dt = g->last_frame ? (now - g->last_frame) / 1e6 : 0.016;
    g->last_frame = now;
    gboolean moving = lp_spring_step(&g->value, dt);
    moving |= lp_spring_step(&g->scale, dt);
    /* Draw only when the line would move by at least half a pixel on a
     * scale-2 screen, or a number is still easing. */
    float pw = gtk_widget_get_width(w) - 2 * G_PAD;
    double dx = pw / WINDOW_S;
    double off = g->line ? -CLAMP((now - g->line_t) / 1e6 - 1.0, -1.0, 3.0) * dx : 0;
    if (moving || fabs(off - g->drawn_off) >= 0.25)
        gtk_widget_queue_draw(w);
    return G_SOURCE_CONTINUE;
}

static gboolean graph_should_tick(LptGraph *g)
{
    if (!gtk_widget_get_mapped(GTK_WIDGET(g)))
        return FALSE;
    GtkNative *nat = gtk_widget_get_native(GTK_WIDGET(g));
    GdkSurface *s = nat ? gtk_native_get_surface(nat) : NULL;
    if (s && GDK_IS_TOPLEVEL(s) &&
        (gdk_toplevel_get_state(GDK_TOPLEVEL(s)) & GDK_TOPLEVEL_STATE_MINIMIZED))
        return FALSE;
    return TRUE;
}

static void graph_update_tick(LptGraph *g)
{
    gboolean want = graph_should_tick(g);
    if (want && !g->tick) {
        g->last_frame = 0;
        g->tick = gtk_widget_add_tick_callback(GTK_WIDGET(g), graph_tick, NULL, NULL);
    } else if (!want && g->tick) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(g), g->tick);
        g->tick = 0;
    }
}

static void on_surface_state(GObject *s, GParamSpec *p, gpointer d)
{
    (void)s; (void)p;
    graph_update_tick(d);
}

static void graph_map(GtkWidget *w)
{
    GTK_WIDGET_CLASS(lpt_graph_parent_class)->map(w);
    LptGraph *g = (LptGraph *)w;
    GtkNative *nat = gtk_widget_get_native(w);
    GdkSurface *s = nat ? gtk_native_get_surface(nat) : NULL;
    if (s && !g->state_handler) {
        g->surface = s;
        g->state_handler = g_signal_connect(s, "notify::state",
                                            G_CALLBACK(on_surface_state), g);
    }
    /* Arriving on a page: the number is already right, not easing up
     * from zero - graphs do not "animate in" (COMMON.md). */
    lp_spring_jump(&g->value, g->a ? series_last(g->a) : 0);
    graph_update_tick(g);
}

static void graph_unmap(GtkWidget *w)
{
    LptGraph *g = (LptGraph *)w;
    if (g->tick) {
        gtk_widget_remove_tick_callback(w, g->tick);
        g->tick = 0;
    }
    if (g->surface && g->state_handler) {
        g_signal_handler_disconnect(g->surface, g->state_handler);
        g->state_handler = 0;
        g->surface = NULL;
    }
    GTK_WIDGET_CLASS(lpt_graph_parent_class)->unmap(w);
}

static void graph_measure(GtkWidget *w, GtkOrientation o, int for_size,
                          int *min, int *nat, int *mb, int *nb)
{
    (void)for_size;
    int h = ((LptGraph *)w)->nat_h > 0 ? ((LptGraph *)w)->nat_h : 230;
    *min = o == GTK_ORIENTATION_HORIZONTAL ? 200 : MIN(200, h);
    *nat = o == GTK_ORIENTATION_HORIZONTAL ? 600 : h;
    *mb = *nb = -1;
}

static void graph_dispose(GObject *o)
{
    LptGraph *g = (LptGraph *)o;
    g_clear_pointer(&g->line, gsk_render_node_unref);
    g_clear_object(&g->big);
    g_clear_object(&g->small);
    g_clear_object(&g->foot);
    g_clear_pointer(&g->detail, g_free);
    g_clear_pointer(&g->caption, g_free);
    g_clear_pointer(&g->legend_a, g_free);
    g_clear_pointer(&g->legend_b, g_free);
    G_OBJECT_CLASS(lpt_graph_parent_class)->dispose(o);
}

static void lpt_graph_class_init(LptGraphClass *k)
{
    G_OBJECT_CLASS(k)->dispose = graph_dispose;
    GTK_WIDGET_CLASS(k)->snapshot = graph_snapshot;
    GTK_WIDGET_CLASS(k)->measure = graph_measure;
    GTK_WIDGET_CLASS(k)->map = graph_map;
    GTK_WIDGET_CLASS(k)->unmap = graph_unmap;
    gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(k), "lptgraph");
}

static void lpt_graph_init(LptGraph *g)
{
    lp_spring_init(&g->value, LP_SPRING_EXPAND, 0);
    lp_spring_init(&g->scale, LP_SPRING_EXPAND, 64 * 1024);
}

static LptGraph *graph_new(const Series *a, const Series *b, GraphFmt fmt,
                           const char *legend_a, const char *legend_b)
{
    LptGraph *g = g_object_new(lpt_graph_get_type(), NULL);
    g->a = a;
    g->b = b;
    g->fmt = fmt;
    g->fixed_max = fmt == FMT_PERCENT ? 100.0 : 0.0;
    g->legend_a = g_strdup(legend_a);
    g->legend_b = g_strdup(legend_b);
    gtk_widget_set_hexpand(GTK_WIDGET(g), TRUE);
    return g;
}

/* A shorter graph, for a page with one per device. */
static void graph_small(LptGraph *g, int h)
{
    g->nat_h = h;
    gtk_widget_add_css_class(GTK_WIDGET(g), "small");
}

static void graph_sampled(LptGraph *g, const char *detail)
{
    g_free(g->detail);
    g->detail = g_strdup(detail);
    if (!gtk_widget_get_mapped(GTK_WIDGET(g)))
        return;
    lp_spring_set_target(&g->value, g->a ? series_last(g->a) : 0);
    if (g->fixed_max <= 0) {
        double m = 0;
        for (int i = 0; g->a && i < g->a->n; i++)
            m = MAX(m, g->a->v[i]);
        for (int i = 0; g->b && i < g->b->n; i++)
            m = MAX(m, g->b->v[i]);
        lp_spring_set_target(&g->scale, nice_max(m));
    }
    gtk_widget_queue_draw(GTK_WIDGET(g));
}

/* ═══════════════════════════════════════════════════════════════════
 * Bars: per-core usage, and segmented meters
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    GtkWidget  parent;
    int        n;
    LpSpring  *s;
    LpMotion  *m;
    gboolean   meter;        /* one segmented bar instead of n bars */
    GdkRGBA    colors[4];
    PangoLayout *lab;
} LptBars;
typedef struct { GtkWidgetClass parent_class; } LptBarsClass;
G_DEFINE_TYPE(LptBars, lpt_bars, GTK_TYPE_WIDGET)

#define BAR_COLS 8

static void bars_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
    LptBars *b = (LptBars *)w;
    float W = gtk_widget_get_width(w);
    static const GdkRGBA track = { 1, 1, 1, 0.08f };
    if (b->meter) {
        float H = 10;
        GskRoundedRect rr;
        gsk_rounded_rect_init_from_rect(&rr, &GRAPHENE_RECT_INIT(0, 0, W, H), 5);
        gtk_snapshot_push_rounded_clip(snap, &rr);
        gtk_snapshot_append_color(snap, &track, &GRAPHENE_RECT_INIT(0, 0, W, H));
        float x = 0;
        for (int i = 0; i < b->n; i++) {
            float sw = (float)CLAMP(b->s[i].x, 0, 1) * W;
            gtk_snapshot_append_color(snap, &b->colors[i], &GRAPHENE_RECT_INIT(x, 0, sw, H));
            x += sw;
        }
        gtk_snapshot_pop(snap);
        return;
    }
    if (!b->lab) {
        b->lab = gtk_widget_create_pango_layout(w, "");
        layout_font(b->lab, 12, 400);
    }
    int cols = MIN(b->n, BAR_COLS);
    float gap = 14, cw = (W - gap * (cols - 1)) / cols;
    for (int i = 0; i < b->n; i++) {
        int r = i / cols, c = i % cols;
        float x = c * (cw + gap), y = r * 40.0f;
        double v = CLAMP(b->s[i].x, 0, 100) / 100.0;
        GskRoundedRect rr;
        gsk_rounded_rect_init_from_rect(&rr, &GRAPHENE_RECT_INIT(x, y, cw, 6), 3);
        gtk_snapshot_push_rounded_clip(snap, &rr);
        gtk_snapshot_append_color(snap, &track, &GRAPHENE_RECT_INIT(x, y, cw, 6));
        gtk_snapshot_append_color(snap, v > 0.85 ? &C_AMBER : &C_T1,
                                  &GRAPHENE_RECT_INIT(x, y, (float)(cw * v), 6));
        gtk_snapshot_pop(snap);
        char t[16];
        g_snprintf(t, sizeof t, "%d", i + 1);
        pango_layout_set_text(b->lab, t, -1);
        gtk_snapshot_save(snap);
        gtk_snapshot_translate(snap, &GRAPHENE_POINT_INIT(x, y + 12));
        gtk_snapshot_append_layout(snap, b->lab, &C_T3);
        gtk_snapshot_restore(snap);
    }
}

static void bars_measure(GtkWidget *w, GtkOrientation o, int for_size,
                         int *min, int *nat, int *mb, int *nb)
{
    (void)for_size;
    LptBars *b = (LptBars *)w;
    if (o == GTK_ORIENTATION_HORIZONTAL) {
        *min = 100;
        *nat = 400;
    } else if (b->meter) {
        *min = *nat = 10;
    } else {
        int rows = (b->n + BAR_COLS - 1) / BAR_COLS;
        *min = *nat = rows * 40;
    }
    *mb = *nb = -1;
}

static void bars_frame(GtkWidget *w, gpointer d)
{
    (void)d;
    gtk_widget_queue_draw(w);
}

static void bars_dispose(GObject *o)
{
    LptBars *b = (LptBars *)o;
    g_clear_pointer(&b->m, lp_motion_free);
    g_clear_pointer(&b->s, g_free);
    g_clear_object(&b->lab);
    G_OBJECT_CLASS(lpt_bars_parent_class)->dispose(o);
}

static void lpt_bars_class_init(LptBarsClass *k)
{
    G_OBJECT_CLASS(k)->dispose = bars_dispose;
    GTK_WIDGET_CLASS(k)->snapshot = bars_snapshot;
    GTK_WIDGET_CLASS(k)->measure = bars_measure;
    gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(k), "lptbars");
}

static void lpt_bars_init(LptBars *b) { (void)b; }

static LptBars *bars_new(int n, gboolean meter)
{
    LptBars *b = g_object_new(lpt_bars_get_type(), NULL);
    b->n = n;
    b->meter = meter;
    b->s = g_new0(LpSpring, n);
    b->m = lp_motion_new(GTK_WIDGET(b), bars_frame, NULL);
    for (int i = 0; i < n; i++) {
        lp_spring_init(&b->s[i], LP_SPRING_EXPAND, 0);
        lp_motion_add(b->m, &b->s[i]);
    }
    gtk_widget_set_hexpand(GTK_WIDGET(b), TRUE);
    return b;
}

/* Values ease while the bars are on screen and are simply set while they
 * are not - nothing animates for nobody. */
static void bars_set(LptBars *b, int i, double v)
{
    if (i >= b->n)
        return;
    if (!gtk_widget_get_mapped(GTK_WIDGET(b)))
        lp_spring_jump(&b->s[i], v);
    else
        lp_spring_set_target(&b->s[i], v);
}

static void bars_kick(LptBars *b)
{
    if (gtk_widget_get_mapped(GTK_WIDGET(b)))
        lp_motion_kick(b->m);
}

/* ═══════════════════════════════════════════════════════════════════
 * The window
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    GtkWidget *box, *value, *unit, *key;
} Stat;

/* The widgets for one disk, one GPU, one CPU package. */
typedef struct {
    GtkWidget *box;
    LptGraph  *graph;
    Stat       st_rd, st_wr, st_active, st_size;
    GtkWidget *fs_box;
} DiskUi;

typedef struct {
    GtkWidget *box;
    LptGraph  *graph;
    Stat       st_clock, st_max, st_mem, st_state;
    GtkWidget *note;
} GpuUi;

typedef struct {
    GtkApplication *gapp;
    GtkWidget *win, *sidebar, *stack;
    char      *page;
    GKeyFile  *state;
    gint64     last_sample;

    /* Apps */
    GtkWidget *apps_list;
    GHashTable *app_rows;        /* desktop id or app_id -> row widgets (AppRow*) */
    GtkWidget *apps_hint;
    GtkWidget *apps_hint_label;
    GtkWidget *apps_empty, *apps_note;
    gboolean   windows;          /* the compositor lists windows (lp-toplevel) */
    GHashTable *win_apps;        /* app_id -> GDesktopAppInfo, or NULL: none */

    /* Processes */
    GtkWidget *proc_view;
    GtkWidget *proc_search;
    GtkCustomFilter *proc_filter;
    GtkSorter *proc_sorter;
    GtkSingleSelection *proc_sel;
    GtkWidget *proc_end, *proc_kill;
    GtkWidget *proc_mine;
    GtkWidget *proc_count;

    /* Resources */
    LptGraph  *g_cpu, *g_mem, *g_net, *g_bat;
    LptGraph **g_pkg;            /* one per CPU package, when more than one */
    LptBars   *cores, *mem_bar;
    Stat       st_procs, st_threads, st_uptime, st_temp;
    Stat       st_mem_used, st_mem_avail, st_mem_cache, st_swap;
    Stat       st_rx, st_tx, st_rx_tot, st_tx_tot;
    Stat       st_bat_pct, st_bat_time, st_bat_w, st_bat_health;
    GtkWidget *cpu_sub, *cpu_kv;
    GtkWidget *mem_top;
    GtkWidget *disks_box, *fs_other;
    GtkWidget *net_ifaces;
    GtkWidget *net_wifi;
    GtkWidget *bat_note, *bat_state;
    Series     bat_series;
    guint      slow_count;
} App;

static App *A;

static GtkWindow *win(void) { return GTK_WINDOW(A->win); }

static gboolean page_is(const char *id)
{
    const char *v = gtk_stack_get_visible_child_name(GTK_STACK(A->stack));
    return v && !strcmp(v, id) && gtk_widget_get_mapped(A->stack);
}

static gboolean window_minimised(void)
{
    if (!A->win)
        return TRUE;        /* closed: nothing to draw for */
    GtkNative *nat = GTK_NATIVE(A->win);
    GdkSurface *s = gtk_native_get_surface(nat);
    return !gtk_widget_get_mapped(A->win) ||
           (s && GDK_IS_TOPLEVEL(s) &&
            (gdk_toplevel_get_state(GDK_TOPLEVEL(s)) & GDK_TOPLEVEL_STATE_MINIMIZED));
}

/* ── small builders ───────────────────────────────────────────────── */

static GtkWidget *label(const char *text, const char *css, float xalign)
{
    GtkWidget *l = gtk_label_new(text);
    if (css)
        gtk_widget_add_css_class(l, css);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    return l;
}

static GtkWidget *page_head(const char *title, const char *sub, GtkWidget **sub_out)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_box_append(GTK_BOX(b), label(title, "lpt-h1", 0));
    GtkWidget *s = label(sub ? sub : "", "lpt-sub", 0);
    gtk_label_set_wrap(GTK_LABEL(s), TRUE);
    gtk_box_append(GTK_BOX(b), s);
    if (sub_out)
        *sub_out = s;
    gtk_widget_set_margin_bottom(b, 12);
    return b;
}

static GtkWidget *section(const char *text)
{
    GtkWidget *l = label(text, "lpt-sec", 0);
    gtk_widget_set_margin_top(l, 18);
    gtk_widget_set_margin_bottom(l, 6);
    return l;
}

static GtkWidget *stat_new(Stat *st, const char *key)
{
    st->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(st->box, "lpt-stat");
    gtk_widget_set_hexpand(st->box, TRUE);
    st->key = label(key, "lpt-stat-k", 0);
    gtk_box_append(GTK_BOX(st->box), st->key);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    st->value = label("—", "lpt-stat-v", 0);
    st->unit = label("", "lpt-stat-u", 0);
    gtk_widget_set_valign(st->unit, GTK_ALIGN_BASELINE);
    gtk_widget_set_valign(st->value, GTK_ALIGN_BASELINE);
    gtk_box_append(GTK_BOX(row), st->value);
    gtk_box_append(GTK_BOX(row), st->unit);
    gtk_box_append(GTK_BOX(st->box), row);
    return st->box;
}

static void stat_set(Stat *st, const char *value, const char *unit, gboolean warn)
{
    gtk_label_set_text(GTK_LABEL(st->value), value);
    gtk_label_set_text(GTK_LABEL(st->unit), unit ? unit : "");
    if (warn)
        gtk_widget_add_css_class(st->box, "warn");
    else
        gtk_widget_remove_css_class(st->box, "warn");
}

/* "9.8 GB" -> value "9.8", unit "GB" */
static void stat_bytes(Stat *st, double b, gboolean rate)
{
    char *s = rate ? fmt_rate(b) : fmt_bytes(b);
    char *sp = strchr(s, ' ');
    if (sp) {
        *sp = '\0';
        stat_set(st, s, sp + 1, FALSE);
    } else {
        stat_set(st, s, "", FALSE);
    }
    g_free(s);
}

static GtkWidget *stats_row(GtkWidget *a, GtkWidget *b, GtkWidget *c, GtkWidget *d)
{
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(g), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(g), 10);
    gtk_widget_set_margin_top(g, 12);
    GtkWidget *w[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++)
        if (w[i])
            gtk_grid_attach(GTK_GRID(g), w[i], i, 0, 1, 1);
    return g;
}

static GtkWidget *kv_row(GtkWidget *box, const char *k, const char *v)
{
    GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(r, "lpt-kv");
    GtkWidget *kl = label(k, "lpt-kv-k", 0);
    gtk_widget_set_hexpand(kl, TRUE);
    GtkWidget *vl = label(v, "lpt-kv-v", 1);
    gtk_label_set_selectable(GTK_LABEL(vl), TRUE);
    gtk_box_append(GTK_BOX(r), kl);
    gtk_box_append(GTK_BOX(r), vl);
    gtk_box_append(GTK_BOX(box), r);
    return vl;
}

static GtkWidget *page_box(void)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(b, "lpt-page");
    return b;
}

/* ── Apps ─────────────────────────────────────────────────────────── */

/* The Apps page lists what has a window - "the browser got slow" is
 * what people come here for - and only the compositor knows what has a
 * window: on Wayland a program sees its own and nobody else's. It says
 * through wlr-foreign-toplevel-management (lp-toplevel.c, the list the
 * dock's dots and the top bar come from), which wayfire 0.7 has in its
 * core and sway too. The first version of this page asked swaymsg
 * instead, which under wayfire fails, and the page then said nothing
 * had a window open with the person's windows in front of them.
 *
 * A window names itself by app_id, not by process, so the processes are
 * found the way the dock finds the application: app_id -> .desktop file
 * (lp-apps.c) -> the program its Exec starts -> the person's processes
 * running that program, or named like the app_id. Then everything
 * those started goes with them - the shell in a terminal, a browser's
 * helpers - so a build running in a terminal shows as the terminal's
 * CPU, as it would in any task manager. End signals only the
 * application's own processes; what they started ends with them.
 *
 * Where the compositor offers no window list the page falls back to
 * every program of the person's that has an application entry, and says
 * that is what it is showing. */

typedef struct {
    char      *id;       /* desktop id, or the window's app_id */
    char      *exe;      /* the program its .desktop starts, lower case */
    GtkWidget *rev, *row, *icon, *name, *count, *cpu, *mem;
    double     cpu_v;
    guint64    mem_v;
    int        n, nwin;
    gboolean   leaving;
    GPtrArray *pids;     /* its own processes: what End signals */
    GPtrArray *wins;     /* LpToplevel*, as of the last update */
} AppRow;

static void approw_free(gpointer d)
{
    AppRow *r = d;
    g_free(r->id);
    g_free(r->exe);
    g_ptr_array_unref(r->pids);
    g_ptr_array_unref(r->wins);
    g_free(r);
}

typedef struct { int *pids; int n; int sig; char *name; } KillReq;

static void do_kill(KillReq *k)
{
    int failed = 0, last_err = 0;
    for (int i = 0; i < k->n; i++)
        if (kill(k->pids[i], k->sig) != 0) {
            failed++;
            last_err = errno;
        }
    if (failed) {
        char *msg = g_strdup_printf(
            last_err == EPERM
            ? T("%s belongs to another account. Only its owner or an "
                "administrator in a terminal (sudo kill) can end it.",
                "%s 은(는) 다른 계정의 것입니다. 그 계정이나 관리자가 터미널에서 "
                "(sudo kill) 끝낼 수 있습니다.")
            : T("%s could not be ended: it may have ended already.",
                "%s 을(를) 끝내지 못했습니다. 이미 끝났을 수 있습니다."),
            k->name);
        LpKitDialog *d = lp_kit_dialog_new(win(), T("Not ended", "끝내지 못함"), msg);
        lp_kit_dialog_button(d, T("OK", "확인"), LP_KIT_OK, "suggested-action");
        lp_kit_dialog_present(d);
        g_free(msg);
    }
}

static void on_kill_confirmed(gboolean ok, gpointer data)
{
    KillReq *k = data;
    if (ok)
        do_kill(k);
    g_free(k->pids);
    g_free(k->name);
    g_free(k);
}

static void ask_kill(const char *name, int *pids, int n, int sig)
{
    KillReq *k = g_new0(KillReq, 1);
    k->pids = g_memdup2(pids, sizeof(int) * (gsize)n);
    k->n = n;
    k->sig = sig;
    k->name = g_strdup(name);
    char *t = g_strdup_printf(sig == SIGKILL ? T("Kill %s?", "%s 을(를) 강제로 끝낼까요?")
                                             : T("End %s?", "%s 을(를) 끝낼까요?"), name);
    const char *body = sig == SIGKILL
        ? T("It stops at once, without a chance to save. Anything not saved is lost.",
            "저장할 기회 없이 즉시 멈춥니다. 저장하지 않은 것은 사라집니다.")
        : T("It is asked to close. Most programs save and quit; unsaved work "
            "may still be lost.",
            "닫으라고 요청합니다. 대부분 저장하고 끝나지만 저장하지 않은 작업은 "
            "사라질 수 있습니다.");
    lp_kit_confirm(win(), t, body, sig == SIGKILL ? T("Kill", "강제 종료") : T("End", "끝내기"),
                   TRUE, on_kill_confirmed, k);
    g_free(t);
}

static void on_app_end(GtkButton *b, gpointer d)
{
    (void)b;
    AppRow *r = d;
    if (!r->pids->len) {
        /* Windows whose program could not be found: asked to close, as
         * their close button would. Only those still open - a window
         * that closed since the last update is gone from the list. */
        for (guint i = 0; i < r->wins->len; i++)
            if (g_list_find(lp_toplevels(), g_ptr_array_index(r->wins, i)))
                lp_toplevel_close(g_ptr_array_index(r->wins, i));
        return;
    }
    int *p = g_new(int, r->pids->len);
    for (guint i = 0; i < r->pids->len; i++)
        p[i] = GPOINTER_TO_INT(g_ptr_array_index(r->pids, i));
    ask_kill(gtk_label_get_text(GTK_LABEL(r->name)), p, (int)r->pids->len, SIGTERM);
    g_free(p);
}

static AppRow *approw_new(const char *id, GDesktopAppInfo *ai, const char *app_id)
{
    AppRow *r = g_new0(AppRow, 1);
    r->id = g_strdup(id);
    r->pids = g_ptr_array_new();
    r->wins = g_ptr_array_new();
    const char *ex = ai ? g_app_info_get_executable(G_APP_INFO(ai)) : NULL;
    if (ex && *ex) {
        char *b = g_path_get_basename(ex);
        r->exe = g_ascii_strdown(b, -1);
        g_free(b);
    }
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(box, "lpt-app-row");
    r->icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(r->icon), 32);
    GIcon *ic = ai ? g_app_info_get_icon(G_APP_INFO(ai)) : NULL;
    GtkIconTheme *th = gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (ic)
        gtk_image_set_from_gicon(GTK_IMAGE(r->icon), ic);
    else if (app_id && gtk_icon_theme_has_icon(th, app_id))
        gtk_image_set_from_icon_name(GTK_IMAGE(r->icon), app_id);
    else
        gtk_image_set_from_icon_name(GTK_IMAGE(r->icon), "application-x-executable");
    gtk_box_append(GTK_BOX(box), r->icon);
    GtkWidget *nb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_hexpand(nb, TRUE);
    r->name = label(ai ? lp_app_name(G_APP_INFO(ai)) : id, "lpt-app-name", 0);
    gtk_label_set_ellipsize(GTK_LABEL(r->name), PANGO_ELLIPSIZE_END);
    r->count = label("", "lpt-dim", 0);
    gtk_box_append(GTK_BOX(nb), r->name);
    gtk_box_append(GTK_BOX(nb), r->count);
    gtk_box_append(GTK_BOX(box), nb);
    r->cpu = label("", "lpt-num", 1);
    gtk_widget_set_size_request(r->cpu, 80, -1);
    r->mem = label("", "lpt-num", 1);
    gtk_widget_set_size_request(r->mem, 100, -1);
    gtk_box_append(GTK_BOX(box), r->cpu);
    gtk_box_append(GTK_BOX(box), r->mem);
    GtkWidget *end = gtk_button_new_with_label(T("End", "끝내기"));
    gtk_widget_add_css_class(end, "lpt-action");
    char *nm = g_strdup_printf("end-%s", id);
    gtk_widget_set_name(end, nm);
    g_free(nm);
    g_signal_connect(end, "clicked", G_CALLBACK(on_app_end), r);
    gtk_box_append(GTK_BOX(box), end);
    r->row = box;
    return r;
}

static int apps_sort(GtkListBoxRow *a, GtkListBoxRow *b, gpointer d)
{
    (void)d;
    AppRow *x = g_object_get_data(G_OBJECT(a), "approw");
    AppRow *y = g_object_get_data(G_OBJECT(b), "approw");
    if (!x || !y)
        return 0;
    /* Bands of 1%, so two apps near each other do not swap places every
     * second; then by name. */
    int cx = (int)x->cpu_v, cy = (int)y->cpu_v;
    if (cx != cy)
        return cy - cx;
    return g_utf8_collate(gtk_label_get_text(GTK_LABEL(x->name)),
                          gtk_label_get_text(GTK_LABEL(y->name)));
}

static void approw_gone(GtkWidget *rev, gpointer d)
{
    (void)d;
    GtkWidget *row = gtk_widget_get_parent(rev);
    if (row && GTK_IS_LIST_BOX_ROW(row))
        gtk_list_box_remove(GTK_LIST_BOX(A->apps_list), row);
}

static void unref_or_null(gpointer o)
{
    if (o)
        g_object_unref(o);
}

/* The application a window belongs to, remembered per app_id: past its
 * two quick guesses lp_app_for_id reads every .desktop file. */
static GDesktopAppInfo *app_for_window(const char *app_id)
{
    gpointer d;
    if (!g_hash_table_lookup_extended(A->win_apps, app_id, NULL, &d)) {
        d = lp_app_for_id(app_id);
        g_hash_table_insert(A->win_apps, g_strdup(app_id), d);
    }
    return d;
}

static AppRow *app_row(const char *id, GDesktopAppInfo *ai, const char *app_id, gboolean first)
{
    AppRow *r = g_hash_table_lookup(A->app_rows, id);
    if (r)
        return r;
    r = approw_new(id, ai, app_id);
    g_hash_table_insert(A->app_rows, r->id, r);
    r->rev = lp_kit_reveal_in(r->row, first);
    gtk_list_box_append(GTK_LIST_BOX(A->apps_list), r->rev);
    GtkWidget *lbr = gtk_widget_get_parent(r->rev);
    g_object_set_data(G_OBJECT(lbr), "approw", r);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(lbr), FALSE);
    return r;
}

static void app_add(AppRow *r, LptProc *pr)
{
    r->cpu_v += pr->cpu;
    r->mem_v += pr->rss;
    r->n++;
}

/* Rows from the windows, then their processes (see above). */
static void apps_from_windows(GHashTable *live, gboolean first)
{
    GHashTable *by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (GList *l = lp_toplevels(); l; l = l->next) {
        LpToplevel *t = l->data;
        if (!t->done)
            continue;
        const char *aid = t->app_id && *t->app_id ? t->app_id : NULL;
        GDesktopAppInfo *ai = aid ? app_for_window(aid) : NULL;
        const char *id = ai ? g_app_info_get_id(G_APP_INFO(ai))
                       : aid ? aid : t->title && *t->title ? t->title : "?";
        AppRow *r = app_row(id, ai, aid, first);
        r->nwin++;
        g_ptr_array_add(r->wins, t);
        g_hash_table_add(live, r->id);
        if (r->exe)
            g_hash_table_replace(by_name, g_strdup(r->exe), r);
        if (aid)
            g_hash_table_replace(by_name, g_ascii_strdown(aid, -1), r);
    }
    guint me = getuid();
    GHashTable *owner = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, procs);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        LptProc *pr = v;
        if (pr->uid != me)
            continue;
        AppRow *r = pr->app && g_hash_table_contains(live, pr->app)
                  ? g_hash_table_lookup(A->app_rows, pr->app) : NULL;
        if (!r) {
            char *low = g_ascii_strdown(pr->name, -1);
            r = g_hash_table_lookup(by_name, low);
            g_free(low);
        }
        if (r) {
            g_hash_table_insert(owner, GINT_TO_POINTER(pr->pid), r);
            g_ptr_array_add(r->pids, GINT_TO_POINTER(pr->pid));
            app_add(r, pr);
        }
    }
    g_hash_table_iter_init(&it, procs);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        LptProc *pr = v;
        if (pr->uid != me || g_hash_table_contains(owner, GINT_TO_POINTER(pr->pid)))
            continue;
        int pp = pr->ppid;
        for (int depth = 0; depth < 16 && pp > 1; depth++) {
            AppRow *r = g_hash_table_lookup(owner, GINT_TO_POINTER(pp));
            if (r) {
                app_add(r, pr);
                break;
            }
            LptProc *up = g_hash_table_lookup(procs, GINT_TO_POINTER(pp));
            if (!up)
                break;
            pp = up->ppid;
        }
    }
    g_hash_table_unref(owner);
    g_hash_table_unref(by_name);
}

/* No window list: every program of the person's with an application
 * entry. */
static void apps_from_processes(GHashTable *live, gboolean first)
{
    guint me = getuid();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, procs);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        LptProc *pr = v;
        if (!pr->app || pr->uid != me)
            continue;
        AppRow *r = g_hash_table_lookup(A->app_rows, pr->app);
        if (!r) {
            GDesktopAppInfo *ai = g_desktop_app_info_new(pr->app);
            r = app_row(pr->app, ai, NULL, first);
            g_clear_object(&ai);
        }
        app_add(r, pr);
        g_ptr_array_add(r->pids, GINT_TO_POINTER(pr->pid));
        g_hash_table_add(live, r->id);
    }
}

static void update_apps(void)
{
    GHashTable *live = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, A->app_rows);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        AppRow *r = v;
        r->cpu_v = 0;
        r->mem_v = 0;
        r->n = r->nwin = 0;
        g_ptr_array_set_size(r->pids, 0);
        g_ptr_array_set_size(r->wins, 0);
    }
    gboolean first = g_hash_table_size(A->app_rows) == 0;
    if (A->windows)
        apps_from_windows(live, first);
    else
        apps_from_processes(live, first);
    AppRow *top = NULL;
    GPtrArray *gone = g_ptr_array_new();
    g_hash_table_iter_init(&it, A->app_rows);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        AppRow *r = v;
        if (!g_hash_table_contains(live, r->id)) {
            if (!r->leaving) {
                r->leaving = TRUE;
                g_ptr_array_add(gone, r);
            }
            continue;
        }
        char t[64];
        if (r->n) {
            g_snprintf(t, sizeof t, "%.1f%%", r->cpu_v);
            char *m = fmt_bytes((double)r->mem_v);
            gtk_label_set_text(GTK_LABEL(r->cpu), t);
            gtk_label_set_text(GTK_LABEL(r->mem), m);
            g_free(m);
        } else {
            /* A window whose program was not found (yet - a process is
             * picked up on the next second's walk). */
            gtk_label_set_text(GTK_LABEL(r->cpu), "—");
            gtk_label_set_text(GTK_LABEL(r->mem), "—");
        }
        if (r->cpu_v >= 25)
            gtk_widget_add_css_class(r->cpu, "hot");
        else
            gtk_widget_remove_css_class(r->cpu, "hot");
        char *c = r->nwin > 1 && r->n > 1
            ? g_strdup_printf(T("%d windows · %d processes", "창 %d개 · 프로세스 %d개"), r->nwin, r->n)
            : r->nwin > 1 ? g_strdup_printf(T("%d windows", "창 %d개"), r->nwin)
            : r->n > 1 ? g_strdup_printf(T("%d processes", "프로세스 %d개"), r->n)
            : g_strdup("");
        gtk_label_set_text(GTK_LABEL(r->count), c);
        g_free(c);
        if (!top || r->cpu_v > top->cpu_v)
            top = r;
    }
    for (guint i = 0; i < gone->len; i++) {
        AppRow *r = g_ptr_array_index(gone, i);
        g_hash_table_steal(A->app_rows, r->id);
        /* The row's data first: with motion reduced the reveal ends at
         * once, approw_gone frees the revealer, and the row set on it
         * after that was set on freed memory. */
        g_object_set_data_full(G_OBJECT(r->rev), "approw-free", r, approw_free);
        lp_kit_reveal_out(r->rev, G_CALLBACK(approw_gone), NULL);
    }
    g_ptr_array_unref(gone);
    gtk_widget_set_visible(A->apps_empty, g_hash_table_size(live) == 0);
    g_hash_table_unref(live);
    gtk_list_box_invalidate_sort(GTK_LIST_BOX(A->apps_list));
    /* The spec's "a sentence next to the number". */
    if (top && top->cpu_v >= 25) {
        char *h = g_strdup_printf(T("▲ %s is using the most CPU", "▲ %s 이(가) CPU를 가장 많이 쓰고 있습니다"),
                                  gtk_label_get_text(GTK_LABEL(top->name)));
        gtk_label_set_text(GTK_LABEL(A->apps_hint_label), h);
        g_free(h);
        gtk_revealer_set_reveal_child(GTK_REVEALER(A->apps_hint), TRUE);
    } else {
        gtk_revealer_set_reveal_child(GTK_REVEALER(A->apps_hint), FALSE);
    }
}

/* A window opened or closed: the list follows at once rather than at the
 * next second. The processes are last second's; a new application's
 * are counted from the next walk. */
static void on_windows(gpointer d)
{
    (void)d;
    if (page_is("apps") && !window_minimised())
        update_apps();
}

static GtkWidget *build_apps(void)
{
    GtkWidget *b = page_box();
    gtk_box_append(GTK_BOX(b), page_head(T("Apps", "앱"),
        A->windows ? T("Programs with a window open", "창이 열려 있는 프로그램입니다")
                   : T("Applications you are running", "지금 실행 중인 프로그램입니다"), NULL));
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(hdr, "lpt-colhead");
    GtkWidget *hn = label(T("Name", "이름"), NULL, 0);
    gtk_widget_set_hexpand(hn, TRUE);
    gtk_widget_set_margin_start(hn, 46);
    gtk_box_append(GTK_BOX(hdr), hn);
    GtkWidget *hc = label("CPU", NULL, 1);
    gtk_widget_set_size_request(hc, 80, -1);
    GtkWidget *hm = label(T("Memory", "메모리"), NULL, 1);
    gtk_widget_set_size_request(hm, 100, -1);
    GtkWidget *he = label("", NULL, 1);
    gtk_widget_set_size_request(he, 96, -1);
    gtk_box_append(GTK_BOX(hdr), hc);
    gtk_box_append(GTK_BOX(hdr), hm);
    gtk_box_append(GTK_BOX(hdr), he);
    gtk_box_append(GTK_BOX(b), hdr);
    A->apps_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(A->apps_list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(A->apps_list, "lpt-list");
    gtk_list_box_set_sort_func(GTK_LIST_BOX(A->apps_list), apps_sort, NULL, NULL);
    gtk_box_append(GTK_BOX(b), A->apps_list);
    A->apps_empty = label(T("Nothing has a window open.", "창이 열려 있는 프로그램이 없습니다."),
                          "lpt-note", 0);
    gtk_widget_set_margin_top(A->apps_empty, 14);
    gtk_widget_set_margin_start(A->apps_empty, 12);
    gtk_widget_set_visible(A->apps_empty, FALSE);
    gtk_box_append(GTK_BOX(b), A->apps_empty);
    A->apps_hint_label = label("", "lpt-hint", 0);
    A->apps_hint = gtk_revealer_new();
    gtk_revealer_set_transition_duration(GTK_REVEALER(A->apps_hint),
                                         lp_spring_ms(LP_SPRING_EXPAND, FALSE));
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(hb, GTK_ALIGN_START);
    gtk_widget_set_margin_top(hb, 16);
    gtk_box_append(GTK_BOX(hb), A->apps_hint_label);
    gtk_revealer_set_child(GTK_REVEALER(A->apps_hint), hb);
    gtk_box_append(GTK_BOX(b), A->apps_hint);
    A->apps_note = label(A->windows
        ? T("A program's CPU and memory include what it started, such as the shell in a "
            "terminal. Programs without a window are under Processes.",
            "프로그램이 띄운 것(터미널 속 셸 등)의 CPU와 메모리도 함께 셉니다. 창이 없는 "
            "프로그램은 프로세스 탭에 있습니다.")
        : T("This compositor does not say which windows are open, so every program with "
            "an application entry is listed. Everything else is under Processes.",
            "이 컴포지터는 어떤 창이 열려 있는지 알려 주지 않아, 앱 항목이 있는 프로그램을 "
            "모두 보여 줍니다. 나머지는 프로세스 탭에 있습니다."), "lpt-note", 0);
    gtk_label_set_wrap(GTK_LABEL(A->apps_note), TRUE);
    gtk_widget_set_margin_top(A->apps_note, 14);
    gtk_box_append(GTK_BOX(b), A->apps_note);
    return lp_kit_scroller(b, FALSE);
}

/* ── Processes ────────────────────────────────────────────────────── */

typedef enum { COL_NAME, COL_PID, COL_USER, COL_CPU, COL_MEM, COL_DISK } Col;

typedef struct {
    GtkWidget *l;
    LptProc   *p;
    gulong     h;
    Col        col;
} Cell;

static void cell_fill(Cell *c)
{
    LptProc *p = c->p;
    char t[64];
    switch (c->col) {
    case COL_NAME: gtk_label_set_text(GTK_LABEL(c->l), p->name); break;
    case COL_PID:  g_snprintf(t, sizeof t, "%d", p->pid); gtk_label_set_text(GTK_LABEL(c->l), t); break;
    case COL_USER: gtk_label_set_text(GTK_LABEL(c->l), p->user); break;
    case COL_CPU:
        g_snprintf(t, sizeof t, "%.1f%%", p->cpu);
        gtk_label_set_text(GTK_LABEL(c->l), t);
        if (p->cpu >= 25) gtk_widget_add_css_class(c->l, "hot");
        else gtk_widget_remove_css_class(c->l, "hot");
        break;
    case COL_MEM: {
        char *s = fmt_bytes((double)p->rss);
        gtk_label_set_text(GTK_LABEL(c->l), s);
        g_free(s);
        break;
    }
    case COL_DISK: {
        char *s = p->io < 0 ? g_strdup("—") : fmt_rate(p->io);
        gtk_label_set_text(GTK_LABEL(c->l), s);
        g_free(s);
        break;
    }
    }
}

static void on_proc_changed(LptProc *p, gpointer d)
{
    (void)p;
    cell_fill(d);
}

static void cell_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer col)
{
    (void)f;
    Cell *c = g_new0(Cell, 1);
    c->col = GPOINTER_TO_INT(col);
    c->l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(c->l), c->col == COL_NAME || c->col == COL_USER ? 0 : 1);
    gtk_label_set_ellipsize(GTK_LABEL(c->l), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(c->l, c->col == COL_NAME ? "lpt-cell-name" : "lpt-num");
    g_object_set_data_full(G_OBJECT(c->l), "cell", c, g_free);
    gtk_list_item_set_child(li, c->l);
}

static void cell_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer col)
{
    (void)f; (void)col;
    Cell *c = g_object_get_data(G_OBJECT(gtk_list_item_get_child(li)), "cell");
    c->p = gtk_list_item_get_item(li);
    c->h = g_signal_connect(c->p, "changed", G_CALLBACK(on_proc_changed), c);
    cell_fill(c);
}

static void cell_unbind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer col)
{
    (void)f; (void)col;
    Cell *c = g_object_get_data(G_OBJECT(gtk_list_item_get_child(li)), "cell");
    if (c->p && c->h)
        g_signal_handler_disconnect(c->p, c->h);
    c->p = NULL;
    c->h = 0;
}

static int cmp_name(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    return g_ascii_strcasecmp(((LptProc *)a)->name, ((LptProc *)b)->name);
}
static int cmp_pid(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    return ((LptProc *)a)->pid - ((LptProc *)b)->pid;
}
static int cmp_user(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    return g_strcmp0(((LptProc *)a)->user, ((LptProc *)b)->user);
}
static int cmp_cpu(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    double x = ((LptProc *)a)->cpu, y = ((LptProc *)b)->cpu;
    return x < y ? -1 : x > y ? 1 : cmp_pid(a, b, NULL);
}
static int cmp_mem(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    guint64 x = ((LptProc *)a)->rss, y = ((LptProc *)b)->rss;
    return x < y ? -1 : x > y ? 1 : cmp_pid(a, b, NULL);
}
static int cmp_disk(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    double x = ((LptProc *)a)->io, y = ((LptProc *)b)->io;
    return x < y ? -1 : x > y ? 1 : cmp_pid(a, b, NULL);
}

static gboolean proc_visible(gpointer item, gpointer d)
{
    (void)d;
    LptProc *p = item;
    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(A->proc_mine)) && p->uid != getuid())
        return FALSE;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(A->proc_search));
    if (!q || !*q)
        return TRUE;
    char *lq = g_utf8_strdown(q, -1);
    char *ln = g_utf8_strdown(p->cmd ? p->cmd : p->name, -1);
    char pid[16];
    g_snprintf(pid, sizeof pid, "%d", p->pid);
    gboolean hit = strstr(ln, lq) || strstr(p->name, q) || !strcmp(pid, q);
    g_free(lq);
    g_free(ln);
    return hit;
}

static void on_proc_search(GtkEditable *e, gpointer d)
{
    (void)e; (void)d;
    gtk_filter_changed(GTK_FILTER(A->proc_filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static void on_mine_toggled(GtkCheckButton *b, gpointer d)
{
    (void)d;
    gtk_filter_changed(GTK_FILTER(A->proc_filter), GTK_FILTER_CHANGE_DIFFERENT);
    g_key_file_set_boolean(A->state, "tasks", "only-mine", gtk_check_button_get_active(b));
    lp_kit_state_save(A->state, STATE_NAME);
}

static LptProc *selected_proc(void)
{
    return gtk_single_selection_get_selected_item(A->proc_sel);
}

static void on_proc_selected(GObject *o, GParamSpec *p, gpointer d)
{
    (void)o; (void)p; (void)d;
    LptProc *pr = selected_proc();
    gtk_widget_set_sensitive(A->proc_end, pr != NULL);
    gtk_widget_set_sensitive(A->proc_kill, pr != NULL);
}

static void on_proc_end(GtkButton *b, gpointer d)
{
    (void)b;
    LptProc *pr = selected_proc();
    if (!pr)
        return;
    int pid = pr->pid;
    char *n = g_strdup_printf("%s (%d)", pr->name, pid);
    ask_kill(n, &pid, 1, GPOINTER_TO_INT(d));
    g_free(n);
}

static void proc_context(GtkWidget *w, double x, double y, gpointer d)
{
    (void)d;
    LptProc *pr = selected_proc();
    if (!pr)
        return;
    GMenu *m = g_menu_new();
    g_menu_append(m, T("End", "끝내기"), "win.proc-end");
    g_menu_append(m, T("Kill", "강제 종료"), "win.proc-kill");
    GtkWidget *pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(m));
    gtk_widget_set_parent(pop, w);
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &(GdkRectangle){ (int)x, (int)y, 1, 1 });
    gtk_popover_popup(GTK_POPOVER(pop));
    g_object_unref(m);
}

static void act_proc(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p;
    on_proc_end(NULL, d);
}

/* The column the list is sorted by, kept for next time. GTK 4.8 - the
 * base's - has no way to ask which column that is (GtkColumnViewSorter
 * became public in 4.10), so there the list opens sorted by CPU every
 * time. The sorter also says "changed" once a second, when the numbers
 * move, so the file is written only when the choice itself changed. */
static void on_sort_changed(GtkSorter *s, GtkSorterChange c, gpointer d)
{
    (void)s; (void)c; (void)d;
#if GTK_CHECK_VERSION(4, 10, 0)
    GtkColumnViewColumn *col = gtk_column_view_sorter_get_primary_sort_column(
        GTK_COLUMN_VIEW_SORTER(s));
    if (!col)
        return;
    const char *id = gtk_column_view_column_get_id(col) ? gtk_column_view_column_get_id(col) : "cpu";
    gboolean desc = gtk_column_view_sorter_get_primary_sort_order(GTK_COLUMN_VIEW_SORTER(s)) ==
                    GTK_SORT_DESCENDING;
    char *was = g_key_file_get_string(A->state, "tasks", "sort", NULL);
    gboolean same = was && !strcmp(was, id) &&
                    g_key_file_get_boolean(A->state, "tasks", "sort-desc", NULL) == desc;
    g_free(was);
    if (same)
        return;
    g_key_file_set_string(A->state, "tasks", "sort", id);
    g_key_file_set_boolean(A->state, "tasks", "sort-desc", desc);
    lp_kit_state_save(A->state, STATE_NAME);
#endif
}

static GtkWidget *build_processes(void)
{
    GtkWidget *b = page_box();
    gtk_box_append(GTK_BOX(b), page_head(T("Processes", "프로세스"),
        T("Everything running on this computer", "이 컴퓨터에서 실행 중인 모든 것"), NULL));

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    A->proc_search = gtk_search_entry_new();
    g_object_set(A->proc_search, "placeholder-text",
                 T("Search by name, command or PID", "이름, 명령, PID로 찾기"), NULL);
    gtk_widget_add_css_class(A->proc_search, "lpt-search");
    gtk_widget_set_name(A->proc_search, "proc-search");
    gtk_widget_set_hexpand(A->proc_search, TRUE);
    g_signal_connect(A->proc_search, "search-changed", G_CALLBACK(on_proc_search), NULL);
    gtk_box_append(GTK_BOX(bar), A->proc_search);
    A->proc_mine = gtk_check_button_new_with_label(T("Only mine", "내 것만"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(A->proc_mine),
        g_key_file_get_boolean(A->state, "tasks", "only-mine", NULL));
    g_signal_connect(A->proc_mine, "toggled", G_CALLBACK(on_mine_toggled), NULL);
    gtk_box_append(GTK_BOX(bar), A->proc_mine);
    A->proc_end = lp_kit_button(NULL, T("End", "끝내기"), NULL);
    gtk_widget_set_name(A->proc_end, "proc-end");
    A->proc_kill = lp_kit_button(NULL, T("Kill", "강제 종료"), "destructive-action");
    gtk_widget_set_sensitive(A->proc_end, FALSE);
    gtk_widget_set_sensitive(A->proc_kill, FALSE);
    g_signal_connect(A->proc_end, "clicked", G_CALLBACK(on_proc_end), GINT_TO_POINTER(SIGTERM));
    g_signal_connect(A->proc_kill, "clicked", G_CALLBACK(on_proc_end), GINT_TO_POINTER(SIGKILL));
    gtk_box_append(GTK_BOX(bar), A->proc_end);
    gtk_box_append(GTK_BOX(bar), A->proc_kill);
    gtk_box_append(GTK_BOX(b), bar);

    A->proc_filter = gtk_custom_filter_new(proc_visible, NULL, NULL);
    GtkFilterListModel *fm = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(proc_store)),
                                                       GTK_FILTER(A->proc_filter));
    GtkWidget *cv = gtk_column_view_new(NULL);
    gtk_widget_add_css_class(cv, "lpt-procs");
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(cv), FALSE);
    GtkSortListModel *sm = gtk_sort_list_model_new(G_LIST_MODEL(fm),
        g_object_ref(gtk_column_view_get_sorter(GTK_COLUMN_VIEW(cv))));
    A->proc_sorter = gtk_column_view_get_sorter(GTK_COLUMN_VIEW(cv));
    A->proc_sel = gtk_single_selection_new(G_LIST_MODEL(sm));
    gtk_single_selection_set_autoselect(A->proc_sel, FALSE);
    gtk_single_selection_set_can_unselect(A->proc_sel, TRUE);
    gtk_column_view_set_model(GTK_COLUMN_VIEW(cv), GTK_SELECTION_MODEL(A->proc_sel));
    g_signal_connect(A->proc_sel, "notify::selected", G_CALLBACK(on_proc_selected), NULL);

    static const struct { const char *id, *en, *ko; GCompareDataFunc cmp; int w; } COLS[] = {
        { "name", "Name", "이름", cmp_name, 0 },
        { "pid", "PID", "PID", cmp_pid, 90 },
        { "user", "User", "사용자", cmp_user, 110 },
        { "cpu", "CPU", "CPU", cmp_cpu, 90 },
        { "mem", "Memory", "메모리", cmp_mem, 110 },
        { "disk", "Disk", "디스크", cmp_disk, 110 },
    };
    char *want = g_key_file_get_string(A->state, "tasks", "sort", NULL);
    gboolean desc = g_key_file_has_key(A->state, "tasks", "sort-desc", NULL)
        ? g_key_file_get_boolean(A->state, "tasks", "sort-desc", NULL) : TRUE;
    GtkColumnViewColumn *sort_col = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(COLS); i++) {
        GtkListItemFactory *f = gtk_signal_list_item_factory_new();
        g_signal_connect(f, "setup", G_CALLBACK(cell_setup), GINT_TO_POINTER(i));
        g_signal_connect(f, "bind", G_CALLBACK(cell_bind), GINT_TO_POINTER(i));
        g_signal_connect(f, "unbind", G_CALLBACK(cell_unbind), GINT_TO_POINTER(i));
        GtkColumnViewColumn *c = gtk_column_view_column_new(T(COLS[i].en, COLS[i].ko), f);
#if GTK_CHECK_VERSION(4, 10, 0)
        gtk_column_view_column_set_id(c, COLS[i].id);
#endif
        gtk_column_view_column_set_sorter(c,
            GTK_SORTER(gtk_custom_sorter_new(COLS[i].cmp, NULL, NULL)));
        if (COLS[i].w)
            gtk_column_view_column_set_fixed_width(c, COLS[i].w);
        else
            gtk_column_view_column_set_expand(c, TRUE);
        gtk_column_view_column_set_resizable(c, TRUE);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(cv), c);
        if ((want && !strcmp(want, COLS[i].id)) || (!want && !strcmp(COLS[i].id, "cpu")))
            sort_col = c;
        g_object_unref(c);
    }
    g_free(want);
    if (sort_col)
        gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(cv), sort_col,
                                       desc ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
    g_signal_connect(A->proc_sorter, "changed", G_CALLBACK(on_sort_changed), NULL);
    lp_kit_context(cv, proc_context, NULL);
    A->proc_view = cv;
    GtkWidget *sc = lp_kit_scroller(cv, FALSE);
    gtk_widget_set_margin_top(sc, 12);
    gtk_box_append(GTK_BOX(b), sc);
    A->proc_count = label("", "lpt-note", 0);
    gtk_widget_set_margin_top(A->proc_count, 8);
    gtk_box_append(GTK_BOX(b), A->proc_count);
    return b;
}

/* ── CPU ──────────────────────────────────────────────────────────── */

static GtkWidget *build_cpu(void)
{
    GtkWidget *b = page_box();
    char *sub = S.npkg > 1
        ? g_strdup_printf(T("%d processors · %d cores, %d threads", "프로세서 %d개 · %d코어 %d스레드"),
                          S.npkg, S.cores, S.ncpu)
        : g_strdup_printf(T("%s · %d cores, %d threads", "%s · %d코어 %d스레드"),
                          S.model, S.cores, S.ncpu);
    gtk_box_append(GTK_BOX(b), page_head("CPU", sub, &A->cpu_sub));
    g_free(sub);
    A->g_cpu = graph_new(&S.cpu_h, NULL, FMT_PERCENT, NULL, NULL);
    gtk_box_append(GTK_BOX(b), GTK_WIDGET(A->g_cpu));
    gtk_box_append(GTK_BOX(b), stats_row(
        stat_new(&A->st_procs, T("Processes", "프로세스")),
        stat_new(&A->st_threads, T("Threads", "스레드")),
        stat_new(&A->st_uptime, T("Up for", "가동 시간")),
        stat_new(&A->st_temp, T("Temperature", "온도"))));
    gtk_box_append(GTK_BOX(b), section(T("Usage per core", "코어별 사용률")));
    A->cores = bars_new(S.ncpu, FALSE);
    gtk_box_append(GTK_BOX(b), GTK_WIDGET(A->cores));
    /* More than one package (socket): each its own graph, named. */
    if (S.npkg > 1) {
        gtk_box_append(GTK_BOX(b), section(T("Usage per processor", "프로세서별 사용률")));
        A->g_pkg = g_new0(LptGraph *, S.npkg);
        for (int k = 0; k < S.npkg; k++) {
            Pkg *pk = &S.pkg[k];
            char *h = lp_korean()
                ? g_strdup_printf("프로세서 %d · %s · %d코어 %d스레드",
                                  pk->id, pk->model, pk->cores, pk->threads)
                : g_strdup_printf("Processor %d · %s · %d core%s, %d thread%s",
                                  pk->id, pk->model, pk->cores, pk->cores == 1 ? "" : "s",
                                  pk->threads, pk->threads == 1 ? "" : "s");
            GtkWidget *hl = label(h, "lpt-dev", 0);
            gtk_label_set_ellipsize(GTK_LABEL(hl), PANGO_ELLIPSIZE_END);
            gtk_widget_set_margin_top(hl, k ? 14 : 2);
            gtk_widget_set_margin_bottom(hl, 6);
            gtk_box_append(GTK_BOX(b), hl);
            g_free(h);
            A->g_pkg[k] = graph_new(&pk->h, NULL, FMT_PERCENT, NULL, NULL);
            graph_small(A->g_pkg[k], 150);
            gtk_box_append(GTK_BOX(b), GTK_WIDGET(A->g_pkg[k]));
        }
    }
    A->cpu_kv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(A->cpu_kv, 10);
    char t[64];
    if (S.base_mhz > 0) {
        g_snprintf(t, sizeof t, "%.2f GHz", S.base_mhz / 1000);
        kv_row(A->cpu_kv, T("Base clock", "기본 클럭"), t);
    }
    if (S.max_mhz > 0) {
        g_snprintf(t, sizeof t, "%.2f GHz", S.max_mhz / 1000);
        kv_row(A->cpu_kv, T("Maximum clock", "최대 클럭"), t);
    }
    if (S.l3)
        kv_row(A->cpu_kv, T("L3 cache", "L3 캐시"), S.l3);
    kv_row(A->cpu_kv, T("Virtualisation", "가상화"),
           S.virt ? T("Available", "사용 가능") : T("Not available", "사용 불가"));
    gtk_box_append(GTK_BOX(b), A->cpu_kv);
    return lp_kit_scroller(b, FALSE);
}

/* ── Memory ───────────────────────────────────────────────────────── */

static GtkWidget *legend_dot(const char *text, const char *css)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *d = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(d, "lpt-dot");
    gtk_widget_add_css_class(d, css);
    gtk_widget_set_valign(d, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(b), d);
    gtk_box_append(GTK_BOX(b), label(text, "lpt-note", 0));
    return b;
}

static GtkWidget *build_memory(void)
{
    GtkWidget *b = page_box();
    char *tot = fmt_bytes((double)S.mem_total);
    char *sub = g_strdup_printf(T("%s installed", "%s 설치됨"), tot);
    gtk_box_append(GTK_BOX(b), page_head(T("Memory", "메모리"), sub, NULL));
    g_free(sub);
    g_free(tot);
    A->g_mem = graph_new(&S.mem_h, NULL, FMT_PERCENT, NULL, NULL);
    gtk_box_append(GTK_BOX(b), GTK_WIDGET(A->g_mem));
    gtk_box_append(GTK_BOX(b), section(T("Where it is", "쓰임새")));
    A->mem_bar = bars_new(2, TRUE);
    A->mem_bar->colors[0] = C_LINE_A;
    A->mem_bar->colors[1] = (GdkRGBA){ 0.50f, 0.72f, 0.63f, 0.8f };
    gtk_box_append(GTK_BOX(b), GTK_WIDGET(A->mem_bar));
    GtkWidget *leg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_set_margin_top(leg, 8);
    gtk_box_append(GTK_BOX(leg), legend_dot(T("In use", "사용 중"), "a"));
    gtk_box_append(GTK_BOX(leg), legend_dot(T("Cache (given back when needed)",
                                              "캐시 (필요하면 돌려줌)"), "b"));
    gtk_box_append(GTK_BOX(leg), legend_dot(T("Free", "여유"), "c"));
    gtk_box_append(GTK_BOX(b), leg);
    gtk_box_append(GTK_BOX(b), stats_row(
        stat_new(&A->st_mem_used, T("In use", "사용 중")),
        stat_new(&A->st_mem_avail, T("Available", "사용 가능")),
        stat_new(&A->st_mem_cache, T("Cache", "캐시")),
        stat_new(&A->st_swap, T("Swap used", "스왑 사용"))));
    gtk_box_append(GTK_BOX(b), section(T("Using the most", "가장 많이 쓰는 것")));
    A->mem_top = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(b), A->mem_top);
    return lp_kit_scroller(b, FALSE);
}

static void update_mem_top(void)
{
    GPtrArray *arr = g_ptr_array_new();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, procs);
    while (g_hash_table_iter_next(&it, &k, &v))
        g_ptr_array_add(arr, v);
    /* Top five by resident memory; a tiny selection, done by hand -
     * no sort first (an earlier sort here was handed no comparison
     * function, and the Memory page crashed the program the moment
     * it was opened, and on every start after, the page being the one
     * remembered). */
    LptProc *top[5] = { 0 };
    for (guint i = 0; i < arr->len; i++) {
        LptProc *p = g_ptr_array_index(arr, i);
        for (int j = 0; j < 5; j++)
            if (!top[j] || p->rss > top[j]->rss) {
                memmove(top + j + 1, top + j, sizeof(top[0]) * (size_t)(4 - j));
                top[j] = p;
                break;
            }
    }
    g_ptr_array_unref(arr);
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(A->mem_top)))
        gtk_box_remove(GTK_BOX(A->mem_top), c);
    for (int j = 0; j < 5 && top[j]; j++) {
        char *m = fmt_bytes((double)top[j]->rss);
        kv_row(A->mem_top, top[j]->name, m);
        g_free(m);
    }
}

/* ── GPU ──────────────────────────────────────────────────────────── */

static const char *engine_label(const char *e)
{
    static const struct { const char *id, *en, *ko; } E[] = {
        { "render", "3D", "3D" }, { "gfx", "3D", "3D" },
        { "copy", "Copy", "복사" }, { "dma", "Copy", "복사" },
        { "video", "Video", "비디오" }, { "video-enhance", "Video enhance", "영상 보정" },
        { "compute", "Compute", "계산" },
        { "dec", "Video decode", "영상 디코드" }, { "enc", "Video encode", "영상 인코드" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(E); i++)
        if (!strcmp(E[i].id, e))
            return T(E[i].en, E[i].ko);
    return e;
}

static GtkWidget *gpu_block(Gpu *g, int index, gboolean many)
{
    GpuUi *u = g_new0(GpuUi, 1);
    g->ui = u;
    u->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    if (many) {
        char *h = g_strdup_printf("GPU %d · %s", index, g->name);
        GtkWidget *hl = label(h, "lpt-dev", 0);
        gtk_label_set_ellipsize(GTK_LABEL(hl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_margin_top(hl, index ? 26 : 0);
        gtk_box_append(GTK_BOX(u->box), hl);
        g_free(h);
    }
    GString *sub = g_string_new(g->driver);
    if (g->pci)
        g_string_append_printf(sub, " · %s", g->pci);
    if (g->boot_vga)
        g_string_append_printf(sub, " · %s", T("drives the screen", "화면에 연결됨"));
    GtkWidget *sl = label(sub->str, "lpt-dim", 0);
    gtk_widget_set_margin_bottom(sl, 8);
    gtk_box_append(GTK_BOX(u->box), sl);
    g_string_free(sub, TRUE);
    u->graph = graph_new(&g->h, NULL, FMT_PERCENT, NULL, NULL);
    if (many)
        graph_small(u->graph, 170);
    gtk_box_append(GTK_BOX(u->box), GTK_WIDGET(u->graph));
    gtk_box_append(GTK_BOX(u->box), stats_row(
        stat_new(&u->st_clock, T("Clock now", "현재 클럭")),
        stat_new(&u->st_max, T("Maximum", "최대 클럭")),
        stat_new(&u->st_mem, T("Memory", "메모리")),
        stat_new(&u->st_state, T("State", "상태"))));
    u->note = label("", "lpt-note", 0);
    gtk_label_set_wrap(GTK_LABEL(u->note), TRUE);
    gtk_widget_set_margin_top(u->note, 10);
    gtk_box_append(GTK_BOX(u->box), u->note);
    return u->box;
}

static GtkWidget *build_gpu(void)
{
    GtkWidget *b = page_box();
    int n = (int)S.gpus->len;
    char *sub = n == 1 ? g_strdup(((Gpu *)g_ptr_array_index(S.gpus, 0))->name)
              : n > 1 ? g_strdup_printf(T("%d graphics devices", "그래픽 장치 %d개"), n)
              : g_strdup(T("No graphics device with a driver was found on this computer.",
                           "이 컴퓨터에서 드라이버가 붙은 그래픽 장치를 찾지 못했습니다."));
    gtk_box_append(GTK_BOX(b), page_head("GPU", sub, NULL));
    g_free(sub);
    for (int i = 0; i < n; i++)
        gtk_box_append(GTK_BOX(b), gpu_block(g_ptr_array_index(S.gpus, i), i, n > 1));
    return lp_kit_scroller(b, FALSE);
}

static void update_gpu(Gpu *g)
{
    GpuUi *u = g->ui;
    char t[160];
    gboolean known = g->asleep || !isnan(g->busy);
    gtk_widget_set_visible(GTK_WIDGET(u->graph), known);
    if (g->asleep) {
        graph_sampled(u->graph, T("asleep", "절전 중"));
    } else if (g->src == SRC_FDINFO && g->neng) {
        /* Each engine by name, in the order the driver lists them. */
        GString *d = g_string_new(NULL);
        for (int e = 0; e < g->neng && e < 4; e++)
            g_string_append_printf(d, "%s%s %.0f%%", e ? " · " : "",
                                   engine_label(g->eng[e]), g->eng_pct[e]);
        graph_sampled(u->graph, d->str);
        g_string_free(d, TRUE);
    } else if (g->mhz) {
        g_snprintf(t, sizeof t, "%d MHz", g->mhz);
        graph_sampled(u->graph, t);
    } else {
        graph_sampled(u->graph, NULL);
    }
    g_snprintf(t, sizeof t, "%d", g->mhz);
    stat_set(&u->st_clock, g->mhz ? t : "—", g->mhz ? "MHz" : "", FALSE);
    g_snprintf(t, sizeof t, "%d", g->max_mhz);
    stat_set(&u->st_max, g->max_mhz ? t : "—", g->max_mhz ? "MHz" : "", FALSE);
    if (!g->asleep && g->vram_total > 0 && g->vram_used >= 0) {
        char *tot = fmt_bytes((double)g->vram_total);
        char *un = g_strdup_printf(T("of %s", "/ %s"), tot);
        stat_bytes(&u->st_mem, (double)g->vram_used, FALSE);
        char *withun = g_strdup_printf("%s %s", gtk_label_get_text(GTK_LABEL(u->st_mem.unit)), un);
        gtk_label_set_text(GTK_LABEL(u->st_mem.unit), withun);
        g_free(withun);
        g_free(un);
        g_free(tot);
    } else if (g->fd_mem) {
        stat_bytes(&u->st_mem, (double)g->mem_fd, FALSE);
    } else {
        stat_set(&u->st_mem, "—", "", FALSE);
    }
    stat_set(&u->st_state, g->asleep ? T("Asleep", "절전 중") : T("Awake", "동작 중"), "", FALSE);
    const char *note;
    if (g->asleep)
        note = T("Asleep, drawing no power. It wakes when a program asks for it; this page "
                 "only reads that it is asleep and does not wake it.",
                 "절전 중이라 전력을 쓰지 않습니다. 프로그램이 요청하면 깨어나며, 이 화면은 "
                 "절전 상태만 읽고 깨우지 않습니다.");
    else if (g->src == SRC_AMD)
        note = T("Busy is the driver's own figure for the whole device.",
                 "사용률은 드라이버가 장치 전체에 대해 알려 준 값입니다.");
    else if (g->src == SRC_FDINFO)
        note = T("Busy is the time its busiest engine spent on the programs using it, as the "
                 "kernel counts it for each program (your own programs; another account's cannot "
                 "be read). Memory is what those programs have placed on it.",
                 "사용률은 커널이 프로그램마다 센, 가장 바쁜 엔진이 일한 시간의 비율입니다 (내 "
                 "프로그램만 읽을 수 있습니다). 메모리는 그 프로그램들이 이 장치에 올린 양입니다.");
    else if (g->src == SRC_RC6)
        note = T("Busy is how much of the time the GPU was not in its sleep state (RC6).",
                 "사용률은 GPU가 절전 상태(RC6)가 아니었던 시간의 비율입니다.");
    else
        note = T("This device does not report how busy it is, so there is no graph to draw.",
                 "이 장치는 사용률을 알려 주지 않습니다. 그래서 그래프를 그리지 않습니다.");
    gtk_label_set_text(GTK_LABEL(u->note), note);
}

/* ── Drives ───────────────────────────────────────────────────────── */

static GtkWidget *build_drives(void)
{
    GtkWidget *b = page_box();
    gtk_box_append(GTK_BOX(b), page_head(T("Drives", "드라이브"),
        T("How fast each disk is being read and written, and how full it is",
          "디스크마다 읽고 쓰는 속도와 남은 공간"), NULL));
    A->disks_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(b), A->disks_box);
    /* Filesystems on something that is not one of the disks above. */
    A->fs_other = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_append(GTK_BOX(b), A->fs_other);
    GtkWidget *n = label(T("Drive health (SMART) is in Disks.", "드라이브 상태(SMART)는 디스크 앱에 있습니다."),
                         "lpt-note", 0);
    gtk_widget_set_margin_top(n, 14);
    gtk_box_append(GTK_BOX(b), n);
    return lp_kit_scroller(b, FALSE);
}

static GtkWidget *disk_block(Disk *d, int index)
{
    DiskUi *u = g_new0(DiskUi, 1);
    d->ui = u;
    u->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    char *h = g_strdup_printf(T("Disk %d · %s", "디스크 %d · %s"), index, d->model);
    GtkWidget *hl = label(h, "lpt-dev", 0);
    gtk_label_set_ellipsize(GTK_LABEL(hl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_top(hl, index ? 26 : 0);
    gtk_box_append(GTK_BOX(u->box), hl);
    g_free(h);
    char *sz = fmt_bytes((double)d->size);
    char *sub = g_strdup_printf("%s · %s · %s", d->name, d->kind, sz);
    GtkWidget *sl = label(sub, "lpt-dim", 0);
    gtk_widget_set_margin_bottom(sl, 8);
    gtk_box_append(GTK_BOX(u->box), sl);
    g_free(sub);
    g_free(sz);
    u->graph = graph_new(&d->rd_h, &d->wr_h, FMT_RATE, T("Read", "읽기"), T("Write", "쓰기"));
    graph_small(u->graph, 170);
    gtk_box_append(GTK_BOX(u->box), GTK_WIDGET(u->graph));
    gtk_box_append(GTK_BOX(u->box), stats_row(
        stat_new(&u->st_rd, T("Reading", "읽기")),
        stat_new(&u->st_wr, T("Writing", "쓰기")),
        stat_new(&u->st_active, T("Active time", "활성 시간")),
        stat_new(&u->st_size, T("Capacity", "용량"))));
    stat_bytes(&u->st_size, (double)d->size, FALSE);
    u->fs_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(u->fs_box, 12);
    gtk_box_append(GTK_BOX(u->box), u->fs_box);
    return u->box;
}

static GtkWidget *fs_card(Fs *f)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(card, "lpt-fs");
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *n = label(f->name, "lpt-app-name", 0);
    gtk_box_append(GTK_BOX(top), n);
    gtk_box_append(GTK_BOX(top), label(f->dev, "lpt-dim", 0));
    GtkWidget *sp = label("", NULL, 0);
    gtk_widget_set_hexpand(sp, TRUE);
    gtk_box_append(GTK_BOX(top), sp);
    char *av = fmt_bytes((double)f->avail), *sz = fmt_bytes((double)f->size);
    char *t = g_strdup_printf(T("%s free of %s", "%s 중 %s 남음"),
                              lp_korean() ? sz : av, lp_korean() ? av : sz);
    gboolean low = f->size && (double)f->avail / (double)f->size < LOW_FREE;
    GtkWidget *tl = label(t, low ? "lpt-warn" : "lpt-num", 1);
    gtk_box_append(GTK_BOX(top), tl);
    g_free(t);
    g_free(av);
    g_free(sz);
    gtk_box_append(GTK_BOX(card), top);
    LptBars *m = bars_new(1, TRUE);
    m->colors[0] = low ? C_AMBER : C_LINE_A;
    lp_spring_jump(&m->s[0], f->size ? 1.0 - (double)f->avail / (double)f->size : 0);
    gtk_box_append(GTK_BOX(card), GTK_WIDGET(m));
    if (low) {
        GtkWidget *w = label(T("Less than 15% left. Empty the trash or move big files "
                               "to another drive before it fills up.",
                               "15% 미만 남았습니다. 가득 차기 전에 휴지통을 비우거나 "
                               "큰 파일을 다른 드라이브로 옮기세요."), "lpt-warn", 0);
        gtk_label_set_wrap(GTK_LABEL(w), TRUE);
        gtk_box_append(GTK_BOX(card), w);
    }
    return card;
}

static void clear_box(GtkWidget *box)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(box)))
        gtk_box_remove(GTK_BOX(box), c);
}

/* Each filesystem under the disk it is on. */
static void update_fs(void)
{
    sample_fs();
    for (guint i = 0; i < S.disks->len; i++)
        clear_box(((DiskUi *)((Disk *)g_ptr_array_index(S.disks, i))->ui)->fs_box);
    clear_box(A->fs_other);
    for (guint i = 0; i < S.fs->len; i++) {
        Fs *f = g_ptr_array_index(S.fs, i);
        GtkWidget *into = A->fs_other;
        for (guint k = 0; k < S.disks->len; k++) {
            Disk *d = g_ptr_array_index(S.disks, k);
            if (f->disk && !strcmp(f->disk, d->name))
                into = ((DiskUi *)d->ui)->fs_box;
        }
        gtk_box_append(GTK_BOX(into), fs_card(f));
    }
    gtk_widget_set_margin_top(A->fs_other, gtk_widget_get_first_child(A->fs_other) ? 18 : 0);
}

/* A disk came or went: the page is laid out again, and only then is an
 * unplugged disk's data freed - its graph drew from it until now. */
static void rebuild_disks(void)
{
    S.disks_changed = FALSE;
    clear_box(A->disks_box);
    for (guint i = S.disks->len; i-- > 0;) {
        Disk *d = g_ptr_array_index(S.disks, i);
        g_clear_pointer(&d->ui, g_free);
        if (!d->seen) {
            g_ptr_array_remove_index(S.disks, i);
            disk_free(d);
        }
    }
    for (guint i = 0; i < S.disks->len; i++)
        gtk_box_append(GTK_BOX(A->disks_box), disk_block(g_ptr_array_index(S.disks, i), (int)i));
    if (!S.disks->len)
        gtk_box_append(GTK_BOX(A->disks_box), label(T("No disks found.", "디스크를 찾지 못했습니다."),
                                                     "lpt-note", 0));
    update_fs();
}

/* ── Network ──────────────────────────────────────────────────────── */

static GtkWidget *build_network(void)
{
    GtkWidget *b = page_box();
    gtk_box_append(GTK_BOX(b), page_head(T("Network", "네트워크"),
        T("Everything this computer sends and receives", "이 컴퓨터가 주고받는 모든 것"), NULL));
    A->g_net = graph_new(&S.rx_h, &S.tx_h, FMT_RATE, T("Receive", "받기"), T("Send", "보내기"));
    gtk_box_append(GTK_BOX(b), GTK_WIDGET(A->g_net));
    gtk_box_append(GTK_BOX(b), stats_row(
        stat_new(&A->st_rx, T("Receiving", "받는 중")),
        stat_new(&A->st_tx, T("Sending", "보내는 중")),
        stat_new(&A->st_rx_tot, T("Received since start-up", "켠 뒤 받은 양")),
        stat_new(&A->st_tx_tot, T("Sent since start-up", "켠 뒤 보낸 양"))));
    A->net_wifi = label("", "lpt-note", 0);
    gtk_widget_set_margin_top(A->net_wifi, 12);
    gtk_box_append(GTK_BOX(b), A->net_wifi);
    gtk_box_append(GTK_BOX(b), section(T("Connections", "연결")));
    A->net_ifaces = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(b), A->net_ifaces);
    return lp_kit_scroller(b, FALSE);
}

static void update_ifaces(void)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(A->net_ifaces)))
        gtk_box_remove(GTK_BOX(A->net_ifaces), c);
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0)
        return;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || !strcmp(i->ifa_name, "lo"))
            continue;
        int fam = i->ifa_addr->sa_family;
        if (fam != AF_INET && fam != AF_INET6)
            continue;
        char addr[64] = "";
        if (fam == AF_INET)
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, addr, sizeof addr);
        else
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr, addr, sizeof addr);
        kv_row(A->net_ifaces, i->ifa_name, addr);
    }
    freeifaddrs(ifa);
    if (!gtk_widget_get_first_child(A->net_ifaces))
        kv_row(A->net_ifaces, T("No connection", "연결 없음"), "");
}

/* lp-net status --json (the networking track's contract) for the Wi-Fi
 * line; the JSON is small and flat enough for two lookups by hand. */
static char *json_str(const char *j, const char *key)
{
    char *k = g_strdup_printf("\"%s\":\"", key);
    const char *p = strstr(j, k);
    g_free(k);
    if (!p)
        return NULL;
    p = strchr(p, ':') + 2;
    const char *e = strchr(p, '"');
    return e ? g_strndup(p, (gsize)(e - p)) : NULL;
}

static int json_int(const char *j, const char *key, int dflt)
{
    char *k = g_strdup_printf("\"%s\":", key);
    const char *p = strstr(j, k);
    g_free(k);
    return p ? atoi(strchr(p, ':') + 1) : dflt;
}

static void on_lpnet(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    char *out = NULL;
    if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, NULL) || !out ||
        !A->win) {
        g_free(out);
        return;
    }
    char *state = json_str(out, "state"), *ssid = json_str(out, "ssid");
    int q = json_int(out, "quality", -1);
    char *t;
    if (state && !strcmp(state, "connected") && ssid && *ssid)
        t = g_strdup_printf(T("Wi-Fi: %s, signal %d%%", "Wi-Fi: %s, 신호 %d%%"), ssid, q);
    else
        t = g_strdup(T("Wi-Fi: not connected", "Wi-Fi: 연결 안 됨"));
    gtk_label_set_text(GTK_LABEL(A->net_wifi), t);
    g_free(t);
    g_free(state);
    g_free(ssid);
    g_free(out);
}

static void query_lpnet(void)
{
    char *p = g_find_program_in_path("lp-net");
    if (!p)
        return;
    const char *argv[] = { p, "status", "--json", NULL };
    GSubprocess *sp = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                        G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    if (sp) {
        g_subprocess_communicate_utf8_async(sp, NULL, NULL, on_lpnet, NULL);
        g_object_unref(sp);
    }
    g_free(p);
}

/* ── Battery ──────────────────────────────────────────────────────── */

static GtkWidget *build_battery(void)
{
    GtkWidget *b = page_box();
    gtk_box_append(GTK_BOX(b), page_head(T("Battery", "배터리"), NULL, &A->bat_state));
    A->g_bat = graph_new(&A->bat_series, NULL, FMT_PERCENT, NULL, NULL);
    A->g_bat->caption = g_strdup(T("Since this window opened, up to 3 hours",
                                   "이 창을 연 뒤부터, 최대 3시간"));
    gtk_box_append(GTK_BOX(b), GTK_WIDGET(A->g_bat));
    gtk_box_append(GTK_BOX(b), stats_row(
        stat_new(&A->st_bat_pct, T("Charge", "잔량")),
        stat_new(&A->st_bat_time, T("Time left", "남은 시간")),
        stat_new(&A->st_bat_w, T("Using now", "현재 소비")),
        stat_new(&A->st_bat_health, T("Health", "수명"))));
    A->bat_note = label("", "lpt-hint", 0);
    gtk_label_set_wrap(GTK_LABEL(A->bat_note), TRUE);
    gtk_widget_set_halign(A->bat_note, GTK_ALIGN_START);
    gtk_widget_set_margin_top(A->bat_note, 16);
    gtk_box_append(GTK_BOX(b), A->bat_note);
    return lp_kit_scroller(b, FALSE);
}

/* ═══════════════════════════════════════════════════════════════════
 * The once-a-second update
 * ═══════════════════════════════════════════════════════════════════ */

static void update_pages(void)
{
    char t[128];
    /* CPU */
    char *det;
    if (!isnan(S.temp))
        det = g_strdup_printf("%.2f GHz · %.0f °C", S.mhz / 1000, S.temp);
    else
        det = S.mhz > 0 ? g_strdup_printf("%.2f GHz", S.mhz / 1000) : g_strdup("");
    graph_sampled(A->g_cpu, det);
    g_free(det);
    for (int i = 0; i < S.ncpu; i++)
        bars_set(A->cores, i, S.core[i]);
    bars_kick(A->cores);
    g_snprintf(t, sizeof t, "%d", S.nprocs);
    stat_set(&A->st_procs, t, "", FALSE);
    g_snprintf(t, sizeof t, "%d", S.nthreads);
    stat_set(&A->st_threads, t, "", FALSE);
    char *up = fmt_duration(S.uptime);
    stat_set(&A->st_uptime, up, "", FALSE);
    g_free(up);
    if (!isnan(S.temp)) {
        g_snprintf(t, sizeof t, "%.0f", S.temp);
        stat_set(&A->st_temp, t, "°C", S.temp >= 85);
    } else {
        stat_set(&A->st_temp, "—", T("no sensor", "센서 없음"), FALSE);
    }

    /* Memory */
    guint64 used = S.mem_total - S.mem_avail;
    char *u = fmt_bytes((double)used), *tot = fmt_bytes((double)S.mem_total);
    char *md = g_strdup_printf(T("%s of %s", "%s / %s"), u, tot);
    graph_sampled(A->g_mem, md);
    g_free(md);
    g_free(u);
    g_free(tot);
    if (S.mem_total) {
        guint64 inuse = S.mem_total - S.mem_free - S.mem_cache;
        bars_set(A->mem_bar, 0, (double)MIN(inuse, S.mem_total) / (double)S.mem_total);
        bars_set(A->mem_bar, 1, (double)S.mem_cache / (double)S.mem_total);
        bars_kick(A->mem_bar);
    }
    stat_bytes(&A->st_mem_used, (double)used, FALSE);
    stat_bytes(&A->st_mem_avail, (double)S.mem_avail, FALSE);
    stat_bytes(&A->st_mem_cache, (double)S.mem_cache, FALSE);
    if (S.swap_total)
        stat_bytes(&A->st_swap, (double)(S.swap_total - S.swap_free), FALSE);
    else
        stat_set(&A->st_swap, "—", T("no swap", "스왑 없음"), FALSE);

    /* CPU packages */
    for (int k = 0; A->g_pkg && k < S.npkg; k++) {
        char *pd = isnan(S.pkg[k].temp) ? NULL : g_strdup_printf("%.0f °C", S.pkg[k].temp);
        graph_sampled(A->g_pkg[k], pd);
        g_free(pd);
    }

    /* GPU */
    for (guint i = 0; i < S.gpus->len; i++)
        update_gpu(g_ptr_array_index(S.gpus, i));

    /* Drives */
    for (guint i = 0; i < S.disks->len; i++) {
        Disk *d = g_ptr_array_index(S.disks, i);
        DiskUi *u = d->ui;
        char *wr = fmt_rate(d->wr);
        char *dd = g_strdup_printf(T("write %s", "쓰기 %s"), wr);
        graph_sampled(u->graph, dd);
        g_free(dd);
        g_free(wr);
        stat_bytes(&u->st_rd, d->rd, TRUE);
        stat_bytes(&u->st_wr, d->wr, TRUE);
        g_snprintf(t, sizeof t, "%.0f", d->active);
        stat_set(&u->st_active, t, "%", d->active >= 90);
    }

    /* Network */
    graph_sampled(A->g_net, NULL);
    char *tx = fmt_rate(S.tx);
    g_free(A->g_net->detail);
    A->g_net->detail = g_strdup_printf(T("send %s", "보내기 %s"), tx);
    g_free(tx);
    stat_bytes(&A->st_rx, S.rx, TRUE);
    stat_bytes(&A->st_tx, S.tx, TRUE);
    stat_bytes(&A->st_rx_tot, (double)S.rx_total, FALSE);
    stat_bytes(&A->st_tx_tot, (double)S.tx_total, FALSE);

    /* Battery */
    if (!S.bat) {
        gtk_label_set_text(GTK_LABEL(A->bat_state), T("This computer has no battery.",
                                                      "이 컴퓨터에는 배터리가 없습니다."));
        graph_sampled(A->g_bat, "");
        gtk_widget_set_visible(A->bat_note, FALSE);
    } else {
        const char *st = S.bat_state ? S.bat_state : "";
        const char *sttext = !strcmp(st, "Charging") ? T("Charging", "충전 중")
                           : !strcmp(st, "Discharging") ? T("On battery", "배터리 사용 중")
                           : !strcmp(st, "Full") ? T("Full", "완충")
                           : T("Plugged in", "전원 연결됨");
        gtk_label_set_text(GTK_LABEL(A->bat_state), sttext);
        graph_sampled(A->g_bat, sttext);
        g_snprintf(t, sizeof t, "%d", S.bat_pct);
        stat_set(&A->st_bat_pct, t, "%", S.bat_pct <= 15);
        if (S.bat_min >= 0) {
            char *d = fmt_duration(S.bat_min * 60.0);
            stat_set(&A->st_bat_time, d, "", FALSE);
            g_free(d);
        } else {
            stat_set(&A->st_bat_time, "—", "", FALSE);
        }
        if (S.bat_w >= 0) {
            g_snprintf(t, sizeof t, "%.1f", S.bat_w);
            stat_set(&A->st_bat_w, t, "W", FALSE);
        } else {
            stat_set(&A->st_bat_w, "—", "", FALSE);
        }
        if (S.bat_health >= 0) {
            g_snprintf(t, sizeof t, "%.0f", S.bat_health);
            char *cy = S.bat_cycles >= 0 ? g_strdup_printf(T("%% · %d cycles", "%% · %d회 충전"),
                                                          S.bat_cycles) : g_strdup("%");
            stat_set(&A->st_bat_health, t, cy, S.bat_health < 70);
            g_free(cy);
        } else {
            stat_set(&A->st_bat_health, "—", "", FALSE);
        }
        /* The sentence the number needs. */
        if (!strcmp(st, "Discharging") && S.bat_min >= 0 && S.bat_w > 0) {
            char *d = fmt_duration(S.bat_min * 60.0);
            char *h = g_strdup_printf(T("At %.1f W the battery lasts about %s more. Lower "
                                        "brightness is the biggest saving on this screen.",
                                        "%.1f W 로 쓰면 약 %s 더 쓸 수 있습니다. 이 화면에서는 "
                                        "밝기를 낮추는 것이 가장 크게 아낍니다."), S.bat_w, d);
            gtk_label_set_text(GTK_LABEL(A->bat_note), h);
            gtk_widget_set_visible(A->bat_note, TRUE);
            g_free(h);
            g_free(d);
        } else {
            gtk_widget_set_visible(A->bat_note, FALSE);
        }
    }
}

static gboolean sample_tick(gpointer d)
{
    (void)d;
    gint64 now = g_get_monotonic_time();
    double secs = A->last_sample ? (now - A->last_sample) / 1e6 : 1.0;
    A->last_sample = now;
    S.now = now;
    sample_cpu();
    sample_mem();
    sample_disks(secs);
    sample_net(secs);
    /* The walk for who has a GPU open: see the comment above find_gpus. */
    gboolean visible = !window_minimised();
    if (S.ticks % 30 == 0 || (visible && page_is("gpu") && S.ticks % 5 == 0))
        scan_drm_fds();
    S.ticks++;
    sample_gpus(secs);
    S.temp = temp_path ? read_num(temp_path, -1000000) / 1000.0 : NAN;
    if (S.temp < -100)
        S.temp = NAN;
    for (int k = 0; k < S.npkg; k++)
        if (S.pkg[k].temp_path) {
            S.pkg[k].temp = read_num(S.pkg[k].temp_path, -1000000) / 1000.0;
            if (S.pkg[k].temp < -100)
                S.pkg[k].temp = NAN;
        }
    sample_battery();
    if (S.disks_changed)
        rebuild_disks();
    if (S.primed) {
        series_push(&S.cpu_h, now, S.cpu);
        series_push(&S.mem_h, now, S.mem_total ?
                    100.0 * (double)(S.mem_total - S.mem_avail) / (double)S.mem_total : 0);
        for (int k = 0; k < S.npkg; k++)
            series_push(&S.pkg[k].h, now, S.pkg[k].pct);
        for (guint i = 0; i < S.disks->len; i++) {
            Disk *d = g_ptr_array_index(S.disks, i);
            series_push(&d->rd_h, now, d->rd);
            series_push(&d->wr_h, now, d->wr);
        }
        series_push(&S.rx_h, now, S.rx);
        series_push(&S.tx_h, now, S.tx);
        for (guint i = 0; i < S.gpus->len; i++) {
            Gpu *g = g_ptr_array_index(S.gpus, i);
            if (!isnan(g->busy))
                series_push(&g->h, now, g->busy);
        }
        if (S.bat)
            series_push(&A->bat_series, now, S.bat_pct);
    }
    S.primed = TRUE;

    /* The walk over /proc only when someone can see its result. */
    if (visible && (page_is("apps") || page_is("processes") || page_is("memory"))) {
        scan_procs();
        if (page_is("apps"))
            update_apps();
        if (page_is("memory"))
            update_mem_top();
        if (page_is("processes")) {
            gtk_sorter_changed(A->proc_sorter, GTK_SORTER_CHANGE_DIFFERENT);
            char t[96];
            g_snprintf(t, sizeof t, T("%d processes, %d threads", "프로세스 %d개, 스레드 %d개"),
                       S.nprocs, S.nthreads);
            gtk_label_set_text(GTK_LABEL(A->proc_count), t);
        }
    } else if (visible && page_is("cpu")) {
        count_procs();
    }
    if (visible && ++A->slow_count % 5 == 1) {
        if (page_is("drives"))
            update_fs();
        if (page_is("network")) {
            update_ifaces();
            query_lpnet();
        }
    }
    if (visible)
        update_pages();
    return G_SOURCE_CONTINUE;
}

/* ── the sidebar ──────────────────────────────────────────────────── */

static const struct { const char *id, *icon, *en, *ko; gboolean group_before; } PAGES[] = {
    { "apps",      "view-app-grid-symbolic",      "Apps",      "앱", FALSE },
    { "processes", "view-list-symbolic",          "Processes", "프로세스", FALSE },
    { "cpu",       "computer-symbolic",           "CPU",       "CPU", TRUE },
    { "memory",    "media-flash-symbolic",        "Memory",    "메모리", FALSE },
    { "gpu",       "video-display-symbolic",      "GPU",       "GPU", FALSE },
    { "drives",    "drive-harddisk-symbolic",     "Drives",    "드라이브", FALSE },
    { "network",   "network-wired-symbolic",      "Network",   "네트워크", FALSE },
    { "battery",   "battery-full-symbolic",       "Battery",   "배터리", FALSE },
};

/* The page to open with next time is written only once the page has
 * been on screen for a few seconds. A page that took the program down
 * (the Memory page did, in 1.405) was otherwise the one remembered, and
 * every start after went straight back to it and down again - Task
 * Manager then never opened at all. Now a page that fails is never the
 * one written, and a start on a remembered page writes "apps" first, so
 * a failure there too is only ever one failed start. */
static guint page_keep_id;

static void page_keep_now(void)
{
    if (page_keep_id) {
        g_source_remove(page_keep_id);
        page_keep_id = 0;
    }
    g_key_file_set_string(A->state, "tasks", "page", A->page ? A->page : "apps");
    lp_kit_state_save(A->state, STATE_NAME);
}

static gboolean page_keep_tick(gpointer d)
{
    (void)d;
    page_keep_id = 0;
    page_keep_now();
    return G_SOURCE_REMOVE;
}

static void page_keep_later(void)
{
    if (page_keep_id)
        g_source_remove(page_keep_id);
    page_keep_id = g_timeout_add_seconds(4, page_keep_tick, NULL);
}

static void on_shutdown(GApplication *app, gpointer d)
{
    (void)app; (void)d;
    if (page_keep_id)       /* closed normally before the page settled */
        page_keep_now();
}

static void go_page(const char *id)
{
    g_free(A->page);
    A->page = g_strdup(id);
    lp_kit_stack_show(GTK_STACK(A->stack), id);
    page_keep_later();
    /* A page just arrived at should not wait a second for its numbers. */
    sample_tick(NULL);
}

static void on_side(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
    (void)lb; (void)d;
    if (!row)
        return;
    const char *id = g_object_get_data(G_OBJECT(row), "page");
    if (id)
        go_page(id);
}

static GtkWidget *build_sidebar(void)
{
    GtkWidget *lb = gtk_list_box_new();
    gtk_widget_add_css_class(lb, "lp-kit-side");
    for (guint i = 0; i < G_N_ELEMENTS(PAGES); i++) {
        if (PAGES[i].group_before) {
            GtkWidget *g = label(T("Resources", "자원"), "lp-kit-group", 0);
            gtk_widget_set_margin_top(g, 14);
            gtk_widget_set_margin_start(g, 10);
            gtk_widget_set_margin_bottom(g, 4);
            gtk_list_box_append(GTK_LIST_BOX(lb), g);
            GtkListBoxRow *gr = gtk_list_box_get_row_at_index(GTK_LIST_BOX(lb),
                (int)i + (i > 0 && PAGES[0].group_before ? 1 : 0));
            GtkWidget *last = GTK_WIDGET(gr);
            (void)last;
            GtkWidget *row = gtk_widget_get_parent(g);
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
            gtk_widget_add_css_class(row, "lp-kit-group-row");
        }
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(PAGES[i].icon));
        gtk_box_append(GTK_BOX(box), label(T(PAGES[i].en, PAGES[i].ko), NULL, 0));
        gtk_list_box_append(GTK_LIST_BOX(lb), box);
        GtkWidget *row = gtk_widget_get_parent(box);
        g_object_set_data(G_OBJECT(row), "page", (gpointer)PAGES[i].id);
        char *nm = g_strdup_printf("side-%s", PAGES[i].id);
        gtk_widget_set_name(row, nm);
        g_free(nm);
    }
    g_signal_connect(lb, "row-activated", G_CALLBACK(on_side), NULL);
    GtkWidget *sc = lp_kit_scroller(lb, FALSE);
    gtk_widget_add_css_class(sc, "lp-kit-sidebar");
    gtk_widget_set_size_request(sc, SIDEBAR_W, -1);
    gtk_widget_set_hexpand(sc, FALSE);
    A->sidebar = lb;
    return sc;
}

static void select_side(const char *id)
{
    for (GtkWidget *c = gtk_widget_get_first_child(A->sidebar); c;
         c = gtk_widget_get_next_sibling(c)) {
        const char *p = g_object_get_data(G_OBJECT(c), "page");
        if (p && !strcmp(p, id))
            gtk_list_box_select_row(GTK_LIST_BOX(A->sidebar), GTK_LIST_BOX_ROW(c));
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Start
 * ═══════════════════════════════════════════════════════════════════ */

static const char APP_CSS[] =
    ".lpt-main { background-color: #0e0e0e; }\n"
    ".lpt-page { padding: 20px 26px 22px 26px; }\n"
    ".lpt-h1 { font-size: 20px; font-weight: 600; letter-spacing: -0.02em; }\n"
    ".lpt-sub { font-size: 13px; color: #6a6a6a; }\n"
    ".lpt-sec { font-size: 12px; color: #6a6a6a; }\n"
    "lptgraph { background-color: #232323; border-radius: 10px; min-height: 230px; }\n"
    "lptgraph.small { min-height: 150px; }\n"
    ".lpt-dev { font-size: 15px; font-weight: 600; }\n"
    ".lpt-stat { background-color: #232323; border-radius: 8px; padding: 11px 13px; }\n"
    ".lpt-stat-k { font-size: 12px; color: #6a6a6a; }\n"
    ".lpt-stat-v { font-size: 20px; font-weight: 600; font-feature-settings: 'tnum'; }\n"
    ".lpt-stat-u { font-size: 12px; color: #6a6a6a; }\n"
    ".lpt-stat.warn .lpt-stat-v { color: #f0b350; }\n"
    ".lpt-kv { padding: 7px 0; border-bottom: 0.5px solid #1e1e1e; }\n"
    ".lpt-kv-k { font-size: 13px; color: #a8a8a8; }\n"
    ".lpt-kv-v { font-size: 13px; color: #e8e8e8; font-feature-settings: 'tnum'; }\n"
    ".lpt-colhead { font-size: 12px; color: #6a6a6a; padding: 0 12px 6px 12px;"
    "  border-bottom: 0.5px solid #333333; }\n"
    ".lpt-list { background: none; }\n"
    ".lpt-list > row { padding: 0; background: none; }\n"
    ".lpt-app-row { padding: 6px 12px; min-height: 48px; border-bottom: 0.5px solid #262626; }\n"
    ".lpt-app-name { font-size: 14px; font-weight: 500; }\n"
    ".lpt-dim { font-size: 12px; color: #6a6a6a; }\n"
    ".lpt-num { font-size: 13px; font-feature-settings: 'tnum'; }\n"
    ".lpt-num.hot { color: #f0b350; }\n"
    ".lpt-warn { font-size: 13px; color: #f0b350; }\n"
    ".lpt-note { font-size: 12px; color: #6a6a6a; }\n"
    ".lpt-hint { background-color: #2a2113; color: #f0b350; border: 0.5px solid #4a3510;"
    "  border-radius: 7px; padding: 9px 13px; font-size: 13px; }\n"
    ".lpt-action { min-height: 40px; min-width: 88px; border-radius: 8px; }\n"
    ".lpt-search { min-height: 40px; border-radius: 8px; }\n"
    ".lpt-procs { background: none; }\n"
    ".lpt-procs listview row { min-height: 40px; }\n"
    ".lpt-procs listview row cell { padding: 0 10px; }\n"
    ".lpt-cell-name { font-size: 13px; }\n"
    ".lpt-fs { background-color: #232323; border-radius: 8px; padding: 12px 14px; }\n"
    ".lpt-dot { min-width: 10px; min-height: 10px; border-radius: 5px; }\n"
    ".lpt-dot.a { background-color: #e8e8e8; }\n"
    ".lpt-dot.b { background-color: rgba(127,184,160,0.8); }\n"
    ".lpt-dot.c { background-color: rgba(255,255,255,0.12); }\n"
    ".lp-kit-group-row { min-height: 0; }\n";

static void on_drive(const char *verb, const char *arg, gpointer d)
{
    (void)d;
    if (!strcmp(verb, "page"))
        go_page(arg), select_side(arg);
}

static char *opt_page;
static guint tick_id;

/* The second's tick and lp-net's answer stop touching the window's
 * widgets the moment it is destroyed: either could otherwise run in the
 * same main-loop pass as the close and read a freed widget. */
static void on_win_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (tick_id) {
        g_source_remove(tick_id);
        tick_id = 0;
    }
    A->win = NULL;
}

static void on_activate(GtkApplication *app, gpointer d)
{
    (void)d;
    if (A->win) {
        gtk_window_present(GTK_WINDOW(A->win));
        return;
    }
    lp_kit_style(APP_CSS);
    A->gapp = app;
    A->win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(A->win), T("Task Manager", "작업 관리자"));
    lp_fit_default_size(GTK_WINDOW(A->win), WINDOW_W, WINDOW_H);
    gtk_window_set_icon_name(GTK_WINDOW(A->win), "utilities-system-monitor");

    static const GActionEntry acts[] = {
        { "proc-end", act_proc, NULL, NULL, NULL, { 0 } },
        { "proc-kill", act_proc, NULL, NULL, NULL, { 0 } },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(A->win), acts, 1, GINT_TO_POINTER(SIGTERM));
    g_action_map_add_action_entries(G_ACTION_MAP(A->win), acts + 1, 1, GINT_TO_POINTER(SIGKILL));

    /* The window list, before the Apps page is built: the page says
     * which of its two ways it is listing things. */
    A->win_apps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        unref_or_null);
    A->windows = lp_toplevels_init();
    if (A->windows)
        lp_toplevels_watch(on_windows, NULL);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(outer), build_sidebar());
    A->stack = lp_kit_stack();
    gtk_widget_add_css_class(A->stack, "lpt-main");
    gtk_widget_set_hexpand(A->stack, TRUE);
    gtk_widget_set_vexpand(A->stack, TRUE);
    gtk_stack_add_named(GTK_STACK(A->stack), build_apps(), "apps");
    gtk_stack_add_named(GTK_STACK(A->stack), build_processes(), "processes");
    gtk_stack_add_named(GTK_STACK(A->stack), build_cpu(), "cpu");
    gtk_stack_add_named(GTK_STACK(A->stack), build_memory(), "memory");
    gtk_stack_add_named(GTK_STACK(A->stack), build_gpu(), "gpu");
    gtk_stack_add_named(GTK_STACK(A->stack), build_drives(), "drives");
    gtk_stack_add_named(GTK_STACK(A->stack), build_network(), "network");
    gtk_stack_add_named(GTK_STACK(A->stack), build_battery(), "battery");
    gtk_box_append(GTK_BOX(outer), A->stack);
    gtk_window_set_child(GTK_WINDOW(A->win), outer);

    char *saved = g_key_file_get_string(A->state, "tasks", "page", NULL);
    const char *page = opt_page ? opt_page : saved ? saved : "apps";
    gboolean known = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(PAGES); i++)
        known |= !strcmp(PAGES[i].id, page);
    if (!known)
        page = "apps";
    A->page = g_strdup(page);
    if (strcmp(page, "apps") != 0) {
        g_key_file_set_string(A->state, "tasks", "page", "apps");
        lp_kit_state_save(A->state, STATE_NAME);
        page_keep_later();
    }
    gtk_stack_set_transition_duration(GTK_STACK(A->stack), 0);
    gtk_stack_set_visible_child_name(GTK_STACK(A->stack), page);
    gtk_stack_set_transition_duration(GTK_STACK(A->stack), 260);
    select_side(page);
    g_free(saved);

    gtk_window_present(GTK_WINDOW(A->win));
    lp_kit_drive(on_drive, NULL);
    g_signal_connect(A->win, "destroy", G_CALLBACK(on_win_destroy), NULL);
    sample_tick(NULL);
    tick_id = g_timeout_add_seconds(1, sample_tick, NULL);
}

int main(int argc, char **argv)
{
    int out = 1;
    for (int i = 1; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "--page="))
            opt_page = g_strdup(argv[i] + 7);
        else
            argv[out++] = argv[i];
    }
    argc = out;
    argv[argc] = NULL;

    page_size = sysconf(_SC_PAGESIZE);
    clk_tck = sysconf(_SC_CLK_TCK);
    A = g_new0(App, 1);
    A->state = lp_kit_state_load(STATE_NAME);
    A->app_rows = g_hash_table_new(g_str_hash, g_str_equal);
    procs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_object_unref);
    proc_store = g_list_store_new(LPT_TYPE_PROC);
    users = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    S.temp = NAN;
    S.disks = g_ptr_array_new();
    S.not_disks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    read_cpuinfo();
    sample_mem();           /* the Memory page's "16 GB installed" */
    find_temp();
    find_gpus();
    find_battery();
    load_exe_apps();

    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
