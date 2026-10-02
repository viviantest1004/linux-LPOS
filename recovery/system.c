/* system.c - what lp-recovery needs from the machine: the three LP
 * partitions, mounts, child processes, the log, and the status line's
 * battery and clock.
 *
 * ── Finding the partitions ──
 *
 * By GPT partition name on the disk the recovery system itself is running
 * from. The names are the disk layout contract (COMMON.md): LP-ESP,
 * LP-RECOVERY, LP-ROOT. Not by label: a USB stick with LP on it carries
 * partitions with the same labels, and it is very often still plugged in
 * when someone needs recovery (they installed from it). The disk holding
 * "/" is the one whose LP-ROOT this recovery belongs to; the GPT is read
 * here directly rather than trusted to udev, which the recovery system
 * does not run.
 *
 * ── Logging ──
 *
 * One file on LP-RECOVERY, appended and fsync'd per line, so the record
 * of what recovery did - every administrator check included - survives
 * the reboot that usually follows. The same lines go to /dev/ttyS0, which
 * is the serial console in a VM and nothing at all on the XPS.
 */
#include "recovery.h"

char disk_dev[48];
part_t p_esp, p_rec, p_root;

static int log_fd = -1, ser_fd = -1;

void mkdirs(const char *path, mode_t mode)
{
    char p[256];
    strlcpy(p, path, sizeof p);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            lp_mkdir(p, mode);
            *s = '/';
        }
    lp_mkdir(p, mode);
}

void sys_init(void)
{
    mkdirs(STATE_DIR, 0700);
    mkdirs("/var/log", 0755);
    mkdirs(MNT_ROOT, 0755);
    mkdirs(MNT_ESP, 0755);
    log_fd = (int)lp_open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    ser_fd = (int)lp_open("/dev/ttyS0", O_WRONLY | O_NOCTTY_ | O_NONBLOCK | O_CLOEXEC, 0);
}

void sys_log_pause(bool pause)
{
    if (pause && log_fd >= 0) {
        lp_fsync(log_fd);
        lp_close(log_fd);
        log_fd = -1;
    } else if (!pause && log_fd < 0)
        log_fd = (int)lp_open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
}

void rlog_line(const char *msg)
{
    char line[480];
    lp_tm_t tm;
    lp_gmtime(lp_time(), &tm);
    int n = snprintf(line, sizeof line, "%04d-%02d-%02dT%02d:%02d:%02dZ %s\n",
                     tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec, msg);
    if (log_fd >= 0) {
        lp_write(log_fd, line, (size_t)n);
        lp_fsync(log_fd);
    }
    if (ser_fd >= 0) {
        char s[500];
        int m = snprintf(s, sizeof s, "lp-recovery: %s\r\n", msg);
        lp_write(ser_fd, s, (size_t)m);
    }
}

/* ── Files ────────────────────────────────────────────────────────── */
bool file_read(const char *path, char *buf, size_t cap)
{
    long fd = lp_open(path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
        buf[0] = 0;
        return false;
    }
    size_t n = 0;
    while (n < cap - 1) {
        long r = lp_read((int)fd, buf + n, cap - 1 - n);
        if (r <= 0)
            break;
        n += (size_t)r;
    }
    lp_close((int)fd);
    buf[n] = 0;
    return true;
}

static void fsync_dir_of(const char *path)
{
    char dir[256];
    strlcpy(dir, path, sizeof dir);
    char *s = strrchr(dir, '/');
    if (!s)
        return;
    if (s == dir)
        s[1] = 0;
    else
        *s = 0;
    long fd = lp_open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (fd >= 0) {
        lp_fsync((int)fd);
        lp_close((int)fd);
    }
}

/* COMMON.md "Persistence": beside the target, fsync, rename, fsync the
 * directory. A power cut leaves the old file or the new one. */
bool file_write_atomic(const char *path, const char *data, size_t n, mode_t mode)
{
    char tmp[272];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    long fd = lp_open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return false;
    size_t off = 0;
    while (off < n) {
        long w = lp_write((int)fd, data + off, n - off);
        if (w <= 0) {
            lp_close((int)fd);
            lp_unlink(tmp);
            return false;
        }
        off += (size_t)w;
    }
    lp_chmod(tmp, mode);
    bool ok = lp_fsync((int)fd) == 0;
    lp_close((int)fd);
    if (!ok || lp_rename(tmp, path) < 0) {
        lp_unlink(tmp);
        return false;
    }
    fsync_dir_of(path);
    return true;
}

