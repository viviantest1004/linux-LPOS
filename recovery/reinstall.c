/* reinstall.c - put a fresh LP back on LP-ROOT from /reinstall.
 *
 * /reinstall on LP-RECOVERY holds the OS exactly as it was built
 * (tools/mkrecovery.sh): root.tar.zst, the whole desktop root with
 * numeric owners, extended attributes (file capabilities live there) and
 * hard links; esp/ with the kernel and the boot menu; and a manifest
 * with the sha256 of every one of them. Two ways to use it:
 *
 *   Keep my files   everything on LP-ROOT is deleted except /home, then
 *                   the archive is unpacked over it (its own /home left
 *                   out), and the settings that make the machine the
 *                   owner's are put back - KEEP_HARD below, exactly those
 *                   paths, and anything in /etc/lp/ the new system does
 *                   not ship itself (KEEP_SOFT).
 *   Erase everything  mkfs.ext4 on LP-ROOT, unpack, and make it a machine
 *                   nobody has used, the way the installer does
 *                   (desktop/installer/lp-install make_fresh): no
 *                   machine-id, no SSH host keys, the first-start setup
 *                   armed, an fstab naming this disk's partitions.
 *
 * Both finish by rewriting the boot files on the EFI partition from the
 * payload: \EFI\LP\vmlinuz.efi, \EFI\LP\lpboot.efi, \EFI\BOOT\BOOTX64.EFI,
 * \EFI\LP\initrd.img when there is one, and \EFI\LP\cmdline.txt naming
 * LP-ROOT by PARTUUID.
 *
 * ── Nothing is touched until the payload checks out ──
 *
 * Every payload file's sha256 is compared with the manifest first. A
 * mismatch stops here, with LP-ROOT exactly as it was.
 *
 * ── Power loss ──
 *
 * A reinstall cannot be atomic - it replaces a filesystem - so it is
 * made RESUMABLE instead, and the machine is made to come back to it:
 *
 *   - Before the first write, STATE_DIR/reinstall (on LP-RECOVERY, not on
 *     the disk being rewritten) records the mode and the stage reached,
 *     rewritten atomically at each stage, and the boot menu's
 *     LPBootCount is set to 100: "a reinstall is unfinished". Until the
 *     reinstall completes, every boot preselects LP Recovery and says
 *     why, and the recovery menu offers to run it again.
 *   - Keep my files saves the settings it will restore into two tar
 *     files on LP-RECOVERY, fsync'd, BEFORE deleting anything. A second
 *     run after a power cut finds the stage "saved" or later and uses
 *     those copies - the originals may already be gone from LP-ROOT.
 *     /home is never deleted or moved, so it cannot be lost half way.
 *   - Deleting and unpacking are idempotent: a second run deletes
 *     whatever the first one left and unpacks everything again.
 *   - Boot files are written beside the old ones and renamed over them,
 *     so the EFI partition holds a whole old file or a whole new one.
 *   - Only when everything is on disk and synced are the state file and
 *     the saved copies removed and LPBootCount set back to 0.
 *
 * So after a power cut there are two possible states: the old system
 * untouched (the cut came during the checksum), or an unfinished
 * reinstall that the next boot leads straight back to.
 */
#include "recovery.h"
#include "lp-efivar.h"

#define STATE_FILE STATE_DIR "/reinstall"
#define KEEP_HARD_TAR STATE_DIR "/keep-hard.tar"
#define KEEP_SOFT_TAR STATE_DIR "/keep-soft.tar"
#define TAR_LOG "/var/log/lp-recovery-tar.log"
#define TAR  "/usr/bin/tar"
#define ZSTD "/usr/bin/zstd"

/* Put back over the new system, replacing what it ships. These are what
 * make the machine the owner's rather than a fresh one: the accounts and
 * their passwords, the name, time zone, language and keyboard, the saved
 * networks, the SSH identity, the per-user crontabs, the brightness and
 * power settings, and the fstab the installer wrote for this disk. */
