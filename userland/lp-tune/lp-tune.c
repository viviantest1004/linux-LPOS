/* lp-tune - power profiles, the battery, and the backlight.
 *
 *   lp-tune                          what is in force, and why
 *   lp-tune status [--json]          the same, as JSON for the top bar
 *   lp-tune set auto|balanced|saver|performance
 *   lp-tune brightness get           0-100
 *   lp-tune brightness set N|+N|-N   N percent, or a step up or down;
 *                                    glides there in ~240ms, and the
 *                                    daemon restores the last value at boot
 *   lp-tune suspend                  lock the session, then sleep
 *   lp-tune config [get]             the lid / power-button settings
 *   lp-tune config set KEY VALUE     change one (validated)
 *   lp-tune -d                       the daemon, run from /etc/services
 *
 *   --sysfs DIR   read and write under DIR instead of /  (tests)
 *   --dry-run     print every write instead of doing it
 *
 * ── Why this exists ──
 * A laptop left to the kernel's defaults runs every device at full power
 * for ever. None of it is wrong - the defaults are chosen so that no
 * machine anywhere misbehaves - but on a Dell XPS 15 9550 the price is
 * an hour or more of battery. The GTX 960M alone is the worst of it: on
 * an Optimus laptop the screen is wired to the Intel GPU, the NVIDIA one
 * is idle nearly always, and it only switches off when its PCI device is
 * allowed to runtime-suspend. Nothing allows that by default.
 *
 * So this is the whole of what a distribution normally spreads over
 * power-profiles-daemon, TLP, udev rules and a brightness helper, in one
 * place that can be read top to bottom:
 *
 *   CPU        intel_pstate's energy/performance preference, and turbo
 *   PCIe       link power management (ASPM), and runtime PM for every
 *              device - which is what powers the dGPU off
 *   USB        autosuspend for everything except input devices
 *   SATA       link power management, for the models that have SATA
 *   audio      the HDA codec's power save
 *   kernel     the NMI watchdog, and how long dirty pages wait
 *   backlight  the one place brightness is written
 *
 * ── Profiles ──
 * Three that do something and one that chooses between them:
 *
 *   saver        on battery. Slower, quieter, much longer.
 *   balanced     plugged in. Everything power-managed that costs nothing
 *                you would notice, turbo on.
 *   performance  when asked. Turbo, no ASPM, no audio power save.
 *   auto         balanced on AC, saver on battery, switched the moment
 *                the charger goes in or comes out. The default.
 *
 * Every profile keeps PCI runtime PM on. "Performance" is about the CPU;
 * the NVIDIA GPU is not drawing the screen in any of them, and letting
 * it stay powered would be spending battery on nothing.
 *
 * ── What is never power-managed ──
 * Input devices. USB autosuspend on a touch controller adds the resume
 * time to the first touch after a pause - a finger on the glass and a
 * tenth of a second of nothing - and the owner's first requirement is
 * that touch is perfect. Any USB device with a HID interface stays on.
 *
 * ── Brightness ──
 * Writing /sys/class/backlight needs root, and the person dragging the
 * brightness slider is not root. Every desktop solves that somehow; this
 * machine solves it here: the daemon owns the backlight and anyone at the
 * machine may ask it for a new value. It never goes below 1% - a panel at
 * zero looks switched off, and on a touch laptop with no keyboard handy
 * there is then no way to find the slider again.
 *
 * ── The lid and the power button ──
 * On a normal distribution logind watches both. There is no logind here,
 * so the daemon opens the ACPI "Lid Switch" and "Power Button" input
 * devices itself and does what /etc/lp-tune.conf says:
 *
 *   lid_on_battery=suspend|nothing|poweroff      (suspend)
 *   lid_on_ac=suspend|nothing|poweroff           (suspend)
 *   power_button=suspend|poweroff|nothing        (suspend; poweroff in a VM)
 *   lock_before_suspend=yes|no                   (yes)
 *   idle_suspend_minutes_battery=N               (15; 0 = never)
 *   idle_suspend_minutes_ac=N                    (0)
 *
 * The idle values are for the session's idle watcher, which can see
 * whether anyone is touching the machine and this daemon cannot; it
 * reads the same file and asks for `lp-tune suspend`.
 *
 * Closing the lid with an external monitor plugged in does nothing -
 * that is a laptop on a desk being used as a desktop, and sleeping would
 * blank the screen somebody is looking at.
 *
 * Suspending locks the session FIRST, and waits for the lock to be on
 * the screen: a laptop opened in a cafe must come back to a lock screen,
 * never to the desktop for a moment before the lock catches up.
 *
 * ── Who may ask ──
 * The socket is /run/lp-tune.sock and the daemon asks the kernel who is
 * on the other end (SO_PEERCRED): root, or a real account (uid >= 1000),
 * which is the rule lp-power uses and the rule polkit arrives at for the
 * person at the console. A service account may not. Only four words are
 * accepted, and the only effect any of them can have is on power and
 * brightness, so this is a convenience boundary, not a security one.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"
#include "syscall.h"

#define SOCK_PATH     "/run/lp-tune.sock"
#define PROFILE_FILE  "/etc/lp-tune.profile"
/* The last brightness someone chose, restored when the daemon starts.
 * Written when a glide ends, so a slider drag is one write, not thirty
 * a second. */
#define BRIGHT_DIR    "/var/lib/lp-tune"
#define BRIGHT_FILE   "/var/lib/lp-tune/brightness"
#define AF_UNIX_      1
#define AF_NETLINK_   16
#define NETLINK_KOBJECT_UEVENT_ 15
#define SO_PEERCRED_  17
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

/* ── Options that apply everywhere ─────────────────────────────────── */

static char root[256];        /* "" normally; a fake tree in tests */
static bool dry_run;

/* ── Small file helpers, all relative to `root` ────────────────────── */

static void rooted(const char *path, char *out, size_t n)
{
    snprintf(out, n, "%s%s", root, path);
}

/* Read a sysfs attribute, newline stripped. false if it is not there. */
static bool rd(const char *path, char *buf, size_t n)
{
    char p[512];
    rooted(path, p, sizeof p);
    long got = proc_read(p, buf, n);
    if (got <= 0)
        return false;
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return true;
}

static long rd_num(const char *path, long dflt)
{
    char b[64];
    if (!rd(path, b, sizeof b))
        return dflt;
    return strtol(b, NULL, 10);
}

/* Write one value. Counts what it did, so the report can say "applied
 * 23 settings, 2 not present" rather than nothing at all. A missing
 * attribute is not an error - most of the table is for hardware a given
 * machine does not have. A present one that refuses the value is, and is
 * said once. */
static int n_written, n_absent, n_refused;