bool file_copy(const char *from, const char *to, mode_t mode)
{
    long in = lp_open(from, O_RDONLY | O_CLOEXEC, 0);
    if (in < 0)
        return false;
    long out = lp_open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (out < 0) {
        lp_close((int)in);
        return false;
    }
    static u8 buf[1 << 16];
    bool ok = true;
    for (;;) {
        long r = lp_read((int)in, buf, sizeof buf);
        if (r < 0) { ok = false; break; }
        if (r == 0)
            break;
        for (long off = 0; off < r; ) {
            long w = lp_write((int)out, buf + off, (size_t)(r - off));
            if (w <= 0) { ok = false; break; }
            off += w;
        }
        if (!ok)
            break;
    }
    if (ok && lp_fsync((int)out) < 0)
        ok = false;
    lp_close((int)in);
    lp_close((int)out);
    return ok;
}

/* ── rm -r, staying on one filesystem ─────────────────────────────── */
typedef struct {
    u64 ino;
    s64 off;
    u16 reclen;
    u8  type;
    char name[];
} __attribute__((packed)) dirent64_t;

static u64 root_dev_id;
void (*rm_hook)(u64, void *);

static void counted(u64 *count)
{
    if (!count)
        return;
    (*count)++;
    if (rm_hook && (*count & 255) == 0)
        rm_hook(*count, 0);
}

static long rm_walk(char *path, size_t len, size_t cap, u64 *count)
{
    lp_stat_t st;
    long r = lp_stat(path, &st, false);
    if (r < 0)
        return r == -2 ? 0 : r;
    if ((st.mode & LP_S_IFMT) != LP_S_IFDIR) {
        r = lp_unlink(path);
        counted(count);
        return r;
    }
    if (st.dev != root_dev_id)
        return 0;               /* a mount point below: not ours to empty */
    /* Read a batch, delete it, read again from the start until only "."
     * and ".." are left: deleting while walking one getdents stream skips
     * entries on some filesystems. */
    static char buf[16384];
    for (;;) {
        long fd = lp_open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
        if (fd < 0)
            return fd;
        long n = sys_getdents((int)fd, buf, sizeof buf);
        lp_close((int)fd);
        if (n < 0)
            return n;
        /* Copy the names out: the buffer is reused by the recursion. */
        char names[64][256];
        int k = 0;
        for (long off = 0; off < n && k < 64; ) {
            dirent64_t *d = (dirent64_t *)(buf + off);
            off += d->reclen;
            if (!strcmp(d->name, ".") || !strcmp(d->name, ".."))
                continue;
            strlcpy(names[k++], d->name, 256);
        }
        if (k == 0)
            break;
        long removed = 0;
        for (int i = 0; i < k; i++) {
            size_t nl = strlen(names[i]);
            if (len + 1 + nl + 1 > cap)
                continue;
            path[len] = '/';
            memcpy(path + len + 1, names[i], nl + 1);
            if (rm_walk(path, len + 1 + nl, cap, count) == 0)
                removed++;
            path[len] = 0;
        }
        if (removed == 0)
            return -39;         /* ENOTEMPTY: something would not go */
    }
    r = lp_rmdir(path);
    counted(count);
    return r;
}

long rm_tree(const char *path, u64 *count)
{
    lp_stat_t st;
    if (lp_stat(MNT_ROOT, &st, false) < 0)
        return -2;
    root_dev_id = st.dev;
    char p[4096];
    strlcpy(p, path, sizeof p);
    return rm_walk(p, strlen(p), sizeof p, count);
}