static const char *const KEEP_HARD[] = {
    "etc/passwd", "etc/shadow", "etc/group", "etc/gshadow", "etc/subuid", "etc/subgid",
    "etc/hostname", "etc/hosts", "etc/machine-id", "var/lib/dbus/machine-id",
    "etc/localtime", "etc/timezone",
    "etc/default/locale", "etc/locale.conf", "etc/default/keyboard", "etc/vconsole.conf",
    "etc/fstab", "etc/wpa.conf", "etc/wpa_supplicant.conf", "etc/authorized_keys",
    "etc/ssh/ssh_host_rsa_key", "etc/ssh/ssh_host_rsa_key.pub",
    "etc/ssh/ssh_host_ecdsa_key", "etc/ssh/ssh_host_ecdsa_key.pub",
    "etc/ssh/ssh_host_ed25519_key", "etc/ssh/ssh_host_ed25519_key.pub",
    "var/spool/cron/crontabs", "var/lib/lp-tune", "etc/lp-tune.profile", "etc/lp-tune.conf",
    0
};
/* Put back only where the new system has nothing of its own: /etc/lp is
 * where LP's system-wide settings live, beside files the OS ships (such
 * as /etc/lp/services) which must come from the new system. */
static const char *const KEEP_SOFT[] = { "etc/lp", 0 };

/* What a machine nobody has used must not have (lp-install make_fresh). */
static const char *const FRESH_REMOVE[] = {
    "etc/lp/installer-medium", "var/lib/dbus/machine-id",
    "etc/ssh/ssh_host_rsa_key", "etc/ssh/ssh_host_rsa_key.pub",
    "etc/ssh/ssh_host_ecdsa_key", "etc/ssh/ssh_host_ecdsa_key.pub",
    "etc/ssh/ssh_host_ed25519_key", "etc/ssh/ssh_host_ed25519_key.pub",
    "var/lib/systemd/random-seed", 0
};

enum { ST_NONE, ST_STARTED, ST_SAVED, ST_CLEARED, ST_EXTRACTED, ST_RESTORED, ST_BOOT };
static const char *const ST_NAMES[] = {
    "none", "started", "saved", "cleared", "extracted", "restored", "boot"
};

static void (*show_cb)(const re_progress_t *, void *);
static void *show_ctx;
static re_progress_t prog;
static s64 last_show;

static void show(int step, int permille, bool force)
{
    if (permille > 1000)
        permille = 1000;
    bool changed = step != prog.step;
    prog.step = step;
    prog.permille = permille;
    s64 now = lp_monotonic_ms();
    if (show_cb && (changed || force || now - last_show >= 100)) {
        last_show = now;
        show_cb(&prog, show_ctx);
    }
}

/* ── The manifest ─────────────────────────────────────────────────── */
typedef struct { char sha[65]; char path[96]; u64 size; } entry_t;
static entry_t ents[16];
static int nents;
static char version[96], lang[32];

static bool manifest_load(void)
{
    static char buf[8192];
    nents = 0;
    version[0] = lang[0] = 0;
    if (!file_read(PAYLOAD_DIR "/manifest", buf, sizeof buf))
        return false;
    for (char *l = buf; *l; ) {
        char *nl = strchr(l, '\n');
        if (nl)
            *nl = 0;
        if (!strncmp(l, "version ", 8))
            strlcpy(version, l + 8, sizeof version);
        else if (!strncmp(l, "lang ", 5))
            strlcpy(lang, l + 5, sizeof lang);
        else if (!strncmp(l, "sha256 ", 7) && nents < 16) {
            entry_t *e = &ents[nents];
            char *p = l + 7;
            if (strlen(p) > 66 && p[64] == ' ') {
                memcpy(e->sha, p, 64);
                e->sha[64] = 0;
                p += 65;
                char *sp = strchr(p, ' ');
                if (sp) {
                    *sp = 0;
                    e->size = (u64)strtoll(sp + 1, NULL, 10);
                }
                strlcpy(e->path, p, sizeof e->path);
                if (!strstr(e->path, ".."))
                    nents++;
            }
        }
        if (!nl)
            break;
        l = nl + 1;
    }
    return nents > 0;
}