static void wr(const char *path, const char *val)
{
    char p[512];
    rooted(path, p, sizeof p);

    if (!lp_exists(p)) {
        n_absent++;
        return;
    }
    /* Do not rewrite a value that is already right: a sysfs write can
     * have side effects (a USB device resumed to re-read its policy),
     * and doing it every time the charger is touched is pointless. */
    char cur[128];
    if (proc_read(p, cur, sizeof cur) > 0) {
        char *nl = strchr(cur, '\n');
        if (nl) *nl = '\0';
        /* The ASPM policy file shows every choice with the current one
         * in brackets: "default performance [powersave] powersupersave" */
        char want[64];
        snprintf(want, sizeof want, "[%s]", val);
        if (strcmp(cur, val) == 0 || strstr(cur, want)) {
            n_written++;
            return;
        }
    }
    if (dry_run) {
        printf("  write %s = %s\n", path, val);
        n_written++;
        return;
    }
    long fd = lp_open(p, O_WRONLY | O_TRUNC, 0);
    if (fd < 0) {
        n_refused++;
        dprintf(STDERR_FILENO, "lp-tune: cannot open %s (%ld)\n", path, -fd);
        return;
    }
    long w = lp_write((int)fd, val, strlen(val));
    lp_close((int)fd);
    if (w == -95 || w == -22) {
        /* EOPNOTSUPP / EINVAL: the file is there but this hardware has no
         * such setting (a VM's SATA links, a controller without DIPM) -
         * the same as the file not being there, not a refusal worth a
         * line on the console at every boot. */
        n_absent++;
    } else if (w < 0) {
        n_refused++;
        dprintf(STDERR_FILENO, "lp-tune: %s refused \"%s\" (%ld)\n",
                path, val, -w);
    } else {
        n_written++;
    }
}

/* Call fn(dir/name) for every entry of a directory, skipping dot files. */
static void each_entry(const char *dir, void (*fn)(const char *path, void *),
                       void *arg)
{
    char p[512];
    rooted(dir, p, sizeof p);
    long fd = lp_open(p, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;
    char buf[4096];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (long off = 0; off < n; ) {
            u16 len;
            memcpy(&len, buf + off + DIRENT_RECLEN, sizeof len);
            const char *name = buf + off + DIRENT_NAME;
            off += len;
            if (name[0] == '.')
                continue;
            char full[512];
            snprintf(full, sizeof full, "%s/%s", dir, name);
            fn(full, arg);
        }
    }
    lp_close((int)fd);
}

/* ── Power supply ──────────────────────────────────────────────────── */

typedef struct {
    bool have_ac, ac;
    bool have_bat;
    int  percent;
    char state[16];         /* charging | discharging | full | unknown */
    long minutes_left;      /* -1 when it cannot be worked out */
} power_t;

static void ps_one(const char *path, void *arg)
{
    power_t *p = arg;
    char f[512], type[32];
    snprintf(f, sizeof f, "%s/type", path);
    if (!rd(f, type, sizeof type))
        return;

    if (strcmp(type, "Mains") == 0) {
        snprintf(f, sizeof f, "%s/online", path);
        p->have_ac = true;
        if (rd_num(f, 0) == 1)
            p->ac = true;
        return;
    }
    if (strcmp(type, "Battery") != 0)
        return;

    /* A peripheral's battery (a wireless mouse) also shows up here, with
     * scope=Device. It is not the machine's battery. */
    char scope[16];
    snprintf(f, sizeof f, "%s/scope", path);
    if (rd(f, scope, sizeof scope) && strcmp(scope, "Device") == 0)
        return;
    snprintf(f, sizeof f, "%s/present", path);
    if (rd_num(f, 1) != 1)
        return;

    p->have_bat = true;
    snprintf(f, sizeof f, "%s/capacity", path);
    p->percent = (int)rd_num(f, -1);

    char st[32] = "unknown";
    snprintf(f, sizeof f, "%s/status", path);
    rd(f, st, sizeof st);
    if      (strcmp(st, "Charging") == 0)    strlcpy(p->state, "charging", sizeof p->state);
    else if (strcmp(st, "Discharging") == 0) strlcpy(p->state, "discharging", sizeof p->state);
    else if (strcmp(st, "Full") == 0)        strlcpy(p->state, "full", sizeof p->state);
    else                                     strlcpy(p->state, "unknown", sizeof p->state);

    /* Time left: batteries report either energy (uWh, with power in uW)
     * or charge (uAh, with current in uA). Dell's report energy. */
    long now  = -1, full = -1, rate = -1;
    snprintf(f, sizeof f, "%s/energy_now", path);  now  = rd_num(f, -1);
    snprintf(f, sizeof f, "%s/energy_full", path); full = rd_num(f, -1);
    snprintf(f, sizeof f, "%s/power_now", path);   rate = rd_num(f, -1);
    if (now < 0) {
        snprintf(f, sizeof f, "%s/charge_now", path);  now  = rd_num(f, -1);
        snprintf(f, sizeof f, "%s/charge_full", path); full = rd_num(f, -1);
        snprintf(f, sizeof f, "%s/current_now", path); rate = rd_num(f, -1);
    }
    p->minutes_left = -1;
    if (rate > 0 && now >= 0) {
        if (strcmp(p->state, "discharging") == 0)
            p->minutes_left = now * 60 / rate;
        else if (strcmp(p->state, "charging") == 0 && full > now)
            p->minutes_left = (full - now) * 60 / rate;
    }
}

static void read_power(power_t *p)
{
    memset(p, 0, sizeof *p);
    p->percent = -1;
    p->minutes_left = -1;
    strlcpy(p->state, "unknown", sizeof p->state);
    each_entry("/sys/class/power_supply", ps_one, p);
    /* A desktop, a VM, or a laptop whose AC adapter the firmware does
     * not describe: with no Mains entry at all, assume the wall. Guessing
     * battery would put a desktop into power-saver for ever. */
    if (!p->have_ac)
        p->ac = !p->have_bat || strcmp(p->state, "discharging") != 0;
}

/* ── Backlight ─────────────────────────────────────────────────────── */

/* Which backlight device drives the panel. The kernel can register
 * several for one screen - one from ACPI ("firmware"), one from a
 * platform driver, and the GPU's own ("raw", intel_backlight on this
 * laptop) - and writing the wrong one does nothing visible. The order
 * here is systemd's, which has had a decade of laptops to learn from:
 * firmware, then platform, then raw. */
typedef struct { char name[64]; int rank; } bl_pick_t;

static void bl_one(const char *path, void *arg)
{
    bl_pick_t *b = arg;
    char f[512], type[32];
    snprintf(f, sizeof f, "%s/type", path);
    if (!rd(f, type, sizeof type))
        return;
    snprintf(f, sizeof f, "%s/max_brightness", path);
    if (rd_num(f, 0) <= 1)          /* an on/off switch, not a dimmer */
        return;
    int rank = strcmp(type, "firmware") == 0 ? 3
             : strcmp(type, "platform") == 0 ? 2
             : strcmp(type, "raw") == 0      ? 1 : 0;
    if (rank > b->rank) {
        const char *slash = strrchr(path, '/');
        strlcpy(b->name, slash ? slash + 1 : path, sizeof b->name);
        b->rank = rank;
    }
}

static bool backlight(char *name, size_t n)
{
    bl_pick_t b;
    memset(&b, 0, sizeof b);
    each_entry("/sys/class/backlight", bl_one, &b);
    if (!b.name[0])
        return false;
    strlcpy(name, b.name, n);
    return true;
}