/* ── Partitions ───────────────────────────────────────────────────── */
static void guid_text(const u8 *g, char *out)
{
    snprintf(out, 40, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* /dev name of partition n of disk `disk`, from sysfs. */
static bool part_name(const char *disk, int n, char *out, size_t cap)
{
    char dir[96], buf[4096];
    snprintf(dir, sizeof dir, "/sys/block/%s", disk);
    long fd = lp_open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (fd < 0)
        return false;
    long len = sys_getdents((int)fd, buf, sizeof buf);
    lp_close((int)fd);
    for (long off = 0; off < len; ) {
        dirent64_t *d = (dirent64_t *)(buf + off);
        off += d->reclen;
        if (strncmp(d->name, disk, strlen(disk)) != 0)
            continue;
        char f[160], v[16];
        snprintf(f, sizeof f, "%s/%s/partition", dir, d->name);
        if (file_read(f, v, sizeof v) && atoi(v) == n) {
            snprintf(out, cap, "/dev/%s", d->name);
            return true;
        }
    }
    return false;
}

/* Read disk's GPT and fill the three LP partitions. true when it has
 * LP-RECOVERY. */
static bool scan_disk(const char *disk, part_t *esp, part_t *rec, part_t *root)
{
    char path[64], v[16];
    snprintf(path, sizeof path, "/sys/block/%s/queue/logical_block_size", disk);
    u32 bs = file_read(path, v, sizeof v) ? (u32)atoi(v) : 512;
    if (bs < 512 || bs > 4096)
        bs = 512;
    snprintf(path, sizeof path, "/dev/%s", disk);
    long fd = lp_open(path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return false;
    static u8 hdr[4096], ents[128 * 128];
    bool found = false;
    memset(esp, 0, sizeof *esp);
    memset(rec, 0, sizeof *rec);
    memset(root, 0, sizeof *root);
    if (lp_lseek((int)fd, bs, 0) == (s64)bs && lp_read((int)fd, hdr, bs) == (long)bs &&
        memcmp(hdr, "EFI PART", 8) == 0) {
        u64 lba;
        u32 num, esz;
        memcpy(&lba, hdr + 72, 8);
        memcpy(&num, hdr + 80, 4);
        memcpy(&esz, hdr + 84, 4);
        if (esz >= 128 && esz <= 512 && num > 0) {
            if (num * esz > sizeof ents)
                num = sizeof ents / esz;
            lp_lseek((int)fd, (s64)(lba * bs), 0);
            if (lp_read((int)fd, ents, num * esz) == (long)(num * esz)) {
                for (u32 i = 0; i < num; i++) {
                    const u8 *e = ents + i * esz;
                    static const u8 zero[16];
                    if (!memcmp(e, zero, 16))
                        continue;
                    char name[40];
                    int k = 0;
                    for (int j = 0; j < 36 && k < 39; j++) {
                        u16 c = (u16)(e[56 + 2 * j] | (e[57 + 2 * j] << 8));
                        if (!c)
                            break;
                        name[k++] = c < 128 ? (char)c : '?';
                    }
                    name[k] = 0;
                    part_t *p = !strcmp(name, "LP-ESP") ? esp :
                                !strcmp(name, "LP-RECOVERY") ? rec :
                                !strcmp(name, "LP-ROOT") ? root : 0;
                    if (!p || p->found)
                        continue;
                    p->num = (int)i + 1;
                    guid_text(e + 16, p->partuuid);
                    p->found = part_name(disk, p->num, p->dev, sizeof p->dev);
                }
                found = rec->found;
            }
        }
    }
    lp_close((int)fd);
    return found;
}

void sys_find_partitions(void)
{
    /* Which disk is "/" on: /sys/dev/block/MAJ:MIN links to
     * .../block/<disk>/<partition>. */
    char mine[48] = "";
    lp_stat_t st;
    if (lp_stat("/", &st, true) == 0) {
        u32 maj = (u32)(((st.dev >> 8) & 0xfff) | ((st.dev >> 32) & ~0xfffull));
        u32 min = (u32)((st.dev & 0xff) | ((st.dev >> 12) & ~0xffull));
        char link[96], target[256];
        snprintf(link, sizeof link, "/sys/dev/block/%u:%u", maj, min);
        long n = lp_readlink(link, target, sizeof target - 1);
        if (n > 0) {
            target[n] = 0;
            char *last = strrchr(target, '/');
            if (last) {
                *last = 0;
                char *disk = strrchr(target, '/');
                if (disk)
                    strlcpy(mine, disk + 1, sizeof mine);
            }
        }
    }
    if (mine[0] && scan_disk(mine, &p_esp, &p_rec, &p_root)) {
        snprintf(disk_dev, sizeof disk_dev, "/dev/%s", mine);
    } else {
        /* "/" is not on an LP disk (a test rig): the first disk that has
         * the layout. */
        char buf[4096];
        long fd = lp_open("/sys/block", O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
        long len = fd >= 0 ? sys_getdents((int)fd, buf, sizeof buf) : 0;
        if (fd >= 0)
            lp_close((int)fd);
        for (long off = 0; off < len; ) {
            dirent64_t *d = (dirent64_t *)(buf + off);
            off += d->reclen;
            if (d->name[0] == '.' || !strncmp(d->name, "loop", 4) || !strncmp(d->name, "ram", 3))
                continue;
            if (scan_disk(d->name, &p_esp, &p_rec, &p_root)) {
                snprintf(disk_dev, sizeof disk_dev, "/dev/%s", d->name);
                break;
            }
        }
    }
    rlog("disk %s: ESP %s, RECOVERY %s, ROOT %s (PARTUUID %s)",
         disk_dev[0] ? disk_dev : "?", p_esp.found ? p_esp.dev : "-",
         p_rec.found ? p_rec.dev : "-", p_root.found ? p_root.dev : "-",
         p_root.found ? p_root.partuuid : "-");
}

/* ── LP-ROOT ──────────────────────────────────────────────────────── */
static bool root_mounted, root_rw;

bool sys_root_mounted(void) { return root_mounted; }

bool sys_root_mount(bool rw)
{
    if (root_mounted)
        return rw ? sys_root_remount(true) : true;
    if (!p_root.found)
        return false;
    long r = lp_mount(p_root.dev, MNT_ROOT, "ext4", rw ? 0 : MS_RDONLY, NULL);
    if (r < 0) {
        rlog("mount %s on %s failed (%ld)", p_root.dev, MNT_ROOT, -r);
        return false;
    }
    root_mounted = true;
    root_rw = rw;
    rlog("LP-ROOT mounted %s", rw ? "read-write" : "read-only");
    return true;
}

bool sys_root_remount(bool rw)
{
    if (!root_mounted)
        return sys_root_mount(rw);
    if (root_rw == rw)
        return true;
    if (!rw)
        lp_sync();
    long r = lp_mount(p_root.dev, MNT_ROOT, "ext4", MS_REMOUNT | (rw ? 0 : MS_RDONLY), NULL);
    if (r < 0) {
        rlog("remount LP-ROOT %s failed (%ld)", rw ? "rw" : "ro", -r);
        return false;
    }
    root_rw = rw;
    rlog("LP-ROOT remounted %s", rw ? "read-write" : "read-only");
    return true;
}

void sys_root_umount(void)
{
    if (!root_mounted)
        return;
    lp_sync();
    long r = lp_umount(MNT_ROOT, 0);
    if (r < 0) {
        rlog("unmount LP-ROOT failed (%ld), detaching", -r);
        lp_umount(MNT_ROOT, MNT_DETACH);
    }
    root_mounted = false;
    root_rw = false;
}

int sys_root_state(void)
{
    if (!p_root.found)
        return -1;
    long fd = lp_open(p_root.dev, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    u8 sb[1024];
    long n = (lp_lseek((int)fd, 1024, 0) == 1024) ? lp_read((int)fd, sb, sizeof sb) : -1;
    lp_close((int)fd);
    if (n != (long)sizeof sb || sb[0x38] != 0x53 || sb[0x39] != 0xEF)
        return -1;
    u16 state = (u16)(sb[0x3A] | (sb[0x3B] << 8));
    u32 errs;
    memcpy(&errs, sb + 0x194, 4);
    return ((state & 2) || errs) ? 1 : 0;
}

/* ── Child processes ──────────────────────────────────────────────── */
static char *const CHILD_ENV[] = {
    "PATH=/usr/sbin:/usr/bin:/sbin:/bin",
    "HOME=/root",
    "LC_ALL=C",
    "TERM=dumb",
    NULL
};

typedef struct { int fd; short events, revents; } pfd_t;

int run_ticking(char *const argv[], void (*on_line)(const char *, void *),
                void *ctx, void (*tick)(void *), void *tctx)
{
    int p[2];
    if (lp_pipe(p) < 0)
        return -1;
    pid_t pid = lp_fork();
    if (pid < 0) {
        lp_close(p[0]);
        lp_close(p[1]);
        return -1;
    }
    if (pid == 0) {
        long null = lp_open("/dev/null", O_RDONLY, 0);
        if (null >= 0)
            lp_dup2((int)null, 0);
        lp_dup2(p[1], 1);
        lp_dup2(p[1], 2);
        lp_close(p[0]);
        lp_execve(argv[0], argv, CHILD_ENV);
        dprintf(2, "cannot run %s\n", argv[0]);
        lp_exit(127);
    }
    lp_close(p[1]);
    char line[512];
    int ll = 0;
    char buf[1024];
    for (;;) {
        pfd_t pf = { p[0], 1, 0 };
        struct { long s, ns; } ts = { 0, 100 * 1000000L };
        long r = sys_call5(SYS_ppoll, (long)&pf, 1, (long)&ts, 0, 8);
        if (tick)
            tick(tctx);
        if (r <= 0)
            continue;
        long n = lp_read(p[0], buf, sizeof buf);
        if (n <= 0)
            break;
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r' || ll == (int)sizeof line - 1) {
                line[ll] = 0;
                if (ll) {
                    rlog("  %s", line);
                    if (on_line)
                        on_line(line, ctx);
                }
                ll = 0;
            } else if (c != '\b')
                line[ll++] = c;
        }
    }
    if (ll) {
        line[ll] = 0;
        rlog("  %s", line);
        if (on_line)
            on_line(line, ctx);
    }
    lp_close(p[0]);
    int status = 0;
    while (lp_waitpid(pid, &status, 0) < 0)
        ;
    return LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : 128;
}

int run(char *const argv[], void (*on_line)(const char *, void *), void *ctx)
{
    return run_ticking(argv, on_line, ctx, NULL, NULL);
}

/* ── The status line ──────────────────────────────────────────────── */
int battery_percent(bool *charging)
{
    char buf[4096];
    long fd = lp_open("/sys/class/power_supply", O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    long len = sys_getdents((int)fd, buf, sizeof buf);
    lp_close((int)fd);
    for (long off = 0; off < len; ) {
        dirent64_t *d = (dirent64_t *)(buf + off);
        off += d->reclen;
        if (d->name[0] == '.')
            continue;
        char f[160], v[32];
        snprintf(f, sizeof f, "/sys/class/power_supply/%s/type", d->name);
        if (!file_read(f, v, sizeof v) || strncmp(v, "Battery", 7))
            continue;
        snprintf(f, sizeof f, "/sys/class/power_supply/%s/capacity", d->name);
        if (!file_read(f, v, sizeof v))
            continue;
        int pct = atoi(v);
        snprintf(f, sizeof f, "/sys/class/power_supply/%s/status", d->name);
        char s[32];
        *charging = file_read(f, s, sizeof s) && !strncmp(s, "Charging", 8);
        return pct;
    }
    return -1;
}

void clock_text(char *out, size_t n)
{
    lp_tm_t tm;
    lp_localtime(lp_time(), &tm);
    lp_strftime_lang(out, n, lpui_korean ? "%a %H:%M" : "%a %H:%M", &tm, 0,
                     lpui_korean ? LP_LANG_KO : LP_LANG_C);
}

/* The installed system's "reduce motion" (COMMON.md Motion): the system
 * switch, or the first account that asked for it. */
bool motion_reduced(void)
{
    if (!sys_root_mounted())
        return false;
    if (lp_exists(MNT_ROOT "/etc/lp/reduce-motion"))
        return true;
    char buf[4096];
    long fd = lp_open(MNT_ROOT "/home", O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (fd < 0)
        return false;
    long len = sys_getdents((int)fd, buf, sizeof buf);
    lp_close((int)fd);
    for (long off = 0; off < len; ) {
        dirent64_t *d = (dirent64_t *)(buf + off);
        off += d->reclen;
        if (d->name[0] == '.')
            continue;
        char f[300];
        snprintf(f, sizeof f, MNT_ROOT "/home/%s/.config/lp/reduce-motion", d->name);
        if (lp_exists(f))
            return true;
    }
    return false;
}
