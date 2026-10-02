/* lp-privd - the one root process the desktop may ask to change packages
 * and disks.
 *
 *   lp-privd -d [--socket PATH]      the daemon; /etc/services starts it
 *   lp-privd <verb> [arg...]          ask it something, print the answer
 *   lp-privd help                     the verbs
 *
 * The software centre installs and removes packages, and the Disks
 * application formats, partitions, checks and mounts drives. Both need
 * root, and the session that runs them is uid 1000 on purpose. Something
 * has to stand between the two, and what it is decides what a bug in
 * either application can cost.
 *
 * ── Why a daemon and not a setuid program ──
 *
 * lp-power is setuid, and it can be, because it takes one word and does
 * one kill(). A package manager is different: it runs dpkg, dpkg runs
 * maintainer scripts, and all of that would inherit whatever the caller
 * handed a setuid process - the environment, the open descriptors, the
 * current directory, the umask and, worst, the resource limits. Setting
 * RLIMIT_FSIZE low before calling a setuid installer is the classic way
 * to make dpkg write half of /usr/lib/x86_64-linux-gnu/libc.so.6 and
 * stop. A setuid process can also be stopped or killed at any moment by
 * the user who started it, which is exactly the wrong property for
 * something halfway through rewriting the package database.
 *
 * A daemon started by init has none of that. It was born with init's
 * environment and limits, the caller cannot signal it, and what the
 * caller can influence is reduced to one line of text on a socket. It
 * costs one sleeping process: a static binary blocked in poll(), which
 * is a few hundred kilobytes and no wakeups at all. It also serialises
 * the work for free - there is one daemon, so there is one dpkg.
 *
 * ── Who may ask ──
 *
 * /run/lp-privd.sock is mode 0666: anybody can connect, and the kernel
 * then says who connected (SO_PEERCRED - uid, gid and pid, taken when
 * the peer called connect(), and not something the peer can choose).
 * libc has the getsockopt number and no wrapper, so peer_cred() below is
 * the whole of that wrapper.
 *
 * Changing anything needs uid 0 or membership of group sudo, the same
 * rule sudo itself applies. Membership is read from /etc/group by user
 * name rather than from the peer's supplementary groups
 * (SO_PEERGROUPS): the desktop session is started by dropprivs with no
 * supplementary groups at all, so the process-level answer would be "no"
 * for everybody, and the account database is also what makes removing
 * somebody from sudo take effect at once instead of at their next login.
 *
 * Three verbs only read - ping, status and probe - and those are open to
 * root and to any real account (uid >= 1000), the rule lp-power and
 * lp-tune use. They tell the applications whether to offer the buttons
 * at all, and what the filesystems are called; reading a filesystem
 * label needs the raw device, which uid 1000 cannot open.
 *
 * ── The password ──
 *
 * Being in group sudo is not enough on its own. The owner's rule for the
 * normal system is that nothing becomes administrator silently: sudo
 * asks the person's own password, and so does this. A browser tab that
 * found a way to run code as uid 1000 could otherwise connect here and
 * reformat a USB disk without anybody typing anything.
 *
 * So every verb that changes something also needs a live "keep": the
 * caller's uid proved its password, with the `auth` verb, less than five
 * minutes ago. That is polkit's auth_admin_keep and sudo's timestamp -
 * one dialog covers the install and the "update all" after it - and it
 * is kept per uid, in this process's memory only, so a restart of the
 * daemon or a reboot forgets it. The five minutes are counted on
 * CLOCK_BOOTTIME, which keeps running while the laptop is suspended: a
 * lid closed for an hour must not come back still authorised. The keep
 * is not stretched by use either; five minutes after the password it
 * ends, however busy the software centre has been.
 *
 * The password travels as hex in the request line. The line has to stay
 * printable ASCII (that rule is what stops a request from smuggling a
 * second one), and a password can be Korean. It is checked against
 * /etc/shadow with libc's crypt6 - the same check sudo, su and login use
 * - and the buffers holding it are wiped as soon as the answer is known.
 * The log says that an auth happened and how it ended, never what was
 * typed: the request as logged reads "auth ***".
 *
 * A wrong password is refused, logged, and makes that uid wait before
 * the next try - two seconds after the first, growing by two a time to
 * thirty. The wait is a time stamp, not a sleep, so one person guessing
 * does not stall anybody else's request. Root (uid 0) never needs a
 * password here: it already is what the password would buy, and the
 * recovery shell, which is root by design, must be able to use this
 * without one.
 *
 * ── What a request can contain ──
 *
 * One line, at most 1024 bytes, fields separated by TAB. The first field
 * is a verb from a fixed table; the rest are checked against the shape
 * that verb takes, character by character, before anything else happens:
 * a Debian package name, one of our package names, a kernel block device
 * name such as "sdb1", one of four filesystem names, a size in MiB, or a
 * volume label from a short alphabet. None of those alphabets contains
 * '/', so no request can ever carry a path, and nothing ever reaches a
 * shell: every command is started with execve() from an absolute path in
 * a table below, with an argv built here and an environment built here.
 *
 * ── What it refuses no matter who asks ──
 *
 * The disk the running system is on - whatever carries /, /boot,
 * /boot/efi, /usr, /var, /home or /data, or an active swap area,
 * followed through device-mapper to the real disk. Any partition that is
 * mounted, and any whole-disk operation on a disk with something
 * mounted. Any device another device is built on (a holder in sysfs).
 * Unmounting anything outside /media and /mnt. Removing a package whose
 * removal apt says would take the desktop's own packages with it.
 *
 * ── Every action is written down ──
 *
 * /var/log/lp-privd.log gets one line per request - who, what, and the
 * verdict - one line per command run, and one for how it ended. The
 * kernel log gets the same lines through lp_log. The full output of the
 * last job is kept in /var/log/lp-privd.last, because "it failed" is
 * what the application shows and the reason is in the forty lines apt
 * printed before that.
 *
 * ── The answer ──
 *
 * A stream of lines, ending with exactly one "done ..." or "fail <why>
 * ...". In between: "log <text>" for output, "progress <0-100> <text>"
 * where there is a number to give (apt's Status-Fd, e2fsck's -C), and
 * "dev ..." lines from probe. <why> is one of denied, invalid, refused,
 * busy, missing, failed or auth, so an application can tell "you may
 * not" from "this broke" - and "auth" from both: it means "ask the
 * person for their password, send `auth`, and try again", which is what
 * the applications' password dialog and this program's own client do.
 *
 * A job keeps going when its client goes away. Closing the software
 * centre halfway through an install must not leave dpkg interrupted, so
 * output to a client that has left is dropped and the work finishes.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"
#include "syscall.h"
#include "disk.h"
#include "crypt6.h"

#define SOCK_PATH     "/run/lp-privd.sock"
#define JOB_FILE      "/run/lp-privd.job"
#define LOG_PATH      "/var/log/lp-privd.log"
#define LAST_PATH     "/var/log/lp-privd.last"
#define LOG_ROTATE    (1024 * 1024)

#define MAX_REQ       1024
#define MAX_FIELDS    34
#define REQ_TIMEOUT   3000       /* ms a client gets to send its line */

#define KEEP_MS       (5 * 60 * 1000)   /* a password lasts this long */
#define KEEP_SLOTS    32                /* uids remembered at once */
#define WAIT_STEP_MS  2000              /* after each wrong password ... */
#define WAIT_MAX_MS   30000             /* ... up to this */

#define AF_UNIX_      1
#define SO_PEERCRED_  17
#define MSG_DONTWAIT_ 0x40
#define MSG_NOSIGNAL_ 0x4000
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

/* umask has no wrapper either, and the number is different on each of
 * the three machines. The child processes get 022 whatever init had. */
#if defined(__x86_64__)
#  define SYS_umask_ 95
#elif defined(__aarch64__)
#  define SYS_umask_ 166
#else
#  define SYS_umask_ 60
#endif

/* ═══════════════════════════════════════════════════════════════════
 * Small pieces
 * ═══════════════════════════════════════════════════════════════════ */

static const char *sock_path = SOCK_PATH;

/* The peer's credentials. struct ucred is three 32-bit words - pid, uid,
 * gid - on every architecture this builds for. */
static bool peer_cred(int fd, u32 *pid, u32 *uid, u32 *gid)
{
    u32 cred[3];
    u32 len = sizeof cred;
    long r = sys_call5(SYS_getsockopt, fd, SOL_SOCKET, SO_PEERCRED_,
                       (long)cred, (long)&len);
    if (r < 0 || len < sizeof cred)
        return false;
    *pid = cred[0];
    *uid = cred[1];
    *gid = cred[2];
    return true;
}

typedef struct { u16 family; char path[108]; } sun_t;

static bool sun_fill(sun_t *sa, const char *path)
{
    memset(sa, 0, sizeof *sa);
    sa->family = AF_UNIX_;
    if (strlen(path) >= sizeof sa->path)
        return false;
    strlcpy(sa->path, path, sizeof sa->path);
    return true;
}

/* A value out of sysfs, newline stripped. */
static bool sys_read(const char *path, char *buf, size_t n)
{
    long r = proc_read(path, buf, n);
    if (r <= 0) {
        if (n) buf[0] = '\0';
        return false;
    }
    if ((size_t)r >= n) r = (long)n - 1;
    buf[r] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return true;
}

static bool starts(const char *s, const char *p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

/* Only bytes from `allowed` (plus a-z and 0-9, which every alphabet
 * here takes), between 1 and max long, and not starting with `-` - a
 * value that starts with a dash is an option to whatever it is handed
 * to, which is the one injection an argv without a shell still has. */
static bool alphabet_ok(const char *s, const char *allowed, size_t max,
                        bool upper)
{
    size_t n = strlen(s);
    if (n == 0 || n > max || s[0] == '-')
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            continue;
        if (upper && c >= 'A' && c <= 'Z')
            continue;
        if (!strchr(allowed, c))
            return false;
    }
    return true;
}

/* Debian Policy 5.6.1: lower case letters, digits, + - . ; at least two
 * characters; must start with a letter or digit. */
static bool debian_name_ok(const char *s)
{
    return strlen(s) >= 2 && alphabet_ok(s, "+-.", 100, false) &&
           s[0] != '.' && s[0] != '+';
}

/* A Flatpak application ID: reverse DNS, at least three parts (org.gimp.GIMP,
 * com.discordapp.Discord, us.zoom.Zoom). Each part is letters, digits, _
 * and -, and does not start with a digit or a dash - the D-Bus rules
 * Flatpak holds application IDs to. Never an option: it cannot start with
 * a dash, and it never contains a slash, so it cannot name a ref or a
 * path either. */
static bool flatpak_id_ok(const char *s)
{
    size_t n = strlen(s), parts = 1;
    if (n < 5 || n > 255)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool first = i == 0 || s[i - 1] == '.';
        if (c == '.') {
            if (first || i == n - 1)
                return false;
            parts++;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            continue;
        } else if ((c >= '0' && c <= '9') || c == '-') {
            if (first)
                return false;
        } else {
            return false;
        }
    }
    return parts >= 3;
}

/* Our own package names are file names in /data/pkg/db. */
static bool lp_name_ok(const char *s)
{
    return alphabet_ok(s, "+-._", 64, false) && s[0] != '.';
}

/* A kernel block device name: sda, sdb1, nvme0n1p2, mmcblk0p1, loop3. */
static bool dev_name_ok(const char *s)
{
    return alphabet_ok(s, "", 31, false) && s[0] >= 'a' && s[0] <= 'z';
}

/* Volume labels. Spaces are allowed and nothing else a shell or a
 * filesystem would read as structure. */
static bool label_ok(const char *s, size_t max)
{
    return alphabet_ok(s, " _.-", max, true) && s[0] != ' ';
}

static bool size_ok(const char *s)
{
    if (strcmp(s, "rest") == 0)
        return true;
    size_t n = strlen(s);
    if (n == 0 || n > 8)
        return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return atoi(s) > 0;
}

typedef enum { FS_NONE, FS_EXT4, FS_FAT32, FS_EXFAT, FS_NTFS } fstype_t;

static fstype_t fs_parse(const char *s)
{
    if (strcmp(s, "ext4") == 0)  return FS_EXT4;
    if (strcmp(s, "fat32") == 0) return FS_FAT32;
    if (strcmp(s, "exfat") == 0) return FS_EXFAT;
    if (strcmp(s, "ntfs") == 0)  return FS_NTFS;
    return FS_NONE;
}

/* How long a label each filesystem takes. FAT and exFAT keep 11
 * characters; ext4 16; NTFS far more, cut to 32 here for the screen. */