static int brightness_get(void)
{
    char dev[64], f[256];
    if (!backlight(dev, sizeof dev))
        return -1;
    snprintf(f, sizeof f, "/sys/class/backlight/%s/max_brightness", dev);
    long max = rd_num(f, 0);
    snprintf(f, sizeof f, "/sys/class/backlight/%s/brightness", dev);
    long cur = rd_num(f, -1);
    if (max <= 0 || cur < 0)
        return -1;
    return (int)((cur * 100 + max / 2) / max);
}

/* Brightness does not jump; it glides.
 *
 * A slider dragged across the screen sends thirty values a second, a
 * brightness key sends one, the idle dimmer sends one. Written straight
 * to the backlight each is a visible step - at the dark end of the range
 * a step of a few percent reads as a flash. So every request becomes a
 * target and the daemon walks the panel to it: an exponential approach
 * with a 45ms time constant, stepped every 8ms, done within 0.5% of the
 * range (about 240ms - the "expand" spring of design/feel.md, which is
 * what a change of level is). A new request mid-way retargets from where
 * the panel is, so dragging never queues up behind the glide.
 *
 * In the daemon the steps ride on its poll loop, which only wakes that
 * often while a glide is running. Called directly (root, --sysfs, or no
 * daemon yet), brightness_set() glides in place before it returns. */
#define RAMP_TICK_MS 8
#define RAMP_ALPHA_1000 163        /* 1 - exp(-8/45), in thousandths */

static struct {
    bool on;
    char dev[64];
    long max;
    long x1000;                    /* current raw level x 1000 */
    long target;                   /* raw */
    s64  last_ms;
} ramp;

static bool in_daemon;
static int  bright_saved = -1;

static void bright_save(void)
{
    if (dry_run || ramp.max <= 0)
        return;
    int pct = (int)((ramp.target * 100 + ramp.max / 2) / ramp.max);
    if (pct == bright_saved)
        return;
    char d[512], f[512], line[16];
    rooted(BRIGHT_DIR, d, sizeof d);
    rooted(BRIGHT_FILE, f, sizeof f);
    lp_mkdir(d, 0755);
    snprintf(line, sizeof line, "%d\n", pct);
    if (lp_write_file_atomic(f, line, strlen(line)))
        bright_saved = pct;
}

static bool ramp_write(long raw)
{
    char f[256], v[32];
    snprintf(v, sizeof v, "%ld", raw);
    snprintf(f, sizeof f, "/sys/class/backlight/%s/brightness", ramp.dev);
    int before = n_refused;
    wr(f, v);
    return n_refused == before;
}

/* One or more 8ms steps, however many fit in the time since the last.
 * False when the glide is over (or the backlight refused a write). */
static bool ramp_step(void)
{
    if (!ramp.on)
        return false;
    s64 now = lp_monotonic_ms();
    long steps = (long)((now - ramp.last_ms) / RAMP_TICK_MS);
    if (steps < 1)
        return true;
    if (steps > 64)
        steps = 64;
    ramp.last_ms = now;
    long tgt = ramp.target * 1000;
    for (long i = 0; i < steps; i++)
        ramp.x1000 += (tgt - ramp.x1000) * RAMP_ALPHA_1000 / 1000;
    long left = tgt - ramp.x1000;
    if (left < 0) left = -left;
    bool done = left <= ramp.max * 5;          /* 0.5% of the range, x1000 */
    long raw = done ? ramp.target : (ramp.x1000 + 500) / 1000;
    if (raw < 1) raw = 1;
    if (!ramp_write(raw) || done) {
        ramp.on = false;
        if (done)
            bright_save();
        return false;
    }
    return true;
}

static bool brightness_set(int pct)
{
    char dev[64], f[256];
    if (!backlight(dev, sizeof dev))
        return false;
    if (pct < 1)   pct = 1;          /* never black - see the header */
    if (pct > 100) pct = 100;
    snprintf(f, sizeof f, "/sys/class/backlight/%s/max_brightness", dev);
    long max = rd_num(f, 0);
    if (max <= 0)
        return false;
    long raw = max * pct / 100;
    if (raw < 1) raw = 1;

    /* Start from what the panel shows now, or from where a glide in
     * progress has got to - never from where it was going. */
    if (!ramp.on || strcmp(ramp.dev, dev) != 0 || ramp.max != max) {
        snprintf(f, sizeof f, "/sys/class/backlight/%s/brightness", dev);
        long cur = rd_num(f, raw);
        strlcpy(ramp.dev, dev, sizeof ramp.dev);
        ramp.max = max;
        ramp.x1000 = cur * 1000;
        ramp.last_ms = lp_monotonic_ms();
    }
    ramp.target = raw;
    ramp.on = true;

    /* The first step now, so the change starts in this frame and a
     * refusal is reported to the one who asked. */
    int before = n_refused;
    ramp.last_ms -= RAMP_TICK_MS;
    ramp_step();
    if (n_refused != before)
        return false;
    if (!in_daemon)
        while (ramp_step())
            lp_sleep_ms(RAMP_TICK_MS);
    return true;
}

/* ── Profiles ──────────────────────────────────────────────────────── */

typedef enum { P_AUTO, P_BALANCED, P_SAVER, P_PERFORMANCE } profile_t;
static const char *PNAME[] = { "auto", "balanced", "saver", "performance" };

static bool parse_profile(const char *s, profile_t *out)
{
    for (int i = 0; i < 4; i++)
        if (strcmp(s, PNAME[i]) == 0) { *out = (profile_t)i; return true; }
    return false;
}

static profile_t load_profile(void)
{
    char b[32];
    profile_t p = P_AUTO;
    if (rd(PROFILE_FILE, b, sizeof b))
        parse_profile(b, &p);
    return p;
}

static void save_profile(profile_t p)
{
    if (dry_run)
        return;
    char path[512], line[32];
    rooted(PROFILE_FILE, path, sizeof path);
    snprintf(line, sizeof line, "%s\n", PNAME[p]);
    lp_write_file_atomic(path, line, strlen(line));
}

static profile_t effective(profile_t p, const power_t *pw)
{
    if (p != P_AUTO)
        return p;
    return pw->ac ? P_BALANCED : P_SAVER;
}

/* Per-CPU: the energy/performance preference and nothing else. The
 * governor stays whatever intel_pstate chose (powersave, which on
 * intel_pstate means "let the hardware decide", not "slow"). */
static const char *cur_epp;
static void cpu_one(const char *path, void *arg)
{
    (void)arg;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (strncmp(name, "cpu", 3) != 0 || name[3] < '0' || name[3] > '9')
        return;
    char f[512];
    snprintf(f, sizeof f, "%s/cpufreq/energy_performance_preference", path);
    wr(f, cur_epp);
}

static const char *cur_pci_pm;
static void pci_one(const char *path, void *arg)
{
    (void)arg;
    char f[512];
    snprintf(f, sizeof f, "%s/power/control", path);
    wr(f, cur_pci_pm);
}

/* True when a USB device has any HID interface - keyboards, mice,
 * touchscreens, touchpads, pens. Those are never autosuspended. */