static const entry_t *manifest_find(const char *path)
{
    for (int i = 0; i < nents; i++)
        if (!strcmp(ents[i].path, path))
            return &ents[i];
    return 0;
}

bool re_payload_present(char *ver, size_t n)
{
    if (!manifest_load() || !manifest_find("root.tar.zst"))
        return false;
    strlcpy(ver, version[0] ? version : "LP", n);
    return true;
}

bool re_interrupted(void)
{
    return lp_exists(STATE_FILE);
}

/* ── State ────────────────────────────────────────────────────────── */
static int st_mode = -1, st_stage = ST_NONE;

static void state_load(void)
{
    char buf[128];
    st_mode = -1;
    st_stage = ST_NONE;
    if (!file_read(STATE_FILE, buf, sizeof buf))
        return;
    st_mode = strstr(buf, "mode=erase") ? RE_ERASE : strstr(buf, "mode=keep") ? RE_KEEP : -1;
    char *s = strstr(buf, "stage=");
    if (s)
        for (int i = 0; i <= ST_BOOT; i++)
            if (!strncmp(s + 6, ST_NAMES[i], strlen(ST_NAMES[i])) &&
                (s[6 + strlen(ST_NAMES[i])] == '\n' || !s[6 + strlen(ST_NAMES[i])]))
                st_stage = i;
}

static void state_set(int mode, int stage)
{
    char buf[128];
    int n = snprintf(buf, sizeof buf, "mode=%s\nstage=%s\n",
                     mode == RE_ERASE ? "erase" : "keep", ST_NAMES[stage]);
    file_write_atomic(STATE_FILE, buf, (size_t)n, 0600);
    st_mode = mode;
    st_stage = stage;
    rlog("reinstall: stage %s", ST_NAMES[stage]);
}

/* ── Steps ────────────────────────────────────────────────────────── */
static const char *verify(void)
{
    static u8 buf[1 << 20];
    u64 total = 0, done = 0;
    for (int i = 0; i < nents; i++)
        total += ents[i].size ? ents[i].size : 1;
    for (int i = 0; i < nents; i++) {
        char path[160], hex[130];
        snprintf(path, sizeof path, PAYLOAD_DIR "/%s", ents[i].path);
        long fd = lp_open(path, O_RDONLY | O_CLOEXEC, 0);
        if (fd < 0)
            return ents[i].path;
        lp_digest_t d;
        lp_digest_init(&d, LP_SHA256);
        for (;;) {
            long n = lp_read((int)fd, buf, sizeof buf);
            if (n <= 0)
                break;
            lp_digest_update(&d, buf, (size_t)n);
            done += (u64)n;
            show(LPS_R_RE_VERIFY, (int)(done * 150 / (total ? total : 1)), false);
        }
        lp_close((int)fd);
        lp_digest_final(&d, hex);
        if (strcmp(hex, ents[i].sha) != 0) {
            rlog("reinstall: %s sha256 %s, manifest says %s", ents[i].path, hex, ents[i].sha);
            return ents[i].path;
        }
    }
    rlog("reinstall: payload %s verified (%d files)", version, nents);
    return 0;
}