static size_t label_max(fstype_t f)
{
    switch (f) {
    case FS_EXT4:  return 16;
    case FS_FAT32: return 11;
    case FS_EXFAT: return 11;
    case FS_NTFS:  return 32;
    default:       return 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * The log
 * ═══════════════════════════════════════════════════════════════════ */

static void stamp(char *out, size_t n)
{
    lp_tm_t tm;
    lp_gmtime(lp_time(), &tm);
    snprintf(out, n, "%d-%02d-%02dT%02d:%02d:%02dZ", tm.year, tm.mon,
             tm.day, tm.hour, tm.min, tm.sec);
}

/* One line in /var/log/lp-privd.log and in the kernel log. The file is
 * cut at 1MB into a single .1 so that a machine left running for years
 * cannot be filled up by its own audit trail. */
static void audit(const char *fmt_line)
{
    char ts[32], line[1200];
    stamp(ts, sizeof ts);
    int n = snprintf(line, sizeof line, "%s %s\n", ts, fmt_line);
    if (n <= 0)
        return;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;

    lp_stat_t st;
    if (lp_stat(LOG_PATH, &st, true) == 0 && st.size > LOG_ROTATE)
        lp_rename(LOG_PATH, LOG_PATH ".1");

    long fd = lp_open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                      0640);
    if (fd >= 0) {
        lp_write((int)fd, line, (size_t)n);
        lp_close((int)fd);
    }
    lp_log("lp-privd", fmt_line);
}

/* ═══════════════════════════════════════════════════════════════════
 * Talking to the client
 * ═══════════════════════════════════════════════════════════════════ */

static int client = -1;          /* the socket of whoever asked */
static int last_fd = -1;         /* /var/log/lp-privd.last while a job runs */

/* Never blocks and never raises SIGPIPE. A client that stopped reading
 * or went away loses lines; the job does not wait for it. */
static void send_raw(const char *s, size_t n)
{
    if (last_fd >= 0)
        lp_write(last_fd, s, n);
    if (client < 0)
        return;
    while (n > 0) {
        long w = lp_sendto(client, s, n, MSG_DONTWAIT_ | MSG_NOSIGNAL_, NULL, 0);
        if (w <= 0) {
            if (w == -11)            /* EAGAIN: full - drop the rest */
                return;
            lp_close(client);
            client = -1;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

/* A reply line. Control characters in `text` (a tab in some output, an
 * escape sequence from a progress bar) become spaces, so every line the
 * client reads has exactly the shape the protocol promises. */
static void reply(const char *kind, const char *text)
{
    char line[1100];
    size_t k = strlcpy(line, kind, sizeof line);
    if (text && text[0] && k < sizeof line - 2) {
        line[k++] = ' ';
        for (const char *p = text; *p && k < sizeof line - 2; p++) {
            unsigned char c = (unsigned char)*p;
            line[k++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
        }
    }
    line[k++] = '\n';
    send_raw(line, k);
}

static void say(const char *text) { reply("log", text); }

static int progress_last = -1;
static char job_desc[256];

/* The job file is how a second window finds out what the first one
 * started, after the first one was closed. */
static void job_file(int pct, const char *text)
{
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%s\n%d %s\n", job_desc, pct,
                     text ? text : "");
    if (n > 0)
        lp_write_file_atomic(JOB_FILE, buf, (size_t)n);
}

static void progress(int pct, const char *text)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char line[300];
    snprintf(line, sizeof line, "%d %s", pct, text ? text : "");
    reply("progress", line);
    if (pct != progress_last) {
        progress_last = pct;
        job_file(pct, text);
    }
}

static void fail(const char *why, const char *text)
{
    char line[1024];
    snprintf(line, sizeof line, "%s %s", why, text);
    reply("fail", line);
}

/* ═══════════════════════════════════════════════════════════════════
 * Block devices, from sysfs
 *
 * Everything here goes by the kernel's own name for a device and by
 * /sys/class/block, not by the MBR - the NVMe disk this is installed on
 * is GPT, and libc's disk_parts() would see one protective entry there.
 * sysfs knows every partition whatever table described it.
 * ═══════════════════════════════════════════════════════════════════ */

static bool blk_exists(const char *name)
{
    char p[96];
    snprintf(p, sizeof p, "/sys/class/block/%s/dev", name);
    return lp_exists(p);
}

static bool blk_is_part(const char *name)
{
    char p[96];
    snprintf(p, sizeof p, "/sys/class/block/%s/partition", name);
    return lp_exists(p);
}

static int blk_partno(const char *name)
{
    char p[96], v[16];
    snprintf(p, sizeof p, "/sys/class/block/%s/partition", name);
    return sys_read(p, v, sizeof v) ? atoi(v) : 0;
}

/* The disk a partition is on. /sys/class/block/sdb1 is a link to
 * .../block/sdb/sdb1, so the parent is the second-to-last component. */
static bool blk_parent(const char *name, char *out, size_t n)
{
    char p[96], link[512];
    snprintf(p, sizeof p, "/sys/class/block/%s", name);
    long r = lp_readlink(p, link, sizeof link - 1);
    if (r <= 0)
        return false;
    link[r] = '\0';
    char *last = strrchr(link, '/');
    if (!last)
        return false;
    *last = '\0';
    char *prev = strrchr(link, '/');
    strlcpy(out, prev ? prev + 1 : link, n);
    return true;
}

/* The whole disk: itself, or its parent. */
static void blk_disk(const char *name, char *out, size_t n)
{
    if (!blk_is_part(name) || !blk_parent(name, out, n))
        strlcpy(out, name, n);
}

static bool blk_devnum(const char *name, u32 *maj, u32 *min)
{
    char p[96], v[32];
    snprintf(p, sizeof p, "/sys/class/block/%s/dev", name);
    if (!sys_read(p, v, sizeof v))
        return false;
    char *colon = strchr(v, ':');
    if (!colon)
        return false;
    *colon = '\0';
    *maj = (u32)atoi(v);
    *min = (u32)atoi(colon + 1);
    return true;
}

/* /dev/<name>, checked to be the block device sysfs says it is. /dev is
 * root's, so this is not a defence against the caller; it catches a
 * stale node from a device that went away and came back numbered
 * differently, which would otherwise send mkfs to the wrong disk. */
static bool dev_path(const char *name, char *out, size_t n)
{
    u32 maj, min;
    if (!blk_devnum(name, &maj, &min))
        return false;
    snprintf(out, n, "/dev/%s", name);
    lp_stat_t st;
    if (lp_stat(out, &st, true) != 0 ||
        (st.mode & LP_S_IFMT) != LP_S_IFBLK)
        return false;
    u32 smaj = (u32)(((st.rdev >> 8) & 0xfff) | ((st.rdev >> 32) & ~0xfffu));
    u32 smin = (u32)((st.rdev & 0xff) | ((st.rdev >> 12) & ~0xffu));
    return smaj == maj && smin == min;
}

/* Every block device name the kernel has, in directory order. */
static int blk_all(char names[][32], int max)
{
    long fd = lp_open("/sys/class/block", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 0;
    int n = 0;
    char buf[4096];
    long got;
    while ((got = sys_getdents((int)fd, buf, sizeof buf)) > 0) {
        for (long off = 0; off < got; ) {
            u16 len = *(u16 *)(buf + off + DIRENT_RECLEN);
            const char *nm = buf + off + DIRENT_NAME;
            off += len;
            if (nm[0] == '.' || n >= max || strlen(nm) >= 32)
                continue;
            strlcpy(names[n++], nm, 32);
        }
    }
    lp_close((int)fd);
    return n;
}

/* The partitions of one disk. */
static int blk_parts(const char *disk, char names[][32], int max)
{
    char all[128][32];
    int na = blk_all(all, 128), n = 0;
    for (int i = 0; i < na && n < max; i++) {
        char parent[32];
        if (blk_is_part(all[i]) && blk_parent(all[i], parent, sizeof parent) &&
            strcmp(parent, disk) == 0)
            strlcpy(names[n++], all[i], 32);
    }
    return n;
}

/* Does anything sit on top of this device - device-mapper, md, a
 * LUKS mapping? Then writing to it pulls the floor out from under that. */
static bool blk_has_holders(const char *name)
{
    char p[96];
    snprintf(p, sizeof p, "/sys/class/block/%s/holders", name);
    long fd = lp_open(p, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return false;
    char buf[1024];
    long got = sys_getdents((int)fd, buf, sizeof buf);
    lp_close((int)fd);
    for (long off = 0; off < got; ) {
        u16 len = *(u16 *)(buf + off + DIRENT_RECLEN);
        const char *nm = buf + off + DIRENT_NAME;
        off += len;
        if (nm[0] != '.')
            return true;
    }
    return false;
}

/* ── Mounts ─────────────────────────────────────────────────────────
 *
 * /proc/self/mountinfo, not /proc/mounts, because it gives the device
 * number: a mount of /dev/disk/by-uuid/..., of /dev/root, or of a
 * device-mapper name all come back as the same major:minor, while their
 * spelled-out names in /proc/mounts have nothing in common. */

typedef struct {
    u32  maj, min;
    char point[256];
    char fstype[24];
} mnt_t;

static char mountinfo[32768];

static int mounts_read(mnt_t *out, int max)
{
    long got = proc_read("/proc/self/mountinfo", mountinfo, sizeof mountinfo - 1);
    if (got <= 0)
        return 0;
    mountinfo[got] = '\0';
    int n = 0;
    for (char *line = mountinfo; line && *line && n < max; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        /* id parent maj:min root point opts ... - fstype source superopts */
        char *f[6] = {0};
        char *p = line;
        for (int i = 0; i < 6 && p; i++) {
            f[i] = p;
            p = strchr(p, ' ');
            if (p) *p++ = '\0';
        }
        char *dash = p ? strstr(p, " - ") : NULL;
        if (!dash && p && starts(p, "- ")) dash = p - 1;
        if (f[2] && f[4] && dash) {
            char *colon = strchr(f[2], ':');
            if (colon) {
                *colon = '\0';
                out[n].maj = (u32)atoi(f[2]);
                out[n].min = (u32)atoi(colon + 1);
                /* mountinfo escapes spaces as \040; undo that one. */
                size_t k = 0;
                for (char *q = f[4]; *q && k < sizeof out[n].point - 1; q++) {
                    if (q[0] == '\\' && q[1] == '0' && q[2] == '4' && q[3] == '0') {
                        out[n].point[k++] = ' ';
                        q += 3;
                    } else {
                        out[n].point[k++] = *q;
                    }
                }
                out[n].point[k] = '\0';
                char *fs = dash + 3;
                char *sp = strchr(fs, ' ');
                if (sp) *sp = '\0';
                strlcpy(out[n].fstype, fs, sizeof out[n].fstype);
                n++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    return n;
}

static mnt_t mnts[256];
static int   nmnts;

static void mounts_refresh(void) { nmnts = mounts_read(mnts, 256); }

/* Where this device is mounted: the first mount point, or NULL. */
static const char *mounted_at(const char *name)
{
    u32 maj, min;
    if (!blk_devnum(name, &maj, &min))
        return NULL;
    for (int i = 0; i < nmnts; i++)
        if (mnts[i].maj == maj && mnts[i].min == min)
            return mnts[i].point;
    return NULL;
}

/* The disks behind a device, through device-mapper and md: a partition
 * gives its disk, and dm-0 gives the disks of everything in its
 * slaves/. `depth` stops a loop in a sysfs that has gone wrong. */
static void disks_under(const char *name, char out[][32], int *n, int max,
                        int depth)
{
    if (depth > 4 || *n >= max)
        return;
    char p[96];
    snprintf(p, sizeof p, "/sys/class/block/%s/slaves", name);
    long fd = lp_open(p, O_RDONLY | O_DIRECTORY, 0);
    bool had_slave = false;
    if (fd >= 0) {
        char buf[1024];
        long got = sys_getdents((int)fd, buf, sizeof buf);
        lp_close((int)fd);
        for (long off = 0; off < got; ) {
            u16 len = *(u16 *)(buf + off + DIRENT_RECLEN);
            const char *nm = buf + off + DIRENT_NAME;
            off += len;
            if (nm[0] == '.')
                continue;
            had_slave = true;
            disks_under(nm, out, n, max, depth + 1);
        }
    }
    if (had_slave)
        return;
    char disk[32];
    blk_disk(name, disk, sizeof disk);
    for (int i = 0; i < *n; i++)
        if (strcmp(out[i], disk) == 0)
            return;
    strlcpy(out[(*n)++], disk, 32);
}

/* The name of the block device with this number, or false. */
static bool name_of(u32 maj, u32 min, char *out, size_t n)
{
    char p[64], link[512];
    snprintf(p, sizeof p, "/sys/dev/block/%u:%u", maj, min);
    long r = lp_readlink(p, link, sizeof link - 1);
    if (r <= 0)
        return false;
    link[r] = '\0';
    char *last = strrchr(link, '/');
    strlcpy(out, last ? last + 1 : link, n);
    return true;
}

/* Is this disk (a whole-disk name) one the running system stands on? */
static bool is_system_disk(const char *disk, char *why, size_t whyn)
{
    static const char *const vital[] = {
        "/", "/boot", "/boot/efi", "/efi", "/usr", "/var", "/home", "/data",
        NULL
    };
    char disks[16][32];
    for (int v = 0; vital[v]; v++) {
        for (int i = 0; i < nmnts; i++) {
            if (strcmp(mnts[i].point, vital[v]) != 0 || mnts[i].maj == 0)
                continue;
            char name[32];
            if (!name_of(mnts[i].maj, mnts[i].min, name, sizeof name))
                continue;
            int nd = 0;
            disks_under(name, disks, &nd, 16, 0);
            for (int k = 0; k < nd; k++) {
                if (strcmp(disks[k], disk) == 0) {
                    snprintf(why, whyn, "%s holds %s, which the running"
                             " system is using", disk, vital[v]);
                    return true;
                }
            }
        }
    }

    /* Swap in use: /proc/swaps names it by path. */
    char sw[2048];
    long got = proc_read("/proc/swaps", sw, sizeof sw - 1);
    if (got > 0) {
        sw[got] = '\0';
        for (char *line = sw; line && *line; ) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (starts(line, "/dev/")) {
                char name[32];
                size_t k = 0;
                for (const char *q = line + 5; *q && *q != ' ' && *q != '\t' &&
                     k < sizeof name - 1; q++)
                    name[k++] = *q;
                name[k] = '\0';
                int nd = 0;
                if (blk_exists(name))
                    disks_under(name, disks, &nd, 16, 0);
                for (int i = 0; i < nd; i++) {
                    if (strcmp(disks[i], disk) == 0) {
                        snprintf(why, whyn, "%s has swap on it that is in use",
                                 disk);
                        return true;
                    }
                }
            }
            line = nl ? nl + 1 : NULL;
        }
    }
    return false;
}

/* Is anything on this disk mounted - the disk itself or any partition? */
static bool disk_busy(const char *disk, char *why, size_t whyn)
{
    const char *at = mounted_at(disk);
    if (at) {
        snprintf(why, whyn, "%s is mounted at %s", disk, at);
        return true;
    }
    char parts[64][32];
    int np = blk_parts(disk, parts, 64);
    for (int i = 0; i < np; i++) {
        at = mounted_at(parts[i]);
        if (at) {
            snprintf(why, whyn, "%s is mounted at %s", parts[i], at);
            return true;
        }
        if (blk_has_holders(parts[i])) {
            snprintf(why, whyn, "%s is in use by another device", parts[i]);
            return true;
        }
    }
    if (blk_has_holders(disk)) {
        snprintf(why, whyn, "%s is in use by another device", disk);
        return true;
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════════
 * What is on a device
 *
 * libc's disk_identify knows ext, FAT and swap - what the boards this
 * system started on can meet. A laptop's USB sticks are exFAT and its
 * external disks NTFS, and Disks has to be able to say so, so the probe
 * here reads those two as well, plus the handful of others a person
 * will actually plug in. Read-only, a few sectors each.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    char fs[16];
    char label[96];            /* UTF-8 */
    char uuid[40];
    char state[16];            /* ext: clean / errors; else empty */
    char table[8];             /* whole disks: mbr / gpt / "" */
} probe_t;

static bool read_at(int fd, u64 off, void *buf, size_t n)
{
    if (lp_lseek(fd, (off_t)off, 0) < 0)
        return false;
    size_t got = 0;
    while (got < n) {
        long r = lp_read(fd, (u8 *)buf + got, n - got);
        if (r <= 0)
            return false;
        got += (size_t)r;
    }
    return true;
}

static u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 le64(const u8 *p) { return (u64)le32(p) | ((u64)le32(p + 4) << 32); }

/* A label as it came off the disk, cut at the first NUL and trimmed.
 * Bytes that are not printable ASCII are kept only if they are UTF-8;
 * anything else becomes '?', so the reply stays one clean line. */
static void copy_label(const u8 *src, size_t n, char *out, size_t outn)
{
    size_t k = 0;
    for (size_t i = 0; i < n && src[i] && k < outn - 1; i++) {
        u8 c = src[i];
        out[k++] = (c >= 0x20 && c != 0x7f) ? (char)c : '?';
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = '\0';
}

/* UTF-16LE (exFAT and NTFS labels) to UTF-8. */
static void utf16_label(const u8 *src, size_t units, char *out, size_t outn)
{
    size_t k = 0;
    for (size_t i = 0; i < units; i++) {
        u32 c = le16(src + 2 * i);
        if (c == 0)
            break;
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < units) {
            u32 lo = le16(src + 2 * (i + 1));
            c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
            i++;
        }
        if (c < 0x20) c = '?';
        if (c < 0x80) {
            if (k + 1 >= outn) break;
            out[k++] = (char)c;
        } else if (c < 0x800) {
            if (k + 2 >= outn) break;
            out[k++] = (char)(0xc0 | (c >> 6));
            out[k++] = (char)(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            if (k + 3 >= outn) break;
            out[k++] = (char)(0xe0 | (c >> 12));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3f));
            out[k++] = (char)(0x80 | (c & 0x3f));
        } else {
            if (k + 4 >= outn) break;
            out[k++] = (char)(0xf0 | (c >> 18));
            out[k++] = (char)(0x80 | ((c >> 12) & 0x3f));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3f));
            out[k++] = (char)(0x80 | (c & 0x3f));
        }
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = '\0';
}

static void hex_uuid(const u8 *p, char *out)
{
    static const char h[] = "0123456789abcdef";
    int k = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[k++] = '-';
        out[k++] = h[p[i] >> 4];
        out[k++] = h[p[i] & 15];
    }
    out[k] = '\0';
}

static void serial_uuid(u32 v, char *out)
{
    static const char h[] = "0123456789ABCDEF";
    int k = 0;
    for (int i = 7; i >= 0; i--) {
        out[k++] = h[(v >> (i * 4)) & 15];
        if (i == 4) out[k++] = '-';
    }
    out[k] = '\0';
}

/* exFAT keeps its label in a directory entry (type 0x83) in the root
 * directory, not in the boot sector. The first cluster of the root is
 * enough: formatters put the label entry first. */
static void exfat_label(int fd, const u8 *boot, probe_t *p)
{
    u32 heap = le32(boot + 88), root = le32(boot + 96);
    int bps = boot[108], spc = boot[109];
    if (bps < 9 || bps > 12 || spc > 25 || root < 2)
        return;
    u64 off = ((u64)heap + ((u64)(root - 2) << spc)) << bps;
    static u8 dir[4096];
    if (!read_at(fd, off, dir, sizeof dir))
        return;
    for (int i = 0; i < 4096; i += 32) {
        if (dir[i] == 0x00)
            break;
        if (dir[i] == 0x83) {
            int count = dir[i + 1];
            if (count > 11) count = 11;
            utf16_label(dir + i + 2, (size_t)count, p->label, sizeof p->label);
            return;
        }
    }
}

/* NTFS keeps its label as attribute 0x60 of MFT record 3, $Volume. */
static void ntfs_label(int fd, const u8 *boot, probe_t *p)
{
    u32 bps = le16(boot + 0x0b);
    u32 spc = boot[0x0d];
    u64 mft = le64(boot + 0x30);
    s8 cpr = (s8)boot[0x40];
    if (bps < 256 || bps > 4096 || spc == 0)
        return;
    u64 cluster = (u64)bps * spc;
    u32 rec = cpr > 0 ? (u32)(cpr * (s32)cluster) : (1u << (u32)(-cpr));
    if (rec < 512 || rec > 4096)
        return;
    static u8 r[4096];
    if (!read_at(fd, mft * cluster + 3ull * rec, r, rec))
        return;
    if (memcmp(r, "FILE", 4) != 0)
        return;
    /* The update sequence: the last two bytes of every sector were
     * swapped out for a check value when the record was written, and
     * the real bytes are kept in the array. Put them back. */
    u16 usa = le16(r + 4), usn = le16(r + 6);
    for (u32 s = 1; s < usn && s * bps <= rec && usa + 2u * s + 1 < rec; s++) {
        r[s * bps - 2] = r[usa + 2 * s];
        r[s * bps - 1] = r[usa + 2 * s + 1];
    }
    u32 a = le16(r + 0x14);
    while (a + 24 < rec) {
        u32 type = le32(r + a), len = le32(r + a + 4);
        if (type == 0xffffffffu || len == 0 || a + len > rec)
            break;
        if (type == 0x60 && r[a + 8] == 0) {
            u32 vlen = le32(r + a + 0x10);
            u32 voff = le16(r + a + 0x14);
            if (a + voff + vlen <= rec)
                utf16_label(r + a + voff, vlen / 2, p->label, sizeof p->label);
            return;
        }
        a += len;
    }
}

static bool probe_dev(const char *name, probe_t *p)
{
    memset(p, 0, sizeof *p);
    char dev[64];
    if (!dev_path(name, dev, sizeof dev))
        return false;
    long fd = lp_open(dev, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return false;

    static u8 h[4096];
    memset(h, 0, sizeof h);
    if (!read_at((int)fd, 0, h, sizeof h)) {
        lp_close((int)fd);
        return false;
    }

    if (le16(h + 1024 + 56) == 0xEF53) {
        const u8 *sb = h + 1024;
        u32 compat = le32(sb + 92), incompat = le32(sb + 96);
        strlcpy(p->fs, (incompat & 0x2c0) ? "ext4" :
                       (compat & 0x4) ? "ext3" : "ext2", sizeof p->fs);
        copy_label(sb + 120, 16, p->label, sizeof p->label);
        hex_uuid(sb + 104, p->uuid);
        u16 state = le16(sb + 58);
        u32 errors = le32(sb + 0x194);
        strlcpy(p->state, (state & 2) || errors ? "errors" :
                          (state & 1) ? "clean" : "not-clean", sizeof p->state);
    } else if (memcmp(h + 3, "EXFAT   ", 8) == 0) {
        strlcpy(p->fs, "exfat", sizeof p->fs);
        serial_uuid(le32(h + 100), p->uuid);
        exfat_label((int)fd, h, p);
    } else if (memcmp(h + 3, "NTFS    ", 8) == 0) {
        strlcpy(p->fs, "ntfs", sizeof p->fs);
        u64 serial = le64(h + 0x48);
        static const char hx[] = "0123456789ABCDEF";
        for (int i = 0; i < 16; i++)
            p->uuid[i] = hx[(serial >> ((15 - i) * 4)) & 15];
        p->uuid[16] = '\0';
        ntfs_label((int)fd, h, p);
    } else if (h[510] == 0x55 && h[511] == 0xAA &&
               (memcmp(h + 82, "FAT32", 5) == 0 || memcmp(h + 54, "FAT1", 4) == 0)) {
        bool f32 = memcmp(h + 82, "FAT32", 5) == 0;
        strlcpy(p->fs, "vfat", sizeof p->fs);
        copy_label(h + (f32 ? 71 : 43), 11, p->label, sizeof p->label);
        if (strcmp(p->label, "NO NAME") == 0)
            p->label[0] = '\0';
        serial_uuid(le32(h + (f32 ? 67 : 39)), p->uuid);
    } else if (memcmp(h + 4086, "SWAPSPACE2", 10) == 0) {
        strlcpy(p->fs, "swap", sizeof p->fs);
        copy_label(h + 1024 + 28, 16, p->label, sizeof p->label);
        hex_uuid(h + 1024 + 12, p->uuid);
    } else if (memcmp(h, "LUKS\xba\xbe", 6) == 0) {
        strlcpy(p->fs, "crypto_LUKS", sizeof p->fs);
    } else {
        static u8 x[512];
        if (read_at((int)fd, 0x10040, x, 8) && memcmp(x, "_BHRfS_M", 8) == 0) {
            strlcpy(p->fs, "btrfs", sizeof p->fs);
            if (read_at((int)fd, 0x1012b, x, 256))
                copy_label(x, 256, p->label, sizeof p->label);
        } else if (read_at((int)fd, 0x8001, x, 64) && memcmp(x, "CD001", 5) == 0) {
            strlcpy(p->fs, "iso9660", sizeof p->fs);
            copy_label(x + 39, 32, p->label, sizeof p->label);
        }
    }

    /* A whole disk may carry a partition table instead. GPT first: its
     * protective MBR also ends in 55AA. */
    if (!blk_is_part(name)) {
        static u8 g[512];
        if (read_at((int)fd, 512, g, 8) && memcmp(g, "EFI PART", 8) == 0)
            strlcpy(p->table, "gpt", sizeof p->table);
        else if (h[510] == 0x55 && h[511] == 0xAA && !p->fs[0])
            strlcpy(p->table, "mbr", sizeof p->table);
    }
    lp_close((int)fd);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * Running things
 * ═══════════════════════════════════════════════════════════════════ */

/* Absolute paths only, looked up in a fixed order. None of this comes
 * from PATH, and PATH in the child is set here too. Debian's directories
 * come first because the tools and the maintainer scripts that apt runs
 * were written against GNU coreutils; `part` and `pkg` are ours and are
 * only in /bin. */
static bool tool(const char *name, char *out, size_t n)
{
    static const char *const dirs[] = { "/usr/sbin", "/usr/bin", "/sbin",
                                        "/bin", NULL };
    for (int i = 0; dirs[i]; i++) {
        snprintf(out, n, "%s/%s", dirs[i], name);
        lp_stat_t st;
        if (lp_stat(out, &st, true) == 0 &&
            (st.mode & LP_S_IFMT) == LP_S_IFREG && (st.mode & 0111))
            return true;
    }
    out[0] = '\0';
    return false;
}

static bool our_tool(const char *name, char *out, size_t n)
{
    snprintf(out, n, "/bin/%s", name);
    lp_stat_t st;
    return lp_stat(out, &st, true) == 0 &&
           (st.mode & LP_S_IFMT) == LP_S_IFREG && (st.mode & 0111);
}

static char *const child_env[] = {
    "PATH=/usr/sbin:/usr/bin:/sbin:/bin",
    "HOME=/root",
    "LANG=C.UTF-8",
    "LC_ALL=C.UTF-8",
    "TERM=dumb",
    "DEBIAN_FRONTEND=noninteractive",
    "APT_LISTCHANGES_FRONTEND=none",
    NULL
};

/* What to make of the second pipe, when there is one. */
typedef enum { STAT_NONE, STAT_APT, STAT_E2FSCK } statkind_t;

/* apt's Status-Fd: "dlstatus:n:pct:msg" while downloading and
 * "pmstatus:pkg:pct:msg" while dpkg works. Downloading is shown as the
 * first 40% and unpacking/configuring as the rest - neither number
 * alone is the whole job. */
static void status_apt(char *line)
{
    char *kind = line, *c1 = strchr(line, ':');
    if (!c1) return;
    *c1 = '\0';
    char *c2 = strchr(c1 + 1, ':');
    if (!c2) return;
    char *c3 = strchr(c2 + 1, ':');
    if (!c3) return;
    *c3 = '\0';
    int pct = atoi(c2 + 1);
    const char *msg = c3 + 1;
    if (strcmp(kind, "dlstatus") == 0)
        progress(pct * 40 / 100, msg);
    else if (strcmp(kind, "pmstatus") == 0)
        progress(40 + pct * 60 / 100, msg);
    else if (strcmp(kind, "pmerror") == 0)
        say(msg);
}

/* e2fsck -C: "pass cur max device", five passes. */
static void status_e2fsck(char *line)
{
    int pass = atoi(line);
    char *a = strchr(line, ' ');
    if (!a) return;
    long cur = strtol(a + 1, &a, 10);
    long max = strtol(a, NULL, 10);
    if (pass < 1 || pass > 5 || max <= 0) return;
    char msg[32];
    snprintf(msg, sizeof msg, "pass %d of 5", pass);
    progress((int)(((pass - 1) * 100 + cur * 100 / max) / 5), msg);
}

/* Lines from a pipe, handed to `fn` whole. Carriage returns end a line
 * as well: mke2fs and friends redraw one line with \r. */
typedef struct {
    char buf[2048];
    size_t len;
} linebuf_t;

static void feed(linebuf_t *lb, const char *data, long n,
                 void (*fn)(char *))
{
    for (long i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n' || c == '\r' || c == '\b') {
            if (lb->len > 0) {
                lb->buf[lb->len] = '\0';
                fn(lb->buf);
                lb->len = 0;
            }
        } else if (lb->len < sizeof lb->buf - 1) {
            lb->buf[lb->len++] = c;
        }
    }
}

/* flatpak has no status pipe. Given -y but not --noninteractive, and no
 * terminal, it prints its plan first - one numbered row per ref, in the
 * order it will work through them:
 *     1.\t    \tnet.example.App\tstable\ti\tflathub\t< 45.7 MB
 * - and then, for the row it is on, lines like
 *     Installing… ████▌   93%  3.0 MB/s  00:01
 * The percentage is that row's alone. A bare "Installing…" starts the next
 * row, and so does the percentage falling back; the sizes in the plan
 * weigh the rows, so a 250 MB runtime moves the bar more than the 5 MB
 * application that needed it. */
#define FP_ROWS 64
static bool flatpak_job;
static int fp_rows, fp_row, fp_last;
static bool fp_next;
static long fp_kb[FP_ROWS];
static char fp_name[FP_ROWS][72];
static char fp_error[200];

static void flatpak_begin(void)
{
    flatpak_job = true;
    fp_rows = fp_row = 0;
    fp_last = 0;
    fp_next = false;
    fp_error[0] = '\0';
}

static bool has_digit(const char *s)
{
    for (; *s; s++)
        if (*s >= '0' && *s <= '9')
            return true;
    return false;
}

/* "< 45.7 MB" in kilobytes - flatpak puts a no-break space before the
 * unit. One decimal is all it ever prints. */
static long fp_size_kb(const char *s)
{
    while (*s && (*s < '0' || *s > '9'))
        s++;
    long whole = 0, tenth = 0;
    while (*s >= '0' && *s <= '9')
        whole = whole * 10 + (*s++ - '0');
    if (*s == '.' && s[1] >= '0' && s[1] <= '9') {
        tenth = s[1] - '0';
        s += 2;
        while (*s >= '0' && *s <= '9')
            s++;
    }
    while (*s == ' ' || (unsigned char)*s == 0xc2 || (unsigned char)*s == 0xa0)
        s++;
    long tenths = whole * 10 + tenth;
    switch (*s) {
    case 'k': return tenths / 10;
    case 'M': return tenths * 100;
    case 'G': return tenths * 100000;
    default:  return tenths / 10000;
    }
}

/* A row of the plan: "N.\t" and tab-separated fields - status, ref,
 * branch, op, remote, size. The first non-blank field is the ref. */
static bool fp_plan_row(const char *s)
{
    long n = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9')
        n = n * 10 + (*p++ - '0');
    if (p == s || p[0] != '.' || p[1] != '\t' || n != fp_rows + 1 || fp_rows >= FP_ROWS)
        return false;
    p += 2;
    fp_name[fp_rows][0] = '\0';
    fp_kb[fp_rows] = 0;
    while (*p) {
        const char *t = strchr(p, '\t');
        size_t len = t ? (size_t)(t - p) : strlen(p);
        char f[80];
        if (len >= sizeof f) len = sizeof f - 1;
        memcpy(f, p, len);
        f[len] = '\0';
        char *v = f;
        while (*v == ' ') v++;
        if (*v) {
            if (!fp_name[fp_rows][0])
                strlcpy(fp_name[fp_rows], v, sizeof fp_name[0]);
            else if (strchr(v, 'B') && has_digit(v))
                fp_kb[fp_rows] = fp_size_kb(v);
        }
        if (!t) break;
        p = t + 1;
    }
    fp_rows++;
    return true;
}

static void flatpak_line(const char *s)
{
    if (starts(s, "Error: ")) {
        strlcpy(fp_error, s + 7, sizeof fp_error);
        return;
    }
    if (*s >= '0' && *s <= '9' && fp_plan_row(s))
        return;
    /* "Installing…", "Updating…", "Uninstalling…": a verb, and maybe a bar. */
    const char *dots = strstr(s, "\xe2\x80\xa6");
    if (!dots || fp_rows == 0)
        return;
    const char *pc = strchr(dots, '%');
    if (!pc) {
        fp_next = true;
        return;
    }
    const char *d = pc;
    while (d > dots && d[-1] >= '0' && d[-1] <= '9')
        d--;
    if (d == pc)
        return;
    int pct = atoi(d);
    if (pct > 100) pct = 100;
    if (fp_row == 0 || fp_next || pct + 5 < fp_last) {
        if (fp_row < fp_rows)
            fp_row++;
        fp_next = false;
    }
    fp_last = pct;

    long long total = 0, before = 0;
    for (int i = 0; i < fp_rows; i++) {
        long w = fp_kb[i] > 0 ? fp_kb[i] : 1000;
        total += w;
        if (i < fp_row - 1)
            before += w;
    }
    long cur = fp_kb[fp_row - 1] > 0 ? fp_kb[fp_row - 1] : 1000;
    int all = (int)((before * 100 + (long long)cur * pct) / total);

    /* What the bar is on, and how fast: "org.freedesktop.Platform (2/6)  3.0 MB/s". */
    const char *speed = pc + 1;
    while (*speed == ' ') speed++;
    int sl = 0;
    while (speed[sl] && speed[sl] != ' ') sl++;
    const char *unit = speed + sl;
    while (*unit == ' ') unit++;
    int ul = 0;
    while (unit[ul] && unit[ul] != ' ') ul++;
    char text[160];
    snprintf(text, sizeof text, "%s (%d/%d)  %.*s %.*s", fp_name[fp_row - 1],
             fp_row, fp_rows, sl, speed, ul, unit);
    progress(all, text);
}

static void out_line(char *s)
{
    /* Drop the blank-ish noise mkfs prints between phases. */
    while (*s == ' ') s++;
    if (!*s)
        return;
    if (flatpak_job)
        flatpak_line(s);
    say(s);
}

/* Run one program to the end: argv[0] is an absolute path. Its stdout
 * and stderr come back to the client as log lines, the status pipe (if
 * any) is fd 3 in the child. Returns the exit status, or -1 when it
 * could not be started or was killed. */
static int run(char *const argv[], statkind_t sk)
{
    char cmd[900];
    size_t k = strlcpy(cmd, "exec", sizeof cmd);
    for (int i = 0; argv[i] && k < sizeof cmd - 2; i++) {
        k = strlcat(cmd, " ", sizeof cmd);
        k = strlcat(cmd, argv[i], sizeof cmd);
    }
    audit(cmd);

    int out[2], st[2] = { -1, -1 };
    if (lp_pipe(out) < 0)
        return -1;
    if (sk != STAT_NONE && lp_pipe(st) < 0) {
        lp_close(out[0]); lp_close(out[1]);
        return -1;
    }

    pid_t pid = lp_fork();
    if (pid < 0) {
        lp_close(out[0]); lp_close(out[1]);
        if (st[0] >= 0) { lp_close(st[0]); lp_close(st[1]); }
        return -1;
    }
    if (pid == 0) {
        long nul = lp_open("/dev/null", O_RDONLY, 0);
        if (nul >= 0) lp_dup2((int)nul, 0);
        lp_dup2(out[1], 1);
        lp_dup2(out[1], 2);
        if (st[1] >= 0)
            lp_dup2(st[1], 3);
        for (int fd = (st[1] >= 0 ? 4 : 3); fd < 1024; fd++)
            lp_close(fd);
        lp_signal_default(13);
        lp_signal_default(SIGINT);
        lp_signal_default(SIGTERM);
        sys_call1(SYS_umask_, 022);
        lp_chdir("/");
        lp_setsid();
        lp_execve(argv[0], argv, child_env);
        dprintf(2, "cannot run %s\n", argv[0]);
        lp_exit(127);
    }
    lp_close(out[1]);
    if (st[1] >= 0) lp_close(st[1]);

    static linebuf_t ob, sb;
    ob.len = sb.len = 0;
    bool out_open = true, st_open = st[0] >= 0;
    while (out_open || st_open) {
        lp_pollfd_t p[2];
        unsigned np = 0;
        int oi = -1, si = -1;
        if (out_open) { p[np].fd = out[0]; p[np].events = LP_POLLIN; p[np].revents = 0; oi = (int)np++; }
        if (st_open)  { p[np].fd = st[0];  p[np].events = LP_POLLIN; p[np].revents = 0; si = (int)np++; }
        long r = lp_poll(p, np, -1);
        if (r < 0)
            continue;
        char buf[4096];
        if (oi >= 0 && p[oi].revents) {
            long n = lp_read(out[0], buf, sizeof buf);
            if (n <= 0) out_open = false;
            else feed(&ob, buf, n, out_line);
        }
        if (si >= 0 && p[si].revents) {
            long n = lp_read(st[0], buf, sizeof buf);
            if (n <= 0) st_open = false;
            else feed(&sb, buf, n, sk == STAT_APT ? status_apt : status_e2fsck);
        }
    }
    lp_close(out[0]);
    if (st[0] >= 0) lp_close(st[0]);

    int status = 0;
    lp_waitpid(pid, &status, 0);
    int code = (status & 0x7f) == 0 ? (status >> 8) & 0xff : -1;
    char line[64];
    snprintf(line, sizeof line, "  exit %d", code);
    audit(line);
    return code;
}

/* ═══════════════════════════════════════════════════════════════════
 * The verbs that do something
 *
 * Each runs in the worker process, after the parent has checked the
 * arguments and who asked. Each ends with done() or fail().
 * ═══════════════════════════════════════════════════════════════════ */

static int job_rc;               /* what the worker exits with */

static void done(const char *text)
{
    reply("done", text);
    job_rc = 0;
}

static void failed(const char *why, const char *text)
{
    fail(why, text);
    job_rc = 1;
    char line[600];
    snprintf(line, sizeof line, "  fail %s %s", why, text);
    audit(line);
}

static bool need_tool(const char *name, const char *pkg, char *out, size_t n)
{
    if (tool(name, out, n))
        return true;
    char msg[160];
    snprintf(msg, sizeof msg, "%s is not installed (package %s)", name, pkg);
    failed("missing", msg);
    return false;
}

/* The apt-get options every call gets. -q drops the progress bars that
 * would otherwise arrive as hundreds of \r-joined log lines; Status-Fd
 * is where the real progress comes from. The conffile options mean an
 * upgrade never stops to ask about a changed file in /etc - there is
 * nobody at a terminal to answer, and keeping the local file is the
 * choice that never breaks a working machine. */
#define APT_COMMON \
    "-y", "-q", "-o", "APT::Status-Fd=3", "-o", "Dpkg::Use-Pty=0", \
    "-o", "DPkg::Lock::Timeout=120", \
    "-o", "Dpkg::Options::=--force-confdef", \
    "-o", "Dpkg::Options::=--force-confold"

static void apt_with(const char *verb, const char *extra, char **names,
                     int n, const char *ok)
{
    char apt[64];
    if (!need_tool("apt-get", "apt", apt, sizeof apt))
        return;
    char *argv[MAX_FIELDS + 24];
    int a = 0;
    argv[a++] = apt;
    const char *common[] = { APT_COMMON };
    for (u32 i = 0; i < sizeof common / sizeof *common; i++)
        argv[a++] = (char *)common[i];
    if (extra)
        argv[a++] = (char *)extra;
    argv[a++] = (char *)verb;
    for (int i = 0; i < n; i++)
        argv[a++] = names[i];
    argv[a] = NULL;
    progress(0, verb);
    int rc = run(argv, STAT_APT);
    if (rc == 0) {
        progress(100, ok);
        done(ok);
    } else {
        char msg[96];
        snprintf(msg, sizeof msg, "apt-get %s exited with %d", verb, rc);
        failed("failed", msg);
    }
}

/* Packages this desktop cannot lose. Removing any one of them from the
 * software centre would take the session down, or apt itself, and the
 * person doing it would be left at a machine with no way to put it back
 * from where they are sitting. Removing them from a terminal with
 * apt-get is still possible; this only stops the button doing it. */
static const char *const PROTECTED[] = {
    "apt", "dpkg", "libc6", "base-files", "bash", "dash", "coreutils",
    "util-linux", "mount", "e2fsprogs", "login", "passwd", "sudo",
    "libgtk-4-1", "libgtk-3-0", "libglib2.0-0", "sway", "wayfire", "foot",
    "waybar", "seatd", "udev", "dbus", "xwayland", "pipewire",
    "wireplumber", "fonts-noto-cjk", "gnupg", "gpgv", "debian-archive-keyring",
    "ca-certificates", NULL
};

static bool is_protected(const char *name)
{
    for (int i = 0; PROTECTED[i]; i++)
        if (strcmp(name, PROTECTED[i]) == 0)
            return true;
    return false;
}

/* A removal is only ever as small as apt decides. Ask apt what it would
 * take away (-s changes nothing) and refuse if the answer reaches a
 * package on the list above. */
static char remv_hit[64];

static void remv_line(char *s)
{
    if (!starts(s, "Remv ") || remv_hit[0])
        return;
    char name[64];
    size_t k = 0;
    for (const char *p = s + 5; *p && *p != ' ' && *p != ':' && k < 63; p++)
        name[k++] = *p;
    name[k] = '\0';
    if (is_protected(name))
        strlcpy(remv_hit, name, sizeof remv_hit);
}

static bool removal_is_safe(char **names, int n)
{
    char apt[64];
    if (!tool("apt-get", apt, sizeof apt))
        return false;
    char *argv[MAX_FIELDS + 8];
    int a = 0;
    argv[a++] = apt;
    argv[a++] = "-s";
    argv[a++] = "-q";
    argv[a++] = "remove";
    for (int i = 0; i < n; i++)
        argv[a++] = names[i];
    argv[a] = NULL;

    int out[2];
    if (lp_pipe(out) < 0)
        return false;
    pid_t pid = lp_fork();
    if (pid == 0) {
        long nul = lp_open("/dev/null", O_RDWR, 0);
        if (nul >= 0) { lp_dup2((int)nul, 0); lp_dup2((int)nul, 2); }
        lp_dup2(out[1], 1);
        for (int fd = 3; fd < 1024; fd++) lp_close(fd);
        lp_signal_default(13);
        lp_execve(argv[0], argv, child_env);
        lp_exit(127);
    }
    lp_close(out[1]);
    remv_hit[0] = '\0';
    static linebuf_t lb;
    lb.len = 0;
    char buf[4096];
    long r;
    while ((r = lp_read(out[0], buf, sizeof buf)) > 0)
        feed(&lb, buf, r, remv_line);
    lp_close(out[0]);
    int status = 0;
    lp_waitpid(pid, &status, 0);
    if (remv_hit[0]) {
        char msg[200];
        snprintf(msg, sizeof msg, "removing this would also remove %s,"
                 " which the desktop needs", remv_hit);
        failed("refused", msg);
        return false;
    }
    return true;
}

static void v_apt_update(char **a, int n)
{
    (void)a; (void)n;
    apt_with("update", NULL, NULL, 0, "package lists updated");
}

static void v_apt_install(char **a, int n)
{
    /* --no-remove: an install that can only proceed by removing other
     * packages fails instead. That is a decision for a person at a
     * terminal, not a side effect of pressing "Install". */
    apt_with("install", "--no-remove", a, n, "installed");
}

static void v_apt_remove(char **a, int n)
{
    for (int i = 0; i < n; i++) {
        if (is_protected(a[i])) {
            char msg[160];
            snprintf(msg, sizeof msg, "%s is part of the desktop itself", a[i]);
            failed("refused", msg);
            return;
        }
    }
    say("checking what else this would remove");
    if (!removal_is_safe(a, n))
        return;
    apt_with("remove", NULL, a, n, "removed");
}

static void v_apt_upgrade(char **a, int n)
{
    if (n == 0)
        apt_with("upgrade", "--with-new-pkgs", NULL, 0, "everything is up to date");
    else
        apt_with("install", "--only-upgrade", a, n, "updated");
}

/* After a power cut in the middle of dpkg, apt refuses everything until
 * this has been run. It is the one repair a person would otherwise have
 * to find in a forum. */
static void v_apt_repair(char **a, int n)
{
    (void)a; (void)n;
    char dpkg[64];
    if (!need_tool("dpkg", "dpkg", dpkg, sizeof dpkg))
        return;
    char *argv[] = { dpkg, "--force-confdef", "--force-confold",
                     "--configure", "-a", NULL };
    progress(0, "dpkg --configure -a");
    if (run(argv, STAT_NONE) != 0) {
        failed("failed", "dpkg --configure -a did not finish");
        return;
    }
    apt_with("install", "-f", NULL, 0, "package system repaired");
}

static void pkg_run(const char *verb, char **names, int n, const char *ok)
{
    char pkg[64];
    if (!our_tool("pkg", pkg, sizeof pkg)) {
        failed("missing", "/bin/pkg is not on this machine");
        return;
    }
    int total = n > 0 ? n : 1;
    for (int i = 0; i < total; i++) {
        char *argv[4] = { pkg, (char *)verb, n > 0 ? names[i] : NULL, NULL };
        progress(i * 100 / total, n > 0 ? names[i] : verb);
        int rc = run(argv, STAT_NONE);
        if (rc != 0) {
            char msg[128];
            snprintf(msg, sizeof msg, "pkg %s %s exited with %d", verb,
                     n > 0 ? names[i] : "", rc);
            failed("failed", msg);
            return;
        }
    }
    progress(100, ok);
    done(ok);
}

static void v_pkg_update(char **a, int n)  { (void)a; (void)n; pkg_run("update", NULL, 0, "index updated"); }

/* ── Flatpak ────────────────────────────────────────────────────────
 *
 * Applications from Flathub, installed system-wide (/var/lib/flatpak),
 * for every account, the way apt installs. Flathub is added the first
 * time it is needed, from its own .flatpakrepo (which carries the signing
 * key), so an image that never installs anything from it never asks
 * Flathub for anything either. -y answers what flatpak would ask - yes to
 * the runtimes an application needs - and stdin is /dev/null, so nothing
 * waits for a terminal; --noninteractive is left off because it also
 * turns off the progress lines the Software app's bar is drawn from. */

#define FLATHUB_REPO "https://dl.flathub.org/repo/flathub.flatpakrepo"

static bool flatpak_ready(char *fp, size_t n)
{
    if (!need_tool("flatpak", "flatpak", fp, n))
        return false;
    char *argv[] = { fp, "remote-add", "--system", "--if-not-exists",
                     "flathub", FLATHUB_REPO, NULL };
    if (run(argv, STAT_NONE) != 0) {
        failed("failed", "could not add Flathub - is the computer online?");
        return false;
    }
    return true;
}

static void flatpak_with(const char *what, const char *const *opts,
                         char **ids, int n, const char *ok)
{
    char fp[64];
    if (!flatpak_ready(fp, sizeof fp))
        return;
    char *argv[MAX_FIELDS + 16];
    int a = 0;
    argv[a++] = fp;
    for (int i = 0; opts[i]; i++)
        argv[a++] = (char *)opts[i];
    for (int i = 0; i < n; i++)
        argv[a++] = ids[i];
    argv[a] = NULL;
    progress(0, what);
    flatpak_begin();
    int rc = run(argv, STAT_NONE);
    flatpak_job = false;
    if (rc == 0) {
        progress(100, ok);
        done(ok);
    } else {
        char msg[240];
        if (fp_error[0])
            strlcpy(msg, fp_error, sizeof msg);
        else
            snprintf(msg, sizeof msg, "flatpak %s exited with %d", what, rc);
        failed("failed", msg);
    }
}

/* The catalogue the Software app searches and draws its icons from
 * (/var/lib/flatpak/appstream). Changes nothing anybody runs, so any
 * account may ask for it, like reading apt's lists. */
static void v_flatpak_refresh(char **a, int n)
{
    (void)a; (void)n;
    static const char *const o[] = { "update", "--system", "--appstream",
                                     "--noninteractive", "flathub", NULL };
    flatpak_with("update --appstream", o, NULL, 0, "Flathub catalogue updated");
}

static void v_flatpak_install(char **a, int n)
{
    static const char *const o[] = { "install", "--system", "-y",
                                     "flathub", NULL };
    flatpak_with("install", o, a, n, "installed");
}

/* The runtimes nothing uses any more go with the last application that
 * needed them: a runtime is hundreds of megabytes nobody would otherwise
 * know to remove. */
static void v_flatpak_remove(char **a, int n)
{
    static const char *const o[] = { "uninstall", "--system", "-y", NULL };
    flatpak_with("uninstall", o, a, n, "removed");
    if (job_rc != 0)
        return;
    char fp[64];
    if (tool("flatpak", fp, sizeof fp)) {
        char *argv[] = { fp, "uninstall", "--system", "-y", "--noninteractive",
                         "--unused", NULL };
        run(argv, STAT_NONE);
    }
}

static void v_flatpak_update(char **a, int n)
{
    static const char *const o[] = { "update", "--system", "-y", NULL };
    flatpak_with("update", o, a, n, n ? "updated" : "everything is up to date");
}
static void v_pkg_install(char **a, int n) { pkg_run("install", a, n, "installed"); }
static void v_pkg_remove(char **a, int n)  { pkg_run("remove", a, n, "removed"); }

/* ── Mounting ───────────────────────────────────────────────────────
 *
 * The same place automount uses: /media/<label>, or /media/<device>
 * when there is no label, with -2, -3 on a clash. The label is cleaned
 * the way automount cleans it, so a label of "../../etc" does not
 * become a mount on /etc. What is different from automount is who the
 * files belong to: FAT, exFAT and NTFS have no owners, the kernel
 * invents them at mount time, and automount - which has no idea who is
 * sitting at the machine - gets root. Here the caller is known, so the
 * files are theirs and the stick they asked to mount is writable. */

static u32 caller_uid, caller_gid;

static void safe_name(const char *label, const char *fallback, char *out,
                      size_t n)
{
    size_t w = 0;
    for (size_t i = 0; label && label[i] && w + 1 < n; i++) {
        char c = label[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                  (c == '.' && w > 0);
        out[w++] = ok ? c : '-';
    }
    while (w > 0 && out[w - 1] == '-') w--;
    if (w == 0) {
        strlcpy(out, fallback, n);
        return;
    }
    out[w] = '\0';
}

static bool point_in_use(const char *path)
{
    for (int i = 0; i < nmnts; i++)
        if (strcmp(mnts[i].point, path) == 0)
            return true;
    return false;
}

static bool pick_point(const char *name, char *out, size_t n)
{
    snprintf(out, n, "/media/%s", name);
    if (!point_in_use(out))
        return true;
    for (int s = 2; s < 20; s++) {
        snprintf(out, n, "/media/%s-%d", name, s);
        if (!point_in_use(out))
            return true;
    }
    return false;
}

static long try_mount(const char *dev, const char *point, const char *fs,
                      unsigned long flags, bool owned)
{
    char data[96];
    const char *opts = NULL;
    if (owned) {
        if (strcmp(fs, "vfat") == 0)
            snprintf(data, sizeof data, "uid=%u,gid=%u,umask=022,utf8=1,"
                     "shortname=mixed,flush", caller_uid, caller_gid);
        else
            snprintf(data, sizeof data, "uid=%u,gid=%u,umask=022,iocharset=utf8",
                     caller_uid, caller_gid);
        opts = data;
    }
    return lp_mount(dev, point, fs, flags, opts);
}

static void v_mount(char **a, int n)
{
    (void)n;
    const char *name = a[0];
    char dev[64];
    if (!dev_path(name, dev, sizeof dev)) {
        failed("invalid", "that device is not there any more");
        return;
    }
    const char *at = mounted_at(name);
    if (at) {
        char msg[300];
        snprintf(msg, sizeof msg, "%s", at);
        done(msg);
        return;
    }
    probe_t p;
    probe_dev(name, &p);
    if (strcmp(p.fs, "swap") == 0 || strcmp(p.fs, "crypto_LUKS") == 0) {
        failed("refused", "this is not a filesystem that can be mounted");
        return;
    }

    char clean[48], point[96];
    safe_name(p.label, name, clean, sizeof clean);
    if (!pick_point(clean, point, sizeof point)) {
        failed("failed", "no free mount point under /media");
        return;
    }
    lp_mkdir("/media", 0755);
    if (lp_mkdir(point, 0755) < 0 && !lp_is_dir(point)) {
        failed("failed", "cannot create the mount point");
        return;
    }

    char line[200];
    snprintf(line, sizeof line, "mount %s %s (%s)", dev, point,
             p.fs[0] ? p.fs : "unknown");
    audit(line);

    unsigned long flags = MS_NOSUID | MS_NODEV;
    long r = -1;
    const char *used = p.fs;
    if (!strcmp(p.fs, "ext4") || !strcmp(p.fs, "ext3") || !strcmp(p.fs, "ext2") ||
        !strcmp(p.fs, "btrfs")) {
        r = try_mount(dev, point, p.fs, flags, false);
    } else if (!strcmp(p.fs, "vfat") || !strcmp(p.fs, "exfat")) {
        r = try_mount(dev, point, p.fs, flags, true);
    } else if (!strcmp(p.fs, "ntfs")) {
        used = "ntfs3";
        r = try_mount(dev, point, "ntfs3", flags, true);
        if (r < 0) {
            /* No ntfs3 in the kernel, or it refused a dirty volume: the
             * FUSE driver is the fallback, if it is installed. */
            char ntfs3g[64];
            if (tool("ntfs-3g", ntfs3g, sizeof ntfs3g)) {
                char opts[96];
                snprintf(opts, sizeof opts, "uid=%u,gid=%u,umask=022,nosuid,nodev",
                         caller_uid, caller_gid);
                char *argv[] = { ntfs3g, dev, point, "-o", opts, NULL };
                used = "ntfs-3g";
                r = run(argv, STAT_NONE) == 0 ? 0 : -1;
            }
        }
    } else if (!strcmp(p.fs, "iso9660")) {
        r = try_mount(dev, point, p.fs, flags | MS_RDONLY, false);
    } else {
        static const char *const guess[] = { "ext4", "vfat", "exfat", "ntfs3", NULL };
        for (int i = 0; guess[i] && r < 0; i++) {
            bool own = strcmp(guess[i], "ext4") != 0;
            r = try_mount(dev, point, guess[i], flags, own);
            used = guess[i];
        }
    }

    if (r < 0 && p.fs[0] && strcmp(used, "ntfs-3g") != 0) {
        /* A dirty FAT or an NTFS left hibernated by Windows often mounts
         * read-only when it will not mount writable. Better than
         * nothing, and said out loud. */
        bool own = strcmp(p.fs, "vfat") == 0 || strcmp(p.fs, "exfat") == 0 ||
                   strcmp(p.fs, "ntfs") == 0;
        const char *fs = strcmp(p.fs, "ntfs") == 0 ? "ntfs3" : p.fs;
        r = try_mount(dev, point, fs, flags | MS_RDONLY, own);
        if (r == 0)
            say("mounted read-only: the filesystem needs checking");
    }

    if (r < 0) {
        lp_rmdir(point);
        char msg[200];
        snprintf(msg, sizeof msg, "the kernel would not mount it (%s, error %ld)",
                 p.fs[0] ? p.fs : "unknown filesystem", -r);
        failed("failed", msg);
        return;
    }
    audit("  mounted");
    done(point);
}

/* Unmount one mount point. Busy is reported, not forced: the file
 * manager can say "a program is using this drive", where a lazy
 * detach would say nothing and leave the drive half-written when
 * it is pulled. */
static bool unmount_point(const char *point)
{
    if (!starts(point, "/media/") && !starts(point, "/mnt/")) {
        char msg[300];
        snprintf(msg, sizeof msg, "%s is not a removable drive's mount point",
                 point);
        failed("refused", msg);
        return false;
    }
    char line[300];
    snprintf(line, sizeof line, "umount %s", point);
    audit(line);
    lp_sync();
    long r = lp_umount(point, 0);
    if (r == -16) {                      /* EBUSY */
        failed("failed", "busy: a program still has files open on this drive");
        return false;
    }
    if (r < 0) {
        char msg[96];
        snprintf(msg, sizeof msg, "the kernel would not unmount it (error %ld)", -r);
        failed("failed", msg);
        return false;
    }
    if (starts(point, "/media/"))
        lp_rmdir(point);
    return true;
}

/* Unmount every mount of this device, in reverse order of mounting so
 * that something stacked on top goes first. */
static bool unmount_dev(const char *name)
{
    u32 maj, min;
    if (!blk_devnum(name, &maj, &min))
        return true;
    for (int i = nmnts - 1; i >= 0; i--) {
        if (mnts[i].maj == maj && mnts[i].min == min) {
            if (!unmount_point(mnts[i].point))
                return false;
        }
    }
    return true;
}

static void v_unmount(char **a, int n)
{
    (void)n;
    if (!mounted_at(a[0])) {
        done("not mounted");
        return;
    }
    if (unmount_dev(a[0]))
        done("unmounted");
}

/* Eject: everything on the disk unmounted, the caches written out, and
 * then - for anything on the SCSI layer, which is every USB stick and
 * card reader - the device removed from the kernel, which is what makes
 * the stick's light go out and what "safe to remove" means. An SD card
 * in the laptop's own reader (mmc) has no such switch: unmounted and
 * synced is as safe as it gets. */
static void v_eject(char **a, int n)
{
    (void)n;
    const char *disk = a[0];
    char parts[64][32];
    int np = blk_parts(disk, parts, 64);
    for (int i = 0; i < np; i++)
        if (!unmount_dev(parts[i]))
            return;
    if (!unmount_dev(disk))
        return;
    lp_sync();

    char p[96];
    snprintf(p, sizeof p, "/sys/block/%s/device/delete", disk);
    if (lp_exists(p)) {
        long fd = lp_open(p, O_WRONLY, 0);
        if (fd >= 0) {
            lp_write((int)fd, "1\n", 2);
            lp_close((int)fd);
            audit("  device removed from the kernel");
        }
    }
    done("safe to remove");
}

/* ── Formatting and partitioning ──────────────────────────────────── */

/* Just before writing: is it still unmounted? The check the parent made
 * is seconds old by now, and automount reacts to a new partition within
 * a moment of `part` creating it. A mount under /media of the very
 * device about to be formatted is automount getting there first, and is
 * taken down; anything else stops the job. */
static bool still_free(const char *name)
{
    mounts_refresh();
    const char *at = mounted_at(name);
    if (!at)
        return true;
    if (starts(at, "/media/") && unmount_dev(name)) {
        mounts_refresh();
        return mounted_at(name) == NULL;
    }
    if (!starts(at, "/media/")) {
        char msg[300];
        snprintf(msg, sizeof msg, "%s was mounted at %s meanwhile", name, at);
        failed("refused", msg);
    }
    return false;
}

static bool wipe(const char *dev)
{
    char wipefs[64];
    if (!tool("wipefs", wipefs, sizeof wipefs))
        return true;                     /* mkfs overwrites the main ones */
    char *argv[] = { wipefs, "-a", (char *)dev, NULL };
    return run(argv, STAT_NONE) == 0;
}

static bool mkfs(const char *name, fstype_t f, const char *label)
{
    char dev[64];
    if (!dev_path(name, dev, sizeof dev)) {
        failed("failed", "the device node did not appear");
        return false;
    }
    if (!still_free(name))
        return false;

    progress(10, "wipefs");
    if (!wipe(dev)) {
        failed("failed", "could not clear the old signatures");
        return false;
    }

    char bin[64], owner[40], up[16];
    char *argv[16];
    int k = 0;
    bool whole = !blk_is_part(name);
    switch (f) {
    case FS_EXT4:
        if (!need_tool("mkfs.ext4", "e2fsprogs", bin, sizeof bin)) return false;
        /* The root directory belongs to whoever asked, so a freshly
         * formatted drive is one they can write to. */
        snprintf(owner, sizeof owner, "root_owner=%u:%u", caller_uid, caller_gid);
        argv[k++] = bin; argv[k++] = "-F"; argv[k++] = "-E"; argv[k++] = owner;
        if (label[0]) { argv[k++] = "-L"; argv[k++] = (char *)label; }
        break;
    case FS_FAT32:
        if (!need_tool("mkfs.fat", "dosfstools", bin, sizeof bin)) return false;
        argv[k++] = bin; argv[k++] = "-F"; argv[k++] = "32";
        if (whole) argv[k++] = "-I";
        if (label[0]) {
            /* FAT labels are upper case on disk; Windows shows them so. */
            size_t i;
            for (i = 0; label[i] && i < sizeof up - 1; i++)
                up[i] = (label[i] >= 'a' && label[i] <= 'z') ?
                        (char)(label[i] - 32) : label[i];
            up[i] = '\0';
            argv[k++] = "-n"; argv[k++] = up;
        }
        break;
    case FS_EXFAT:
        if (!need_tool("mkfs.exfat", "exfatprogs", bin, sizeof bin)) return false;
        argv[k++] = bin;
        if (label[0]) { argv[k++] = "-L"; argv[k++] = (char *)label; }
        break;
    case FS_NTFS:
        if (!need_tool("mkfs.ntfs", "ntfs-3g", bin, sizeof bin)) return false;
        /* -Q: quick. A full format zeroes the whole device first, which on
         * a 1TB USB disk is hours of a window saying "formatting". */
        argv[k++] = bin; argv[k++] = "-Q"; argv[k++] = "-F";
        if (label[0]) { argv[k++] = "-L"; argv[k++] = (char *)label; }
        break;
    default:
        failed("invalid", "no such filesystem");
        return false;
    }
    argv[k++] = dev;
    argv[k] = NULL;

    progress(30, argv[0]);
    int rc = run(argv, STAT_NONE);
    if (rc != 0) {
        char msg[96];
        snprintf(msg, sizeof msg, "mkfs exited with %d", rc);
        failed("failed", msg);
        return false;
    }
    lp_sync();
    return true;
}

static void v_format(char **a, int n)
{
    fstype_t f = fs_parse(a[1]);
    const char *label = n > 2 ? a[2] : "";
    if (mkfs(a[0], f, label)) {
        progress(100, "formatted");
        done("formatted");
    }
}

/* `part` refers to slots and writes the whole table; these read the
 * table through libc to find the slot to name. */
static const char *mbr_type(fstype_t f)
{
    switch (f) {
    case FS_EXT4:  return "linux";
    case FS_FAT32: return "fat32";
    default:       return "0x07";       /* exFAT and NTFS share it */
    }
}

static void kernel_match_table(const char *disk, const char *dev);

static bool part_run(const char *disk_dev, const char *v1, const char *v2,
                     const char *v3, const char *v4, const char *v5)
{
    char part[64];
    if (!our_tool("part", part, sizeof part)) {
        failed("missing", "/bin/part is not on this machine");
        return false;
    }
    char *argv[10];
    int k = 0;
    argv[k++] = part;
    argv[k++] = (char *)disk_dev;
    if (v1) argv[k++] = (char *)v1;
    if (v2) argv[k++] = (char *)v2;
    if (v3) argv[k++] = (char *)v3;
    if (v4) argv[k++] = (char *)v4;
    if (v5) argv[k++] = (char *)v5;
    argv[k++] = "-y";
    argv[k] = NULL;
    int rc = run(argv, STAT_NONE);
    if (rc != 0) {
        char msg[96];
        snprintf(msg, sizeof msg, "part exited with %d", rc);
        failed("failed", msg);
        return false;
    }
    kernel_match_table(disk_dev + 5, disk_dev);
    return true;
}

/* The name of partition `slot` on `disk`: sdb + 1 = sdb1, but
 * nvme0n1 + 1 = nvme0n1p1 and loop0 + 1 = loop0p1. */
static void part_name(const char *disk, int slot, char *out, size_t n)
{
    size_t l = strlen(disk);
    bool p = l > 0 && disk[l - 1] >= '0' && disk[l - 1] <= '9';
    snprintf(out, n, "%s%s%d", disk, p ? "p" : "", slot);
}

/* BLKPG, for telling the kernel about one partition. libc keeps its
 * copy of these private to disk.c. */
#define BLKPG_           0x1269
#define BLKPG_ADD_       1
#define BLKPG_DEL_       2

typedef struct {
    s64  start, length;
    int  pno;
    char devname[64], volname[64];
} blkpg_part_t;

typedef struct {
    int   op, flags, datalen;
    void *data;
} blkpg_arg_t;

/* Make the kernel's partitions match the MBR `part` just wrote, slot by
 * slot. `part` asks with BLKRRPART first, and BLKRRPART can succeed and
 * still add nothing: it re-reads the table with the kernel's own MBR
 * parser, and a kernel built without CONFIG_MSDOS_PARTITION - the build
 * host this was tested on is one - parses nothing and says "done". The
 * laptop's kernel has the parser; this costs nothing there, because
 * every slot is already right and nothing is sent. */
static void kernel_match_table(const char *disk, const char *dev)
{
    u8 mbr[DISK_SECTOR];
    if (!disk_read_mbr(dev, mbr) || !mbr_valid(mbr))
        return;
    long fd = lp_open(dev, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return;
    mounts_refresh();
    for (int slot = 1; slot <= DISK_PARTS; slot++) {
        u8 t; u32 st, c;
        mbr_get(mbr, slot, &t, &st, &c, NULL);
        char pn[40];
        part_name(disk, slot, pn, sizeof pn);
        bool want = !(t == PART_TYPE_EMPTY || c == 0);
        bool have = blk_exists(pn);
        blkpg_part_t part;
        memset(&part, 0, sizeof part);
        part.pno = slot;
        blkpg_arg_t arg = { 0, 0, (int)sizeof part, &part };
        if (have && !mounted_at(pn)) {
            /* Present: drop it if the table no longer has it, or if it
             * is at the wrong place - a slot reused with a new size. */
            char p[96], v[32];
            snprintf(p, sizeof p, "/sys/class/block/%s/start", pn);
            u64 kstart = sys_read(p, v, sizeof v) ? (u64)strtoll(v, NULL, 10) : 0;
            snprintf(p, sizeof p, "/sys/class/block/%s/size", pn);
            u64 ksize = sys_read(p, v, sizeof v) ? (u64)strtoll(v, NULL, 10) : 0;
            if (!want || kstart != st || ksize != c) {
                arg.op = BLKPG_DEL_;
                lp_ioctl((int)fd, BLKPG_, &arg);
                have = false;
            }
        }
        if (want && !have) {
            part.start = (s64)st * DISK_SECTOR;
            part.length = (s64)c * DISK_SECTOR;
            arg.op = BLKPG_ADD_;
            lp_ioctl((int)fd, BLKPG_, &arg);
        }
    }
    lp_close((int)fd);
}

/* After the table changes, the kernel makes the partition and devtmpfs
 * makes its node, a little later. */
static bool wait_for(const char *name)
{
    char dev[64];
    for (int i = 0; i < 50; i++) {
        if (blk_exists(name) && dev_path(name, dev, sizeof dev))
            return true;
        lp_sleep_ms(100);
    }
    return false;
}

static bool mbr_is_gpt(const char *disk_dev)
{
    u8 mbr[DISK_SECTOR];
    if (!disk_read_mbr(disk_dev, mbr))
        return false;
    for (int s = 1; s <= DISK_PARTS; s++) {
        u8 t;
        mbr_get(mbr, s, &t, NULL, NULL, NULL);
        if (t == PART_TYPE_GPT)
            return true;
    }
    return false;
}

static void v_format_disk(char **a, int n)
{
    const char *disk = a[0];
    fstype_t f = fs_parse(a[1]);
    const char *label = n > 2 ? a[2] : "";
    char dev[64];
    if (!dev_path(disk, dev, sizeof dev)) {
        failed("invalid", "that disk is not there any more");
        return;
    }
    /* The whole drive: every old signature, GPT included - `part` will
     * not touch a GPT disk, and a USB stick that was once a Linux
     * installer usually is one. */
    progress(5, "wipefs");
    if (!wipe(dev)) {
        failed("failed", "could not clear the old partition table");
        return;
    }
    progress(15, "new partition table");
    if (!part_run(dev, "clear", NULL, NULL, NULL, NULL))
        return;
    progress(25, "new partition");
    if (!part_run(dev, "new", "1", mbr_type(f), "1", "rest"))
        return;
    char p1[40];
    part_name(disk, 1, p1, sizeof p1);
    if (!wait_for(p1)) {
        failed("failed", "the new partition did not appear");
        return;
    }
    if (mkfs(p1, f, label)) {
        progress(100, "formatted");
        done(p1);
    }
}

static void v_mklabel(char **a, int n)
{
    (void)n;
    char dev[64];
    if (!dev_path(a[0], dev, sizeof dev)) {
        failed("invalid", "that disk is not there any more");
        return;
    }
    if (mbr_is_gpt(dev) && !wipe(dev)) {
        failed("failed", "could not clear the old GPT");
        return;
    }
    if (part_run(dev, "clear", NULL, NULL, NULL, NULL))
        done("empty partition table");
}

static void v_mkpart(char **a, int n)
{
    const char *disk = a[0];
    fstype_t f = fs_parse(a[1]);
    const char *size = a[2];
    const char *label = n > 3 ? a[3] : "";
    char dev[64];
    if (!dev_path(disk, dev, sizeof dev)) {
        failed("invalid", "that disk is not there any more");
        return;
    }
    if (mbr_is_gpt(dev)) {
        failed("refused", "this disk has a GPT table, and part writes MBR only");
        return;
    }
    u8 mbr[DISK_SECTOR];
    int slot = 1;
    if (disk_read_mbr(dev, mbr) && mbr_valid(mbr)) {
        slot = 0;
        for (int s = 1; s <= DISK_PARTS && !slot; s++) {
            u8 t; u32 st, c;
            mbr_get(mbr, s, &t, &st, &c, NULL);
            if (t == PART_TYPE_EMPTY && c == 0)
                slot = s;
        }
    }
    if (!slot) {
        failed("refused", "all four MBR slots are used");
        return;
    }
    char sl[4];
    snprintf(sl, sizeof sl, "%d", slot);
    progress(10, "new partition");
    if (!part_run(dev, "new", sl, mbr_type(f), "next", size))
        return;
    char pn[40];
    part_name(disk, slot, pn, sizeof pn);
    if (!wait_for(pn)) {
        failed("failed", "the new partition did not appear");
        return;
    }
    if (mkfs(pn, f, label)) {
        progress(100, "created");
        done(pn);
    }
}

static void v_rmpart(char **a, int n)
{
    (void)n;
    char disk[32], dev[64];
    blk_disk(a[0], disk, sizeof disk);
    if (!dev_path(disk, dev, sizeof dev)) {
        failed("invalid", "that disk is not there any more");
        return;
    }
    int slot = blk_partno(a[0]);
    if (slot < 1 || slot > DISK_PARTS) {
        failed("refused", "only the four MBR slots can be removed here");
        return;
    }
    if (mbr_is_gpt(dev)) {
        failed("refused", "this disk has a GPT table, and part writes MBR only");
        return;
    }
    char sl[4];
    snprintf(sl, sizeof sl, "%d", slot);
    if (part_run(dev, "del", sl, NULL, NULL, NULL))
        done("partition removed");
}

/* Check, and check-and-repair. The check never writes (-n everywhere),
 * so it is safe to run on anything that is not mounted. */
static void fsck_dev(const char *name, bool repair)
{
    char dev[64];
    if (!dev_path(name, dev, sizeof dev)) {
        failed("invalid", "that device is not there any more");
        return;
    }
    if (!still_free(name))
        return;
    probe_t p;
    probe_dev(name, &p);
    char bin[64], fd3[8];
    char *argv[10];
    int k = 0;
    statkind_t sk = STAT_NONE;
    if (starts(p.fs, "ext")) {
        if (!need_tool("e2fsck", "e2fsprogs", bin, sizeof bin)) return;
        strlcpy(fd3, "3", sizeof fd3);
        argv[k++] = bin; argv[k++] = "-f"; argv[k++] = repair ? "-y" : "-n";
        argv[k++] = "-C"; argv[k++] = fd3;
        sk = STAT_E2FSCK;
    } else if (!strcmp(p.fs, "vfat")) {
        if (!need_tool("fsck.fat", "dosfstools", bin, sizeof bin)) return;
        argv[k++] = bin; argv[k++] = repair ? "-a" : "-n";
        if (repair) argv[k++] = "-w";
    } else if (!strcmp(p.fs, "exfat")) {
        if (!need_tool("fsck.exfat", "exfatprogs", bin, sizeof bin)) return;
        argv[k++] = bin; argv[k++] = repair ? "-y" : "-n";
    } else if (!strcmp(p.fs, "ntfs")) {
        if (!need_tool("ntfsfix", "ntfs-3g", bin, sizeof bin)) return;
        argv[k++] = bin;
        if (!repair) argv[k++] = "-n";
    } else {
        failed("refused", p.fs[0] ? "there is no checker for this filesystem"
                                  : "no filesystem found to check");
        return;
    }
    argv[k++] = dev;
    argv[k] = NULL;
    progress(0, repair ? "repairing" : "checking");
    int rc = run(argv, sk);
    /* e2fsck: 0 clean, 1 fixed, 4 errors left; fsck.fat 1 means errors
     * found with -n. Both are answers, not failures to run. */
    if (rc == 0) {
        progress(100, "clean");
        done("clean");
    } else if (rc == 1 && repair) {
        done("errors were found and repaired");
    } else if (rc > 0 && rc < 8) {
        failed("failed", repair ? "errors remain after repair"
                                : "errors found: repair is needed");
    } else {
        char msg[64];
        snprintf(msg, sizeof msg, "the checker exited with %d", rc);
        failed("failed", msg);
    }
}

static void v_check(char **a, int n)  { (void)n; fsck_dev(a[0], false); }
static void v_repair(char **a, int n) { (void)n; fsck_dev(a[0], true); }

/* ═══════════════════════════════════════════════════════════════════
 * The password (see "The password" at the top)
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    u32 uid;
    s64 until;          /* keep ends at this boottime ms; 0: none      */
    s64 not_before;     /* no attempt is looked at before this         */
    int fails;          /* wrong passwords in a row                    */
    s64 used;           /* last touched, to pick a slot to reuse       */
} keep_t;

static keep_t keeps[KEEP_SLOTS];

/* CLOCK_BOOTTIME: like CLOCK_MONOTONIC, but it goes on counting while
 * the machine is suspended - see the top for why that matters here. */
static s64 boottime_ms(void)
{
    s64 ts[2] = { 0, 0 };
    if (sys_call2(SYS_clock_gettime, 7 /* CLOCK_BOOTTIME */, (long)ts) < 0)
        return lp_monotonic_ms();
    return ts[0] * 1000 + ts[1] / 1000000;
}

/* The slot for this uid; a new one (the least recently used, when all
 * are taken) if `make`. Forgetting the oldest keep only ever costs
 * somebody one more password dialog. */
static keep_t *keep_for(u32 uid, bool make)
{
    keep_t *free_slot = NULL, *oldest = &keeps[0];
    for (int i = 0; i < KEEP_SLOTS; i++) {
        keep_t *k = &keeps[i];
        if (k->used && k->uid == uid)
            return k;
        if (!k->used && !free_slot)
            free_slot = k;
        if (k->used < oldest->used)
            oldest = k;
    }
    if (!make)
        return NULL;
    keep_t *k = free_slot ? free_slot : oldest;
    memset(k, 0, sizeof *k);
    k->uid = uid;
    k->used = boottime_ms();
    return k;
}

/* Seconds left on this uid's keep; 0 when there is none. */
static long keep_left(u32 uid)
{
    keep_t *k = keep_for(uid, false);
    s64 now = boottime_ms();
    if (!k || k->until <= now)
        return 0;
    return (long)((k->until - now + 999) / 1000);
}

/* Writes the compiler may not drop: the password is gone after this
 * even though nothing reads the buffer again. */
static void wipe_mem(void *p, size_t n)
{
    volatile u8 *v = (volatile u8 *)p;
    while (n--)
        *v++ = 0;
}

static int hexdigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The shape of an auth argument: an even number of hex digits, one to
 * LP_CRYPT6_PW_MAX - 1 bytes once decoded, none of them NUL - a NUL
 * would cut the password short where the person did not. A request of
 * the wrong shape is "invalid" and does not count as a wrong password. */
static bool hex_ok(const char *s)
{
    size_t n = strlen(s);
    if (n < 2 || n % 2 || n > 2 * (LP_CRYPT6_PW_MAX - 1))
        return false;
    for (size_t i = 0; i < n; i++)
        if (hexdigit(s[i]) < 0)
            return false;
    for (size_t i = 0; i < n; i += 2)
        if (s[i] == '0' && s[i + 1] == '0')
            return false;
    return true;
}

/* Decode. hex_ok has been through it already; the NUL test stays here
 * too so that this function is safe on its own. */
static bool hex_decode(const char *s, char *out, size_t outn)
{
    size_t n = strlen(s) / 2;
    if (n + 1 > outn)
        return false;
    for (size_t i = 0; i < n; i++) {
        int b = hexdigit(s[2 * i]) << 4 | hexdigit(s[2 * i + 1]);
        if (b == 0)
            return false;
        out[i] = (char)b;
    }
    out[n] = '\0';
    return true;
}

/* The auth verb. The caller is already known to be a real account and
 * an administrator; this is only about the password. */
static void answer_auth(u32 uid, const char *user, const char *who,
                        char *hex)
{
    keep_t *k = keep_for(uid, true);
    s64 now = boottime_ms();
    char line[300], msg[160];
    k->used = now;

    if (now < k->not_before) {
        long secs = (long)((k->not_before - now + 999) / 1000);
        snprintf(line, sizeof line, "%s: auth *** -> auth (waiting, %lds left)",
                 who, secs);
        audit(line);
        snprintf(msg, sizeof msg, "wait %ld s: too soon after a wrong password",
                 secs);
        fail("auth", msg);
        wipe_mem(hex, strlen(hex));
        return;
    }

    char pw[LP_CRYPT6_PW_MAX];
    bool shaped = hex_decode(hex, pw, sizeof pw);
    wipe_mem(hex, strlen(hex));
    int r = shaped ? lp_shadow_check(NULL, user, pw) : LP_SHADOW_WRONG;
    wipe_mem(pw, sizeof pw);

    const char *code = "auth", *text = NULL, *verdict = NULL;
    switch (r) {
    case LP_SHADOW_OK:
        k->until = now + KEEP_MS;
        k->fails = 0;
        k->not_before = 0;
        snprintf(line, sizeof line, "%s: auth *** -> ok, kept %ds", who,
                 KEEP_MS / 1000);
        audit(line);
        snprintf(msg, sizeof msg, "authorised for %d s", KEEP_MS / 1000);
        reply("done", msg);
        return;
    case LP_SHADOW_WRONG:
        k->fails++;
        k->until = 0;
        {
            s64 wait = (s64)WAIT_STEP_MS * k->fails;
            k->not_before = now + (wait > WAIT_MAX_MS ? WAIT_MAX_MS : wait);
        }
        snprintf(line, sizeof line, "%s: auth *** -> wrong password (%d in a row)",
                 who, k->fails);
        audit(line);
        fail("auth", "wrong password");
        return;
    case LP_SHADOW_LOCKED:
    case LP_SHADOW_EMPTY:
        code = "denied";
        verdict = "no usable password";
        text = "this account has no password to check; set one with passwd";
        break;
    case LP_SHADOW_UNSUPPORTED:
        code = "denied";
        verdict = "password not in $6$ form";
        text = "this account's password is stored in a form this system"
               " cannot check; set it again with passwd";
        break;
    case LP_SHADOW_NOUSER:
        code = "denied";
        verdict = "no shadow entry";
        text = "this account has no entry in /etc/shadow";
        break;
    default:
        code = "failed";
        verdict = "cannot read /etc/shadow";
        text = "cannot read /etc/shadow";
        break;
    }
    snprintf(line, sizeof line, "%s: auth *** -> %s (%s)", who, code, verdict);
    audit(line);
    fail(code, text);
}

/* ═══════════════════════════════════════════════════════════════════
 * The table of verbs
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum { ARG_DEBIAN, ARG_LPPKG, ARG_FLATPAK, ARG_DEV, ARG_DISK, ARG_PART,
               ARG_FS, ARG_SIZE, ARG_LABEL, ARG_HEX } argkind_t;

typedef struct {
    const char *verb;
    bool admin;                    /* false: read-only, any real account */
    bool storage;                  /* applies the disk refusal rules */
    int  min, max;
    argkind_t kind[4];             /* per position; the last repeats */
    void (*fn)(char **args, int n);
    const char *help;
} verb_t;

static const verb_t VERBS[] = {
    { "ping",        false, false, 0, 0,  {0}, NULL, "who am I, and may I change things" },
    { "status",      false, false, 0, 0,  {0}, NULL, "what is running now" },
    { "auth",        false, false, 1, 1,  {ARG_HEX}, NULL, "<password as hex>: allow changes for 5 minutes" },
    { "forget",      false, false, 0, 0,  {0}, NULL, "end those 5 minutes now" },
    { "probe",       false, false, 0, 1,  {ARG_DEV}, NULL, "filesystems, labels and tables of block devices" },
    { "apt-update",  true,  false, 0, 0,  {0}, v_apt_update, "refresh the package lists" },
    { "apt-install", true,  false, 1, 32, {ARG_DEBIAN}, v_apt_install, "install Debian packages" },
    { "apt-remove",  true,  false, 1, 32, {ARG_DEBIAN}, v_apt_remove, "remove Debian packages" },
    { "apt-upgrade", true,  false, 0, 32, {ARG_DEBIAN}, v_apt_upgrade, "upgrade everything, or the named packages" },
    { "apt-repair",  true,  false, 0, 0,  {0}, v_apt_repair, "finish an interrupted dpkg run" },
    { "pkg-update",  true,  false, 0, 0,  {0}, v_pkg_update, "refresh our own package index" },
    { "pkg-install", true,  false, 1, 16, {ARG_LPPKG}, v_pkg_install, "install our own packages" },
    { "pkg-remove",  true,  false, 1, 16, {ARG_LPPKG}, v_pkg_remove, "remove our own packages" },
    { "flatpak-refresh", false, false, 0, 0,  {0}, v_flatpak_refresh, "download Flathub's catalogue" },
    { "flatpak-install", true,  false, 1, 16, {ARG_FLATPAK}, v_flatpak_install, "install applications from Flathub" },
    { "flatpak-remove",  true,  false, 1, 16, {ARG_FLATPAK}, v_flatpak_remove, "remove Flathub applications" },
    { "flatpak-update",  true,  false, 0, 16, {ARG_FLATPAK}, v_flatpak_update, "update everything from Flathub, or the named applications" },
    { "mount",       true,  true,  1, 1,  {ARG_DEV}, v_mount, "mount a partition under /media" },
    { "unmount",     true,  true,  1, 1,  {ARG_DEV}, v_unmount, "unmount it again" },
    { "eject",       true,  true,  1, 1,  {ARG_DISK}, v_eject, "unmount a whole drive and power it down" },
    { "format",      true,  true,  2, 3,  {ARG_DEV, ARG_FS, ARG_LABEL}, v_format, "make a filesystem: <dev> ext4|fat32|exfat|ntfs [label]" },
    { "format-disk", true,  true,  2, 3,  {ARG_DISK, ARG_FS, ARG_LABEL}, v_format_disk, "one partition over the whole drive, formatted" },
    { "mklabel",     true,  true,  1, 1,  {ARG_DISK}, v_mklabel, "a new, empty MBR partition table" },
    { "mkpart",      true,  true,  3, 4,  {ARG_DISK, ARG_FS, ARG_SIZE, ARG_LABEL}, v_mkpart, "<disk> <fs> <MiB|rest> [label]: new partition, formatted" },
    { "rmpart",      true,  true,  1, 1,  {ARG_PART}, v_rmpart, "delete a partition" },
    { "check",       true,  true,  1, 1,  {ARG_DEV}, v_check, "check a filesystem without changing it" },
    { "repair",      true,  true,  1, 1,  {ARG_DEV}, v_repair, "check and repair a filesystem" },
    { NULL, false, false, 0, 0, {0}, NULL, NULL }
};

/* Is this argument the shape its position wants? `why` says what was
 * wrong, for the log and for the person. */
static bool arg_ok(argkind_t k, const char *s, fstype_t fs, char *why,
                   size_t whyn)
{
    switch (k) {
    case ARG_DEBIAN:
        if (debian_name_ok(s)) return true;
        snprintf(why, whyn, "not a package name");
        return false;
    case ARG_LPPKG:
        if (lp_name_ok(s)) return true;
        snprintf(why, whyn, "not a package name");
        return false;
    case ARG_FLATPAK:
        if (flatpak_id_ok(s)) return true;
        snprintf(why, whyn, "not a Flatpak application ID");
        return false;
    case ARG_DEV:
    case ARG_DISK:
    case ARG_PART:
        if (!dev_name_ok(s)) {
            snprintf(why, whyn, "not a device name (sdb1, not /dev/sdb1)");
            return false;
        }
        if (!blk_exists(s)) {
            snprintf(why, whyn, "there is no block device called that");
            return false;
        }
        if (k == ARG_DISK && blk_is_part(s)) {
            snprintf(why, whyn, "that is a partition, and this wants a whole disk");
            return false;
        }
        if (k == ARG_PART && !blk_is_part(s)) {
            snprintf(why, whyn, "that is not a partition");
            return false;
        }
        return true;
    case ARG_FS:
        if (fs_parse(s) != FS_NONE) return true;
        snprintf(why, whyn, "the filesystem is one of ext4, fat32, exfat, ntfs");
        return false;
    case ARG_SIZE:
        if (size_ok(s)) return true;
        snprintf(why, whyn, "the size is a number of MiB, or rest");
        return false;
    case ARG_HEX:
        if (hex_ok(s)) return true;
        snprintf(why, whyn, "the password is sent as hex, 1 to %d bytes",
                 LP_CRYPT6_PW_MAX - 1);
        return false;
    case ARG_LABEL:
        if (label_ok(s, fs != FS_NONE ? label_max(fs) : 32)) return true;
        snprintf(why, whyn, "a label is letters, digits, space, _ . - and at"
                 " most %u of them", (unsigned)(fs != FS_NONE ? label_max(fs) : 32));
        return false;
    }
    return false;
}

/* Is `uid` allowed to change things? Root, or a member of group sudo -
 * listed in the group's line, or with it as their primary group. */
static bool is_admin(u32 uid, u32 gid, char *user, size_t usern)
{
    lp_user_t u;
    if (lp_user_by_uid(uid, &u))
        strlcpy(user, u.name, usern);
    else
        snprintf(user, usern, "%u", uid);
    if (uid == 0)
        return true;
    if (!lp_user_by_uid(uid, &u))
        return false;

    static char grp[16384];
    long got = proc_read("/etc/group", grp, sizeof grp - 1);
    if (got <= 0)
        return false;
    grp[got] = '\0';
    for (char *line = grp; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (starts(line, "sudo:")) {
            /* sudo:x:27:alice,bob */
            char *f2 = strchr(line + 5, ':');
            char *f3 = f2 ? strchr(f2 + 1, ':') : NULL;
            if (f2 && (u32)atoi(f2 + 1) == gid)
                return true;
            if (f2 && (u32)atoi(f2 + 1) == u.gid)
                return true;
            for (char *m = f3 ? f3 + 1 : NULL; m && *m; ) {
                char *comma = strchr(m, ',');
                if (comma) *comma = '\0';
                if (strcmp(m, u.name) == 0)
                    return true;
                m = comma ? comma + 1 : NULL;
            }
            return false;
        }
        line = nl ? nl + 1 : NULL;
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════════
 * The daemon
 * ═══════════════════════════════════════════════════════════════════ */

static pid_t worker = 0;             /* the running job, or 0 */
static char  worker_desc[256];

static void answer_probe(const char *only)
{
    char names[128][32];
    int n = blk_all(names, 128);
    mounts_refresh();
    for (int i = 0; i < n; i++) {
        const char *nm = names[i];
        if (only && strcmp(only, nm) != 0)
            continue;
        if (starts(nm, "ram") || starts(nm, "zram"))
            continue;
        char p[96], v[32];
        snprintf(p, sizeof p, "/sys/class/block/%s/size", nm);
        if (!sys_read(p, v, sizeof v) || strtoll(v, NULL, 10) == 0)
            continue;               /* an empty loop device or card slot */
        probe_t pr;
        if (!probe_dev(nm, &pr))
            continue;
        char line[400];
        snprintf(line, sizeof line, "name=%s\tfs=%s\tlabel=%s\tuuid=%s\tstate=%s\ttable=%s",
                 nm, pr.fs, pr.label, pr.uuid, pr.state, pr.table);
        /* reply() would turn the tabs into spaces, and they are the
         * field separators here - so this one line is sent as it is.
         * The label is the only free text in it, and copy_label and
         * utf16_label already turned every control byte into '?'. */
        char out[420];
        int k = snprintf(out, sizeof out, "dev\t%s\n", line);
        if (k > 0) send_raw(out, (size_t)k);
    }
    reply("done", "");
}

static void answer_status(void)
{
    if (!worker) {
        reply("done", "idle");
        return;
    }
    char buf[512];
    long got = proc_read(JOB_FILE, buf, sizeof buf - 1);
    if (got > 0) {
        buf[got] = '\0';
        char *nl = strchr(buf, '\n');
        if (nl) {
            *nl = '\0';
            char *nl2 = strchr(nl + 1, '\n');
            if (nl2) *nl2 = '\0';
            reply("progress", nl + 1);
        }
    }
    reply("done", worker_desc);
}

/* Read one request line, waiting at most REQ_TIMEOUT for it: a client
 * that connects and says nothing must not hold the daemon. */
static long read_request(int fd, char *buf, size_t n)
{
    size_t got = 0;
    s64 deadline = lp_monotonic_ms() + REQ_TIMEOUT;
    while (got < n - 1) {
        s64 left = deadline - lp_monotonic_ms();
        if (left <= 0)
            return -1;
        lp_pollfd_t p = { fd, LP_POLLIN, 0 };
        if (lp_poll(&p, 1, (int)left) <= 0)
            continue;
        long r = lp_read(fd, buf + got, n - 1 - got);
        if (r <= 0)
            return -1;
        for (long i = 0; i < r; i++) {
            if (buf[got + (size_t)i] == '\n') {
                /* Exactly one line. Anything after it in the same
                 * breath is a second request trying to ride along. */
                if (i != r - 1)
                    return -1;
                buf[got + (size_t)i] = '\0';
                return (long)(got + (size_t)i);
            }
        }
        got += (size_t)r;
    }
    return -1;                          /* too long */
}

static void handle(int fd)
{
    client = fd;
    u32 pid = 0, uid = 0, gid = 0;
    if (!peer_cred(fd, &pid, &uid, &gid)) {
        fail("denied", "could not tell who is asking");
        return;
    }

    char req[MAX_REQ];
    long len = read_request(fd, req, sizeof req);
    char user[40];
    bool admin = is_admin(uid, gid, user, sizeof user);
    char who[96];
    snprintf(who, sizeof who, "uid=%u(%s) pid=%u", uid, user, pid);

    if (len < 0) {
        char line[160];
        snprintf(line, sizeof line, "%s: no request line -> invalid", who);
        audit(line);
        fail("invalid", "one line of at most 1024 bytes, ending in a newline,"
             " within 3 seconds, and nothing after it");
        return;
    }

    /* The whole line is printable ASCII and tabs. Anything else - a
     * newline smuggled in, an escape, a byte from a UTF-8 path - is
     * refused before any field is looked at. */
    for (long i = 0; i < len; i++) {
        unsigned char c = (unsigned char)req[i];
        if (c != '\t' && (c < 0x20 || c > 0x7e)) {
            char line[160];
            snprintf(line, sizeof line, "%s: a byte outside the protocol -> invalid", who);
            audit(line);
            fail("invalid", "requests are printable ASCII fields separated by tabs");
            return;
        }
    }

    /* Log the request as it arrived. It is only ASCII at this point, so
     * it cannot forge a second log line. An auth request is the one
     * exception: what follows the verb is a password, and it is never
     * copied anywhere, the log included. */
    char printable[MAX_REQ];
    if (starts(req, "auth\t") || strcmp(req, "auth") == 0)
        strlcpy(printable, "auth ***", sizeof printable);
    else
        strlcpy(printable, req, sizeof printable);
    for (char *q = printable; *q; q++)
        if (*q == '\t') *q = ' ';

    char *fields[MAX_FIELDS];
    int nf = 0;
    for (char *p = req; p && nf < MAX_FIELDS; ) {
        fields[nf++] = p;
        char *t = strchr(p, '\t');
        if (t) *t++ = '\0';
        p = t;
    }

    const verb_t *v = NULL;
    for (int i = 0; VERBS[i].verb; i++)
        if (strcmp(VERBS[i].verb, fields[0]) == 0)
            v = &VERBS[i];

    char line[1400];
    char why[200] = "";
    const char *verdict = NULL, *code = NULL;
    int nargs = nf - 1;
    char **args = fields + 1;

    mounts_refresh();

    if (!v) {
        verdict = "unknown verb"; code = "invalid";
    } else if (uid != 0 && uid < 1000) {
        verdict = "a service account"; code = "denied";
        snprintf(why, sizeof why, "only a person at this machine may ask");
    } else if ((v->admin || strcmp(v->verb, "auth") == 0) && !admin) {
        verdict = "not in group sudo"; code = "denied";
        snprintf(why, sizeof why, "%s is not an administrator (group sudo)", user);
    } else if (nargs < v->min || nargs > v->max) {
        verdict = "wrong number of arguments"; code = "invalid";
    } else {
        /* A label is checked against the length its filesystem allows,
         * so the filesystem is read first. Package lists take the same
         * kind of argument at every position. */
        fstype_t fs = (nargs > 1 && v->kind[1] == ARG_FS) ? fs_parse(args[1])
                                                          : FS_NONE;
        for (int i = 0; i < nargs && !verdict; i++) {
            argkind_t k = v->max > 4 ? v->kind[0] : v->kind[i];
            if (!arg_ok(k, args[i], fs, why, sizeof why)) {
                verdict = "malformed argument"; code = "invalid";
            }
        }
        /* The storage rules, for everything that changes a device. */
        if (!verdict && v->storage) {
            char disk[32];
            blk_disk(args[0], disk, sizeof disk);
            bool whole_op = strcmp(v->verb, "format-disk") == 0 ||
                            strcmp(v->verb, "mklabel") == 0 ||
                            strcmp(v->verb, "mkpart") == 0 ||
                            strcmp(v->verb, "rmpart") == 0;
            /* Destructive: never on the system's own disk. A read-only
             * check may look at an unmounted partition anywhere - the
             * Windows partition next to ours, say - but a check of a
             * mounted filesystem reports errors that are only writes in
             * flight, so it wants the same "unmounted" as the rest. */
            bool destroys = whole_op || strcmp(v->verb, "format") == 0 ||
                            strcmp(v->verb, "repair") == 0;
            bool unmounted = destroys || strcmp(v->verb, "check") == 0;
            if ((destroys || strcmp(v->verb, "eject") == 0) &&
                is_system_disk(disk, why, sizeof why)) {
                verdict = "the running system's disk"; code = "refused";
            } else if (whole_op && disk_busy(disk, why, sizeof why)) {
                verdict = "mounted"; code = "refused";
            } else if (unmounted && !whole_op && mounted_at(args[0])) {
                snprintf(why, sizeof why, "%s is mounted at %s - unmount it first",
                         args[0], mounted_at(args[0]));
                verdict = "mounted"; code = "refused";
            } else if (unmounted && blk_has_holders(args[0])) {
                snprintf(why, sizeof why, "%s is in use by another device", args[0]);
                verdict = "held"; code = "refused";
            }
        }
    }

    /* Last, and only for what would otherwise go ahead: the password.
     * Asking for it before the refusals above would have somebody type
     * it for a request that was never going to be allowed. */
    if (!verdict && v->admin && uid != 0 && keep_left(uid) == 0) {
        verdict = "no password in the last 5 minutes"; code = "auth";
        snprintf(why, sizeof why, "password required");
    }

    if (verdict) {
        snprintf(line, sizeof line, "%s: %s -> %s (%s)", who, printable, code,
                 why[0] ? why : verdict);
        audit(line);
        fail(code, why[0] ? why : verdict);
        if (v && strcmp(v->verb, "auth") == 0 && nargs > 0)
            wipe_mem(args[0], strlen(args[0]));
        return;
    }

    if (strcmp(v->verb, "ping") == 0) {
        char msg[160];
        snprintf(msg, sizeof msg, "uid=%u user=%s admin=%s auth=%ld", uid,
                 user, admin ? "yes" : "no",
                 uid == 0 ? (long)(KEEP_MS / 1000) : keep_left(uid));
        reply("done", msg);
        return;
    }
    if (strcmp(v->verb, "auth") == 0) {
        if (uid == 0) {
            wipe_mem(args[0], strlen(args[0]));
            reply("done", "root needs no password here");
            return;
        }
        answer_auth(uid, user, who, args[0]);
        return;
    }
    if (strcmp(v->verb, "forget") == 0) {
        keep_t *k = keep_for(uid, false);
        if (k)
            k->until = 0;
        snprintf(line, sizeof line, "%s: forget -> done", who);
        audit(line);
        reply("done", "the password will be asked for again");
        return;
    }
    if (strcmp(v->verb, "status") == 0) {
        answer_status();
        return;
    }
    if (strcmp(v->verb, "probe") == 0) {
        answer_probe(nargs ? args[0] : NULL);
        return;
    }

    if (worker) {
        snprintf(line, sizeof line, "%s: %s -> busy (%s)", who, printable, worker_desc);
        audit(line);
        char msg[300];
        snprintf(msg, sizeof msg, "another job is running: %s", worker_desc);
        fail("busy", msg);
        return;
    }

    snprintf(line, sizeof line, "%s: %s -> accepted", who, printable);
    audit(line);
    snprintf(worker_desc, sizeof worker_desc, "%s", printable);

    pid_t w = lp_fork();
    if (w < 0) {
        fail("failed", "could not start the job");
        return;
    }
    if (w == 0) {
        caller_uid = uid;
        caller_gid = gid;
        strlcpy(job_desc, printable, sizeof job_desc);
        long lf = lp_open(LAST_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
        last_fd = lf >= 0 ? (int)lf : -1;
        if (last_fd >= 0) {
            char head[400];
            int k = snprintf(head, sizeof head, "# %s %s\n", who, printable);
            if (k > 0) lp_write(last_fd, head, (size_t)k);
        }
        job_file(0, "starting");
        s64 t0 = lp_monotonic_ms();
        job_rc = 1;
        v->fn(args, nargs);
        char end[400];
        snprintf(end, sizeof end, "%s: %s -> %s after %lds", who, printable,
                 job_rc == 0 ? "done" : "failed",
                 (long)((lp_monotonic_ms() - t0) / 1000));
        audit(end);
        lp_unlink(JOB_FILE);
        if (client >= 0) lp_close(client);
        lp_exit(job_rc);
    }
    worker = w;
    client = -1;                         /* the worker has it now */
}

static void reap(void)
{
    if (!worker)
        return;
    int status;
    if (lp_waitpid(worker, &status, WNOHANG) == worker) {
        worker = 0;
        worker_desc[0] = '\0';
        lp_unlink(JOB_FILE);
    }
}

static int serve(void)
{
    lp_signal_ignore(13);                /* SIGPIPE */
    sys_call1(SYS_umask_, 022);
    lp_chdir("/");

    lp_unlink(sock_path);
    sun_t sa;
    long ls = sun_fill(&sa, sock_path) ? lp_socket(AF_UNIX_, SOCK_STREAM, 0) : -1;
    if (ls < 0 || lp_bind((int)ls, &sa, sizeof sa) < 0 ||
        lp_listen((int)ls, 16) < 0) {
        dprintf(STDERR_FILENO, "lp-privd: cannot listen on %s\n", sock_path);
        return 1;
    }
    /* Anyone may connect; who may do what is decided per request. */
    lp_chmod(sock_path, 0666);
    audit("listening");

    for (;;) {
        lp_pollfd_t p = { (int)ls, LP_POLLIN, 0 };
        lp_poll(&p, 1, worker ? 500 : -1);
        reap();
        if (!(p.revents & LP_POLLIN))
            continue;
        long fd = lp_accept((int)ls, NULL, NULL, LP_SOCK_CLOEXEC);
        if (fd < 0)
            continue;
        handle((int)fd);
        if (client >= 0) {
            /* Whatever the client sent that was not read - the rest of
             * an over-long line - would turn the close into a reset,
             * and the reset would arrive before the "fail" that says
             * why. Read it away first. */
            lp_shutdown(client, 1);
            char junk[4096];
            for (int i = 0; i < 16; i++)
                if (lp_recvfrom(client, junk, sizeof junk, MSG_DONTWAIT_,
                                NULL, NULL) <= 0)
                    break;
            lp_close(client);
            client = -1;
        } else {
            lp_close((int)fd);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * The client
 * ═══════════════════════════════════════════════════════════════════ */

/* One request, one connection. Lines are printed as they come, except
 * a "fail auth password required" when `hold_auth` is set: that one is
 * the cue to ask for the password and try again, not something for the
 * person to read. Returns 0 on done, 1 on fail, 2 when the daemon could
 * not be reached; *need_auth says whether the held line came. */
static int talk(const char *req, size_t k, bool print, bool hold_auth,
                bool *need_auth, char *last, size_t lastn)
{
    sun_t sa;
    if (!sun_fill(&sa, sock_path))
        return 2;
    long fd = lp_socket(AF_UNIX_, SOCK_STREAM, 0);
    if (fd < 0 || lp_connect((int)fd, &sa, sizeof sa) < 0) {
        dprintf(STDERR_FILENO, "lp-privd: the daemon is not running (%s)\n",
                sock_path);
        if (fd >= 0) lp_close((int)fd);
        return 2;
    }
    lp_write((int)fd, req, k);

    /* The exit status is the last line's. */
    int rc = 1;
    char line[2048];
    long n;
    while ((n = readline((int)fd, line, sizeof line)) >= 0) {
        if (hold_auth && starts(line, "fail auth password required")) {
            *need_auth = true;
            rc = 1;
            continue;
        }
        if (print)
            printf("%s\n", line);
        if (last)
            strlcpy(last, line, lastn);
        if (starts(line, "done"))
            rc = 0;
        else if (starts(line, "fail"))
            rc = 1;
    }
    lp_close((int)fd);
    return rc;
}

/* The password from the terminal, with nothing echoed, the way passwd
 * and sudo read it. False on end of input or Ctrl-C; the terminal is
 * put back either way. */
static bool read_password(char *out, size_t n)
{
    lp_user_t u;
    char name[40];
    if (lp_user_by_uid((uid_t)lp_getuid(), &u))
        strlcpy(name, u.name, sizeof name);
    else
        snprintf(name, sizeof name, "uid %d", lp_getuid());
    dprintf(STDERR_FILENO, "[lp-privd] password for %s: ", name);

    lp_termios_t saved;
    bool quiet = lp_term_cbreak(STDIN_FILENO, &saved) == 0;
    size_t used = 0;
    bool ok = true;
    for (;;) {
        char ch;
        long r = lp_read(STDIN_FILENO, &ch, 1);
        if (r <= 0) { ok = used > 0; break; }
        if (ch == '\n' || ch == '\r')
            break;
        if (ch == 3) { ok = false; break; }         /* Ctrl-C */
        if (ch == 0x7f || ch == '\b') {
            if (used) used--;
            continue;
        }
        if (used < n - 1)
            out[used++] = ch;
    }
    out[used] = '\0';
    if (quiet)
        lp_term_restore(STDIN_FILENO, &saved);
    dprintf(STDERR_FILENO, "\n");
    return ok;
}

/* "auth <hex>\n" for a password, then everything wiped. 0 when it was
 * accepted. A "wait N s" answer - a wrong password was typed a moment
 * ago - is waited out here and the same password sent again, once:
 * making the person type it a second time for the daemon's pause would
 * only look like the first one had been wrong. */
static int send_auth_once(const char *pw, char *last, size_t lastn);

static int send_auth(const char *pw, bool print)
{
    for (int round = 0; ; round++) {
        char last[256] = "";
        int rc = send_auth_once(pw, last, sizeof last);
        long secs = 0;
        if (rc == 1 && round == 0 && starts(last, "fail auth wait ")) {
            secs = strtol(last + 15, NULL, 10);
            if (secs > 0 && secs <= WAIT_MAX_MS / 1000) {
                if (print)
                    dprintf(STDERR_FILENO, "lp-privd: waiting %ld s after a"
                            " wrong password\n", secs);
                lp_sleep_ms((unsigned)secs * 1000 + 100);
                continue;
            }
        }
        if (rc != 0 && print && last[0])
            dprintf(STDERR_FILENO, "lp-privd: %s\n", last);
        return rc;
    }
}

static int send_auth_once(const char *pw, char *last, size_t lastn)
{
    static const char hx[] = "0123456789abcdef";
    char req[MAX_REQ];
    size_t k = strlcpy(req, "auth\t", sizeof req);
    for (const char *p = pw; *p && k + 3 < sizeof req; p++) {
        req[k++] = hx[(u8)*p >> 4];
        req[k++] = hx[(u8)*p & 15];
    }
    req[k++] = '\n';
    bool dummy = false;
    int rc = talk(req, k, false, false, &dummy, last, lastn);
    wipe_mem(req, sizeof req);
    return rc;
}

static int ask(int argc, char **argv)
{
    bool tty = lp_isatty(STDIN_FILENO);
    char pw[LP_CRYPT6_PW_MAX];

    /* `lp-privd auth` on its own: ask here, like `sudo -v`. */
    if (argc == 1 && strcmp(argv[0], "auth") == 0) {
        if (!tty) {
            dprintf(STDERR_FILENO, "lp-privd: auth needs a terminal, or the"
                    " password as hex after it\n");
            return 2;
        }
        if (!read_password(pw, sizeof pw))
            return 1;
        int rc = send_auth(pw, true);
        wipe_mem(pw, sizeof pw);
        if (rc == 0)
            printf("done authorised for %d s\n", KEEP_MS / 1000);
        return rc;
    }

    char req[MAX_REQ];
    size_t k = 0;
    for (int i = 0; i < argc; i++) {
        if (i) {
            if (k + 1 >= sizeof req) break;
            req[k++] = '\t';
        }
        k += strlcpy(req + k, argv[i], sizeof req - k);
        if (k >= sizeof req - 1) {
            dprintf(STDERR_FILENO, "lp-privd: the request is too long\n");
            return 2;
        }
    }
    req[k++] = '\n';

    bool need = false;
    int rc = talk(req, k, true, tty, &need, NULL, 0);
    /* The daemon wants the password: three tries, as sudo gives. The
     * daemon itself makes each wrong one wait longer. */
    for (int attempt = 0; need && attempt < 3; attempt++) {
        if (!read_password(pw, sizeof pw))
            return 1;
        int ar = send_auth(pw, true);
        wipe_mem(pw, sizeof pw);
        if (ar == 0) {
            need = false;
            rc = talk(req, k, true, false, &need, NULL, 0);
            break;
        }
        if (ar == 2)
            return 2;
    }
    if (need) {
        printf("fail auth password required\n");
        return 1;
    }
    return rc;
}

static void usage(void)
{
    printf("usage: lp-privd -d [--socket PATH]    run the daemon\n"
           "       lp-privd [--socket PATH] <verb> [arg...]\n\n"
           "The root side of the software centre and of Disks. Changing\n"
           "anything needs root, or group sudo and your password (asked\n"
           "for here when stdin is a terminal, then kept for 5 minutes);\n"
           "ping, status and probe are open to every person with an\n"
           "account. Every request is logged in %s.\n\n", LOG_PATH);
    for (int i = 0; VERBS[i].verb; i++)
        printf("  %-12s %s%s\n", VERBS[i].verb, VERBS[i].help,
               VERBS[i].admin ? "" :
               strcmp(VERBS[i].verb, "auth") == 0 ? "  (group sudo)" :
                                                    "  (anyone)");
    printf("\nDevices are kernel names - sdb, sdb1 - never paths.\n");
}

int main(int argc, char **argv)
{
    int a = 1;
    bool daemon = false;
    while (a < argc && argv[a][0] == '-') {
        if (strcmp(argv[a], "-d") == 0) {
            daemon = true;
            a++;
        } else if (strcmp(argv[a], "--socket") == 0 && a + 1 < argc) {
            sock_path = argv[a + 1];
            a += 2;
        } else if (strcmp(argv[a], "-h") == 0 || strcmp(argv[a], "--help") == 0) {
            usage();
            return 0;
        } else {
            break;
        }
    }
    if (daemon) {
        if (lp_getuid() != 0) {
            dprintf(STDERR_FILENO, "lp-privd: the daemon has to run as root\n");
            return 1;
        }
        return serve();
    }
    if (a >= argc || strcmp(argv[a], "help") == 0) {
        usage();
        return a >= argc ? 2 : 0;
    }
    return ask(argc - a, argv + a);
}