typedef struct { const char *dev; bool hid; } hid_scan_t;
static void usb_if_one(const char *path, void *arg)
{
    hid_scan_t *h = arg;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    size_t dl = strlen(h->dev);
    /* interfaces of device 1-2 are named 1-2:1.0, 1-2:1.1 ... */
    if (strncmp(name, h->dev, dl) != 0 || name[dl] != ':')
        return;
    char f[512], cls[8];
    snprintf(f, sizeof f, "%s/bInterfaceClass", path);
    if (rd(f, cls, sizeof cls) && strcmp(cls, "03") == 0)
        h->hid = true;
}

static const char *cur_usb;
static void usb_one(const char *path, void *arg)
{
    (void)arg;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (strchr(name, ':'))          /* an interface, not a device */
        return;
    hid_scan_t h = { name, false };
    each_entry("/sys/bus/usb/devices", usb_if_one, &h);
    char f[512];
    snprintf(f, sizeof f, "%s/power/control", path);
    wr(f, h.hid ? "on" : cur_usb);
}

static const char *cur_alpm;
static void scsi_one(const char *path, void *arg)
{
    (void)arg;
    char f[512];
    snprintf(f, sizeof f, "%s/link_power_management_policy", path);
    wr(f, cur_alpm);
}

static void apply(profile_t eff)
{
    n_written = n_absent = n_refused = 0;

    bool saver = eff == P_SAVER, perf = eff == P_PERFORMANCE;

    cur_epp = saver ? "power" : perf ? "performance" : "balance_performance";
    each_entry("/sys/devices/system/cpu", cpu_one, NULL);
    wr("/sys/devices/system/cpu/intel_pstate/no_turbo", saver ? "1" : "0");

    wr("/sys/module/pcie_aspm/parameters/policy",
       saver ? "powersupersave" : perf ? "default" : "powersave");

    cur_pci_pm = "auto";                    /* every profile - see header */
    each_entry("/sys/bus/pci/devices", pci_one, NULL);

    cur_usb = "auto";
    each_entry("/sys/bus/usb/devices", usb_one, NULL);

    cur_alpm = perf ? "max_performance" : "med_power_with_dipm";
    each_entry("/sys/class/scsi_host", scsi_one, NULL);

    wr("/sys/module/snd_hda_intel/parameters/power_save", perf ? "0" : "1");
    wr("/sys/module/snd_hda_intel/parameters/power_save_controller",
       perf ? "N" : "Y");

    /* The NMI watchdog wakes every CPU periodically to check for hard
     * lockups. A laptop has the hardware watchdog for that. */
    wr("/proc/sys/kernel/nmi_watchdog", "0");

    /* How long dirty pages may sit before the flusher writes them: long
     * on battery so the NVMe can stay in its deepest state. */
    wr("/proc/sys/vm/dirty_writeback_centisecs",
       saver ? "6000" : perf ? "500" : "1500");
    wr("/proc/sys/vm/laptop_mode", saver ? "5" : "0");
}

/* ── /etc/lp-tune.conf ─────────────────────────────────────────────── */

#define CONF_FILE "/etc/lp-tune.conf"

typedef struct {
    char lid_bat[12], lid_ac[12], power[12];
    bool lock;
    int  idle_bat, idle_ac;
} conf_t;

/* Running in a virtual machine: the CPU says so (the hypervisor flag
 * every x86 hypervisor sets, KVM, UTM and QEMU's emulator included), or
 * the firmware's maker does (/sys/class/dmi/id). */
static bool virtual_machine(void)
{
    static int known = -1;
    if (known >= 0)
        return known;
    char p[512], buf[4096];
    rooted("/proc/cpuinfo", p, sizeof p);
    known = proc_read(p, buf, sizeof buf) > 0 && strstr(buf, " hypervisor") != NULL;
    static const char *const makers[] = {
        "QEMU", "VMware", "innotek", "Xen", "Parallels", "Bochs", NULL
    };
    char v[128];
    if (!known && rd("/sys/class/dmi/id/sys_vendor", v, sizeof v))
        for (int i = 0; makers[i]; i++)
            if (strstr(v, makers[i]))
                known = 1;
    if (!known && rd("/sys/class/dmi/id/product_name", v, sizeof v) &&
        strstr(v, "Virtual Machine"))              /* Hyper-V */
        known = 1;
    return known;
}

static void conf_defaults(conf_t *c)
{
    strlcpy(c->lid_bat, "suspend", sizeof c->lid_bat);
    strlcpy(c->lid_ac,  "suspend", sizeof c->lid_ac);
    /* In a virtual machine the power button is how the host asks the
     * guest to shut down (virsh shutdown, UTM's and VirtualBox's "ACPI
     * shutdown"): suspending there left the VM paused, never off. */
    strlcpy(c->power, virtual_machine() ? "poweroff" : "suspend", sizeof c->power);
    c->lock = true;
    c->idle_bat = 15;
    c->idle_ac = 0;
}

/* Validate one key=value. Returns false - and says why in `why` - for
 * an unknown key or a value outside the key's set. Used for the file and
 * for requests from the socket alike, so the file can never hold a
 * value the daemon would not accept from a person. */
static bool conf_apply(conf_t *c, const char *k, const char *v, char *why,
                       size_t wn)
{
    bool action3 = strcmp(v, "suspend") == 0 || strcmp(v, "nothing") == 0 ||
                   strcmp(v, "poweroff") == 0;
    if (strcmp(k, "lid_on_battery") == 0 || strcmp(k, "lid_on_ac") == 0 ||
        strcmp(k, "power_button") == 0) {
        if (!action3) {
            snprintf(why, wn, "%s is suspend, nothing or poweroff", k);
            return false;
        }
        char *dst = strcmp(k, "lid_on_battery") == 0 ? c->lid_bat
                  : strcmp(k, "lid_on_ac") == 0      ? c->lid_ac : c->power;
        strlcpy(dst, v, 12);
        return true;
    }
    if (strcmp(k, "lock_before_suspend") == 0) {
        if (strcmp(v, "yes") && strcmp(v, "no")) {
            snprintf(why, wn, "lock_before_suspend is yes or no");
            return false;
        }
        c->lock = strcmp(v, "yes") == 0;
        return true;
    }
    if (strcmp(k, "idle_suspend_minutes_battery") == 0 ||
        strcmp(k, "idle_suspend_minutes_ac") == 0) {
        char *end = NULL;
        long n = strtol(v, &end, 10);
        if (!v[0] || (end && *end) || n < 0 || n > 600) {
            snprintf(why, wn, "%s is a number of minutes, 0 to 600", k);
            return false;
        }
        if (k[21] == 'b') c->idle_bat = (int)n; else c->idle_ac = (int)n;
        return true;
    }
    snprintf(why, wn, "no setting called %s", k);
    return false;
}

static void conf_load(conf_t *c)
{
    conf_defaults(c);
    char p[512];
    rooted(CONF_FILE, p, sizeof p);
    long fd = lp_open(p, O_RDONLY, 0);
    if (fd < 0)
        return;
    char line[160], why[96];
    while (readline((int)fd, line, sizeof line) >= 0) {
        char *h = strchr(line, '#');
        if (h) *h = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line, *v = eq + 1;
        while (*k == ' ') k++;
        char *e = k + strlen(k);
        while (e > k && e[-1] == ' ') *--e = '\0';
        while (*v == ' ') v++;
        e = v + strlen(v);
        while (e > v && (e[-1] == ' ' || e[-1] == '\r')) *--e = '\0';
        /* A bad line keeps the default and is said once; it must not
         * stop the lid from working. */
        if (!conf_apply(c, k, v, why, sizeof why))
            dprintf(STDERR_FILENO, "lp-tune: %s: %s - using the default\n",
                    CONF_FILE, why);
    }
    lp_close((int)fd);
}