/* tar with a list of relative paths under MNT_ROOT, only those present. */
static bool save_keep(const char *tarfile, const char *const *list)
{
    char *argv[64];
    int a = 0;
    argv[a++] = TAR;
    argv[a++] = "-C";
    argv[a++] = MNT_ROOT;
    argv[a++] = "-cpf";
    argv[a++] = (char *)tarfile;
    argv[a++] = "--numeric-owner";
    argv[a++] = "--xattrs";
    argv[a++] = "--xattrs-include=*";
    int first = a;
    for (int i = 0; list[i] && a < 62; i++) {
        char p[160];
        lp_stat_t st;
        snprintf(p, sizeof p, MNT_ROOT "/%s", list[i]);
        if (lp_stat(p, &st, false) == 0)
            argv[a++] = (char *)list[i];
    }
    argv[a] = 0;
    lp_unlink(tarfile);
    if (a == first) {
        rlog("reinstall: nothing to keep for %s", tarfile);
        return true;
    }
    int r = run(argv, 0, 0);
    if (r != 0)
        return false;
    long fd = lp_open(tarfile, O_RDONLY | O_CLOEXEC, 0);
    if (fd >= 0) {
        lp_fsync((int)fd);
        lp_close((int)fd);
    }
    return true;
}

static bool restore_keep(const char *tarfile, bool overwrite)
{
    if (!lp_exists(tarfile))
        return true;
    char *argv[] = { TAR, "-C", MNT_ROOT, "-xpf", (char *)tarfile, "--numeric-owner",
                     "--xattrs", "--xattrs-include=*",
                     overwrite ? "--overwrite" : "--skip-old-files", 0 };
    return run(argv, 0, 0) == 0;
}

static void rm_progress_cb(u64 n, void *ctx);
static u64 rm_total;

static const char *clear_root(void)
{
    /* How many inodes are in use is a good guess at how many deletions
     * this will take, which is all the progress bar needs. */
    lp_statfs_t sf;
    rm_total = lp_statfs(MNT_ROOT, &sf) == 0 && sf.files > sf.ffree ? sf.files - sf.ffree : 100000;
    static char buf[8192];
    long fd = lp_open(MNT_ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (fd < 0)
        return "LP-ROOT cannot be read";
    long n = sys_getdents((int)fd, buf, sizeof buf);
    lp_close((int)fd);
    char names[128][64];
    int k = 0;
    for (long off = 0; off < n && k < 128; ) {
        u16 rl;
        memcpy(&rl, buf + off + 16, 2);
        const char *name = buf + off + 19;
        off += rl;
        if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, "home") ||
            !strcmp(name, "lost+found"))
            continue;
        strlcpy(names[k++], name, 64);
    }
    u64 count = 0;
    rm_hook = rm_progress_cb;
    for (int i = 0; i < k; i++) {
        char p[128];
        snprintf(p, sizeof p, MNT_ROOT "/%s", names[i]);
        long r = rm_tree(p, &count);
        if (r < 0)
            rlog("reinstall: removing /%s: error %ld", names[i], -r);
    }
    rm_hook = 0;
    rlog("reinstall: removed %llu entries (kept /home)", (unsigned long long)count);
    return 0;
}

static int rm_lo = 180, rm_hi = 250;
static void rm_progress_cb(u64 n, void *ctx)
{
    (void)ctx;
    u64 t = rm_total ? rm_total : 1;
    if (n > t)
        n = t;
    show(LPS_R_RE_REMOVE, rm_lo + (int)((u64)(rm_hi - rm_lo) * n / t), false);
}

/* zstd -dc < archive | tar -x, fed from here so progress is the share of
 * the compressed file handed over. Their stderr goes to a log file, not a
 * pipe: a pipe nobody reads while this blocks writing would stop tar,
 * then zstd, then us. */