static bool conf_save(const conf_t *c)
{
    if (dry_run)
        return true;
    char p[512], text[1024];
    rooted(CONF_FILE, p, sizeof p);
    int n = snprintf(text, sizeof text,
            "# Written by lp-tune (the Settings app's Power panel). One key=value\n"
            "# per line; see `lp-tune --help`.\n"
            "lid_on_battery=%s\nlid_on_ac=%s\npower_button=%s\n"
            "lock_before_suspend=%s\nidle_suspend_minutes_battery=%d\n"
            "idle_suspend_minutes_ac=%d\n",
            c->lid_bat, c->lid_ac, c->power, c->lock ? "yes" : "no",
            c->idle_bat, c->idle_ac);
    /* Durable, not just atomic: a setting changed a second before the
     * battery died must still be there on the next boot. */
    return n > 0 && (size_t)n < sizeof text &&
           lp_write_file_atomic(p, text, (size_t)n);
}

static void conf_print(int fd, const conf_t *c)
{
    dprintf(fd, "lid_on_battery=%s\nlid_on_ac=%s\npower_button=%s\n"
                "lock_before_suspend=%s\nidle_suspend_minutes_battery=%d\n"
                "idle_suspend_minutes_ac=%d\n",
            c->lid_bat, c->lid_ac, c->power, c->lock ? "yes" : "no",
            c->idle_bat, c->idle_ac);
}

/* ── Suspend ───────────────────────────────────────────────────────── */

/* An external monitor is connected: any DRM connector reporting
 * "connected" that is not the laptop's own panel. */
static bool ext_found;
static void drm_one(const char *path, void *arg)
{
    (void)arg;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    const char *dash = strchr(name, '-');          /* card0-HDMI-A-1 */
    if (!dash)
        return;
    const char *conn = dash + 1;
    if (strncmp(conn, "eDP", 3) == 0 || strncmp(conn, "LVDS", 4) == 0 ||
        strncmp(conn, "DSI", 3) == 0 || strncmp(conn, "Virtual", 7) == 0)
        return;
    char f[512], st[32];
    snprintf(f, sizeof f, "%s/status", path);
    if (rd(f, st, sizeof st) && strcmp(st, "connected") == 0)
        ext_found = true;
}

static bool external_display(void)
{
    ext_found = false;
    each_entry("/sys/class/drm", drm_one, NULL);
    return ext_found;
}

/* The person whose session is on the screen: the first uid >= 1000 with
 * a Wayland socket in its runtime directory. */
typedef struct { u32 uid; char sock[32]; } sess_t;
static void wl_one(const char *path, void *arg)
{
    sess_t *s = arg;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (strncmp(name, "wayland-", 8) == 0 && !strchr(name, '.') && !s->sock[0])
        strlcpy(s->sock, name, sizeof s->sock);
}
static void run_one(const char *path, void *arg)
{
    sess_t *s = arg;
    if (s->sock[0])
        return;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    long uid = strtol(name, NULL, 10);
    if (uid < 1000)
        return;
    each_entry(path, wl_one, s);
    if (s->sock[0])
        s->uid = (u32)uid;
}

/* Run the session's lock command as that user and wait until it says
 * the lock is up (it exits 0 once the lock surface is shown) - two
 * seconds at most, because a machine that will not sleep with its lid
 * shut cooks in a bag. */
static bool lock_session(void)
{
    sess_t s;
    memset(&s, 0, sizeof s);
    each_entry("/run/user", run_one, &s);
    if (!s.sock[0]) {
        printf("lp-tune: nobody is logged in - nothing to lock\n");
        return true;
    }
    char cmd[256];
    const char *cands[] = { "/usr/bin/lp-lock", "/bin/lp-lock", NULL };
    cmd[0] = '\0';
    for (int i = 0; cands[i]; i++) {
        rooted(cands[i], cmd, sizeof cmd);
        if (lp_exists(cmd)) break;
        cmd[0] = '\0';
    }
    if (!cmd[0]) {
        dprintf(STDERR_FILENO, "lp-tune: ** no lp-lock - suspending WITHOUT"
                " locking the session\n");
        return false;
    }
    lp_user_t u;
    gid_t gid = s.uid;
    if (lp_user_by_uid(s.uid, &u))
        gid = u.gid;

    char rt[128], wd[64], home[96];
    snprintf(rt, sizeof rt, "XDG_RUNTIME_DIR=%s/run/user/%u", root, s.uid);
    snprintf(wd, sizeof wd, "WAYLAND_DISPLAY=%s", s.sock);
    snprintf(home, sizeof home, "HOME=%s", lp_user_by_uid(s.uid, &u) ? u.home : "/");
    char *envp[] = { rt, wd, home, (char *)"PATH=/usr/bin:/bin", NULL };
    char *argv[] = { cmd, NULL };

    pid_t pid = lp_fork();
    if (pid == 0) {
        /* Drop to the person entirely before exec: groups, then gid,
         * then uid - in that order, because after setuid there is no
         * permission left to change the other two. */
        if (lp_setgroups(0, NULL) < 0 || lp_setgid(gid) < 0 ||
            lp_setuid(s.uid) < 0)
            lp_exit(126);
        lp_execve(cmd, argv, envp);
        lp_exit(127);
    }
    if (pid < 0)
        return false;
    for (int t = 0; t < 40; t++) {              /* 40 x 50 ms = 2 s */
        int st = 0;
        if (lp_waitpid(pid, &st, 1 /* WNOHANG */) == pid) {
            if ((st & 0x7f) || ((st >> 8) & 0xff) != 0) {
                dprintf(STDERR_FILENO, "lp-tune: lp-lock failed (%d)\n",
                        (st >> 8) & 0xff);
                return false;
            }
            return true;
        }
        lp_sleep_ms(50);
    }
    dprintf(STDERR_FILENO, "lp-tune: lp-lock did not confirm within 2s -"
            " suspending anyway\n");
    return false;
}

static void do_suspend(const conf_t *c)
{
    /* If the lock could not be confirmed, this still suspends: a laptop
     * that refuses to sleep with its lid shut overheats in a bag, and
     * that is the worse failure. But it tries again the instant the
     * machine wakes, so the window in which an unlocked desktop can be
     * seen is the resume itself, not the rest of the day. */
    bool locked = !c->lock || lock_session();
    lp_sync();
    printf("lp-tune: suspending\n");
    /* "mem" is whatever /sys/power/mem_sleep selects - deep (S3) on the
     * XPS 15 9550, s2idle on machines without S3. */
    wr("/sys/power/state", "mem");
    /* Some of what apply() set does not survive a resume on every
     * machine (USB devices re-enumerate). Cheap to set again. */
    if (!locked) {
        printf("lp-tune: resumed - locking now, since it did not happen before\n");
        lock_session();
    }
    power_t pw;
    read_power(&pw);
    apply(effective(load_profile(), &pw));
    printf("lp-tune: resumed\n");
}

static void do_action(const char *act, const conf_t *c)
{
    if (strcmp(act, "suspend") == 0)
        do_suspend(c);
    else if (strcmp(act, "poweroff") == 0) {
        printf("lp-tune: powering off\n");
        if (!dry_run && !root[0])
            lp_kill(1, 10 /* SIGUSR1: init's "off", the same signal lp-power sends */);
    }
}

/* ── The lid and power-button devices ──────────────────────────────── */

/* Find input devices by the names the ACPI button driver gives them.
 * /proc/bus/input/devices lists each device as a block of lines; the
 * N: line has the name and the H: line the event handler. */
static int find_inputs(int *fds, int max, int *kinds)
{
    char p[512];
    rooted("/proc/bus/input/devices", p, sizeof p);
    long fd = lp_open(p, O_RDONLY, 0);
    if (fd < 0)
        return 0;
    int n = 0, kind = 0;
    char line[512];
    while (readline((int)fd, line, sizeof line) >= 0 && n < max) {
        if (strncmp(line, "N: Name=", 8) == 0)
            kind = strstr(line, "\"Lid Switch\"")   ? 1
                 : strstr(line, "\"Power Button\"") ? 2 : 0;
        else if (kind && strncmp(line, "H: Handlers=", 12) == 0) {
            char *ev = strstr(line, "event");
            if (ev) {
                char dev[64], path[512];
                int k = 0;
                while (ev[k] && ev[k] != ' ' && k < 30) { dev[k] = ev[k]; k++; }
                dev[k] = '\0';
                snprintf(path, sizeof path, "%s/dev/input/%s", root, dev);
                /* Non-blocking: poll() says when there is an event, and an
                 * open or a read must never be able to hang the daemon -
                 * a daemon stuck at startup is a lid that never sleeps. */
                long efd = lp_open(path, O_RDONLY | O_NONBLOCK, 0);
                if (efd >= 0) {
                    fds[n] = (int)efd;
                    kinds[n] = kind;
                    n++;
                }
            }
            kind = 0;
        }
    }
    lp_close((int)fd);
    return n;
}

/* struct input_event is { struct timeval; u16 type; u16 code; s32 value; }
 * and timeval is two longs - 8 bytes on the Pi Zero W, 16 on amd64 and
 * arm64. Computed, never hard-coded: this project has already shipped
 * one 32-bit struct layout bug (BLKPG) and does not need another. */
#define EV_OFF   (2 * sizeof(long))
#define EV_SIZE  (EV_OFF + 8)
#define EV_KEY_  1
#define EV_SW_   5
#define KEY_POWER_ 116
#define SW_LID_  0

/* ── Reporting ─────────────────────────────────────────────────────── */

static void json_status(int fd, profile_t prof)
{
    power_t pw;
    read_power(&pw);
    profile_t eff = effective(prof, &pw);
    dprintf(fd, "{\"profile\":\"%s\",\"effective\":\"%s\",\"ac\":%s,"
                "\"battery\":{\"present\":%s,\"percent\":%d,\"state\":\"%s\","
                "\"minutes_left\":%ld},\"brightness\":%d}\n",
            PNAME[prof], PNAME[eff], pw.ac ? "true" : "false",
            pw.have_bat ? "true" : "false", pw.percent, pw.state,
            pw.minutes_left, brightness_get());
}

static void human_status(profile_t prof)
{
    power_t pw;
    read_power(&pw);
    profile_t eff = effective(prof, &pw);
    printf("\n  profile      %s", PNAME[prof]);
    if (prof == P_AUTO)
        printf("  (%s now - %s)", PNAME[eff],
               pw.ac ? "on the charger" : "on battery");
    printf("\n");
    if (pw.have_bat) {
        printf("  battery      %d%%  %s", pw.percent, pw.state);
        if (pw.minutes_left >= 0)
            printf(", %ldh %02ldm %s", pw.minutes_left / 60,
                   pw.minutes_left % 60,
                   strcmp(pw.state, "charging") == 0 ? "to full" : "left");
        printf("\n");
    } else {
        printf("  battery      none - this machine runs from the wall\n");
    }
    int b = brightness_get();
    if (b >= 0) printf("  brightness   %d%%\n", b);
    else        printf("  brightness   no backlight to control here\n");
    printf("\n");
}

/* ── The daemon's socket ───────────────────────────────────────────── */

typedef struct { u16 family; char path[108]; } sun_t;

/* Fill a sockaddr_un, or refuse. sun_path is 108 bytes and strlcpy would
 * cut a longer path short without a word - the daemon would then listen
 * on one name and a client look for another, and "the service is not
 * running" would be the only thing anyone ever saw. */
static bool sun_fill(sun_t *sa, const char *path)
{
    memset(sa, 0, sizeof *sa);
    sa->family = AF_UNIX_;
    if (strlen(path) >= sizeof sa->path) {
        dprintf(STDERR_FILENO, "lp-tune: socket path too long (%u bytes, the"
                " limit is %u): %s\n", (unsigned)strlen(path),
                (unsigned)sizeof sa->path - 1, path);
        return false;
    }
    strlcpy(sa->path, path, sizeof sa->path);
    return true;
}

static bool peer_uid(int fd, u32 *uid)
{
    u32 cred[3];            /* struct ucred { pid; uid; gid; } - 12 bytes on every arch */
    u32 len = sizeof cred;
    long r = sys_call5(SYS_getsockopt, fd, SOL_SOCKET, SO_PEERCRED_,
                       (long)cred, (long)&len);
    if (r < 0 || len < sizeof cred)
        return false;
    *uid = cred[1];
    return true;
}

/* Handle one request line. Returns the reply. `prof` is the daemon's
 * state and is updated in place. */