static const char *extract(bool skip_home, int lo, int hi)
{
    const entry_t *e = manifest_find("root.tar.zst");
    long in = lp_open(PAYLOAD_DIR "/root.tar.zst", O_RDONLY | O_CLOEXEC, 0);
    if (!e || in < 0)
        return "root.tar.zst is missing";
    u64 size = e->size ? e->size : 1;
    long logfd = lp_open(TAR_LOG, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    int a[2], b[2];
    if (lp_pipe(a) < 0 || lp_pipe(b) < 0)
        return "no pipes";
    static char *const env[] = { "PATH=/usr/bin:/usr/sbin:/bin", "LC_ALL=C", 0 };
    pid_t z = lp_fork();
    if (z == 0) {
        lp_dup2(a[0], 0);
        lp_dup2(b[1], 1);
        if (logfd >= 0)
            lp_dup2((int)logfd, 2);
        lp_close(a[1]);
        lp_close(b[0]);
        char *argv[] = { ZSTD, "-d", "-c", "-q", 0 };
        lp_execve(ZSTD, argv, env);
        lp_exit(127);
    }
    pid_t t = lp_fork();
    if (t == 0) {
        lp_dup2(b[0], 0);
        if (logfd >= 0) {
            lp_dup2((int)logfd, 1);
            lp_dup2((int)logfd, 2);
        }
        lp_close(a[0]);
        lp_close(a[1]);
        lp_close(b[1]);
        char *argv[] = { TAR, "-x", "-p", "-f", "-", "-C", MNT_ROOT, "--numeric-owner",
                         "--xattrs", "--xattrs-include=*", "--acls",
                         skip_home ? "--exclude=./home" : 0, 0 };
        lp_execve(TAR, argv, env);
        lp_exit(127);
    }
    lp_close(a[0]);
    lp_close(b[0]);
    lp_close(b[1]);
    if (logfd >= 0)
        lp_close((int)logfd);
    static u8 buf[1 << 20];
    u64 fed = 0;
    bool broken = false;
    for (;;) {
        long n = lp_read((int)in, buf, sizeof buf);
        if (n <= 0)
            break;
        for (long off = 0; off < n; ) {
            long w = lp_write(a[1], buf + off, (size_t)(n - off));
            if (w <= 0) { broken = true; break; }
            off += w;
        }
        if (broken)
            break;
        fed += (u64)n;
        show(LPS_R_RE_COPY, lo + (int)((u64)(hi - lo) * fed / size), false);
    }
    lp_close(a[1]);
    lp_close((int)in);
    int zs = 0, ts = 0;
    lp_waitpid(z, &zs, 0);
    lp_waitpid(t, &ts, 0);
    int zc = LP_WIFEXITED(zs) ? LP_WEXITSTATUS(zs) : 128;
    int tc = LP_WIFEXITED(ts) ? LP_WEXITSTATUS(ts) : 128;
    rlog("reinstall: unpacked %llu bytes, zstd %d, tar %d (details in " TAR_LOG ")",
         (unsigned long long)fed, zc, tc);
    if (zc != 0)
        return "decompressing the system failed";
    if (tc != 0)
        return "unpacking the system failed";
    return 0;
}

static void make_fresh(void)
{
    for (int i = 0; FRESH_REMOVE[i]; i++) {
        char p[160];
        snprintf(p, sizeof p, MNT_ROOT "/%s", FRESH_REMOVE[i]);
        lp_unlink(p);
    }
    file_write_atomic(MNT_ROOT "/etc/machine-id", "", 0, 0444);
    mkdirs(MNT_ROOT "/etc/lp", 0755);
    char buf[256];
    int n = snprintf(buf, sizeof buf,
                     "# lp-firstboot runs while this file exists and removes it.\n"
                     "lang=%s\n", lpui_korean ? "ko_KR.UTF-8" : (lang[0] ? lang : "en_US.UTF-8"));
    file_write_atomic(MNT_ROOT "/etc/lp/firstboot", buf, (size_t)n, 0644);
    n = snprintf(buf, sizeof buf,
                 "# Written by LP Recovery. By PARTUUID, which is this disk's alone.\n"
                 "PARTUUID=%s  /      ext4  defaults,errors=remount-ro  0 1\n"
                 "PARTUUID=%s  /boot  vfat  ro,umask=0077              0 2\n",
                 p_root.partuuid, p_esp.partuuid);
    file_write_atomic(MNT_ROOT "/etc/fstab", buf, (size_t)n, 0644);
}

static const char *boot_files(void)
{
    if (!p_esp.found)
        return "the EFI partition (LP-ESP) was not found";
    lp_umount(MNT_ESP, 0);
    long r = lp_mount(p_esp.dev, MNT_ESP, "vfat", 0, NULL);
    if (r < 0) {
        rlog("reinstall: mount %s failed (%ld)", p_esp.dev, -r);
        return "the EFI partition cannot be mounted";
    }
    mkdirs(MNT_ESP "/EFI/LP", 0755);
    mkdirs(MNT_ESP "/EFI/BOOT", 0755);
    static const struct { const char *from, *to; bool need; } F[] = {
        { "esp/vmlinuz.efi", "EFI/LP/vmlinuz.efi", true },
        { "esp/lpboot.efi",  "EFI/LP/lpboot.efi",  true },
        { "esp/lpboot.efi",  "EFI/BOOT/BOOTX64.EFI", true },
        { "esp/initrd.img",  "EFI/LP/initrd.img",  false },
    };
    const char *err = 0;
    for (unsigned i = 0; i < sizeof F / sizeof F[0] && !err; i++) {
        char from[160], to[160], tmp[170];
        snprintf(from, sizeof from, PAYLOAD_DIR "/%s", F[i].from);
        snprintf(to, sizeof to, MNT_ESP "/%s", F[i].to);
        snprintf(tmp, sizeof tmp, "%s.new", to);
        if (!lp_exists(from)) {
            if (F[i].need)
                err = "a boot file is missing from the payload";
            continue;
        }
        if (!file_copy(from, tmp, 0644) || lp_rename(tmp, to) < 0) {
            lp_unlink(tmp);
            err = "writing the boot files failed";
        }
        show(LPS_R_RE_BOOT, 930 + (int)i * 10, false);
    }
    if (!err) {
        /* This disk's root by PARTUUID, then every other word the line
         * already had (console=, quiet, the splash's fbcon font ...) -
         * the installer's rule. Writing the root alone left a system that
         * booted with the kernel's defaults: the boot messages over the
         * splash and no serial console. With no line to start from (the
         * EFI partition was lost), the payload's own is used. */
        static char old[1024], cmd[1100];
        if (!file_read(MNT_ESP "/EFI/LP/cmdline.txt", old, sizeof old) &&
            !file_read(PAYLOAD_DIR "/esp/cmdline.txt", old, sizeof old))
            old[0] = 0;
        int n = snprintf(cmd, sizeof cmd, "root=PARTUUID=%s", p_root.partuuid);
        for (char *w = old; *w && n < (int)sizeof cmd - 2; ) {
            while (*w == ' ' || *w == '\t' || *w == '\r' || *w == '\n')
                w++;
            if (!*w)
                break;
            char *e = w;
            while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n')
                e++;
            if (strncmp(w, "root=", 5) != 0 && n + 1 + (e - w) < (int)sizeof cmd - 2) {
                cmd[n++] = ' ';
                memcpy(cmd + n, w, (size_t)(e - w));
                n += (int)(e - w);
            }
            w = e;
        }
        cmd[n++] = '\n';
        cmd[n] = 0;
        if (!file_write_atomic(MNT_ESP "/EFI/LP/cmdline.txt", cmd, (size_t)n, 0644))
            err = "writing cmdline.txt failed";
    }
    lp_sync();
    lp_umount(MNT_ESP, 0);
    rlog("reinstall: boot files %s", err ? err : "written");
    return err;
}

/* ── The whole thing ──────────────────────────────────────────────── */
const char *re_run(int mode, void (*cb)(const re_progress_t *, void *), void *ctx,
                   bool *damaged)
{
    show_cb = cb;
    show_ctx = ctx;
    *damaged = false;
    rlog("reinstall: %s requested", mode == RE_ERASE ? "erase everything" : "keep my files");
    if (!manifest_load() || !manifest_find("root.tar.zst"))
        return "there are no installation files";
    if (!p_root.found)
        return "LP-ROOT was not found";

    show(LPS_R_RE_VERIFY, 0, true);
    const char *bad = verify();
    if (bad) {
        *damaged = true;
        return bad;
    }

    /* From here on LP-ROOT changes. */
    state_load();
    if (st_mode != mode) {
        /* A different kind of reinstall than the one interrupted: start
         * from the beginning (and a keep after an erase has nothing left
         * to keep but whatever is there now). */
        lp_unlink(KEEP_HARD_TAR);
        lp_unlink(KEEP_SOFT_TAR);
        st_stage = ST_NONE;
    }
    if (st_stage < ST_STARTED)
        state_set(mode, ST_STARTED);
    if (lp_efivars_ready())
        lp_bootcount_set(LP_BOOT_REINSTALL_MARK);

    const char *err = 0;
    if (mode == RE_KEEP) {
        if (!sys_root_mount(true))
            return "LP-ROOT cannot be mounted - run Check and repair disks, or Erase everything";
        if (st_stage < ST_SAVED) {
            show(LPS_R_RE_SAVE, 150, true);
            if (!save_keep(KEEP_HARD_TAR, KEEP_HARD) || !save_keep(KEEP_SOFT_TAR, KEEP_SOFT))
                return "saving your settings failed - nothing was deleted";
            lp_sync();
            state_set(mode, ST_SAVED);
        }
        if (st_stage < ST_CLEARED) {
            show(LPS_R_RE_REMOVE, 180, true);
            if ((err = clear_root()) != 0)
                return err;
            lp_sync();
            state_set(mode, ST_CLEARED);
        }
        if (st_stage < ST_EXTRACTED) {
            show(LPS_R_RE_COPY, 250, true);
            if ((err = extract(true, 250, 900)) != 0)
                return err;
            show(LPS_R_RE_FINISH, 900, true);
            lp_sync();
            state_set(mode, ST_EXTRACTED);
        }
        if (st_stage < ST_RESTORED) {
            show(LPS_R_RE_RESTORE, 905, true);
            if (!restore_keep(KEEP_HARD_TAR, true) || !restore_keep(KEEP_SOFT_TAR, false))
                return "restoring your settings failed - run Reinstall LP again";
            lp_unlink(MNT_ROOT "/etc/lp/firstboot");
            lp_unlink(MNT_ROOT "/etc/lp/installer-medium");
            lp_sync();
            state_set(mode, ST_RESTORED);
        }
    } else {
        if (st_stage < ST_EXTRACTED) {
            show(LPS_R_RE_FORMAT, 150, true);
            sys_root_umount();
            char *argv[] = { "/usr/sbin/mkfs.ext4", "-F", "-q", "-L", "LP-ROOT", p_root.dev, 0 };
            int r = run(argv, 0, 0);
            if (r != 0)
                return "formatting LP-ROOT failed";
            show(LPS_R_RE_FORMAT, 200, true);
            if (!sys_root_mount(true))
                return "the new LP-ROOT cannot be mounted";
            show(LPS_R_RE_COPY, 200, true);
            if ((err = extract(false, 200, 900)) != 0)
                return err;
            show(LPS_R_RE_FINISH, 900, true);
            lp_sync();
            state_set(mode, ST_EXTRACTED);
        } else if (!sys_root_mount(true))
            return "LP-ROOT cannot be mounted";
        if (st_stage < ST_RESTORED) {
            make_fresh();
            lp_sync();
            state_set(mode, ST_RESTORED);
        }
    }

    show(LPS_R_RE_BOOT, 930, true);
    if ((err = boot_files()) != 0)
        return err;
    state_set(mode, ST_BOOT);

    show(LPS_R_RE_FINISH, 980, true);
    lp_sync();
    lp_unlink(KEEP_HARD_TAR);
    lp_unlink(KEEP_SOFT_TAR);
    lp_unlink(STATE_FILE);
    lp_sync();
    if (lp_efivars_ready())
        lp_bootcount_set(0);
    rlog("reinstall: finished (%s)", mode == RE_ERASE ? "erase everything" : "keep my files");
    show(LPS_R_RE_FINISH, 1000, true);
    return 0;
}