static void handle(const char *req, u32 uid, profile_t *prof, char *reply,
                   size_t n)
{
    const char *req_all = req;
    if (uid != 0 && uid < 1000) {
        snprintf(reply, n, "err only a person at this machine may change this\n");
        return;
    }
    char word[16], arg[16];
    word[0] = arg[0] = '\0';
    int k = 0;
    while (*req == ' ') req++;
    while (*req && *req != ' ' && *req != '\n' && k < 15) word[k++] = *req++;
    word[k] = '\0';
    while (*req == ' ') req++;
    k = 0;
    while (*req && *req != ' ' && *req != '\n' && k < 15) arg[k++] = *req++;
    arg[k] = '\0';

    if (strcmp(word, "set") == 0) {
        profile_t p;
        if (!parse_profile(arg, &p)) {
            snprintf(reply, n, "err no profile called \"%s\"\n", arg);
            return;
        }
        *prof = p;
        save_profile(p);
        power_t pw;
        read_power(&pw);
        apply(effective(p, &pw));
        snprintf(reply, n, "ok %s\n", PNAME[p]);
        return;
    }
    if (strcmp(word, "brightness") == 0) {
        int cur = brightness_get(), want;
        if (cur < 0) {
            snprintf(reply, n, "err no backlight on this machine\n");
            return;
        }
        if (arg[0] == '+' || arg[0] == '-')
            want = cur + (int)strtol(arg, NULL, 10);
        else if (arg[0] >= '0' && arg[0] <= '9')
            want = (int)strtol(arg, NULL, 10);
        else {
            snprintf(reply, n, "err brightness wants a number\n");
            return;
        }
        if (!brightness_set(want)) {
            snprintf(reply, n, "err the backlight refused it\n");
            return;
        }
        /* The target, not a reading taken a few milliseconds into the
         * glide towards it. */
        snprintf(reply, n, "ok %d\n", want < 1 ? 1 : want > 100 ? 100 : want);
        return;
    }
    if (strcmp(word, "suspend") == 0) {
        conf_t c;
        conf_load(&c);
        snprintf(reply, n, "ok suspending\n");
        do_suspend(&c);
        return;
    }
    if (strcmp(word, "config") == 0) {
        /* "config set KEY VALUE" - the key and value are re-read from the
         * request here because the generic parser above keeps one arg. */
        const char *rest = strstr(req_all, "set");
        char k[40] = "", v[16] = "";
        if (!rest) {
            snprintf(reply, n, "err config set KEY VALUE\n");
            return;
        }
        rest += 3;
        while (*rest == ' ') rest++;
        int i = 0;
        while (*rest && *rest != ' ' && i < 39) k[i++] = *rest++;
        k[i] = '\0';
        while (*rest == ' ') rest++;
        i = 0;
        while (*rest && *rest != ' ' && *rest != '\n' && i < 15) v[i++] = *rest++;
        v[i] = '\0';
        conf_t c;
        char why[96];
        conf_load(&c);
        if (!conf_apply(&c, k, v, why, sizeof why)) {
            snprintf(reply, n, "err %s\n", why);
            return;
        }
        if (!conf_save(&c)) {
            snprintf(reply, n, "err cannot write %s\n", CONF_FILE);
            return;
        }
        snprintf(reply, n, "ok %s=%s\n", k, v);
        return;
    }
    snprintf(reply, n, "err say set <profile>, brightness <n>, suspend or config set <k> <v>\n");
}

static int daemon_main(void)
{
    profile_t prof = load_profile();
    power_t pw;
    read_power(&pw);
    bool last_ac = pw.ac;
    apply(effective(prof, &pw));

    /* The brightness someone left it at, faded in from wherever the
     * firmware put it. Never below 5% at boot: a panel that comes up
     * nearly black reads as a machine that did not start. */
    char bf[32];
    if (rd(BRIGHT_FILE, bf, sizeof bf)) {
        int pct = (int)strtol(bf, NULL, 10);
        if (pct >= 1 && pct <= 100) {
            bright_saved = pct;
            brightness_set(pct < 5 ? 5 : pct);
        }
    }
    printf("lp-tune: %s (%s) - %d settings applied, %d not on this machine%s\n",
           PNAME[prof], PNAME[effective(prof, &pw)], n_written, n_absent,
           n_refused ? ", some refused - see above" : "");

    lp_signal_ignore(13);                   /* SIGPIPE: a client that left */

    char sp[256];
    rooted(SOCK_PATH, sp, sizeof sp);
    lp_unlink(sp);
    sun_t sa;
    long ls = sun_fill(&sa, sp) ? lp_socket(AF_UNIX_, SOCK_STREAM, 0) : -1;
    if (ls < 0 || lp_bind((int)ls, &sa, sizeof sa) < 0 ||
        lp_listen((int)ls, 8) < 0) {
        dprintf(STDERR_FILENO, "lp-tune: cannot listen on %s - brightness and"
                " profiles are root-only until this is fixed\n", SOCK_PATH);
        ls = -1;
    } else {
        lp_chmod(sp, 0666);     /* who may ask is decided by SO_PEERCRED */
    }

    /* Charger in, charger out: the kernel announces a power_supply
     * change on the uevent socket. Listening costs nothing; polling every
     * few seconds would cost wakeups on the one machine where they
     * matter. A minute-long poll stays as the backstop for a dropped
     * event. */
    long ue = lp_socket(AF_NETLINK_, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT_);
    if (ue >= 0) {
        struct { u16 fam, pad; u32 pid, groups; } nl = { AF_NETLINK_, 0, 0, 1 };
        if (lp_bind((int)ue, &nl, sizeof nl) < 0) {
            lp_close((int)ue);
            ue = -1;
        }
    }

    int infd[6], inkind[6];
    int nin = find_inputs(infd, 6, inkind);
    int nlid = 0, npwr = 0;
    for (int i = 0; i < nin; i++) { if (inkind[i] == 1) nlid++; else npwr++; }
    printf("lp-tune: watching %d lid switch%s and %d power button%s\n",
           nlid, nlid == 1 ? "" : "es", npwr, npwr == 1 ? "" : "s");

    in_daemon = true;
    for (;;) {
        lp_pollfd_t fds[8];
        unsigned nf = 0;
        bool gliding = ramp.on;
        if (ls >= 0) { fds[nf].fd = (int)ls; fds[nf].events = LP_POLLIN; fds[nf].revents = 0; nf++; }
        if (ue >= 0) { fds[nf].fd = (int)ue; fds[nf].events = LP_POLLIN; fds[nf].revents = 0; nf++; }
        for (int i = 0; i < nin; i++) { fds[nf].fd = infd[i]; fds[nf].events = LP_POLLIN; fds[nf].revents = 0; nf++; }
        /* Wake every 8ms only while the backlight is gliding. */
        long r = lp_poll(fds, nf, gliding ? RAMP_TICK_MS : 60000);
        if (ramp.on)
            ramp_step();

        bool recheck = (r == 0 && !gliding);
        for (unsigned i = 0; r > 0 && i < nf; i++) {
            if (!(fds[i].revents & LP_POLLIN))
                continue;
            int kind = 0;
            for (int j = 0; j < nin; j++)
                if (fds[i].fd == infd[j]) kind = inkind[j];
            if (kind) {
                u8 evbuf[EV_SIZE * 16];
                long n = lp_read(fds[i].fd, evbuf, sizeof evbuf);
                for (long off = 0; n > 0 && off + (long)EV_SIZE <= n; off += EV_SIZE) {
                    u16 type, code;
                    s32 value;
                    memcpy(&type,  evbuf + off + EV_OFF,     2);
                    memcpy(&code,  evbuf + off + EV_OFF + 2, 2);
                    memcpy(&value, evbuf + off + EV_OFF + 4, 4);
                    conf_t conf;
                    if (kind == 1 && type == EV_SW_ && code == SW_LID_ && value == 1) {
                        conf_load(&conf);
                        read_power(&pw);
                        if (external_display()) {
                            printf("lp-tune: lid closed with an external monitor"
                                   " connected - staying awake\n");
                            continue;
                        }
                        const char *act = pw.ac ? conf.lid_ac : conf.lid_bat;
                        printf("lp-tune: lid closed on %s - %s\n",
                               pw.ac ? "the charger" : "battery", act);
                        do_action(act, &conf);
                    } else if (kind == 2 && type == EV_KEY_ &&
                               code == KEY_POWER_ && value == 1) {
                        conf_load(&conf);
                        printf("lp-tune: power button - %s\n", conf.power);
                        do_action(conf.power, &conf);
                    }
                }
                continue;
            }
            if (fds[i].fd == (int)ue) {
                char ev[2048];
                long n = lp_recvfrom((int)ue, ev, sizeof ev - 1, 0, NULL, NULL);
                if (n > 0) {
                    ev[n] = '\0';
                    for (long j = 0; j < n; j += (long)strlen(ev + j) + 1)
                        if (strcmp(ev + j, "SUBSYSTEM=power_supply") == 0)
                            recheck = true;
                }
            } else {
                long c = lp_accept((int)ls, NULL, NULL, 0);
                if (c < 0)
                    continue;
                /* One short line, and a client that does not send it
                 * within a second is dropped rather than waited for. */
                s64 tv[2] = { 1, 0 };
                lp_setsockopt((int)c, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof tv);
                char req[96], reply[160];
                long n = lp_read((int)c, req, sizeof req - 1);
                u32 uid = 65534;
                if (n > 0 && peer_uid((int)c, &uid)) {
                    req[n] = '\0';
                    if (strncmp(req, "status", 6) == 0)
                        json_status((int)c, prof);
                    else {
                        handle(req, uid, &prof, reply, sizeof reply);
                        lp_write((int)c, reply, strlen(reply));
                    }
                }
                lp_close((int)c);
            }
        }

        if (recheck) {
            read_power(&pw);
            if (pw.ac != last_ac) {
                last_ac = pw.ac;
                if (prof == P_AUTO) {
                    apply(effective(prof, &pw));
                    printf("lp-tune: %s - now %s\n",
                           pw.ac ? "on the charger" : "on battery",
                           PNAME[effective(prof, &pw)]);
                }
            }
        }
    }
}

/* Ask the running daemon. Returns false when there is none to ask. */
static bool ask_daemon(const char *line, char *reply, size_t n)
{
    char sp[256];
    rooted(SOCK_PATH, sp, sizeof sp);
    sun_t sa;
    if (!sun_fill(&sa, sp))
        return false;
    long fd = lp_socket(AF_UNIX_, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    if (lp_connect((int)fd, &sa, sizeof sa) < 0) {
        lp_close((int)fd);
        return false;
    }
    lp_write((int)fd, line, strlen(line));
    long got = 0, r;
    while (got < (long)n - 1 && (r = lp_read((int)fd, reply + got, n - 1 - (size_t)got)) > 0)
        got += r;
    reply[got > 0 ? got : 0] = '\0';
    lp_close((int)fd);
    return got > 0;
}

static void usage(void)
{
    printf("usage: lp-tune [status [--json]]\n"
           "       lp-tune set auto|balanced|saver|performance\n"
           "       lp-tune brightness get | set N|+N|-N\n"
           "       lp-tune suspend\n"
           "       lp-tune config [get] | config set KEY VALUE\n"
           "       lp-tune -d\n"
           "\n"
           "  auto          balanced on the charger, saver on battery (default)\n"
           "  balanced      power-managed, turbo on\n"
           "  saver         longest battery: no turbo, deeper link and CPU states\n"
           "  performance   turbo, no ASPM, no audio power save\n");
}

int main(int argc, char **argv)
{
    int a = 1;
    while (a < argc && argv[a][0] == '-' && argv[a][1] == '-') {
        if (strcmp(argv[a], "--sysfs") == 0 && a + 1 < argc) {
            strlcpy(root, argv[a + 1], sizeof root);
            a += 2;
        } else if (strcmp(argv[a], "--dry-run") == 0) {
            dry_run = true;
            a++;
        } else if (strcmp(argv[a], "--help") == 0) {
            usage();
            return 0;
        } else {
            break;
        }
    }
    const char *cmd = a < argc ? argv[a] : "status";

    if (strcmp(cmd, "-d") == 0)
        return daemon_main();

    if (strcmp(cmd, "status") == 0) {
        bool json = a + 1 < argc && strcmp(argv[a + 1], "--json") == 0;
        char reply[512];
        /* Prefer the daemon's view: it knows the profile it is holding,
         * which may be newer than the file if a write failed. */
        if (json) {
            if (ask_daemon("status\n", reply, sizeof reply))
                printf("%s", reply);
            else
                json_status(STDOUT_FILENO, load_profile());
        } else {
            human_status(load_profile());
        }
        return 0;
    }

    if (strcmp(cmd, "config") == 0 &&
        (a + 1 >= argc || strcmp(argv[a + 1], "get") == 0)) {
        conf_t c;
        conf_load(&c);
        conf_print(STDOUT_FILENO, &c);
        return 0;
    }

    if (strcmp(cmd, "set") == 0 || strcmp(cmd, "brightness") == 0 ||
        strcmp(cmd, "suspend") == 0 || strcmp(cmd, "config") == 0) {
        char line[96], reply[160];
        if (strcmp(cmd, "brightness") == 0) {
            const char *sub = a + 1 < argc ? argv[a + 1] : "get";
            if (strcmp(sub, "get") == 0) {
                int b = brightness_get();
                if (b < 0) {
                    dprintf(STDERR_FILENO, "lp-tune: no backlight on this machine\n");
                    return 1;
                }
                printf("%d\n", b);
                return 0;
            }
            if (strcmp(sub, "set") != 0 || a + 2 >= argc) {
                usage();
                return 2;
            }
            snprintf(line, sizeof line, "brightness %s\n", argv[a + 2]);
        } else if (strcmp(cmd, "suspend") == 0) {
            snprintf(line, sizeof line, "suspend\n");
        } else if (strcmp(cmd, "config") == 0) {
            if (a + 3 >= argc || strcmp(argv[a + 1], "set") != 0) { usage(); return 2; }
            snprintf(line, sizeof line, "config set %s %s\n", argv[a + 2], argv[a + 3]);
        } else {
            if (a + 1 >= argc) { usage(); return 2; }
            snprintf(line, sizeof line, "set %s\n", argv[a + 1]);
        }

        if (!dry_run && ask_daemon(line, reply, sizeof reply)) {
            if (strncmp(reply, "ok", 2) == 0) {
                printf("%s", reply + 3);
                return 0;
            }
            dprintf(STDERR_FILENO, "lp-tune: %s", reply + 4);
            return 1;
        }
        /* No daemon: root may act directly (the boot, a rescue shell,
         * a test with --sysfs); anyone else has to have the daemon.
         * --sysfs is not an exception - it only moves where things are
         * written, and a rule that bends for a test flag is not a rule. */
        if (lp_getuid() != 0) {
            dprintf(STDERR_FILENO, "lp-tune: the lp-tune service is not running,"
                    " and only root can do this without it\n");
            return 1;
        }
        profile_t prof = load_profile();
        handle(line, 0, &prof, reply, sizeof reply);
        if (strncmp(reply, "ok", 2) == 0) {
            if (!dry_run) printf("%s", reply + 3);
            printf("  (%d written, %d not on this machine, %d refused)\n",
                   n_written, n_absent, n_refused);
            return n_refused ? 1 : 0;
        }
        dprintf(STDERR_FILENO, "lp-tune: %s", reply + 4);
        return 1;
    }

    usage();
    return 2;
}
