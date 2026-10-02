/* lp-diskd - the one root process allowed to change disks, and
 * lp-diskctl, the same program used from a terminal.
 *
 *   lp-diskd -d [--socket PATH]       the daemon; /etc/lp/services starts it
 *   lp-diskctl <command> [arg...]     list, check, resize, move, ... (see help)
 *   lp-diskd ctl <command> [arg...]   the same, without the second name
 *
 * The Disks application (desktop/disks, lp-disks) is a partition manager:
 * it shrinks the system partition to make room for a shared one, moves a
 * FAT partition out of the way, formats, labels, checks and repairs. All
 * of that needs root, and the session that runs the application is
 * uid 1000 on purpose. This program is what stands between the two.
 *
 * ── Why a daemon and not a setuid program ──
 *
 * The same reasons lp-privd gives, only sharper: a resize is resize2fs
 * running for minutes over a filesystem it is rewriting, and a move is
 * this program copying gigabytes over the place they came from. A setuid
 * process inherits the caller's resource limits (RLIMIT_FSIZE set low is
 * the classic way to make a root program stop half way through a write),
 * its environment, its descriptors, and can be stopped with ^Z by the
 * person who started it. A daemon started by init has none of that, the
 * caller cannot signal it, and what the caller can influence shrinks to a
 * line of text on a socket. It also serialises the work for free.
 *
 * lp-privd already has a handful of disk verbs (format, mkpart, check);
 * they stay as they are, frozen, and everything a partition manager needs
 * is here instead - GPT as well as MBR, resize, move, flags, SMART, a
 * benchmark, images, /etc/fstab - so that the software centre's daemon
 * does not grow into a disk tool and this one never has to run apt.
 *
 * ── Who may ask ──
 *
 * /run/lp-diskd.sock is mode 0666 and SO_PEERCRED says who connected.
 * Reading - the disk list, SMART, ping, status, /etc/fstab - is open to
 * root and to any real account (uid >= 1000): lsblk shows the same to
 * everybody, and the raw devices it is read from are root's.
 *
 * Changing anything needs root, or membership of group sudo AND the
 * caller's own password within the last five minutes. The password
 * arrives in an "auth" request, is checked with libc's crypt6
 * (lp_shadow_check against /etc/shadow, SHA-512 crypt), and is then
 * remembered for that uid for five minutes - the same bargain sudo and
 * pkexec's auth_admin_keep make. It is never written anywhere. A wrong
 * password makes that uid wait (2 s, doubling, at most 30 s) before the
 * next try is even looked at.
 *
 * Root never needs a password here, because root is what the password
 * would buy. That is what makes the recovery shell work: it becomes root
 * only after the person has proved they are an administrator (an admin
 * account's password, or the recovery password), and lp-diskctl run as
 * root then does the work in its own process - no daemon, no socket, no
 * second question. In the normal system a person gets root only through
 * sudo, which asks.
 *
 * ── What a request can contain ──
 *
 * One line of printable ASCII, fields separated by TAB, at most 16 KiB.
 * The first field is a verb from a fixed table; every other field is
 * checked against the one shape its position takes before anything
 * else happens: a kernel block device name ("sdb1", never "/dev/sdb1"),
 * a partition GUID, a byte count, a filesystem from a fixed list, a GPT
 * type from a fixed list, a label. Labels may be Korean: a field that
 * starts "x:" is hex-encoded UTF-8 and is checked, after decoding, to be
 * valid UTF-8 without control characters, quotes, slashes or any of the
 * characters Windows refuses in a volume name. None of the alphabets
 * can make a path, and nothing reaches a shell: every tool is started
 * with execve() from an absolute path looked up in fixed directories,
 * with an argv and an environment built here.
 *
 * The only thing a client can hand over that is not text is an open
 * file, for "save an image of this partition" and "restore it": the
 * client opens the file itself, with its own permissions, and passes the
 * descriptor over the socket (SCM_RIGHTS). The daemon checks it is a
 * regular file and never learns, or needs, its name. That is how "never
 * a path from the client" and "save it in my home folder" are both true.
 *
 * ── Plans ──
 *
 * The application queues operations and shows them in plain words; the
 * disk is not touched until Apply. Apply sends the whole queue as one
 * "plan" request, steps separated by a field containing "|". The daemon
 * first plays the plan through on a model of every partition table it
 * touches - overlaps, bounds, alignment, free slots, which filesystems
 * can shrink - and refuses the whole plan if any step would fail. Only
 * then does it start, and after every step it reads the table back from
 * the disk, makes the kernel's view match it, and checks both against
 * what the model said should be there. The first step that fails stops
 * the plan, and the reply ends with "state" lines saying exactly what is
 * on the disk now: which steps finished, which half of the failed step
 * happened ("the filesystem is already 300 GiB but the partition is still
 * 400 GiB - safe"), and what to do next.
 *
 * Partitions are named in a plan by their PARTUUID (the GPT entry's own
 * GUID, or signature-number on MBR), not by sdb2: numbers and kernel
 * names can shift under a plan that deletes and creates, a GUID cannot.
 * A partition that a step earlier in the same plan creates is "@N".
 *
 * ── Telling the kernel ──
 *
 * The partition table is written with sfdisk (util-linux) and the kernel
 * is told with BLKPG - add, delete, resize one partition - rather than
 * with BLKRRPART alone. BLKRRPART re-reads the whole table and fails with
 * EBUSY if any partition of the disk is mounted, which on the laptop is
 * always: shrinking LP-ROOT from the recovery system means editing the
 * disk the recovery system is running from. BLKPG changes only the
 * partitions that changed, and those are never mounted - that is checked
 * first. It is also the only way at all on a kernel without the GPT or
 * MBR parser, which the build host this was tested on is.
 *
 * ── Moving a partition ──
 *
 * Moving is copying every block of the partition to its new place, and
 * when old and new overlap, the copy overwrites the data it is copying.
 * Done in the right direction that is correct; interrupted, it leaves a
 * partition that is neither here nor there. So the copy runs from the
 * end that is safe, in chunks no bigger than half the distance moved, and
 * every so many chunks it flushes the device and writes how far it got
 * to /var/lib/lp-diskd/move.journal (atomically, fsynced). The interval
 * is chosen so that repeating everything after the last journal entry
 * never reads a block the copy has already overwritten. After a power
 * cut, `lp-diskctl resume` (or the app's banner) finishes the move from
 * the journal, and only then is the table pointed at the new place.
 *
 * ── What it refuses no matter who asks ──
 *
 * Any partition that is mounted, is active swap, or has something built
 * on it (device-mapper, LUKS, md). The partition that holds /, /boot,
 * /boot/efi, /usr, /var, /home or /data of the running system: it says
 * to restart into Recovery, where that partition is not in use. A new
 * partition table, or an erase, on a disk with anything in use. Deleting
 * or formatting the ESP or LP-RECOVERY unless the request carries the
 * typed confirmation token the application only sends after the person
 * has typed the name. Secure erase of anything that is not removable.
 *
 * ── Every action is written down ──
 *
 * /var/log/lp-diskd.log gets one line per request - who, what, verdict -
 * one per command run and one for how it ended, and the kernel log gets
 * the same through lp_log. Passwords and passphrases never appear in it.
 *
 * ── The answer ──
 *
 * Lines, ending with exactly one "done ..." or "fail <why> ...". In
 * between: "log <text>", "progress <0-100> <text>", "step <i> <n>
 * <text>" and "stepdone <i>" for plans, "state <text>" before a failed
 * plan's "fail", and TAB-separated "disk"/"part"/"smart"/"fstab"/"bench"
 * records. <why> is denied, auth, invalid, refused, busy, missing,
 * cancelled or failed, so an application can tell "you may not" from
 * "type your password" from "this broke".
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"
#include "syscall.h"
#include "crypt6.h"

#define SOCK_PATH     "/run/lp-diskd.sock"
#define LOCK_PATH     "/run/lp-diskd.lock"
#define JOB_FILE      "/run/lp-diskd.job"
#define LOG_PATH      "/var/log/lp-diskd.log"
#define LAST_PATH     "/var/log/lp-diskd.last"
#define STATE_DIR     "/var/lib/lp-diskd"
#define JOURNAL_PATH  STATE_DIR "/move.journal"
#define FSTAB_PATH    "/etc/fstab"
#define LOG_ROTATE    (1024 * 1024)

#define MAX_REQ       16384
#define MAX_FIELDS    160
#define MAX_STEPS     24
#define REQ_TIMEOUT   3000       /* ms a client gets to send its line */
#define AUTH_KEEP_MS  (5 * 60 * 1000)

#define MIB           (1024ull * 1024ull)
#define GIB           (1024ull * MIB)
#define ALIGN_BYTES   MIB        /* every start we choose is on a MiB */

#define AF_UNIX_      1
#define SO_PEERCRED_  17
#define SCM_RIGHTS_   1
#define MSG_DONTWAIT_ 0x40
#define MSG_NOSIGNAL_ 0x4000
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19
#define SIGUSR1_      10
#define EINTR_        4
#define EAGAIN_       11
#define EBUSY_        16
#define LOCK_EX_      2
#define LOCK_NB_      4

/* The system calls libc has no wrapper for, per machine. umask decides
 * the mode of what the tools create; flock serialises the daemon's
 * worker with a root lp-diskctl in another terminal; socketpair is how
 * lp-diskctl runs a request in its own process through exactly the code
 * the daemon uses; recvmsg/sendmsg carry the image file descriptor. */
#if defined(__x86_64__)
#  define SYS_umask_      95
#  define SYS_flock_      73
#  define SYS_socketpair_ 53
#  define SYS_recvmsg_    47
#  define SYS_sendmsg_    46
#elif defined(__aarch64__)
#  define SYS_umask_      166
#  define SYS_flock_      32
#  define SYS_socketpair_ 199
#  define SYS_recvmsg_    212
#  define SYS_sendmsg_    211
#else
#  define SYS_umask_      60
#  define SYS_flock_      143
#  define SYS_socketpair_ 288
#  define SYS_recvmsg_    297
#  define SYS_sendmsg_    296
#endif

/* ═══════════════════════════════════════════════════════════════════
 * Small pieces
 * ═══════════════════════════════════════════════════════════════════ */

static const char *sock_path = SOCK_PATH;

static bool starts(const char *s, const char *p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

/* strcasecmp's answer for ASCII: 0 when equal ignoring case. UUIDs are
 * written in either case, by different tools, for the same thing. */
static int ieq_not(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y)
            return 1;
    }
    return *a != *b;
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
    /* sysfs pads some values (model names) with trailing spaces. */
    size_t l = strlen(buf);
    while (l > 0 && buf[l - 1] == ' ') buf[--l] = '\0';
    return true;
}

static u64 sys_u64(const char *path)
{
    char v[40];
    if (!sys_read(path, v, sizeof v))
        return 0;
    return (u64)strtoll(v, NULL, 10);
}

/* A decimal number with nothing else in it, and no more digits than a
 * u64 holds. Every size and offset in a request goes through this. */
static bool parse_u64(const char *s, u64 *out)
{
    size_t n = strlen(s);
    if (n == 0 || n > 19)
        return false;
    u64 v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + (u64)(s[i] - '0');
    }
    *out = v;
    return true;
}

/* "1.5 GiB". Binary units, because that is what the partitions are cut
 * in and what the application shows; one decimal below 100, rounded to
 * the nearest (1200 MiB is "1.2 GiB", not "1.1"). */
static void human(u64 bytes, char *out, size_t n)
{
    static const char *const unit[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    int u = 0;
    u64 div = 1;
    while (u < 5 && bytes / div >= 1024) {
        div *= 1024;
        u++;
    }
    if (u == 0) {
        snprintf(out, n, "%llu B", (unsigned long long)bytes);
        return;
    }
    u64 tenths = (bytes / div) * 10 + ((bytes % div) * 10 + div / 2) / div;
    if (tenths >= 1000 || tenths % 10 == 0)
        snprintf(out, n, "%llu %s", (unsigned long long)((tenths + 5) / 10), unit[u]);
    else
        snprintf(out, n, "%llu.%llu %s", (unsigned long long)(tenths / 10),
                 (unsigned long long)(tenths % 10), unit[u]);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Hex to bytes. false on anything that is not an even run of hex. */
static bool unhex(const char *s, char *out, size_t outn, size_t *len)
{
    size_t n = strlen(s);
    if (n % 2 || n / 2 >= outn)
        return false;
    for (size_t i = 0; i < n; i += 2) {
        int a = hexval(s[i]), b = hexval(s[i + 1]);
        if (a < 0 || b < 0)
            return false;
        out[i / 2] = (char)(a * 16 + b);
    }
    out[n / 2] = '\0';
    if (len) *len = n / 2;
    return true;
}

/* Wipe a secret so it does not sit in a freed stack frame. volatile, so
 * the compiler cannot decide the stores are dead. */
static void scrub(void *p, size_t n)
{
    volatile u8 *v = p;
    while (n--) *v++ = 0;
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

/* A kernel block device name: sda, sdb1, nvme0n1p2, mmcblk0p1, loop3. */
static bool dev_name_ok(const char *s)
{
    return alphabet_ok(s, "", 31, false) && s[0] >= 'a' && s[0] <= 'z';
}

/* 8-4-4-4-12 hex, either case. */
static bool guid_ok(const char *s)
{
    if (strlen(s) != 36)
        return false;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else if (hexval(s[i]) < 0) {
            return false;
        }
    }
    return true;
}

/* MBR's PARTUUID: the disk signature and the slot, "1234abcd-02". */
static bool mbr_partuuid_ok(const char *s)
{
    if (strlen(s) != 11 || s[8] != '-')
        return false;
    for (int i = 0; i < 11; i++)
        if (i != 8 && hexval(s[i]) < 0)
            return false;
    return true;
}

static void upcase(char *s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
}

static void downcase(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}

/* ── Labels ─────────────────────────────────────────────────────────
 *
 * A label arrives either as itself - letters, digits, space, _ . - - or
 * as "x:" and the hex of its UTF-8, which is how a Korean label crosses
 * a protocol that is ASCII on purpose. "-" alone means "no label".
 *
 * After decoding it has to be well-formed UTF-8 (no overlong forms, no
 * surrogates), with no control characters and none of " \ / : * ? < > |
 * , - quotes and backslashes would break sfdisk's script syntax, and the
 * rest are what Windows refuses in a volume name, so a label that is
 * accepted here is one every system the stick meets will show. */
static bool utf8_clean(const char *s, size_t n, size_t *units16)
{
    size_t u = 0;
    for (size_t i = 0; i < n; ) {
        u8 c = (u8)s[i];
        u32 cp;
        int len;
        if (c < 0x80)      { cp = c; len = 1; }
        else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; len = 2; }
        else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; len = 3; }
        else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; len = 4; }
        else return false;
        if (i + (size_t)len > n)
            return false;
        for (int k = 1; k < len; k++) {
            u8 cc = (u8)s[i + (size_t)k];
            if ((cc & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
            (len == 4 && (cp < 0x10000 || cp > 0x10ffff)) ||
            (cp >= 0xd800 && cp <= 0xdfff))
            return false;
        if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0))
            return false;
        if (cp < 0x80 && strchr("\"\\/:*?<>|,'`$", (int)cp))
            return false;
        u += cp >= 0x10000 ? 2 : 1;
        i += (size_t)len;
    }
    if (units16) *units16 = u;
    return true;
}

typedef enum { FS_NONE, FS_EXT4, FS_BTRFS, FS_FAT32, FS_EXFAT, FS_NTFS,
               FS_SWAP, FS_LUKS } fstype_t;

static const struct { const char *name; fstype_t fs; } FS_NAMES[] = {
    { "ext4", FS_EXT4 }, { "btrfs", FS_BTRFS }, { "fat32", FS_FAT32 },
    { "exfat", FS_EXFAT }, { "ntfs", FS_NTFS }, { "swap", FS_SWAP },
    { "luks-ext4", FS_LUKS }, { "none", FS_NONE }, { NULL, FS_NONE }
};

static bool fs_parse(const char *s, fstype_t *out)
{
    for (int i = 0; FS_NAMES[i].name; i++)
        if (strcmp(s, FS_NAMES[i].name) == 0) {
            *out = FS_NAMES[i].fs;
            return true;
        }
    return false;
}

static const char *fs_word(fstype_t f)
{
    for (int i = 0; FS_NAMES[i].name; i++)
        if (FS_NAMES[i].fs == f)
            return FS_NAMES[i].name;
    return "none";
}

/* How long a label each filesystem keeps, in bytes (ext4, btrfs, swap:
 * the superblock field is bytes) or UTF-16 units (exFAT, NTFS, GPT
 * names). FAT stores its label in the OEM code page, so it takes ASCII
 * only - a Korean FAT label would come back as question marks on the
 * next machine. */
typedef enum { LBL_BYTES, LBL_UTF16, LBL_ASCII } lblkind_t;

static void label_rule(fstype_t f, size_t *max, lblkind_t *kind)
{
    switch (f) {
    case FS_EXT4:  *max = 16;  *kind = LBL_BYTES; break;
    case FS_LUKS:  *max = 16;  *kind = LBL_BYTES; break;
    case FS_BTRFS: *max = 255; *kind = LBL_BYTES; break;
    case FS_SWAP:  *max = 15;  *kind = LBL_BYTES; break;
    case FS_FAT32: *max = 11;  *kind = LBL_ASCII; break;
    case FS_EXFAT: *max = 11;  *kind = LBL_UTF16; break;
    case FS_NTFS:  *max = 32;  *kind = LBL_UTF16; break;
    default:       *max = 36;  *kind = LBL_UTF16; break;   /* GPT name */
    }
}

/* Decode a label field into out. "-" is the empty label. */
static bool label_decode(const char *field, fstype_t f, bool gpt_name,
                         char *out, size_t outn, char *why, size_t whyn)
{
    size_t max;
    lblkind_t kind;
    if (gpt_name) { max = 36; kind = LBL_UTF16; }
    else label_rule(f, &max, &kind);

    if (strcmp(field, "-") == 0) {
        out[0] = '\0';
        return true;
    }
    size_t len = 0;
    if (starts(field, "x:")) {
        if (!unhex(field + 2, out, outn, &len) || len == 0) {
            snprintf(why, whyn, "a label written as x: must be hex UTF-8");
            return false;
        }
    } else {
        if (!alphabet_ok(field, " _.-", 255, true) || field[0] == ' ') {
            snprintf(why, whyn, "a label is letters, digits, space, _ . -"
                     " (or x: and the hex of its UTF-8)");
            return false;
        }
        strlcpy(out, field, outn);
        len = strlen(out);
    }
    size_t units = 0;
    if (!utf8_clean(out, len, &units) || out[0] == '-' || out[0] == ' ') {
        snprintf(why, whyn, "that label has characters a volume name cannot"
                 " hold");
        return false;
    }
    if (kind == LBL_ASCII) {
        for (size_t i = 0; i < len; i++)
            if ((u8)out[i] >= 0x80) {
                snprintf(why, whyn, "a FAT32 label can only use English"
                         " letters and digits");
                return false;
            }
    }
    size_t used = kind == LBL_UTF16 ? units : len;
    if (used > max) {
        snprintf(why, whyn, "that label is too long for %s: at most %u %s",
                 gpt_name ? "a partition name" : fs_word(f), (unsigned)max,
                 kind == LBL_BYTES ? "bytes" : "characters");
        return false;
    }
    return true;
}

/* ── Partition types ────────────────────────────────────────────────
 *
 * A fixed list, by the names parted and GParted use. The client never
 * sends a GUID or a type byte, so no request can set a type this table
 * does not know. */
typedef struct {
    const char *name;
    const char *gpt;
    u8          mbr;             /* 0 = not on MBR */
    const char *desc;
} ptype_t;

static const ptype_t PTYPES[] = {
    { "linux",   "0FC63DAF-8483-4772-8E79-3D69D8477DE4", 0x83, "Linux filesystem" },
    { "esp",     "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", 0xef, "EFI System" },
    { "msdata",  "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", 0x07, "Microsoft basic data" },
    { "fat32",   "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", 0x0c, "FAT32 (LBA)" },
    { "swap",    "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", 0x82, "Linux swap" },
    { "lvm",     "E6D6D379-F507-44C2-A23C-238F2A3DF928", 0x8e, "Linux LVM" },
    { "raid",    "A19D880F-05FC-4D3B-A006-743F0F84911E", 0xfd, "Linux RAID" },
    { "home",    "933AC7E1-2EB4-4F13-B844-0E14E2AEF915", 0,    "Linux /home" },
    { "root",    "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709", 0,    "Linux root (x86-64)" },
    { "luks",    "CA7D7CCB-63ED-4C53-861C-1742536059CC", 0,    "Linux LUKS" },
    { "msres",   "E3C9E316-0B5C-4DB8-817D-F92DF00215AE", 0,    "Microsoft reserved" },
    { "winre",   "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", 0x27, "Windows recovery" },
    { "bios",    "21686148-6449-6E6F-744E-656564454649", 0,    "BIOS boot" },
    { NULL, NULL, 0, NULL }
};

static const ptype_t *ptype_by_name(const char *s)
{
    for (int i = 0; PTYPES[i].name; i++)
        if (strcmp(PTYPES[i].name, s) == 0)
            return &PTYPES[i];
    return NULL;
}

/* The name for a type as found on disk: a GUID, or "0x83". */
static const ptype_t *ptype_find(const char *type)
{
    if (starts(type, "0x")) {
        int v = hexval(type[2]) * 16 + hexval(type[3]);
        for (int i = 0; PTYPES[i].name; i++)
            if (PTYPES[i].mbr == v)
                return &PTYPES[i];
        return NULL;
    }
    for (int i = 0; PTYPES[i].name; i++)
        if (strcmp(PTYPES[i].gpt, type) == 0)
            return &PTYPES[i];
    return NULL;
}

/* The type a filesystem gets when the plan says "auto". */
static const char *auto_type(fstype_t f, bool gpt)
{
    switch (f) {
    case FS_FAT32: return gpt ? "msdata" : "fat32";
    case FS_EXFAT:
    case FS_NTFS:  return "msdata";
    case FS_SWAP:  return "swap";
    default:       return "linux";
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

/* One line in /var/log/lp-diskd.log and in the kernel log. Cut at 1MB
 * into a single .1, so years of use cannot fill the disk with it. */
static void audit(const char *text)
{
    char ts[32], line[1400];
    stamp(ts, sizeof ts);
    int n = snprintf(line, sizeof line, "%s %s\n", ts, text);
    if (n <= 0)
        return;
    if (n >= (int)sizeof line) {
        n = (int)sizeof line - 1;
        line[n - 1] = '\n';
    }
    lp_stat_t st;
    if (lp_stat(LOG_PATH, &st, true) == 0 && st.size > LOG_ROTATE)
        lp_rename(LOG_PATH, LOG_PATH ".1");
    long fd = lp_open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (fd >= 0) {
        lp_write((int)fd, line, (size_t)n);
        lp_close((int)fd);
    }
    lp_log("lp-diskd", text);
}

/* ═══════════════════════════════════════════════════════════════════
 * Talking to the client
 * ═══════════════════════════════════════════════════════════════════ */

static int client = -1;          /* the socket of whoever asked */
static int last_fd = -1;         /* /var/log/lp-diskd.last while a job runs */

/* Never blocks and never raises SIGPIPE. A client that stopped reading
 * or went away loses lines; the job does not wait for it - a resize
 * must finish whether or not the window that asked for it is open. */
static void send_raw(const char *s, size_t n)
{
    if (last_fd >= 0)
        lp_write(last_fd, s, n);
    if (client < 0)
        return;
    int tries = 0;
    while (n > 0) {
        long w = lp_sendto(client, s, n, MSG_DONTWAIT_ | MSG_NOSIGNAL_, NULL, 0);
        if (w == -EAGAIN_ && tries++ < 20) {
            /* The client is slow, not gone: give it a moment, then drop
             * the line rather than stall the job behind it. */
            lp_sleep_ms(5);
            continue;
        }
        if (w <= 0) {
            if (w == -EAGAIN_ || w == -EINTR_)
                return;
            lp_close(client);
            client = -1;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

/* A reply line. Control characters in `text` become spaces, so every
 * line has exactly the shape the protocol promises. */
static void reply(const char *kind, const char *text)
{
    char line[1200];
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

static int  progress_last = -1;
static char job_desc[300];

/* The job file is how a second window, or `lp-diskctl status`, finds
 * out what is running after the window that started it was closed. */
static void job_file(int pct, const char *text)
{
    char buf[600];
    int n = snprintf(buf, sizeof buf, "%s\n%d %s\n", job_desc, pct,
                     text ? text : "");
    if (n > 0)
        lp_write_file_atomic(JOB_FILE, buf, (size_t)n);
}

static void progress(int pct, const char *text)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char line[400];
    snprintf(line, sizeof line, "%d %s", pct, text ? text : "");
    reply("progress", line);
    if (pct != progress_last) {
        progress_last = pct;
        job_file(pct, text);
    }
}

/* A TAB-separated record - "disk", "part", "smart" ... Fields are
 * built by the caller; any control byte in them (a tab in a label read
 * off a disk) is turned into '?' so the record keeps its columns. */
static void record(const char *kind, const char *fields)
{
    char out[2400];
    size_t k = strlcpy(out, kind, sizeof out);
    if (k < sizeof out - 2) out[k++] = '\t';
    for (const char *p = fields; *p && k < sizeof out - 2; p++) {
        unsigned char c = (unsigned char)*p;
        out[k++] = (c == '\t') ? '\t' : (c < 0x20 || c == 0x7f) ? '?' : (char)c;
    }
    out[k++] = '\n';
    send_raw(out, k);
}

/* ═══════════════════════════════════════════════════════════════════
 * Block devices, from sysfs
 *
 * Everything goes by the kernel's own name for a device and by
 * /sys/class/block, which knows every partition whatever table
 * described it, and by the partition tables read straight off the disk
 * below - never by udev's /dev/disk links, which the laptop's init does
 * not necessarily have.
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

/* Size in bytes. sysfs counts 512-byte units whatever the sector is. */
static u64 blk_bytes(const char *name)
{
    char p[96];
    snprintf(p, sizeof p, "/sys/class/block/%s/size", name);
    return sys_u64(p) * 512;
}

static u32 blk_lss(const char *disk)
{
    char p[96];
    snprintf(p, sizeof p, "/sys/block/%s/queue/logical_block_size", disk);
    u64 v = sys_u64(p);
    return (v == 512 || v == 1024 || v == 2048 || v == 4096) ? (u32)v : 512;
}

/* /dev/<name>, checked to be the block device sysfs says it is. This
 * catches a stale node left by a device that went away and came back
 * numbered differently - which would otherwise send mkfs to the wrong
 * disk - and waits a moment for devtmpfs after a partition appears. */
static bool dev_path(const char *name, char *out, size_t n)
{
    u32 maj, min;
    for (int tries = 0; tries < 30; tries++) {
        if (blk_devnum(name, &maj, &min)) {
            snprintf(out, n, "/dev/%s", name);
            lp_stat_t st;
            if (lp_stat(out, &st, true) == 0 &&
                (st.mode & LP_S_IFMT) == LP_S_IFBLK) {
                u32 smaj = (u32)(((st.rdev >> 8) & 0xfff) | ((st.rdev >> 32) & ~0xfffu));
                u32 smin = (u32)((st.rdev & 0xff) | ((st.rdev >> 12) & ~0xffu));
                if (smaj == maj && smin == min)
                    return true;
            }
        }
        lp_sleep_ms(100);
    }
    return false;
}

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

/* The partitions of one disk that the kernel has now. */
static int blk_parts(const char *disk, char names[][32], int max)
{
    static char all[256][32];
    int na = blk_all(all, 256), n = 0;
    for (int i = 0; i < na && n < max; i++) {
        char parent[32];
        if (blk_is_part(all[i]) && blk_parent(all[i], parent, sizeof parent) &&
            strcmp(parent, disk) == 0)
            strlcpy(names[n++], all[i], 32);
    }
    return n;
}

static bool dir_nonempty(const char *path)
{
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
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

/* Does anything sit on top of this device - device-mapper, md, an open
 * LUKS mapping? Then writing to it pulls the floor from under that. */
static bool blk_has_holders(const char *name)
{
    char p[96];
    snprintf(p, sizeof p, "/sys/class/block/%s/holders", name);
    return dir_nonempty(p);
}

/* The partition name for slot `num` of `disk`: sdb + 1 = sdb1, but
 * nvme0n1 + 1 = nvme0n1p1, mmcblk0 + 1 = mmcblk0p1, loop0 + 1 = loop0p1. */
static void part_name(const char *disk, int num, char *out, size_t n)
{
    size_t l = strlen(disk);
    bool p = l > 0 && disk[l - 1] >= '0' && disk[l - 1] <= '9';
    snprintf(out, n, "%s%s%d", disk, p ? "p" : "", num);
}

/* Which kind of disk this is, for the icon and for "removable". The
 * device link says the bus: .../usb.../ is USB whatever the SCSI layer
 * above it claims; mmcblk is the SD reader; nvme and loop say so in the
 * name. */
static void blk_transport(const char *disk, char *out, size_t n)
{
    char p[96], link[512];
    snprintf(p, sizeof p, "/sys/block/%s", disk);
    long r = lp_readlink(p, link, sizeof link - 1);
    link[r > 0 ? r : 0] = '\0';
    if (starts(disk, "loop"))           strlcpy(out, "loop", n);
    else if (starts(disk, "nvme"))      strlcpy(out, "nvme", n);
    else if (starts(disk, "mmcblk"))    strlcpy(out, "sd", n);
    else if (strstr(link, "/usb"))      strlcpy(out, "usb", n);
    else if (starts(disk, "vd"))        strlcpy(out, "virtio", n);
    else if (starts(disk, "sd") && strstr(link, "/ata")) strlcpy(out, "sata", n);
    else if (starts(disk, "sd"))        strlcpy(out, "scsi", n);
    else if (starts(disk, "sr"))        strlcpy(out, "optical", n);
    else                                strlcpy(out, "other", n);
}

/* Removable means "a person can pull it out": the kernel's own flag,
 * USB, or the SD slot. Loop devices count too, because they are files
 * and nothing is lost that is not a file somebody chose to attach -
 * and because that is what every test of this program runs on. */
static bool blk_removable(const char *disk)
{
    char p[96], tr[16];
    snprintf(p, sizeof p, "/sys/block/%s/removable", disk);
    if (sys_u64(p) == 1)
        return true;
    blk_transport(disk, tr, sizeof tr);
    return !strcmp(tr, "usb") || !strcmp(tr, "sd") || !strcmp(tr, "loop");
}

static void blk_model(const char *disk, char *out, size_t n)
{
    char p[128], v[128], w[64];
    out[0] = '\0';
    snprintf(p, sizeof p, "/sys/block/%s/device/model", disk);
    if (sys_read(p, v, sizeof v) && v[0]) {
        snprintf(p, sizeof p, "/sys/block/%s/device/vendor", disk);
        if (sys_read(p, w, sizeof w) && w[0] && strncmp(v, w, strlen(w)) != 0 &&
            strcmp(w, "ATA") != 0)
            snprintf(out, n, "%s %s", w, v);
        else
            strlcpy(out, v, n);
        return;
    }
    snprintf(p, sizeof p, "/sys/block/%s/device/name", disk);   /* mmc */
    if (sys_read(p, v, sizeof v) && v[0]) {
        snprintf(out, n, "SD card %s", v);
        return;
    }
    if (starts(disk, "loop")) {
        snprintf(p, sizeof p, "/sys/block/%s/loop/backing_file", disk);
        if (sys_read(p, v, sizeof v) && v[0]) {
            const char *b = strrchr(v, '/');
            snprintf(out, n, "Loop: %s", b ? b + 1 : v);
            return;
        }
    }
    strlcpy(out, disk, n);
}

/* The disks worth listing: real ones and attached loop devices, not
 * RAM disks, zram, device-mapper or md, and nothing of size zero (an
 * empty card slot, a loop device with no file). */
static bool blk_listable(const char *name)
{
    if (blk_is_part(name))
        return false;
    if (starts(name, "ram") || starts(name, "zram") || starts(name, "dm-") ||
        starts(name, "md") || starts(name, "sr") || starts(name, "fd"))
        return false;
    return blk_bytes(name) > 0;
}

/* ── Mounts ─────────────────────────────────────────────────────────
 *
 * /proc/self/mountinfo, not /proc/mounts, because it gives the device
 * number: a mount of /dev/disk/by-uuid/..., of /dev/root or of a
 * device-mapper name all come back as the same major:minor. */

typedef struct {
    u32  maj, min;
    char point[256];
    char fstype[24];
} mnt_t;

static char   mountinfo[65536];
static mnt_t  mnts[512];
static int    nmnts;

static void mounts_refresh(void)
{
    nmnts = 0;
    long got = proc_read("/proc/self/mountinfo", mountinfo, sizeof mountinfo - 1);
    if (got <= 0)
        return;
    mountinfo[got] = '\0';
    for (char *line = mountinfo; line && *line && nmnts < 512; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
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
                mnt_t *m = &mnts[nmnts];
                *colon = '\0';
                m->maj = (u32)atoi(f[2]);
                m->min = (u32)atoi(colon + 1);
                size_t k = 0;
                for (char *q = f[4]; *q && k < sizeof m->point - 1; q++) {
                    if (q[0] == '\\' && q[1] >= '0' && q[1] <= '3' &&
                        q[2] >= '0' && q[2] <= '7' && q[3] >= '0' && q[3] <= '7') {
                        m->point[k++] = (char)((q[1] - '0') * 64 + (q[2] - '0') * 8 + (q[3] - '0'));
                        q += 3;
                    } else {
                        m->point[k++] = *q;
                    }
                }
                m->point[k] = '\0';
                char *fs = dash + 3;
                char *sp = strchr(fs, ' ');
                if (sp) *sp = '\0';
                strlcpy(m->fstype, fs, sizeof m->fstype);
                nmnts++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
}

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

/* The devices at the bottom of a stack: a partition is itself, dm-0 is
 * whatever is in its slaves/, recursively. */
static void bottoms(const char *name, char out[][32], int *n, int max, int depth)
{
    if (depth > 4 || *n >= max)
        return;
    char p[96];
    snprintf(p, sizeof p, "/sys/class/block/%s/slaves", name);
    long fd = lp_open(p, O_RDONLY | O_DIRECTORY, 0);
    bool had = false;
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
            had = true;
            bottoms(nm, out, n, max, depth + 1);
        }
    }
    if (had)
        return;
    for (int i = 0; i < *n; i++)
        if (strcmp(out[i], name) == 0)
            return;
    strlcpy(out[(*n)++], name, 32);
}

static const char *const VITAL[] = {
    "/", "/boot", "/boot/efi", "/efi", "/usr", "/var", "/home", "/data", NULL
};

/* Why this device (a partition, or a whole disk used without a table)
 * cannot be written to right now, or NULL. `vital` is set when the
 * reason is that the running system stands on it - the case where the
 * answer is "restart into Recovery" rather than "unmount it". */
static const char *in_use(const char *name, bool *vital, char *why, size_t whyn)
{
    if (vital) *vital = false;
    for (int v = 0; VITAL[v]; v++) {
        for (int i = 0; i < nmnts; i++) {
            if (strcmp(mnts[i].point, VITAL[v]) != 0 || mnts[i].maj == 0)
                continue;
            char dev[32];
            if (!name_of(mnts[i].maj, mnts[i].min, dev, sizeof dev))
                continue;
            char bot[16][32];
            int nb = 0;
            bottoms(dev, bot, &nb, 16, 0);
            for (int k = 0; k < nb; k++) {
                if (strcmp(bot[k], name) == 0) {
                    if (vital) *vital = true;
                    snprintf(why, whyn, "%s holds %s of the running system;"
                             " restart into Recovery to change it", name,
                             VITAL[v]);
                    return "vital";
                }
            }
        }
    }
    const char *at = mounted_at(name);
    if (at) {
        snprintf(why, whyn, "%s is mounted at %s - unmount it first", name, at);
        return "mounted";
    }
    char sw[4096];
    long got = proc_read("/proc/swaps", sw, sizeof sw - 1);
    if (got > 0) {
        sw[got] = '\0';
        for (char *line = sw; line && *line; ) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (starts(line, "/dev/")) {
                char dn[32];
                size_t k = 0;
                for (const char *q = line + 5; *q && *q != ' ' && *q != '\t' &&
                     k < sizeof dn - 1; q++)
                    dn[k++] = *q;
                dn[k] = '\0';
                char bot[16][32];
                int nb = 0;
                if (blk_exists(dn))
                    bottoms(dn, bot, &nb, 16, 0);
                for (int i = 0; i < nb; i++)
                    if (strcmp(bot[i], name) == 0) {
                        snprintf(why, whyn, "%s is swap in use", name);
                        return "swap";
                    }
            }
            line = nl ? nl + 1 : NULL;
        }
    }
    if (blk_has_holders(name)) {
        snprintf(why, whyn, "%s is in use by another device (an open"
                 " encrypted volume, RAID or LVM)", name);
        return "holder";
    }
    return NULL;
}

/* Anything on this disk in use - the disk itself or any partition. */
static bool disk_busy(const char *disk, bool *vital, char *why, size_t whyn)
{
    if (in_use(disk, vital, why, whyn))
        return true;
    char parts[128][32];
    int np = blk_parts(disk, parts, 128);
    for (int i = 0; i < np; i++)
        if (in_use(parts[i], vital, why, whyn))
            return true;
    return false;
}

/* Is any partition of this disk in use (the running system's disk)? */
static bool disk_is_system(const char *disk)
{
    char why[200];
    bool vital = false;
    char parts[128][32];
    int np = blk_parts(disk, parts, 128);
    for (int i = 0; i < np; i++)
        if (in_use(parts[i], &vital, why, sizeof why) && vital)
            return true;
    return in_use(disk, &vital, why, sizeof why) && vital;
}

/* ═══════════════════════════════════════════════════════════════════
 * Reading the disk
 * ═══════════════════════════════════════════════════════════════════ */

static bool read_at(int fd, u64 off, void *buf, size_t n)
{
    if (lp_lseek(fd, (off_t)off, SEEK_SET) < 0)
        return false;
    size_t got = 0;
    while (got < n) {
        long r = lp_read(fd, (u8 *)buf + got, n - got);
        if (r == -EINTR_)
            continue;
        if (r <= 0)
            return false;
        got += (size_t)r;
    }
    return true;
}

static bool write_at(int fd, u64 off, const void *buf, size_t n)
{
    if (lp_lseek(fd, (off_t)off, SEEK_SET) < 0)
        return false;
    size_t put = 0;
    while (put < n) {
        long w = lp_write(fd, (const u8 *)buf + put, n - put);
        if (w == -EINTR_)
            continue;
        if (w <= 0)
            return false;
        put += (size_t)w;
    }
    return true;
}

static u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 le64(const u8 *p) { return (u64)le32(p) | ((u64)le32(p + 4) << 32); }

/* ── What filesystem is on a device ─────────────────────────────────
 *
 * Read-only, a few sectors each. Besides the name, label and UUID it
 * reads how big the filesystem thinks it is and, where the superblock
 * says, how much of it is used: the resize dialog needs both before
 * anything is mounted, and "the filesystem is bigger than its
 * partition" is the check that stands between a shrink and a table
 * that cuts a filesystem short. */

typedef struct {
    char fs[16];               /* ext4 vfat exfat ntfs btrfs swap crypto_LUKS ... */
    char label[96];            /* UTF-8 */
    char uuid[40];
    char state[16];            /* ext: clean / not-clean / errors */
    u64  size;                 /* bytes the filesystem spans; 0 = unknown */
    u64  used;                 /* bytes in use; 0 = unknown */
    bool used_known;
} probe_t;

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

static size_t put_utf8(u32 c, char *out, size_t k, size_t outn)
{
    if (c < 0x80) {
        if (k + 1 < outn) out[k++] = (char)c;
    } else if (c < 0x800) {
        if (k + 2 < outn) {
            out[k++] = (char)(0xc0 | (c >> 6));
            out[k++] = (char)(0x80 | (c & 0x3f));
        }
    } else if (c < 0x10000) {
        if (k + 3 < outn) {
            out[k++] = (char)(0xe0 | (c >> 12));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3f));
            out[k++] = (char)(0x80 | (c & 0x3f));
        }
    } else if (k + 4 < outn) {
        out[k++] = (char)(0xf0 | (c >> 18));
        out[k++] = (char)(0x80 | ((c >> 12) & 0x3f));
        out[k++] = (char)(0x80 | ((c >> 6) & 0x3f));
        out[k++] = (char)(0x80 | (c & 0x3f));
    }
    return k;
}

/* UTF-16LE (exFAT and NTFS labels, GPT names) to UTF-8. */
static void utf16_to8(const u8 *src, size_t units, char *out, size_t outn)
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
        k = put_utf8(c, out, k, outn);
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = '\0';
}

static void guid_str(const u8 *p, char *out)
{
    /* The first three fields are little-endian on disk. */
    static const char h[] = "0123456789ABCDEF";
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    int k = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[k++] = '-';
        u8 b = p[order[i]];
        out[k++] = h[b >> 4];
        out[k++] = h[b & 15];
    }
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

static void exfat_label(int fd, const u8 *boot, probe_t *p)
{
    u32 heap = le32(boot + 88), root = le32(boot + 96);
    int bps = boot[108], spc = boot[109];
    if (bps < 9 || bps > 12 || spc > 25 || root < 2)
        return;
    u64 off = ((u64)heap << bps) + ((u64)(root - 2) << (spc + bps));
    static u8 dir[4096];
    if (!read_at(fd, off, dir, sizeof dir))
        return;
    for (int i = 0; i < 4096; i += 32) {
        if (dir[i] == 0x00)
            break;
        if (dir[i] == 0x83) {
            int count = dir[i + 1];
            if (count > 11) count = 11;
            utf16_to8(dir + i + 2, (size_t)count, p->label, sizeof p->label);
            return;
        }
    }
}

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
    /* The update sequence array: put back the last two bytes of every
     * sector, which were swapped for a check value on write. */
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
                utf16_to8(r + a + voff, vlen / 2, p->label, sizeof p->label);
            return;
        }
        a += len;
    }
}

static bool probe_fd(int fd, probe_t *p)
{
    memset(p, 0, sizeof *p);
    static u8 h[4096];
    memset(h, 0, sizeof h);
    if (!read_at(fd, 0, h, sizeof h))
        return false;

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
        u64 bsize = 1024ull << le32(sb + 24);
        bool b64 = (incompat & 0x80) != 0;
        u64 blocks = le32(sb + 4) | (b64 ? (u64)le32(sb + 0x150) << 32 : 0);
        u64 freeb  = le32(sb + 12) | (b64 ? (u64)le32(sb + 0x158) << 32 : 0);
        p->size = blocks * bsize;
        if (freeb <= blocks) {
            p->used = (blocks - freeb) * bsize;
            p->used_known = true;
        }
    } else if (memcmp(h + 3, "EXFAT   ", 8) == 0) {
        strlcpy(p->fs, "exfat", sizeof p->fs);
        serial_uuid(le32(h + 100), p->uuid);
        int bps = h[108];
        if (bps >= 9 && bps <= 12)
            p->size = le64(h + 72) << bps;
        /* PercentInUse: the formatter's own count, 0xFF when unknown. */
        if (h[112] <= 100 && p->size) {
            p->used = p->size / 100 * h[112];
            p->used_known = true;
        }
        exfat_label(fd, h, p);
    } else if (memcmp(h + 3, "NTFS    ", 8) == 0) {
        strlcpy(p->fs, "ntfs", sizeof p->fs);
        u64 serial = le64(h + 0x48);
        static const char hx[] = "0123456789ABCDEF";
        for (int i = 0; i < 16; i++)
            p->uuid[i] = hx[(serial >> ((15 - i) * 4)) & 15];
        p->uuid[16] = '\0';
        /* Total sectors excludes the backup boot sector at the end. */
        p->size = (le64(h + 0x28) + 1) * le16(h + 0x0b);
        ntfs_label(fd, h, p);
    } else if (h[510] == 0x55 && h[511] == 0xAA &&
               (memcmp(h + 82, "FAT32", 5) == 0 || memcmp(h + 54, "FAT1", 4) == 0)) {
        bool f32 = memcmp(h + 82, "FAT32", 5) == 0;
        strlcpy(p->fs, "vfat", sizeof p->fs);
        copy_label(h + (f32 ? 71 : 43), 11, p->label, sizeof p->label);
        if (strcmp(p->label, "NO NAME") == 0)
            p->label[0] = '\0';
        serial_uuid(le32(h + (f32 ? 67 : 39)), p->uuid);
        u32 bps = le16(h + 11);
        u32 tot = le16(h + 19) ? le16(h + 19) : le32(h + 32);
        p->size = (u64)tot * bps;
        if (f32) {
            /* FSInfo's free-cluster count is a hint the driver keeps up
             * to date on a clean unmount; 0xFFFFFFFF means "not known". */
            static u8 fsi[512];
            u32 spc = h[13];
            u32 rsv = le16(h + 14), nfat = h[16], fatsz = le32(h + 36);
            u64 clusters = spc ? (tot - rsv - nfat * fatsz) / spc : 0;
            if (read_at(fd, (u64)le16(h + 48) * bps, fsi, 512) &&
                le32(fsi) == 0x41615252 && le32(fsi + 488) != 0xffffffffu &&
                le32(fsi + 488) <= clusters) {
                p->used = (clusters - le32(fsi + 488)) * spc * bps;
                p->used_known = true;
            }
        }
    } else if (memcmp(h + 4086, "SWAPSPACE2", 10) == 0) {
        strlcpy(p->fs, "swap", sizeof p->fs);
        copy_label(h + 1024 + 28, 16, p->label, sizeof p->label);
        hex_uuid(h + 1024 + 12, p->uuid);
        p->size = ((u64)le32(h + 1024 + 4) + 1) * 4096;
    } else if (memcmp(h, "LUKS\xba\xbe", 6) == 0) {
        strlcpy(p->fs, "crypto_LUKS", sizeof p->fs);
        copy_label(h + 168, 36, p->uuid, sizeof p->uuid);
        if (le16(h + 6) == 2)
            copy_label(h + 24, 48, p->label, sizeof p->label);
    } else {
        static u8 x[4096];
        if (read_at(fd, 0x10000, x, 4096) && memcmp(x + 0x40, "_BHRfS_M", 8) == 0) {
            strlcpy(p->fs, "btrfs", sizeof p->fs);
            copy_label(x + 0x12b, 256, p->label, sizeof p->label);
            hex_uuid(x + 0x20, p->uuid);
            p->size = le64(x + 0x70);
            p->used = le64(x + 0x78);
            p->used_known = true;
        } else if (read_at(fd, 0x8001, x, 64) && memcmp(x, "CD001", 5) == 0) {
            strlcpy(p->fs, "iso9660", sizeof p->fs);
            copy_label(x + 39, 32, p->label, sizeof p->label);
        }
    }
    return true;
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
    bool ok = probe_fd((int)fd, p);
    lp_close((int)fd);
    return ok;
}

static fstype_t fs_of_probe(const probe_t *p)
{
    if (starts(p->fs, "ext"))            return FS_EXT4;
    if (!strcmp(p->fs, "vfat"))          return FS_FAT32;
    if (!strcmp(p->fs, "exfat"))         return FS_EXFAT;
    if (!strcmp(p->fs, "ntfs"))          return FS_NTFS;
    if (!strcmp(p->fs, "btrfs"))         return FS_BTRFS;
    if (!strcmp(p->fs, "swap"))          return FS_SWAP;
    if (!strcmp(p->fs, "crypto_LUKS"))   return FS_LUKS;
    return FS_NONE;
}

/* ── Partition tables ───────────────────────────────────────────────
 *
 * Read straight off the disk, both kinds. This is the source of truth
 * for every check and every "after" in a plan; sysfs only says what the
 * kernel currently believes, which is what kernel_sync() corrects. */

#define MAX_PARTS 128

typedef struct {
    int  num;                  /* the kernel's partition number */
    u64  start, size;          /* logical sectors */
    char type[40];             /* GPT GUID, upper case, or "0x83" */
    char uuid[40];             /* GPT unique GUID (upper), or "sig-nn" */
    char name[112];            /* GPT name, UTF-8 */
    u64  attrs;                /* GPT attribute bits; MBR: bit 7 = active */
    bool logical;              /* MBR logical partition (read, not edited) */
    /* Only used while a plan is played through on a model: */
    int  mfs;                  /* fstype_t of what is (or will be) on it */
    bool mfs_other;            /* something we cannot name (iso9660 ...) */
    u64  mfs_size;             /* bytes the filesystem spans, 0 = unknown */
} pent_t;

typedef struct {
    char   disk[32];
    char   kind[8];            /* gpt, mbr, none */
    char   id[40];             /* disk GUID, or the MBR signature */
    u32    lss;                /* logical sector size */
    u64    nsect;              /* disk size in logical sectors */
    u64    first, last;        /* usable sectors, inclusive */
    bool   has_extended;
    char   err[160];           /* what is wrong with the table, if anything */
    int    n;
    pent_t p[MAX_PARTS];
} table_t;

static u32 crc32_tab[256];

static u32 crc32(const u8 *p, size_t n)
{
    if (!crc32_tab[1]) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc32_tab[i] = c;
        }
    }
    u32 c = 0xffffffffu;
    for (size_t i = 0; i < n; i++)
        c = crc32_tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static bool gpt_read(int fd, table_t *t, u64 lba, bool primary)
{
    static u8 hdr[4096];
    if (!read_at(fd, lba * t->lss, hdr, t->lss) || memcmp(hdr, "EFI PART", 8) != 0)
        return false;
    u32 hsize = le32(hdr + 12);
    if (hsize < 92 || hsize > t->lss)
        return false;
    u32 want = le32(hdr + 16);
    static u8 tmp[4096];
    memcpy(tmp, hdr, hsize);
    memset(tmp + 16, 0, 4);
    if (crc32(tmp, hsize) != want) {
        snprintf(t->err, sizeof t->err, "the %s GPT header is damaged (CRC)",
                 primary ? "primary" : "backup");
        return false;
    }
    t->first = le64(hdr + 40);
    t->last = le64(hdr + 48);
    guid_str(hdr + 56, t->id);
    u64 elba = le64(hdr + 72);
    u32 count = le32(hdr + 80), esize = le32(hdr + 84);
    if (count > MAX_PARTS * 4 || esize < 128 || esize > 1024)
        return false;
    size_t bytes = (size_t)count * esize;
    u8 *ents = malloc(bytes + 1);
    if (!ents)
        return false;
    if (!read_at(fd, elba * t->lss, ents, bytes)) {
        free(ents);
        return false;
    }
    if (crc32(ents, bytes) != le32(hdr + 88)) {
        snprintf(t->err, sizeof t->err, "the %s GPT entry array is damaged (CRC)",
                 primary ? "primary" : "backup");
        free(ents);
        return false;
    }
    t->n = 0;
    for (u32 i = 0; i < count && t->n < MAX_PARTS; i++) {
        const u8 *e = ents + (size_t)i * esize;
        static const u8 zero[16];
        if (memcmp(e, zero, 16) == 0)
            continue;
        pent_t *p = &t->p[t->n++];
        memset(p, 0, sizeof *p);
        p->num = (int)i + 1;
        guid_str(e, p->type);
        guid_str(e + 16, p->uuid);
        p->start = le64(e + 32);
        p->size = le64(e + 40) - p->start + 1;
        p->attrs = le64(e + 48);
        utf16_to8(e + 56, 36, p->name, sizeof p->name);
    }
    free(ents);
    return true;
}

static void mbr_entry(pent_t *p, const u8 *e, int num, u64 base, u32 sig)
{
    memset(p, 0, sizeof *p);
    p->num = num;
    snprintf(p->type, sizeof p->type, "0x%02x", e[4]);
    p->start = base + le32(e + 8);
    p->size = le32(e + 12);
    p->attrs = e[0] & 0x80;
    snprintf(p->uuid, sizeof p->uuid, "%08x-%02x", sig, num);
}

static bool table_read(const char *disk, table_t *t)
{
    memset(t, 0, sizeof *t);
    strlcpy(t->disk, disk, sizeof t->disk);
    strlcpy(t->kind, "none", sizeof t->kind);
    t->lss = blk_lss(disk);
    t->nsect = blk_bytes(disk) / t->lss;
    char dev[64];
    if (!dev_path(disk, dev, sizeof dev))
        return false;
    long fd = lp_open(dev, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return false;

    static u8 mbr[512];
    bool have_mbr = read_at((int)fd, 0, mbr, 512) && mbr[510] == 0x55 && mbr[511] == 0xAA;
    bool protective = false;
    for (int s = 0; have_mbr && s < 4; s++)
        if (mbr[446 + s * 16 + 4] == 0xee)
            protective = true;

    if (gpt_read((int)fd, t, 1, true)) {
        strlcpy(t->kind, "gpt", sizeof t->kind);
    } else if (protective) {
        /* Primary damaged: the backup at the last sector is what sgdisk
         * and parted fall back to, and it is what we show - with the
         * damage said out loud. */
        char err[160];
        strlcpy(err, t->err, sizeof err);
        if (gpt_read((int)fd, t, t->nsect - 1, false)) {
            strlcpy(t->kind, "gpt", sizeof t->kind);
            snprintf(t->err, sizeof t->err, "%s; showing the backup copy",
                     err[0] ? err : "the primary GPT is missing");
        } else {
            strlcpy(t->kind, "gpt", sizeof t->kind);
            if (!t->err[0])
                strlcpy(t->err, "GPT is damaged beyond reading", sizeof t->err);
        }
    } else if (have_mbr) {
        /* An MBR, or a filesystem boot sector (FAT and NTFS end in 55AA
         * too). A table has sane entries and no filesystem signature. */
        bool fsboot = memcmp(mbr + 3, "NTFS", 4) == 0 || memcmp(mbr + 3, "EXFAT", 5) == 0 ||
                      memcmp(mbr + 82, "FAT32", 5) == 0 || memcmp(mbr + 54, "FAT", 3) == 0;
        bool sane = true;
        for (int s = 0; s < 4; s++) {
            u8 st = mbr[446 + s * 16];
            if (st != 0 && st != 0x80) sane = false;
        }
        if (sane && !fsboot) {
            strlcpy(t->kind, "mbr", sizeof t->kind);
            u32 sig = le32(mbr + 440);
            snprintf(t->id, sizeof t->id, "%08x", sig);
            t->first = 2048 * 512 / t->lss;
            if (t->first < 1) t->first = 1;
            t->last = t->nsect - 1;
            for (int s = 0; s < 4; s++) {
                const u8 *e = mbr + 446 + s * 16;
                if (e[4] == 0 || le32(e + 12) == 0)
                    continue;
                if (e[4] == 0x05 || e[4] == 0x0f || e[4] == 0x85) {
                    t->has_extended = true;
                    /* Walk the chain of logical partitions: each EBR
                     * describes one, and links to the next relative to
                     * the start of the extended partition. */
                    u64 ext = le32(e + 8), next = ext;
                    int num = 5;
                    static u8 ebr[512];
                    for (int guard = 0; guard < 64 && t->n < MAX_PARTS; guard++) {
                        if (!read_at((int)fd, next * t->lss, ebr, 512) ||
                            ebr[510] != 0x55 || ebr[511] != 0xAA)
                            break;
                        const u8 *l = ebr + 446;
                        if (l[4] && le32(l + 12)) {
                            mbr_entry(&t->p[t->n], l, num++, next, sig);
                            t->p[t->n++].logical = true;
                        }
                        const u8 *link = ebr + 446 + 16;
                        if (!le32(link + 8))
                            break;
                        next = ext + le32(link + 8);
                    }
                    continue;
                }
                mbr_entry(&t->p[t->n++], e, s + 1, 0, sig);
            }
        }
    }
    lp_close((int)fd);

    /* In number order: GPT entries are, MBR logicals come after. */
    for (int i = 1; i < t->n; i++)
        for (int j = i; j > 0 && t->p[j - 1].num > t->p[j].num; j--) {
            pent_t x = t->p[j]; t->p[j] = t->p[j - 1]; t->p[j - 1] = x;
        }
    return true;
}

static pent_t *table_num(table_t *t, int num)
{
    for (int i = 0; i < t->n; i++)
        if (t->p[i].num == num)
            return &t->p[i];
    return NULL;
}

static pent_t *table_uuid(table_t *t, const char *uuid)
{
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->p[i].uuid, uuid) == 0)
            return &t->p[i];
    return NULL;
}

/* The disk and table entry a PARTUUID names, searched over every disk.
 * GPT GUIDs are compared upper case, MBR ones lower, as they are read. */
static bool find_partuuid(const char *uuid_in, char *disk, size_t dn, int *num)
{
    char uuid[40];
    strlcpy(uuid, uuid_in, sizeof uuid);
    if (guid_ok(uuid)) upcase(uuid); else downcase(uuid);
    static char names[256][32];
    int n = blk_all(names, 256);
    static table_t t;
    for (int i = 0; i < n; i++) {
        if (!blk_listable(names[i]))
            continue;
        if (!table_read(names[i], &t))
            continue;
        pent_t *p = table_uuid(&t, uuid);
        if (p) {
            strlcpy(disk, names[i], dn);
            *num = p->num;
            return true;
        }
    }
    return false;
}

/* ESP and LP-RECOVERY are what the machine boots and recovers with.
 * Deleting or formatting them wants a token the person typed. */
static const char *protected_tag(const table_t *t, const pent_t *p)
{
    if (!strcmp(p->type, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B") ||
        (!strcmp(t->kind, "mbr") && !strcmp(p->type, "0xef")))
        return "ESP";
    if (!strcmp(p->name, "LP-RECOVERY"))
        return "LP-RECOVERY";
    return NULL;
}

/* The flags GParted shows, from type and attributes together. */
static void flags_str(const table_t *t, const pent_t *p, char *out, size_t n)
{
    out[0] = '\0';
    bool gpt = !strcmp(t->kind, "gpt");
    const ptype_t *ty = ptype_find(p->type);
    const char *nm = ty ? ty->name : "";
    #define ADD(s) do { if (out[0]) strlcat(out, ",", n); strlcat(out, s, n); } while (0)
    if (!strcmp(nm, "esp"))   { ADD("esp"); ADD("boot"); }
    if (!gpt && (p->attrs & 0x80) && strcmp(nm, "esp")) ADD("boot");
    if (!strcmp(nm, "msdata") || (!gpt && !strcmp(nm, "fat32"))) ADD("msftdata");
    if (!strcmp(nm, "lvm"))   ADD("lvm");
    if (!strcmp(nm, "raid"))  ADD("raid");
    if (!strcmp(nm, "swap"))  ADD("swap");
    if (!strcmp(nm, "bios"))  ADD("bios_grub");
    if (!strcmp(nm, "msres")) ADD("msftres");
    if (!strcmp(nm, "winre")) ADD("diag");
    if (gpt && (p->attrs & (1ull << 0)))  ADD("required");
    if (gpt && (p->attrs & (1ull << 2)))  ADD("legacy_boot");
    if (gpt && (p->attrs & (1ull << 60))) ADD("readonly");
    if (gpt && (p->attrs & (1ull << 62))) ADD("hidden");
    if (gpt && (p->attrs & (1ull << 63))) ADD("no_automount");
    /* MBR hides a partition by setting 0x10 in its type: 0x11, 0x14,
     * 0x16, 0x17, 0x1b, 0x1c, 0x1e are FAT and NTFS, hidden. */
    if (!gpt && p->type[2] == '1' && p->type[3] && strchr("1467bce", p->type[3]))
        ADD("hidden");
    #undef ADD
}

/* ═══════════════════════════════════════════════════════════════════
 * Telling the kernel
 * ═══════════════════════════════════════════════════════════════════ */

#define BLKRRPART_     0x125f
#define BLKFLSBUF_     0x1261
#define BLKDISCARD_    0x1277
#define BLKPG_         0x1269
#define BLKPG_ADD_     1
#define BLKPG_DEL_     2
#define BLKPG_RESIZE_  3

typedef struct {
    long long start, length;
    int  pno;
    char devname[64], volname[64];
} blkpg_part_t;

typedef struct {
    int   op, flags, datalen;
    void *data;
} blkpg_arg_t;

static long blkpg(int fd, int op, int pno, u64 start_b, u64 len_b)
{
    blkpg_part_t part;
    memset(&part, 0, sizeof part);
    part.pno = pno;
    part.start = (long long)start_b;
    part.length = (long long)len_b;
    blkpg_arg_t arg = { op, 0, (int)sizeof part, &part };
    return lp_ioctl(fd, BLKPG_, &arg);
}

/* Make the kernel's partitions of `disk` match the table on the disk.
 *
 * BLKRRPART first when nothing on the disk is in use - that is the
 * ordinary way, and it makes udev see a clean change. Then, whatever it
 * did or did not do, compare partition by partition and fix the rest
 * with BLKPG: a disk with a mounted partition refuses BLKRRPART
 * outright, and a kernel without the parser for this table type accepts
 * it and adds nothing. A partition that is in use is never touched;
 * if the table and the kernel disagree about one of those, the plan
 * checks should already have stopped us, and it is reported. */
static bool kernel_sync(const char *disk, char *why, size_t whyn)
{
    static table_t t;
    if (!table_read(disk, &t)) {
        snprintf(why, whyn, "cannot read the partition table of %s", disk);
        return false;
    }
    char dev[64];
    if (!dev_path(disk, dev, sizeof dev)) {
        snprintf(why, whyn, "%s disappeared", disk);
        return false;
    }
    long fd = lp_open(dev, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
        snprintf(why, whyn, "cannot open %s", dev);
        return false;
    }
    lp_sync();
    lp_ioctl((int)fd, BLKFLSBUF_, NULL);
    mounts_refresh();
    bool vital;
    char tmp[200];
    if (!disk_busy(disk, &vital, tmp, sizeof tmp))
        lp_ioctl((int)fd, BLKRRPART_, NULL);

    u64 f = t.lss / 512;                 /* table sectors -> sysfs units */
    bool ok = true;
    char parts[128][32];
    int np = blk_parts(disk, parts, 128);
    /* Remove or shrink what the table no longer has. */
    for (int i = 0; i < np; i++) {
        int num = blk_partno(parts[i]);
        char p[96];
        snprintf(p, sizeof p, "/sys/class/block/%s/start", parts[i]);
        u64 kstart = sys_u64(p);
        snprintf(p, sizeof p, "/sys/class/block/%s/size", parts[i]);
        u64 ksize = sys_u64(p);
        pent_t *e = table_num(&t, num);
        bool same = e && e->start * f == kstart && e->size * f == ksize;
        if (same)
            continue;
        /* An MBR extended container shows as a 1 KiB partition in the
         * kernel; the table has it as a whole range. Leave it. */
        if (e && e->start * f == kstart && ksize == 2)
            continue;
        if (in_use(parts[i], &vital, tmp, sizeof tmp)) {
            snprintf(why, whyn, "the kernel still uses the old %s (%s)", parts[i], tmp);
            ok = false;
            continue;
        }
        long r;
        if (e && e->start * f == kstart)
            r = blkpg((int)fd, BLKPG_RESIZE_, num, e->start * t.lss, e->size * t.lss);
        else
            r = blkpg((int)fd, BLKPG_DEL_, num, 0, 0);
        if (r < 0) {
            snprintf(why, whyn, "the kernel would not update %s (error %ld)",
                     parts[i], -r);
            ok = false;
        }
    }
    /* Add what the kernel does not have. */
    for (int i = 0; i < t.n; i++) {
        char pn[40];
        part_name(disk, t.p[i].num, pn, sizeof pn);
        if (blk_exists(pn))
            continue;
        long r = blkpg((int)fd, BLKPG_ADD_, t.p[i].num, t.p[i].start * t.lss,
                       t.p[i].size * t.lss);
        if (r < 0 && r != -EBUSY_) {
            snprintf(why, whyn, "the kernel would not add %s (error %ld)", pn, -r);
            ok = false;
        }
    }
    lp_close((int)fd);

    /* Check: every table entry is now a kernel partition of the right
     * size, and every one of them has its /dev node. */
    for (int i = 0; ok && i < t.n; i++) {
        char pn[40], p[96], d[64];
        part_name(disk, t.p[i].num, pn, sizeof pn);
        snprintf(p, sizeof p, "/sys/class/block/%s/size", pn);
        if (!blk_exists(pn) || sys_u64(p) != t.p[i].size * f || !dev_path(pn, d, sizeof d)) {
            snprintf(why, whyn, "the kernel's view of %s does not match the table", pn);
            ok = false;
        }
    }
    return ok;
}

/* ═══════════════════════════════════════════════════════════════════
 * Running things
 * ═══════════════════════════════════════════════════════════════════ */

/* Absolute paths only, looked up in a fixed order; PATH in the child is
 * set here too. The Debian tools are in /usr/sbin and /usr/bin; /sbin
 * and /bin are for the recovery system, which copies the few it needs
 * next to our own userland. */
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

static char *const child_env[] = {
    "PATH=/usr/sbin:/usr/bin:/sbin:/bin",
    "HOME=/root",
    "LANG=C.UTF-8",
    "LC_ALL=C.UTF-8",
    "TERM=dumb",
    NULL
};

/* What to make of the status pipe (fd 3 in the child), when there is one. */
typedef enum { STAT_NONE, STAT_E2FSCK } statkind_t;

static int pct_lo = 0, pct_hi = 100;    /* the slice of the step a tool gets */
static const char *pct_text = "";

static void scaled(int pct, const char *text)
{
    progress(pct_lo + (pct_hi - pct_lo) * pct / 100, text);
}

static void status_e2fsck(char *line)
{
    int pass = atoi(line);
    char *a = strchr(line, ' ');
    if (!a) return;
    long cur = strtol(a + 1, &a, 10);
    long max = strtol(a, NULL, 10);
    if (pass < 1 || pass > 5 || max <= 0) return;
    char msg[64];
    snprintf(msg, sizeof msg, "%s: pass %d of 5", pct_text, pass);
    scaled((int)(((pass - 1) * 100 + cur * 100 / max) / 5), msg);
}

typedef struct {
    char buf[2048];
    size_t len;
} linebuf_t;

static void feed(linebuf_t *lb, const char *data, long n, void (*fn)(char *))
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

/* Tools that print their own progress: resize2fs -p draws a bar of X,
 * ntfsresize prints "NN.NN percent completed". Both become progress. */
static int xs_seen;
static void out_line(char *s)
{
    while (*s == ' ') s++;
    if (!*s)
        return;
    char *pc = strstr(s, " percent completed");
    if (pc) {
        char *b = pc;
        while (b > s && ((b[-1] >= '0' && b[-1] <= '9') || b[-1] == '.')) b--;
        scaled(atoi(b), pct_text);
        return;
    }
    size_t xs = 0;
    for (const char *q = s; *q; q++) if (*q == 'X') xs++;
    if (xs > 10 && xs == strlen(s)) {     /* a resize2fs progress bar row */
        xs_seen += (int)xs;
        scaled(xs_seen % 100, pct_text);
        return;
    }
    say(s);
}

static volatile int cancel_req;          /* SIGUSR1 in the worker */
static void on_cancel(int sig) { (void)sig; cancel_req = 1; }

/* Run one program to the end. argv[0] is an absolute path. stdout and
 * stderr come back to the client as log lines; `input` (may be NULL)
 * is written to its stdin and the pipe closed - how a passphrase or an
 * sfdisk script goes in without ever being an argument, which every
 * user could read in /proc. `cancellable`: a cancel request kills it
 * (only for things that change nothing, like a read-only check).
 * Returns the exit status, -1 when it could not start or was killed. */
static int run_in(char *const argv[], statkind_t sk, const char *input,
                  size_t inlen, bool cancellable, bool secret)
{
    char cmd[900];
    size_t k = strlcpy(cmd, "exec", sizeof cmd);
    for (int i = 0; argv[i] && k < sizeof cmd - 2; i++) {
        k = strlcat(cmd, " ", sizeof cmd);
        k = strlcat(cmd, argv[i], sizeof cmd);
    }
    audit(cmd);
    if (input && !secret) {
        char in[300];
        snprintf(in, sizeof in, "  stdin: %.*s", (int)(inlen > 250 ? 250 : inlen), input);
        for (char *q = in; *q; q++) if (*q == '\n') *q = ' ';
        audit(in);
    }

    int out[2], st[2] = { -1, -1 }, inp[2] = { -1, -1 };
    if (lp_pipe(out) < 0)
        return -1;
    if (sk != STAT_NONE && lp_pipe(st) < 0)
        return -1;
    if (input && lp_pipe(inp) < 0)
        return -1;

    pid_t pid = lp_fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (input) {
            lp_dup2(inp[0], 0);
        } else {
            long nul = lp_open("/dev/null", O_RDONLY, 0);
            if (nul >= 0) lp_dup2((int)nul, 0);
        }
        lp_dup2(out[1], 1);
        lp_dup2(out[1], 2);
        if (st[1] >= 0)
            lp_dup2(st[1], 3);
        for (int fd = (st[1] >= 0 ? 4 : 3); fd < 1024; fd++)
            lp_close(fd);
        lp_signal_default(13);
        lp_signal_default(SIGINT);
        lp_signal_default(SIGTERM);
        lp_signal_default(SIGUSR1_);
        sys_call1(SYS_umask_, 022);
        lp_chdir("/");
        lp_setsid();
        lp_execve(argv[0], argv, child_env);
        dprintf(2, "cannot run %s\n", argv[0]);
        lp_exit(127);
    }
    lp_close(out[1]);
    if (st[1] >= 0) lp_close(st[1]);
    if (input) {
        lp_close(inp[0]);
        size_t put = 0;
        while (put < inlen) {
            long w = lp_write(inp[1], input + put, inlen - put);
            if (w == -EINTR_) continue;
            if (w <= 0) break;
            put += (size_t)w;
        }
        lp_close(inp[1]);
    }

    static linebuf_t ob, sb;
    ob.len = sb.len = 0;
    xs_seen = 0;
    bool out_open = true, st_open = st[0] >= 0, killed = false;
    while (out_open || st_open) {
        lp_pollfd_t p[2];
        unsigned np = 0;
        int oi = -1, si = -1;
        if (out_open) { p[np].fd = out[0]; p[np].events = LP_POLLIN; p[np].revents = 0; oi = (int)np++; }
        if (st_open)  { p[np].fd = st[0];  p[np].events = LP_POLLIN; p[np].revents = 0; si = (int)np++; }
        long r = lp_poll(p, np, 250);
        if (cancel_req && cancellable && !killed) {
            lp_kill(-pid, SIGTERM);
            killed = true;
        }
        if (r <= 0)
            continue;
        char buf[4096];
        if (oi >= 0 && p[oi].revents) {
            long n = lp_read(out[0], buf, sizeof buf);
            if (n == -EINTR_) continue;
            if (n <= 0) out_open = false;
            else feed(&ob, buf, n, out_line);
        }
        if (si >= 0 && p[si].revents) {
            long n = lp_read(st[0], buf, sizeof buf);
            if (n == -EINTR_) continue;
            if (n <= 0) st_open = false;
            else feed(&sb, buf, n, status_e2fsck);
        }
    }
    if (ob.len) { ob.buf[ob.len] = '\0'; out_line(ob.buf); }
    lp_close(out[0]);
    if (st[0] >= 0) lp_close(st[0]);

    int status = 0;
    while (lp_waitpid(pid, &status, 0) == -EINTR_)
        ;
    int code = (status & 0x7f) == 0 ? (status >> 8) & 0xff : -1;
    char line[64];
    snprintf(line, sizeof line, "  exit %d", code);
    audit(line);
    return code;
}

static int run(char *const argv[])
{
    return run_in(argv, STAT_NONE, NULL, 0, false, false);
}

/* A failure message, the job's result code, and the audit line. */
static int  job_rc;
static char fail_why[16];
static char fail_text[600];

static void done(const char *text)
{
    reply("done", text);
    job_rc = 0;
}

static bool failed(const char *why, const char *text)
{
    strlcpy(fail_why, why, sizeof fail_why);
    strlcpy(fail_text, text, sizeof fail_text);
    job_rc = 1;
    char line[700];
    snprintf(line, sizeof line, "  fail %s %s", why, text);
    audit(line);
    return false;
}

static void finish_fail(void)
{
    char line[700];
    snprintf(line, sizeof line, "%s %s", fail_why[0] ? fail_why : "failed",
             fail_text);
    reply("fail", line);
}

static bool need_tool(const char *name, const char *pkg, char *out, size_t n)
{
    if (tool(name, out, n))
        return true;
    char msg[200];
    snprintf(msg, sizeof msg, "%s is not installed (Debian package %s)", name, pkg);
    return failed("missing", msg);
}

/* ═══════════════════════════════════════════════════════════════════
 * The partition table, through sfdisk
 *
 * sfdisk writes both GPT copies and the protective MBR, or the MBR,
 * with the same syntax for both, from a script on stdin or one option
 * per change. --no-reread and --no-tell-kernel because kernel_sync()
 * does that part, partition by partition, after every change.
 * ═══════════════════════════════════════════════════════════════════ */

static bool sfdisk_run(const char *disk, const char *const pre[],
                       const char *const post[], const char *script)
{
    char bin[64], dev[64];
    if (!need_tool("sfdisk", "fdisk", bin, sizeof bin))
        return false;
    if (!dev_path(disk, dev, sizeof dev))
        return failed("failed", "the disk disappeared");
    char *argv[24];
    int k = 0;
    argv[k++] = bin;
    argv[k++] = "--no-reread";
    argv[k++] = "--no-tell-kernel";
    for (int i = 0; pre && pre[i] && k < 14; i++)
        argv[k++] = (char *)pre[i];
    argv[k++] = dev;
    for (int i = 0; post && post[i] && k < 22; i++)
        argv[k++] = (char *)post[i];
    argv[k] = NULL;
    int rc = run_in(argv, STAT_NONE, script, script ? strlen(script) : 0,
                    false, false);
    if (rc != 0) {
        char msg[96];
        snprintf(msg, sizeof msg, "sfdisk exited with %d", rc);
        return failed("failed", msg);
    }
    lp_sync();
    return true;
}

static void numstr(int n, char *out) { snprintf(out, 8, "%d", n); }

/* The type as sfdisk wants it: a GUID for GPT, two hex digits for MBR. */
static bool type_code(const table_t *t, const ptype_t *ty, char *out, size_t n)
{
    if (!strcmp(t->kind, "gpt")) {
        strlcpy(out, ty->gpt, n);
        return true;
    }
    if (!ty->mbr)
        return false;
    snprintf(out, n, "%02x", ty->mbr);
    return true;
}

static void attrs_str(u64 a, char *out, size_t n)
{
    out[0] = '\0';
    if (a & 1) strlcat(out, "RequiredPartition,", n);
    if (a & 2) strlcat(out, "NoBlockIOProtocol,", n);
    if (a & 4) strlcat(out, "LegacyBIOSBootable,", n);
    for (int b = 48; b < 64; b++) {
        if (a & (1ull << b)) {
            char g[16];
            snprintf(g, sizeof g, "GUID:%d,", b);
            strlcat(out, g, n);
        }
    }
    size_t l = strlen(out);
    if (l) out[l - 1] = '\0';
}

static bool tbl_new(const char *disk, bool gpt)
{
    static const char *const pre[] = { "--wipe", "always", NULL };
    return sfdisk_run(disk, pre, NULL, gpt ? "label: gpt\n" : "label: dos\n");
}

static bool tbl_add(const char *disk, const table_t *t, int num, u64 start,
                    u64 size, const char *code, const char *uuid,
                    const char *name)
{
    char nb[8], script[400];
    numstr(num, nb);
    const char *pre[] = { "--append", "--wipe-partitions", "always", "-N", nb, NULL };
    if (!strcmp(t->kind, "gpt"))
        snprintf(script, sizeof script,
                 "start=%llu, size=%llu, type=%s, uuid=%s%s%s%s\n",
                 (unsigned long long)start, (unsigned long long)size, code, uuid,
                 name[0] ? ", name=\"" : "", name, name[0] ? "\"" : "");
    else
        snprintf(script, sizeof script, "start=%llu, size=%llu, type=%s\n",
                 (unsigned long long)start, (unsigned long long)size, code);
    return sfdisk_run(disk, pre, NULL, script);
}

static bool tbl_del(const char *disk, int num)
{
    char nb[8];
    numstr(num, nb);
    const char *pre[] = { "--delete", NULL };
    const char *post[] = { nb, NULL };
    return sfdisk_run(disk, pre, post, NULL);
}

static bool tbl_geom(const char *disk, int num, u64 start, u64 size)
{
    char nb[8], script[128];
    numstr(num, nb);
    const char *pre[] = { "--wipe-partitions", "never", "-N", nb, NULL };
    snprintf(script, sizeof script, "start=%llu, size=%llu\n",
             (unsigned long long)start, (unsigned long long)size);
    return sfdisk_run(disk, pre, NULL, script);
}

static bool tbl_set(const char *disk, const char *opt, int num, const char *value)
{
    char nb[8];
    numstr(num, nb);
    const char *pre[] = { opt, NULL };
    const char *post[] = { nb, value, NULL };
    return sfdisk_run(disk, pre, post, NULL);
}

/* ═══════════════════════════════════════════════════════════════════
 * Filesystems
 * ═══════════════════════════════════════════════════════════════════ */

static u32 caller_uid, caller_gid;

/* Push what the page cache holds for a device to the disk, and drop
 * it. The whole-disk device and each partition device have caches of
 * their own; a move reads through the disk device what may have been
 * written through the partition one. */
static void flush_dev(const char *name)
{
    char dev[64];
    if (!dev_path(name, dev, sizeof dev))
        return;
    long fd = lp_open(dev, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return;
    lp_fsync((int)fd);
    lp_ioctl((int)fd, BLKFLSBUF_, NULL);
    lp_close((int)fd);
}

static bool wipe_sigs(const char *dev)
{
    char bin[64];
    if (!tool("wipefs", bin, sizeof bin))
        return true;                 /* mkfs overwrites the main ones */
    char *argv[] = { bin, "-a", (char *)dev, NULL };
    return run(argv) == 0;
}

/* Make a filesystem on `name`. The passphrase, for LUKS, only ever goes
 * to cryptsetup's stdin. */
static bool mkfs_on(const char *name, fstype_t f, const char *label,
                    const char *pass, size_t passlen)
{
    char dev[64];
    if (!dev_path(name, dev, sizeof dev))
        return failed("failed", "the device node did not appear");
    progress_last = -1;
    if (!wipe_sigs(dev))
        return failed("failed", "could not clear the old signatures");

    char bin[64], owner[48], up[16];
    char *argv[20];
    int k = 0;
    bool whole = !blk_is_part(name);
    /* The top directory of a new ext4 belongs to whoever asked, so a
     * data partition they just made is one they can write to. */
    snprintf(owner, sizeof owner, "root_owner=%u:%u", caller_uid, caller_gid);
    switch (f) {
    case FS_EXT4:
        if (!need_tool("mkfs.ext4", "e2fsprogs", bin, sizeof bin)) return false;
        argv[k++] = bin; argv[k++] = "-F"; argv[k++] = "-q";
        if (caller_uid) { argv[k++] = "-E"; argv[k++] = owner; }
        if (label[0]) { argv[k++] = "-L"; argv[k++] = (char *)label; }
        break;
    case FS_BTRFS:
        if (!need_tool("mkfs.btrfs", "btrfs-progs", bin, sizeof bin)) return false;
        argv[k++] = bin; argv[k++] = "-f";
        if (label[0]) { argv[k++] = "-L"; argv[k++] = (char *)label; }
        break;
    case FS_FAT32: {
        if (!need_tool("mkfs.fat", "dosfstools", bin, sizeof bin)) return false;
        argv[k++] = bin; argv[k++] = "-F"; argv[k++] = "32";
        if (whole) argv[k++] = "-I";
        size_t i;
        for (i = 0; label[i] && i < sizeof up - 1; i++)
            up[i] = (label[i] >= 'a' && label[i] <= 'z') ? (char)(label[i] - 32) : label[i];
        up[i] = '\0';
        if (up[0]) { argv[k++] = "-n"; argv[k++] = up; }
        break;
    }
    case FS_EXFAT:
        if (!need_tool("mkfs.exfat", "exfatprogs", bin, sizeof bin)) return false;
        argv[k++] = bin;
        if (label[0]) { argv[k++] = "-L"; argv[k++] = (char *)label; }
        break;
    case FS_NTFS:
        if (!need_tool("mkfs.ntfs", "ntfs-3g", bin, sizeof bin)) return false;
        /* -Q: quick. A full format zeroes the device first, which on a
         * 1 TB disk is hours of a window saying "formatting". */
        argv[k++] = bin; argv[k++] = "-Q"; argv[k++] = "-F";
        if (label[0]) { argv[k++] = "-L"; argv[k++] = (char *)label; }
        break;
    case FS_SWAP:
        if (!need_tool("mkswap", "util-linux", bin, sizeof bin)) return false;
        argv[k++] = bin;
        if (label[0]) { argv[k++] = "-L"; argv[k++] = (char *)label; }
        break;
    case FS_LUKS: {
        char cs[64], mk[64];
        if (!need_tool("cryptsetup", "cryptsetup", cs, sizeof cs)) return false;
        if (!need_tool("mkfs.ext4", "e2fsprogs", mk, sizeof mk)) return false;
        if (!pass || passlen == 0)
            return failed("invalid", "an encrypted partition needs a passphrase");
        progress(20, "encrypting (LUKS2)");
        char *fa[12];
        int j = 0;
        fa[j++] = cs; fa[j++] = "luksFormat"; fa[j++] = "--type"; fa[j++] = "luks2";
        fa[j++] = "--batch-mode"; fa[j++] = "--key-file=-";
        if (label[0]) { fa[j++] = "--label"; fa[j++] = (char *)label; }
        fa[j++] = dev; fa[j] = NULL;
        if (run_in(fa, STAT_NONE, pass, passlen, false, true) != 0)
            return failed("failed", "cryptsetup could not format the partition");
        char map[48], mdev[80];
        snprintf(map, sizeof map, "lp-%s", name);
        snprintf(mdev, sizeof mdev, "/dev/mapper/%s", map);
        progress(55, "opening the encrypted volume");
        char *oa[] = { cs, "open", "--key-file=-", dev, map, NULL };
        if (run_in(oa, STAT_NONE, pass, passlen, false, true) != 0)
            return failed("failed", "the new encrypted volume would not open");
        progress(70, "making ext4 inside it");
        char *ma[10];
        j = 0;
        ma[j++] = mk; ma[j++] = "-F"; ma[j++] = "-q";
        if (caller_uid) { ma[j++] = "-E"; ma[j++] = owner; }
        if (label[0]) { ma[j++] = "-L"; ma[j++] = (char *)label; }
        ma[j++] = mdev; ma[j] = NULL;
        int rc = run(ma);
        char *ca[] = { cs, "close", map, NULL };
        run(ca);
        if (rc != 0)
            return failed("failed", "mkfs.ext4 inside the encrypted volume failed;"
                          " the partition is LUKS2 with no filesystem inside");
        lp_sync();
        return true;
    }
    default:
        return true;                  /* "none": wiped, left empty */
    }
    argv[k++] = dev;
    argv[k] = NULL;
    progress(30, argv[0]);
    int rc = run(argv);
    if (rc != 0) {
        char msg[96];
        snprintf(msg, sizeof msg, "%s exited with %d", argv[0], rc);
        return failed("failed", msg);
    }
    lp_sync();
    return true;
}

static bool set_fslabel(const char *name, const char *label)
{
    char dev[64], bin[64];
    if (!dev_path(name, dev, sizeof dev))
        return failed("failed", "the device is gone");
    probe_t p;
    probe_dev(name, &p);
    fstype_t f = fs_of_probe(&p);
    char *argv[8];
    int k = 0;
    char up[16];
    switch (f) {
    case FS_EXT4:
        if (!need_tool("e2label", "e2fsprogs", bin, sizeof bin)) return false;
        argv[k++] = bin; argv[k++] = dev; argv[k++] = (char *)label;
        break;
    case FS_FAT32: {
        if (!need_tool("fatlabel", "dosfstools", bin, sizeof bin)) return false;
        size_t i;
        for (i = 0; label[i] && i < sizeof up - 1; i++)
            up[i] = (label[i] >= 'a' && label[i] <= 'z') ? (char)(label[i] - 32) : label[i];
        up[i] = '\0';
        argv[k++] = bin; argv[k++] = dev;
        if (up[0]) argv[k++] = up; else argv[k++] = "-r";
        break;
    }
    case FS_EXFAT:
        if (!need_tool("exfatlabel", "exfatprogs", bin, sizeof bin)) return false;
        argv[k++] = bin; argv[k++] = dev; argv[k++] = (char *)label;
        break;
    case FS_NTFS:
        if (!need_tool("ntfslabel", "ntfs-3g", bin, sizeof bin)) return false;
        argv[k++] = bin; argv[k++] = "--force"; argv[k++] = dev; argv[k++] = (char *)label;
        break;
    case FS_BTRFS:
        if (!need_tool("btrfs", "btrfs-progs", bin, sizeof bin)) return false;
        argv[k++] = bin; argv[k++] = "filesystem"; argv[k++] = "label";
        argv[k++] = dev; argv[k++] = (char *)label;
        break;
    case FS_SWAP:
        if (!need_tool("swaplabel", "util-linux", bin, sizeof bin)) return false;
        argv[k++] = bin; argv[k++] = "-L"; argv[k++] = (char *)label; argv[k++] = dev;
        break;
    case FS_LUKS:
        if (!need_tool("cryptsetup", "cryptsetup", bin, sizeof bin)) return false;
        argv[k++] = bin; argv[k++] = "config"; argv[k++] = "--label";
        argv[k++] = (char *)label; argv[k++] = dev;
        break;
    default:
        return failed("refused", "there is no filesystem here to label");
    }
    argv[k] = NULL;
    if (run(argv) != 0)
        return failed("failed", "the label could not be set");
    lp_sync();
    return true;
}

/* Check (never writes) or repair. Returns 0 clean or repaired, 1 errors
 * found by a check, -1 failed (reason set). */
static int fsck_on(const char *name, bool repair)
{
    char dev[64], bin[64], fd3[4] = "3";
    if (!dev_path(name, dev, sizeof dev))
        return failed("failed", "the device is gone"), -1;
    probe_t p;
    probe_dev(name, &p);
    char *argv[10];
    int k = 0;
    statkind_t sk = STAT_NONE;
    if (starts(p.fs, "ext")) {
        if (!need_tool("e2fsck", "e2fsprogs", bin, sizeof bin)) return -1;
        argv[k++] = bin; argv[k++] = "-f"; argv[k++] = repair ? "-y" : "-n";
        argv[k++] = "-C"; argv[k++] = fd3;
        sk = STAT_E2FSCK;
    } else if (!strcmp(p.fs, "vfat")) {
        if (!need_tool("fsck.fat", "dosfstools", bin, sizeof bin)) return -1;
        argv[k++] = bin; argv[k++] = repair ? "-a" : "-n";
        if (repair) argv[k++] = "-w";
    } else if (!strcmp(p.fs, "exfat")) {
        if (!need_tool("fsck.exfat", "exfatprogs", bin, sizeof bin)) return -1;
        argv[k++] = bin; argv[k++] = repair ? "-y" : "-n";
    } else if (!strcmp(p.fs, "ntfs")) {
        if (!need_tool("ntfsfix", "ntfs-3g", bin, sizeof bin)) return -1;
        argv[k++] = bin;
        argv[k++] = repair ? "-d" : "-n";
    } else if (!strcmp(p.fs, "btrfs")) {
        if (!need_tool("btrfs", "btrfs-progs", bin, sizeof bin)) return -1;
        if (repair)
            return failed("refused", "btrfs check --repair can make things worse;"
                          " back up first and repair from a terminal"), -1;
        argv[k++] = bin; argv[k++] = "check"; argv[k++] = "--readonly";
    } else {
        return failed("refused", p.fs[0] ? "there is no checker for this filesystem"
                                         : "no filesystem found to check"), -1;
    }
    argv[k++] = dev;
    argv[k] = NULL;
    pct_text = repair ? "repairing" : "checking";
    int rc = run_in(argv, sk, NULL, 0, !repair, false);
    if (cancel_req && !repair)
        return failed("cancelled", "the check was stopped; nothing was changed"), -1;
    /* e2fsck: 0 clean, 1 fixed, 2 fixed+reboot, 4 left uncorrected;
     * fsck.fat/exfat: 1 = errors found (with -n) or corrected. */
    if (rc == 0)
        return 0;
    if (repair && (rc == 1 || rc == 2))
        return 0;
    if (!repair && rc > 0 && rc < 8)
        return 1;
    char msg[96];
    snprintf(msg, sizeof msg, "%s exited with %d", argv[0], rc);
    return failed("failed", msg), -1;
}

/* The smallest this filesystem can be shrunk to, in bytes, or 0 when
 * the tool that knows cannot say. resize2fs -P and ntfsresize --info
 * both read the filesystem without touching it. */
static u64 min_found;
static void min_line(char *s)
{
    const char *k1 = "Estimated minimum size of the filesystem: ";
    const char *k2 = "You might resize at ";
    char *p;
    if ((p = strstr(s, k1)) != NULL)
        min_found = (u64)strtoll(p + strlen(k1), NULL, 10);   /* blocks */
    else if ((p = strstr(s, k2)) != NULL)
        min_found = (u64)strtoll(p + strlen(k2), NULL, 10);   /* bytes */
}

static u64 capture_run(char *const argv[], void (*fn)(char *))
{
    int out[2];
    if (lp_pipe(out) < 0)
        return 0;
    pid_t pid = lp_fork();
    if (pid == 0) {
        long nul = lp_open("/dev/null", O_RDWR, 0);
        if (nul >= 0) lp_dup2((int)nul, 0);
        lp_dup2(out[1], 1);
        lp_dup2(out[1], 2);
        for (int fd = 3; fd < 1024; fd++) lp_close(fd);
        lp_execve(argv[0], argv, child_env);
        lp_exit(127);
    }
    lp_close(out[1]);
    static linebuf_t lb;
    lb.len = 0;
    char buf[2048];
    long r;
    while ((r = lp_read(out[0], buf, sizeof buf)) != 0) {
        if (r == -EINTR_) continue;
        if (r < 0) break;
        feed(&lb, buf, r, fn);
    }
    if (lb.len) { lb.buf[lb.len] = '\0'; fn(lb.buf); }
    lp_close(out[0]);
    int st;
    while (lp_waitpid(pid, &st, 0) == -EINTR_)
        ;
    return (st & 0x7f) == 0 ? (u64)((st >> 8) & 0xff) : 255;
}

static u64 fs_min_size(const char *name, const probe_t *p)
{
    char dev[64], bin[64];
    if (!dev_path(name, dev, sizeof dev))
        return 0;
    min_found = 0;
    fstype_t f = fs_of_probe(p);
    if (f == FS_EXT4 && tool("resize2fs", bin, sizeof bin)) {
        char *argv[] = { bin, "-P", dev, NULL };
        capture_run(argv, min_line);
        u64 bsize = p->used_known && p->size ? 0 : 0;
        (void)bsize;
        /* -P answers in filesystem blocks; the block size is in the
         * superblock the probe already read. */
        char d2[64];
        dev_path(name, d2, sizeof d2);
        long fd = lp_open(d2, O_RDONLY | O_CLOEXEC, 0);
        u8 sb[1024];
        u64 bs = 4096;
        if (fd >= 0) {
            if (read_at((int)fd, 1024, sb, sizeof sb))
                bs = 1024ull << le32(sb + 24);
            lp_close((int)fd);
        }
        return min_found * bs;
    }
    if (f == FS_NTFS && tool("ntfsresize", bin, sizeof bin)) {
        char *argv[] = { bin, "--info", "--force", "--no-progress-bar", dev, NULL };
        capture_run(argv, min_line);
        return min_found;
    }
    if (p->used_known) {
        u64 min = p->used + p->used / 10 + 16 * MIB;   /* a margin: metadata */
        if (f == FS_FAT32) {
            /* FAT32 with fewer than 65525 clusters is not FAT32 any more,
             * and fatresize refuses it: that is the floor, whatever is
             * used. */
            char d[64];
            u8 bs[512];
            long fd = dev_path(name, d, sizeof d) ? lp_open(d, O_RDONLY | O_CLOEXEC, 0) : -1;
            if (fd >= 0) {
                if (read_at((int)fd, 0, bs, sizeof bs)) {
                    u64 bps = le16(bs + 11), spc = bs[13];
                    u64 meta = ((u64)le16(bs + 14) + (u64)bs[16] * le32(bs + 36)) * bps;
                    u64 floor = 65525ull * spc * bps + meta;
                    if (min < floor) min = floor;
                }
                lp_close((int)fd);
            }
        }
        return min;
    }
    return 0;
}

/* Can this filesystem change size in place, and in which direction? */
static const char *resize_refusal(fstype_t f, bool shrink)
{
    switch (f) {
    case FS_EXT4:
    case FS_NTFS:
    case FS_SWAP:
    case FS_NONE:
        return NULL;
    case FS_FAT32:
        return NULL;                  /* with fatresize, if installed */
    case FS_EXFAT:
        return "exFAT cannot be resized in place by any Linux tool: copy the"
               " files somewhere else, delete it, make a new one of the size"
               " you want, and copy them back";
    case FS_BTRFS:
        return shrink ? "btrfs is resized while mounted (btrfs filesystem resize);"
                        " this tool only works on unmounted filesystems"
                      : "btrfs is grown while mounted (btrfs filesystem resize max);"
                        " grow the partition here, then run that";
    case FS_LUKS:
        return "an encrypted partition would have to be unlocked to resize what is"
               " inside it; that is not done here";
    }
    return "unknown filesystem";
}

/* Shrink (before the table) or grow (after it) the filesystem itself.
 * `bytes` is the new partition size. */
static bool fs_resize(const char *name, fstype_t f, u64 bytes, bool shrink)
{
    char dev[64], bin[64];
    if (!dev_path(name, dev, sizeof dev))
        return failed("failed", "the device is gone");
    switch (f) {
    case FS_EXT4: {
        char e2[64];
        if (!need_tool("e2fsck", "e2fsprogs", e2, sizeof e2)) return false;
        if (!need_tool("resize2fs", "e2fsprogs", bin, sizeof bin)) return false;
        /* resize2fs refuses a filesystem that was not checked since it
         * was last mounted, and it is right to: shrinking moves blocks,
         * and moving them on top of an error spreads it. */
        char fd3[4] = "3";
        char *ck[] = { e2, "-f", "-y", "-C", fd3, dev, NULL };
        int save_hi = pct_hi;
        pct_hi = pct_lo + (pct_hi - pct_lo) / 3;
        pct_text = "checking before resizing";
        int rc = run_in(ck, STAT_E2FSCK, NULL, 0, false, false);
        pct_lo = pct_hi; pct_hi = save_hi;
        if (rc != 0 && rc != 1 && rc != 2)
            return failed("failed", "the filesystem has errors e2fsck could not"
                          " repair; nothing was resized");
        char sz[32];
        snprintf(sz, sizeof sz, "%lluK", (unsigned long long)(bytes / 1024));
        pct_text = shrink ? "shrinking the filesystem" : "growing the filesystem";
        char *rs_shrink[] = { bin, "-p", dev, sz, NULL };
        char *rs_grow[]   = { bin, "-p", dev, NULL };
        rc = run(shrink ? rs_shrink : rs_grow);
        if (rc != 0)
            return failed("failed", "resize2fs failed");
        break;
    }
    case FS_NTFS: {
        if (!need_tool("ntfsresize", "ntfs-3g", bin, sizeof bin)) return false;
        char sz[32];
        snprintf(sz, sizeof sz, "%llu", (unsigned long long)bytes);
        /* A dry run first: ntfsresize finds every reason to refuse
         * (hibernated Windows, too much data) without touching a byte. */
        char *dry_s[] = { bin, "--no-action", "--force", "--no-progress-bar", "--size", sz, dev, NULL };
        char *dry_g[] = { bin, "--no-action", "--force", "--no-progress-bar", dev, NULL };
        if (run(shrink ? dry_s : dry_g) != 0)
            return failed("failed", "ntfsresize's trial run refused; nothing was"
                          " changed (if Windows is hibernated or used Fast Startup,"
                          " shut it down fully first)");
        pct_text = shrink ? "shrinking NTFS" : "growing NTFS";
        char *real_s[] = { bin, "--force", "--size", sz, dev, NULL };
        char *real_g[] = { bin, "--force", dev, NULL };
        /* It asks "Are you sure you want to proceed (y/[n])?". */
        if (run_in(shrink ? real_s : real_g, STAT_NONE, "y\n", 2, false, false) != 0)
            return failed("failed", "ntfsresize failed");
        break;
    }
    case FS_FAT32:
        /* fatresize resizes the filesystem AND the partition entry
         * (it goes through libparted), so it is called in place of
         * both halves; resize_part() knows. */
        return true;
    case FS_SWAP: {
        if (shrink)
            return true;              /* remade after the table, below */
        probe_t p;
        probe_dev(name, &p);
        if (!need_tool("mkswap", "util-linux", bin, sizeof bin)) return false;
        char *argv[8];
        int k = 0;
        argv[k++] = bin;
        if (p.uuid[0]) { argv[k++] = "-U"; argv[k++] = p.uuid; }
        if (p.label[0]) { argv[k++] = "-L"; argv[k++] = p.label; }
        argv[k++] = dev; argv[k] = NULL;
        if (run(argv) != 0)
            return failed("failed", "mkswap failed");
        break;
    }
    default:
        break;
    }
    lp_sync();
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * Moving a partition's data
 *
 * See the top of the file for why the chunk size and the journal
 * interval are what they are. Everything is in bytes here; the table
 * speaks sectors and the caller converts.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    char disk[32], diskid[40], partuuid[40];
    int  num;
    u64  from, to, len;          /* bytes, from the start of the disk */
    u64  done;                   /* bytes copied, in copy order */
    int  phase;                  /* 0 copying, 1 copied - table next */
} journal_t;

static bool journal_write(const journal_t *j)
{
    char buf[512];
    int n = snprintf(buf, sizeof buf,
                     "disk=%s\ndiskid=%s\npartuuid=%s\nnum=%d\nfrom=%llu\n"
                     "to=%llu\nlen=%llu\ndone=%llu\nphase=%d\n",
                     j->disk, j->diskid, j->partuuid, j->num,
                     (unsigned long long)j->from, (unsigned long long)j->to,
                     (unsigned long long)j->len, (unsigned long long)j->done,
                     j->phase);
    lp_mkdir("/var/lib", 0755);
    lp_mkdir(STATE_DIR, 0700);
    return n > 0 && lp_write_file_atomic(JOURNAL_PATH, buf, (size_t)n);
}

static bool journal_read(journal_t *j)
{
    memset(j, 0, sizeof *j);
    static char buf[1024];
    long got = proc_read(JOURNAL_PATH, buf, sizeof buf - 1);
    if (got <= 0)
        return false;
    buf[got] = '\0';
    int seen = 0;
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            const char *v = eq + 1;
            u64 x = 0;
            if (!strcmp(line, "disk") && dev_name_ok(v))      { strlcpy(j->disk, v, sizeof j->disk); seen |= 1; }
            else if (!strcmp(line, "diskid"))                { strlcpy(j->diskid, v, sizeof j->diskid); seen |= 2; }
            else if (!strcmp(line, "partuuid"))              { strlcpy(j->partuuid, v, sizeof j->partuuid); seen |= 4; }
            else if (!strcmp(line, "num"))                   { j->num = atoi(v); seen |= 8; }
            else if (!strcmp(line, "from") && parse_u64(v, &x)) { j->from = x; seen |= 16; }
            else if (!strcmp(line, "to") && parse_u64(v, &x))   { j->to = x; seen |= 32; }
            else if (!strcmp(line, "len") && parse_u64(v, &x))  { j->len = x; seen |= 64; }
            else if (!strcmp(line, "done") && parse_u64(v, &x)) { j->done = x; seen |= 128; }
            else if (!strcmp(line, "phase"))                 { j->phase = atoi(v); seen |= 256; }
        }
        line = nl ? nl + 1 : NULL;
    }
    return seen == 511;
}

/* Copy j->len bytes from j->from to j->to on the whole-disk device,
 * starting at j->done. Returns true when every byte is across. */
static bool move_copy(journal_t *j, bool *stopped_clean)
{
    *stopped_clean = false;
    char dev[64];
    if (!dev_path(j->disk, dev, sizeof dev))
        return failed("failed", "the disk disappeared");
    long fd = lp_open(dev, O_RDWR | O_CLOEXEC, 0);
    if (fd < 0)
        return failed("failed", "cannot open the disk for writing");

    bool right = j->to > j->from;
    u64 shift = right ? j->to - j->from : j->from - j->to;
    bool overlap = shift < j->len;
    u64 chunk = 4 * MIB;
    u64 every;                       /* chunks between journal entries */
    if (overlap) {
        if (chunk > shift / 2) chunk = shift / 2;
        chunk &= ~4095ull;
        if (chunk < 4096) chunk = 4096;
        every = shift / chunk - 1;
        if (every < 1) every = 1;
        if (every > 64) every = 64;
    } else {
        every = 64;
    }
    u8 *buf = malloc((size_t)chunk);
    if (!buf) {
        lp_close((int)fd);
        return failed("failed", "out of memory");
    }

    char msg[160], a[32], b[32];
    human(j->len, a, sizeof a);
    human(shift, b, sizeof b);
    snprintf(msg, sizeof msg, "moving %s of data %s by %s%s", a,
             right ? "right" : "left", b,
             overlap ? " (overlapping: this step cannot be stopped)" : "");
    say(msg);

    u64 since = 0;
    s64 last = 0;
    bool ok = true;
    while (j->done < j->len) {
        if (cancel_req && !overlap) {
            *stopped_clean = true;
            ok = false;
            break;
        }
        u64 c = j->len - j->done < chunk ? j->len - j->done : chunk;
        u64 off = right ? j->len - j->done - c : j->done;
        if (!read_at((int)fd, j->from + off, buf, (size_t)c) ||
            !write_at((int)fd, j->to + off, buf, (size_t)c)) {
            ok = false;
            failed("failed", "a read or write error stopped the move");
            break;
        }
        j->done += c;
        if (++since >= every || j->done == j->len) {
            /* The data first, then the claim that it is there. */
            if (lp_fsync((int)fd) < 0 || !journal_write(j)) {
                ok = false;
                failed("failed", "could not record the move's progress");
                break;
            }
            since = 0;
        }
        s64 now = lp_monotonic_ms();
        if (now - last > 250) {
            last = now;
            scaled((int)(j->done * 100 / j->len), "moving data");
        }
    }
    lp_fsync((int)fd);
    lp_ioctl((int)fd, BLKFLSBUF_, NULL);
    lp_close((int)fd);
    free(buf);
    return ok;
}

/* ═══════════════════════════════════════════════════════════════════
 * Plans
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum { S_MKLABEL, S_CREATE, S_DELETE, S_RESIZE, S_MOVE, S_FORMAT,
               S_LABEL, S_NAME, S_TYPE, S_FLAGS, S_CHECK, S_REPAIR,
               S_WIPE } skind_t;

static const struct { const char *verb; skind_t k; int min, max; } SVERBS[] = {
    { "mklabel", S_MKLABEL, 2, 3 },
    { "create",  S_CREATE,  6, 8 },
    { "delete",  S_DELETE,  1, 2 },
    { "resize",  S_RESIZE,  2, 2 },
    { "move",    S_MOVE,    2, 2 },
    { "format",  S_FORMAT,  3, 5 },
    { "label",   S_LABEL,   2, 2 },
    { "name",    S_NAME,    2, 2 },
    { "type",    S_TYPE,    2, 2 },
    { "flags",   S_FLAGS,   2, 2 },
    { "check",   S_CHECK,   1, 1 },
    { "repair",  S_REPAIR,  1, 1 },
    { "wipe",    S_WIPE,    1, 2 },
    { NULL, S_CHECK, 0, 0 }
};

typedef struct {
    skind_t  kind;
    char     ref[40];            /* as it arrived */
    int      at;                 /* @N: the step that created the target */
    char     disk[32];           /* resolved */
    int      num;                /* table entry; 0 = the whole disk */
    char     uuid[40];           /* PARTUUID of the target (or the new one) */
    u64      a, b;               /* bytes: start and size, new size, new start */
    fstype_t fs;
    char     label[112];
    char     ptype[16];
    char     flags[200];
    bool     gpt;                /* mklabel: which kind */
    char     pass[260];
    size_t   passlen;
    char     confirm[40];
    char     desc[360];
    /* How far execution got, for the state report. */
    int      phase;
} step_t;

static step_t steps[MAX_STEPS];
static int    nsteps;
/* How a plan that does not play through is answered: "invalid" for a
 * malformed step, "refused" for one that is well formed but not allowed
 * (the target is in use, or protected without the typed confirmation). */
static const char *plan_code = "invalid";

/* ── The model ──────────────────────────────────────────────────── */

typedef struct {
    bool    used;
    table_t t;
} model_t;

static model_t models[6];

static table_t *model_of(const char *disk)
{
    for (int i = 0; i < 6; i++)
        if (models[i].used && !strcmp(models[i].t.disk, disk))
            return &models[i].t;
    for (int i = 0; i < 6; i++) {
        if (models[i].used)
            continue;
        table_t *t = &models[i].t;
        if (!table_read(disk, t))
            return NULL;
        models[i].used = true;
        for (int k = 0; k < t->n; k++) {
            char kn[40];
            probe_t pr;
            part_name(disk, t->p[k].num, kn, sizeof kn);
            if (probe_dev(kn, &pr)) {
                t->p[k].mfs = (int)fs_of_probe(&pr);
                t->p[k].mfs_other = pr.fs[0] && fs_of_probe(&pr) == FS_NONE;
                t->p[k].mfs_size = pr.size;
            }
        }
        return t;
    }
    return NULL;
}

static u64 align_up(u64 v, u64 a) { return (v + a - 1) / a * a; }

/* Is [start, start+size) inside the usable area and clear of every
 * entry but `skip`? Sectors. */
static bool region_free(const table_t *t, u64 start, u64 size, int skip,
                        char *why, size_t whyn)
{
    if (start < t->first || start + size - 1 > t->last || size == 0) {
        snprintf(why, whyn, "that would reach outside the usable part of the disk");
        return false;
    }
    for (int i = 0; i < t->n; i++) {
        const pent_t *p = &t->p[i];
        if (p->num == skip)
            continue;
        if (start < p->start + p->size && p->start < start + size) {
            char kn[40];
            part_name(t->disk, p->num, kn, sizeof kn);
            snprintf(why, whyn, "that overlaps %s", kn);
            return false;
        }
    }
    return true;
}

static void fs_desc(const pent_t *p, char *out, size_t n)
{
    if (p->mfs_other)
        strlcpy(out, "unknown filesystem", n);
    else if (p->mfs == FS_NONE)
        strlcpy(out, "no filesystem", n);
    else
        strlcpy(out, fs_word((fstype_t)p->mfs), n);
}

/* Resolve a step's target on the model. */
static bool resolve(step_t *s, int idx, bool allow_disk, char *why, size_t whyn)
{
    const char *r = s->ref;
    if (r[0] == '@') {
        u64 k;
        if (!parse_u64(r + 1, &k) || k < 1 || (int)k > idx ||
            steps[k - 1].kind != S_CREATE) {
            snprintf(why, whyn, "%s does not name an earlier create step", r);
            return false;
        }
        s->at = (int)k;
        strlcpy(s->disk, steps[k - 1].disk, sizeof s->disk);
        s->num = steps[k - 1].num;
        strlcpy(s->uuid, steps[k - 1].uuid, sizeof s->uuid);
        return true;
    }
    if (guid_ok(r) || mbr_partuuid_ok(r)) {
        strlcpy(s->uuid, r, sizeof s->uuid);
        if (guid_ok(r)) upcase(s->uuid); else downcase(s->uuid);
        if (!find_partuuid(s->uuid, s->disk, sizeof s->disk, &s->num)) {
            snprintf(why, whyn, "no partition has PARTUUID %s", r);
            return false;
        }
        return true;
    }
    if (dev_name_ok(r) && blk_exists(r)) {
        if (blk_is_part(r)) {
            blk_disk(r, s->disk, sizeof s->disk);
            s->num = blk_partno(r);
            table_t *t = model_of(s->disk);
            pent_t *p = t ? table_num(t, s->num) : NULL;
            if (!p) {
                snprintf(why, whyn, "%s is not in the partition table on the disk", r);
                return false;
            }
            /* From here on the plan names it by its GUID. */
            strlcpy(s->uuid, p->uuid, sizeof s->uuid);
            return true;
        }
        if (!allow_disk) {
            snprintf(why, whyn, "%s is a whole disk; this step wants a partition", r);
            return false;
        }
        strlcpy(s->disk, r, sizeof s->disk);
        s->num = 0;
        return true;
    }
    snprintf(why, whyn, "\"%s\" is not a partition, a PARTUUID or @step", r);
    return false;
}

/* The kernel name of a step's target: loop5p2, or loop5. */
static void target_name(const step_t *s, char *out, size_t n)
{
    if (s->num)
        part_name(s->disk, s->num, out, n);
    else
        strlcpy(out, s->disk, n);
}

/* Things that stop every step whatever else is true: the target is in
 * use now, or was just checked in an earlier step of the same plan. */
static bool target_free(const step_t *s, char *why, size_t whyn)
{
    char kn[40];
    target_name(s, kn, sizeof kn);
    if (!blk_exists(kn))
        return true;                   /* created by this plan */
    bool vital;
    if (s->num == 0)
        return !disk_busy(kn, &vital, why, whyn);
    return !in_use(kn, &vital, why, whyn);
}

/* A new random GPT GUID, version 4. */
static void new_guid(char *out)
{
    u8 r[16];
    if (lp_getrandom(r, sizeof r, 0) != (long)sizeof r) {
        u64 t = (u64)lp_monotonic_ms() ^ ((u64)lp_getpid() << 32);
        for (int i = 0; i < 16; i++) { t = t * 6364136223846793005ull + 1; r[i] = (u8)(t >> 56); }
    }
    r[6] = (u8)((r[6] & 0x0f) | 0x40);
    r[8] = (u8)((r[8] & 0x3f) | 0x80);
    hex_uuid(r, out);
    upcase(out);
}

static int free_num(const table_t *t)
{
    int max = !strcmp(t->kind, "gpt") ? 128 : 4;
    for (int n = 1; n <= max; n++) {
        bool taken = false;
        for (int i = 0; i < t->n; i++)
            if (t->p[i].num == n) taken = true;
        if (!taken)
            return n;
    }
    return 0;
}

/* Parse one step from its fields and play it on the model. */
static bool plan_step(step_t *s, int idx, char **f, int nf, char *why, size_t whyn)
{
    memset(s, 0, sizeof *s);
    int v = -1;
    for (int i = 0; SVERBS[i].verb; i++)
        if (!strcmp(f[0], SVERBS[i].verb)) v = i;
    if (v < 0) {
        snprintf(why, whyn, "\"%.20s\" is not a step", f[0]);
        return false;
    }
    s->kind = SVERBS[v].k;
    /* Optional trailing fields, recognised by their prefix. */
    int nargs = nf - 1;
    while (nargs > 0) {
        char *last = f[nargs];
        if (starts(last, "confirm=") && alphabet_ok(last + 8, "-", 20, true)) {
            strlcpy(s->confirm, last + 8, sizeof s->confirm);
        } else if (starts(last, "uuid=") && guid_ok(last + 5)) {
            strlcpy(s->uuid, last + 5, sizeof s->uuid);
            upcase(s->uuid);
        } else if (starts(last, "k:")) {
            if (!unhex(last + 2, s->pass, sizeof s->pass, &s->passlen) ||
                s->passlen < 8) {
                snprintf(why, whyn, "a passphrase is hex after k:, at least 8 bytes");
                return false;
            }
        } else {
            break;
        }
        nargs--;
    }
    if (nargs < SVERBS[v].min || nargs > SVERBS[v].max) {
        snprintf(why, whyn, "%s takes %d to %d arguments", f[0],
                 SVERBS[v].min, SVERBS[v].max);
        return false;
    }
    char **a = f + 1;
    char tmp[200], sz1[32], sz2[32], kn[40];

    if (s->kind == S_MKLABEL || s->kind == S_CREATE) {
        if (!dev_name_ok(a[0]) || !blk_exists(a[0]) || blk_is_part(a[0])) {
            snprintf(why, whyn, "%.31s is not a whole disk", a[0]);
            return false;
        }
        strlcpy(s->disk, a[0], sizeof s->disk);
        strlcpy(s->ref, a[0], sizeof s->ref);
    } else {
        strlcpy(s->ref, a[0], sizeof s->ref);
        bool disk_ok = s->kind == S_FORMAT || s->kind == S_LABEL ||
                       s->kind == S_CHECK || s->kind == S_REPAIR || s->kind == S_WIPE;
        if (!resolve(s, idx, disk_ok, why, whyn))
            return false;
    }
    table_t *t = model_of(s->disk);
    if (!t) {
        snprintf(why, whyn, "cannot read %s", s->disk);
        return false;
    }
    bool gpt = !strcmp(t->kind, "gpt");
    pent_t *p = s->num ? table_num(t, s->num) : NULL;
    if (s->num && !p) {
        snprintf(why, whyn, "the partition %s is gone by this step", s->ref);
        return false;
    }
    target_name(s, kn, sizeof kn);
    u64 lss = t->lss;
    u64 al = ALIGN_BYTES / lss;
    char what[160];
    if (p) {
        char fd_[40];
        fs_desc(p, fd_, sizeof fd_);
        human(p->size * lss, sz1, sizeof sz1);
        snprintf(what, sizeof what, "%s (%s, %s%s%s%s)", kn, sz1, fd_,
                 p->name[0] ? ", \"" : "", p->name, p->name[0] ? "\"" : "");
    } else {
        strlcpy(what, kn, sizeof what);
    }

    /* Everything but a create and a check needs its target unused. */
    if (s->kind != S_CREATE && s->kind != S_MKLABEL && !target_free(s, why, whyn)) {
        plan_code = "refused";
        return false;
    }

    switch (s->kind) {
    case S_MKLABEL: {
        if (strcmp(a[1], "gpt") && strcmp(a[1], "mbr")) {
            snprintf(why, whyn, "the table type is gpt or mbr");
            return false;
        }
        bool vital;
        if (disk_busy(s->disk, &vital, why, whyn)) {
            plan_code = "refused";
            return false;
        }
        bool prot = false;
        for (int i = 0; i < t->n; i++)
            if (protected_tag(t, &t->p[i])) prot = true;
        if (prot && strcmp(s->confirm, "ALL")) {
            plan_code = "refused";
            snprintf(why, whyn, "%s holds the ESP or LP-RECOVERY; a new table"
                     " needs the typed confirmation", s->disk);
            return false;
        }
        s->gpt = !strcmp(a[1], "gpt");
        u64 total = t->nsect;
        memset(t->p, 0, sizeof t->p);
        t->n = 0;
        strlcpy(t->kind, s->gpt ? "gpt" : "mbr", sizeof t->kind);
        t->first = s->gpt ? (u64)(align_up(34 * 512, ALIGN_BYTES) / lss) : al;
        t->last = s->gpt ? total - 34 * 512 / lss : total - 1;
        t->has_extended = false;
        snprintf(s->desc, sizeof s->desc, "Create a new, empty %s partition"
                 " table on %s - everything on it is lost",
                 s->gpt ? "GPT" : "MBR", s->disk);
        return true;
    }
    case S_CREATE: {
        u64 start, size;
        if (!parse_u64(a[1], &start) || !parse_u64(a[2], &size)) {
            snprintf(why, whyn, "start and size are byte counts");
            return false;
        }
        if (!fs_parse(a[3], &s->fs)) {
            snprintf(why, whyn, "the filesystem is one of ext4 btrfs fat32 exfat"
                     " ntfs swap luks-ext4 none");
            return false;
        }
        if (!label_decode(a[4], s->fs, false, s->label, sizeof s->label, why, whyn))
            return false;
        if (!strcmp(t->kind, "none")) {
            snprintf(why, whyn, "%s has no partition table yet - create one first",
                     s->disk);
            return false;
        }
        if (t->has_extended) {
            snprintf(why, whyn, "this MBR disk has logical partitions; new"
                     " partitions on it are made from a terminal (fdisk)");
            return false;
        }
        if (start % ALIGN_BYTES || size % lss || size < MIB) {
            snprintf(why, whyn, "a new partition starts on a MiB boundary and is"
                     " at least 1 MiB");
            return false;
        }
        const char *tn = strcmp(a[5], "auto") ? a[5] : auto_type(s->fs, gpt);
        const ptype_t *ty = ptype_by_name(tn);
        if (!ty || (!gpt && !ty->mbr)) {
            snprintf(why, whyn, "\"%.16s\" is not a partition type this %s table"
                     " can have", a[5], t->kind);
            return false;
        }
        strlcpy(s->ptype, ty->name, sizeof s->ptype);
        if (s->fs == FS_LUKS && s->passlen == 0) {
            snprintf(why, whyn, "an encrypted partition needs a passphrase");
            return false;
        }
        s->a = start;
        s->b = size;
        u64 ss = start / lss, sn = size / lss;
        if (!region_free(t, ss, sn, 0, tmp, sizeof tmp)) {
            snprintf(why, whyn, "the new partition does not fit: %s", tmp);
            return false;
        }
        s->num = free_num(t);
        if (!s->num || t->n >= MAX_PARTS) {
            snprintf(why, whyn, "the partition table is full");
            return false;
        }
        if (gpt) {
            if (!s->uuid[0])
                new_guid(s->uuid);
            if (table_uuid(t, s->uuid)) {
                snprintf(why, whyn, "that PARTUUID is already on the disk");
                return false;
            }
        } else {
            snprintf(s->uuid, sizeof s->uuid, "%s-%02x", t->id, s->num);
        }
        pent_t *np = &t->p[t->n++];
        memset(np, 0, sizeof *np);
        np->num = s->num;
        np->start = ss;
        np->size = sn;
        strlcpy(np->uuid, s->uuid, sizeof np->uuid);
        if (gpt) strlcpy(np->type, ty->gpt, sizeof np->type);
        else snprintf(np->type, sizeof np->type, "0x%02x", ty->mbr);
        if (gpt) strlcpy(np->name, s->label, sizeof np->name);
        np->mfs = (int)s->fs;
        np->mfs_size = s->fs == FS_NONE ? 0 : size;
        human(size, sz1, sizeof sz1);
        human(start, sz2, sizeof sz2);
        snprintf(s->desc, sizeof s->desc, "Create a %s %s partition%s%s%s on %s at %s",
                 sz1, s->fs == FS_NONE ? "empty" : fs_word(s->fs),
                 s->label[0] ? " \"" : "", s->label, s->label[0] ? "\"" : "",
                 s->disk, sz2);
        return true;
    }
    case S_DELETE: {
        const char *tag = protected_tag(t, p);
        if (tag && strcmp(s->confirm, tag)) {
            plan_code = "refused";
            snprintf(why, whyn, "%s is the %s; deleting it needs the typed"
                     " confirmation", kn, tag);
            return false;
        }
        if (p->logical) {
            snprintf(why, whyn, "logical MBR partitions are removed from a terminal (fdisk)");
            return false;
        }
        snprintf(s->desc, sizeof s->desc, "Delete %s", what);
        *p = t->p[--t->n];
        return true;
    }
    case S_RESIZE: {
        u64 size;
        if (!parse_u64(a[1], &size) || size % lss || size < MIB) {
            snprintf(why, whyn, "the new size is a byte count, at least 1 MiB");
            return false;
        }
        u64 sn = size / lss;
        if (sn == p->size) {
            snprintf(why, whyn, "%s is already that size", kn);
            return false;
        }
        if (p->logical) {
            snprintf(why, whyn, "logical MBR partitions are resized from a terminal");
            return false;
        }
        bool shrink = sn < p->size;
        if (p->mfs_other) {
            snprintf(why, whyn, "%s has a filesystem this tool cannot resize", kn);
            return false;
        }
        const char *no = resize_refusal((fstype_t)p->mfs, shrink);
        if (no) {
            snprintf(why, whyn, "%s: %s", kn, no);
            return false;
        }
        if (!region_free(t, p->start, sn, p->num, tmp, sizeof tmp)) {
            snprintf(why, whyn, "%s cannot grow that far: %s", kn, tmp);
            return false;
        }
        /* The new end must still be on a MiB, or on the last sector. */
        if ((p->start + sn) % al && p->start + sn - 1 != t->last) {
            snprintf(why, whyn, "the new size has to end on a MiB boundary");
            return false;
        }
        human(p->size * lss, sz1, sizeof sz1);
        human(size, sz2, sizeof sz2);
        snprintf(s->desc, sizeof s->desc, "%s %s from %s to %s",
                 shrink ? "Shrink" : "Grow", what, sz1, sz2);
        s->b = size;
        s->a = p->start * lss;
        p->size = sn;
        if (p->mfs != FS_NONE) p->mfs_size = size;
        return true;
    }
    case S_MOVE: {
        u64 start;
        if (!parse_u64(a[1], &start) || start % ALIGN_BYTES) {
            snprintf(why, whyn, "the new start is a byte count on a MiB boundary");
            return false;
        }
        u64 ss = start / lss;
        if (ss == p->start) {
            snprintf(why, whyn, "%s already starts there", kn);
            return false;
        }
        if (p->logical) {
            snprintf(why, whyn, "logical MBR partitions are moved from a terminal");
            return false;
        }
        if (!region_free(t, ss, p->size, p->num, tmp, sizeof tmp)) {
            snprintf(why, whyn, "%s cannot move there: %s", kn, tmp);
            return false;
        }
        human(p->start * lss, sz1, sizeof sz1);
        human(start, sz2, sizeof sz2);
        snprintf(s->desc, sizeof s->desc, "Move %s from %s to %s on the disk"
                 " (slow: every block is copied)", what, sz1, sz2);
        s->a = start;
        s->b = p->size * lss;
        p->start = ss;
        return true;
    }
    case S_FORMAT: {
        if (!fs_parse(a[1], &s->fs)) {
            snprintf(why, whyn, "the filesystem is one of ext4 btrfs fat32 exfat"
                     " ntfs swap luks-ext4 none");
            return false;
        }
        if (!label_decode(a[2], s->fs, false, s->label, sizeof s->label, why, whyn))
            return false;
        if (s->fs == FS_LUKS && s->passlen == 0) {
            snprintf(why, whyn, "an encrypted partition needs a passphrase");
            return false;
        }
        const char *tag = p ? protected_tag(t, p) : NULL;
        if (tag && strcmp(s->confirm, tag)) {
            plan_code = "refused";
            snprintf(why, whyn, "%s is the %s; formatting it needs the typed"
                     " confirmation", kn, tag);
            return false;
        }
        if (!p && strcmp(t->kind, "none")) {
            snprintf(why, whyn, "%s has a partition table; format a partition,"
                     " or make a new table first", kn);
            return false;
        }
        snprintf(s->desc, sizeof s->desc, "Format %s as %s%s%s%s - what is on it"
                 " now is lost", what, fs_word(s->fs), s->label[0] ? " \"" : "",
                 s->label, s->label[0] ? "\"" : "");
        if (p) { p->mfs = (int)s->fs; p->mfs_other = false; p->mfs_size = p->size * lss; }
        return true;
    }
    case S_LABEL: {
        fstype_t f = p ? (fstype_t)p->mfs : FS_NONE;
        if (!p) {
            probe_t pr;
            probe_dev(kn, &pr);
            f = fs_of_probe(&pr);
        }
        if (f == FS_NONE) {
            snprintf(why, whyn, "%s has no filesystem to label", kn);
            return false;
        }
        if (!label_decode(a[1], f, false, s->label, sizeof s->label, why, whyn))
            return false;
        snprintf(s->desc, sizeof s->desc, "Label the filesystem on %s \"%s\"",
                 kn, s->label);
        return true;
    }
    case S_NAME:
        if (!gpt) {
            snprintf(why, whyn, "only GPT partitions have names");
            return false;
        }
        if (!label_decode(a[1], FS_NONE, true, s->label, sizeof s->label, why, whyn))
            return false;
        if (!strcmp(s->label, "LP-RECOVERY") || !strcmp(s->label, "LP-ROOT") ||
            !strcmp(p->name, "LP-RECOVERY") || !strcmp(p->name, "LP-ROOT")) {
            snprintf(why, whyn, "LP-ROOT and LP-RECOVERY are how the boot menu"
                     " finds the system; those names are not changed here");
            return false;
        }
        snprintf(s->desc, sizeof s->desc, "Name %s \"%s\"", what, s->label);
        strlcpy(p->name, s->label, sizeof p->name);
        return true;
    case S_TYPE: {
        const ptype_t *ty = ptype_by_name(a[1]);
        if (!ty || (!gpt && !ty->mbr)) {
            snprintf(why, whyn, "\"%.16s\" is not a type this table can have", a[1]);
            return false;
        }
        strlcpy(s->ptype, ty->name, sizeof s->ptype);
        snprintf(s->desc, sizeof s->desc, "Set the type of %s to %s", what, ty->desc);
        if (gpt) strlcpy(p->type, ty->gpt, sizeof p->type);
        else snprintf(p->type, sizeof p->type, "0x%02x", ty->mbr);
        return true;
    }
    case S_FLAGS:
        if (!alphabet_ok(a[1], ",_", 190, false)) {
            snprintf(why, whyn, "flags are a comma list, or none");
            return false;
        }
        strlcpy(s->flags, a[1], sizeof s->flags);
        snprintf(s->desc, sizeof s->desc, "Set the flags of %s to %s", what,
                 !strcmp(a[1], "none") ? "none" : a[1]);
        return true;
    case S_CHECK:
    case S_REPAIR:
        snprintf(s->desc, sizeof s->desc, "%s the filesystem on %s",
                 s->kind == S_CHECK ? "Check" : "Check and repair", what);
        return true;
    case S_WIPE: {
        const char *tag = p ? protected_tag(t, p) : NULL;
        if (tag && strcmp(s->confirm, tag)) {
            plan_code = "refused";
            snprintf(why, whyn, "%s is the %s; wiping it needs the typed"
                     " confirmation", kn, tag);
            return false;
        }
        if (!p) {
            bool vital;
            if (disk_busy(kn, &vital, why, whyn)) {
                plan_code = "refused";
                return false;
            }
        }
        snprintf(s->desc, sizeof s->desc, "Wipe the signatures on %s so nothing"
                 " recognises what was on it", what);
        if (p) { p->mfs = FS_NONE; p->mfs_other = false; }
        else strlcpy(t->kind, "none", sizeof t->kind);
        return true;
    }
    }
    return false;
}

/* ── Flags ──────────────────────────────────────────────────────────
 *
 * "flags" is the whole set a partition should have afterwards, the way
 * GParted's flag dialog shows it. Some of those flags are really the
 * partition type (esp, msftdata, lvm, raid ...), the rest GPT
 * attribute bits or MBR's active byte. This turns the set into the
 * type and the bits, or says which flag makes no sense. */
static bool flags_compute(const table_t *t, const pent_t *p, const char *list,
                          char *type, size_t typen, u64 *attrs,
                          char *why, size_t whyn)
{
    bool gpt = !strcmp(t->kind, "gpt");
    const char *typeflag = NULL;
    u64 a = gpt ? (p->attrs & ~((1ull << 0) | (1ull << 2) | (1ull << 60) |
                                (1ull << 62) | (1ull << 63))) : 0;
    bool hidden = false;
    char buf[200];
    strlcpy(buf, list, sizeof buf);
    if (strcmp(buf, "none") != 0) {
        for (char *f = buf; f && *f; ) {
            char *c = strchr(f, ',');
            if (c) *c++ = '\0';
            const char *tf = NULL;
            if (!strcmp(f, "esp") || (!strcmp(f, "boot") && gpt)) tf = "esp";
            else if (!strcmp(f, "boot"))        a |= 0x80;
            else if (!strcmp(f, "msftdata"))    tf = "msdata";
            else if (!strcmp(f, "lvm"))         tf = "lvm";
            else if (!strcmp(f, "raid"))        tf = "raid";
            else if (!strcmp(f, "swap"))        tf = "swap";
            else if (!strcmp(f, "bios_grub"))   tf = "bios";
            else if (!strcmp(f, "msftres"))     tf = "msres";
            else if (!strcmp(f, "diag"))        tf = "winre";
            else if (!strcmp(f, "hidden"))      hidden = true;
            else if (gpt && !strcmp(f, "legacy_boot"))  a |= 1ull << 2;
            else if (gpt && !strcmp(f, "required"))     a |= 1ull << 0;
            else if (gpt && !strcmp(f, "readonly"))     a |= 1ull << 60;
            else if (gpt && !strcmp(f, "no_automount")) a |= 1ull << 63;
            else {
                snprintf(why, whyn, "\"%.20s\" is not a flag a %s partition can have",
                         f, t->kind);
                return false;
            }
            if (tf) {
                if (typeflag && strcmp(typeflag, tf)) {
                    snprintf(why, whyn, "%s and %s are both partition types;"
                             " a partition has one", typeflag, tf);
                    return false;
                }
                typeflag = tf;
            }
            f = c;
        }
    }
    if (gpt && hidden) a |= 1ull << 62;

    /* The type: the one a type-flag names; otherwise keep the current
     * type unless it IS one of the type-flags, which was just switched
     * off - then back to the ordinary type for what is on it. */
    const ptype_t *cur = ptype_find(p->type);
    const ptype_t *ty = typeflag ? ptype_by_name(typeflag) : cur;
    static const char *const flagtypes[] = { "esp", "msdata", "lvm", "raid",
                                             "swap", "bios", "msres", "winre", NULL };
    if (!typeflag && cur) {
        for (int i = 0; flagtypes[i]; i++)
            if (!strcmp(cur->name, flagtypes[i]))
                ty = ptype_by_name(auto_type((fstype_t)p->mfs, gpt));
        if (!gpt && !strcmp(cur->name, "fat32"))
            ty = cur;
    }
    if (gpt) {
        strlcpy(type, ty ? ty->gpt : p->type, typen);
    } else {
        u8 code = ty && ty->mbr ? ty->mbr : (u8)(hexval(p->type[2]) * 16 + hexval(p->type[3]));
        /* MBR hides FAT and NTFS by adding 0x10 to the type. */
        static const u8 hideable[] = { 0x01, 0x04, 0x06, 0x07, 0x0b, 0x0c, 0x0e, 0 };
        u8 base = code & (u8)~0x10;
        bool can = false;
        for (int i = 0; hideable[i]; i++) if (hideable[i] == base) can = true;
        if (hidden && !can) {
            snprintf(why, whyn, "only FAT and NTFS partitions can be hidden on MBR");
            return false;
        }
        if (can) code = hidden ? (u8)(base | 0x10) : base;
        snprintf(type, typen, "0x%02x", code);
    }
    *attrs = a;
    return true;
}

/* ── Doing it ───────────────────────────────────────────────────── */

static int  cur_step;                  /* 1-based, while executing */

static bool reread(const char *disk, table_t *t)
{
    if (!table_read(disk, t))
        return failed("failed", "the partition table could not be read back");
    return true;
}

static pent_t *entry_of(table_t *t, step_t *s)
{
    pent_t *p = s->uuid[0] ? table_uuid(t, s->uuid) : NULL;
    if (!p && s->num)
        p = table_num(t, s->num);      /* an MBR entry whose id changed */
    if (p)
        s->num = p->num;
    return p;
}

/* Is it still free? The plan checked seconds or minutes ago. */
static bool still_free(const step_t *s)
{
    mounts_refresh();
    char why[300];
    if (!target_free(s, why, sizeof why))
        return failed("refused", why);
    return true;
}

static bool sync_or_fail(const char *disk)
{
    char why[300];
    if (!kernel_sync(disk, why, sizeof why))
        return failed("failed", why);
    return true;
}

static bool exec_step(step_t *s)
{
    static table_t t;
    char kn[40], why[300], code[48];
    if (s->kind != S_MKLABEL && s->kind != S_CREATE && s->at) {
        /* The target was made by an earlier step; take what it became. */
        step_t *c = &steps[s->at - 1];
        strlcpy(s->disk, c->disk, sizeof s->disk);
        s->num = c->num;
        strlcpy(s->uuid, c->uuid, sizeof s->uuid);
    }
    if (!reread(s->disk, &t))
        return false;
    bool gpt = !strcmp(t.kind, "gpt");
    pent_t *p = NULL;
    if (s->kind != S_MKLABEL && s->kind != S_CREATE && s->num) {
        p = entry_of(&t, s);
        if (!p)
            return failed("failed", "the partition is not in the table any more");
    }
    target_name(s, kn, sizeof kn);
    if (s->kind != S_CREATE && !still_free(s))
        return false;
    u64 lss = t.lss;

    switch (s->kind) {
    case S_MKLABEL:
        flush_dev(s->disk);
        if (!tbl_new(s->disk, s->gpt))
            return false;
        s->phase = 1;
        if (!sync_or_fail(s->disk) || !reread(s->disk, &t))
            return false;
        if (strcmp(t.kind, s->gpt ? "gpt" : "mbr") || t.n)
            return failed("failed", "the new table did not read back as written");
        return true;

    case S_CREATE: {
        const ptype_t *ty = ptype_by_name(s->ptype);
        if (!type_code(&t, ty, code, sizeof code))
            return failed("failed", "that type is not possible on this table");
        u64 ss = s->a / lss, sn = s->b / lss;
        if (!region_free(&t, ss, sn, 0, why, sizeof why))
            return failed("failed", why);
        if (table_num(&t, s->num))
            s->num = free_num(&t);
        const char *name = gpt ? s->label : "";
        if (!tbl_add(s->disk, &t, s->num, ss, sn, code, s->uuid, name))
            return false;
        s->phase = 1;
        if (!sync_or_fail(s->disk) || !reread(s->disk, &t))
            return false;
        pent_t *np = table_num(&t, s->num);
        if (!np || np->start != ss || np->size != sn)
            return failed("failed", "the new partition did not read back where it"
                          " was put");
        strlcpy(s->uuid, np->uuid, sizeof s->uuid);
        target_name(s, kn, sizeof kn);
        if (s->fs != FS_NONE) {
            pct_lo = 30; pct_hi = 95;
            if (!mkfs_on(kn, s->fs, s->label, s->pass, s->passlen))
                return false;
            s->phase = 2;
        }
        return true;
    }

    case S_DELETE:
        flush_dev(kn);
        if (!tbl_del(s->disk, p->num))
            return false;
        s->phase = 1;
        if (!sync_or_fail(s->disk) || !reread(s->disk, &t))
            return false;
        if (table_uuid(&t, s->uuid))
            return failed("failed", "the partition is still in the table");
        return true;

    case S_RESIZE: {
        probe_t pr;
        probe_dev(kn, &pr);
        fstype_t f = fs_of_probe(&pr);
        if (pr.fs[0] && f == FS_NONE)
            return failed("refused", "that filesystem cannot be resized here");
        u64 sn = s->b / lss;
        bool shrink = sn < p->size;
        const char *no = resize_refusal(f, shrink);
        if (no)
            return failed("refused", no);
        if (!region_free(&t, p->start, sn, p->num, why, sizeof why))
            return failed("failed", why);
        flush_dev(kn);
        if (f == FS_FAT32 && pr.fs[0]) {
            /* fatresize does both halves itself, through libparted. */
            char bin[64], dev[64], sz[32], nb[8];
            if (!need_tool("fatresize", "fatresize", bin, sizeof bin))
                return false;
            dev_path(s->disk, dev, sizeof dev);
            snprintf(sz, sizeof sz, "%llu", (unsigned long long)s->b);
            numstr(p->num, nb);
            char *argv[] = { bin, "-f", "-s", sz, "-n", nb, dev, NULL };
            pct_text = "resizing FAT";
            if (run(argv) != 0)
                return failed("failed", "fatresize failed");
            s->phase = 2;
        } else if (shrink) {
            pct_lo = 5; pct_hi = 80;
            if (!fs_resize(kn, f, s->b, true))
                return false;
            s->phase = 1;
            probe_dev(kn, &pr);
            if (pr.size > s->b)
                return failed("failed", "the filesystem is still bigger than the"
                              " new partition size; the partition was not touched");
            if (!tbl_geom(s->disk, p->num, p->start, sn))
                return false;
            s->phase = 2;
            if (f == FS_SWAP) {
                if (!sync_or_fail(s->disk))
                    return false;
                if (!fs_resize(kn, f, s->b, false))
                    return false;
            }
        } else {
            if (!tbl_geom(s->disk, p->num, p->start, sn))
                return false;
            s->phase = 3;
            if (!sync_or_fail(s->disk))
                return false;
            pct_lo = 10; pct_hi = 95;
            if (f != FS_NONE && !fs_resize(kn, f, s->b, false))
                return false;
            s->phase = 4;
        }
        if (!sync_or_fail(s->disk) || !reread(s->disk, &t))
            return false;
        p = entry_of(&t, s);
        if (!p || p->size != sn)
            return failed("failed", "the partition did not read back at the new size");
        probe_dev(kn, &pr);
        if (pr.size > s->b)
            return failed("failed", "the filesystem is bigger than its partition");
        return true;
    }

    case S_MOVE: {
        journal_t j;
        memset(&j, 0, sizeof j);
        strlcpy(j.disk, s->disk, sizeof j.disk);
        strlcpy(j.diskid, t.id, sizeof j.diskid);
        strlcpy(j.partuuid, p->uuid, sizeof j.partuuid);
        j.num = p->num;
        j.from = p->start * lss;
        j.to = s->a;
        j.len = p->size * lss;
        if (!region_free(&t, s->a / lss, p->size, p->num, why, sizeof why))
            return failed("failed", why);
        flush_dev(kn);
        flush_dev(s->disk);
        if (!journal_write(&j))
            return failed("failed", "could not write the move journal");
        s->phase = 1;
        bool clean;
        pct_lo = 2; pct_hi = 90;
        if (!move_copy(&j, &clean)) {
            if (clean) {
                lp_unlink(JOURNAL_PATH);
                s->phase = 0;
                return failed("cancelled", "the move was stopped before the table"
                              " changed; the partition is where it was, intact");
            }
            return false;
        }
        j.phase = 1;
        journal_write(&j);
        s->phase = 2;
        if (!tbl_geom(s->disk, p->num, s->a / lss, p->size))
            return false;
        s->phase = 3;
        lp_unlink(JOURNAL_PATH);
        if (!sync_or_fail(s->disk) || !reread(s->disk, &t))
            return false;
        p = entry_of(&t, s);
        if (!p || p->start != s->a / lss)
            return failed("failed", "the partition did not read back at its new place");
        probe_t pr;
        if (probe_dev(kn, &pr) && pr.fs[0] && strcmp(pr.fs, "swap") &&
            strcmp(pr.fs, "crypto_LUKS") && strcmp(pr.fs, "btrfs")) {
            say("checking the moved filesystem (read-only)");
            pct_lo = 90; pct_hi = 100;
            if (fsck_on(kn, false) != 0)
                return failed("failed", "the moved filesystem does not check clean");
        }
        return true;
    }

    case S_FORMAT: {
        pct_lo = 5; pct_hi = 90;
        flush_dev(kn);
        if (!mkfs_on(kn, s->fs, s->label, s->pass, s->passlen))
            return false;
        s->phase = 1;
        /* A FAT or NTFS partition typed "Linux" is invisible to Windows
         * and a Linux one typed "basic data" is offered to it; follow the
         * filesystem, but only between the ordinary types. */
        if (p && s->fs != FS_NONE) {
            const ptype_t *cur = ptype_find(p->type);
            if (cur && (!strcmp(cur->name, "linux") || !strcmp(cur->name, "msdata") ||
                        !strcmp(cur->name, "fat32") || !strcmp(cur->name, "swap"))) {
                const ptype_t *want = ptype_by_name(auto_type(s->fs, gpt));
                if (want && want != cur && type_code(&t, want, code, sizeof code) &&
                    strcmp(want->gpt, cur->gpt) != 0)
                    tbl_set(s->disk, "--part-type", p->num, code);
            }
        }
        probe_t pr;
        probe_dev(kn, &pr);
        if (s->fs != FS_NONE && fs_of_probe(&pr) != s->fs &&
            !(s->fs == FS_LUKS && !strcmp(pr.fs, "crypto_LUKS")))
            return failed("failed", "the new filesystem did not read back");
        return true;
    }

    case S_LABEL: {
        if (!set_fslabel(kn, s->label))
            return false;
        probe_t pr;
        probe_dev(kn, &pr);
        char want[112];
        strlcpy(want, s->label, sizeof want);
        if (!strcmp(pr.fs, "vfat")) upcase(want);
        if (strcmp(pr.label, want) != 0)
            return failed("failed", "the label did not read back");
        return true;
    }

    case S_NAME:
        if (!tbl_set(s->disk, "--part-label", p->num, s->label))
            return false;
        if (!reread(s->disk, &t))
            return false;
        p = entry_of(&t, s);
        if (!p || strcmp(p->name, s->label) != 0)
            return failed("failed", "the name did not read back");
        return true;

    case S_TYPE: {
        const ptype_t *ty = ptype_by_name(s->ptype);
        if (!type_code(&t, ty, code, sizeof code))
            return failed("failed", "that type is not possible on this table");
        if (!tbl_set(s->disk, "--part-type", p->num, code))
            return false;
        if (!reread(s->disk, &t))
            return false;
        p = entry_of(&t, s);
        const ptype_t *got = p ? ptype_find(p->type) : NULL;
        if (!got || (gpt ? strcmp(got->gpt, ty->gpt) : got->mbr != ty->mbr))
            return failed("failed", "the type did not read back");
        return true;
    }

    case S_FLAGS: {
        char type[48];
        u64 attrs;
        p->mfs = (int)fs_of_probe(&(probe_t){0});
        probe_t pr;
        if (probe_dev(kn, &pr)) p->mfs = (int)fs_of_probe(&pr);
        if (!flags_compute(&t, p, s->flags, type, sizeof type, &attrs, why, sizeof why))
            return failed("invalid", why);
        if (strcmp(type, p->type) != 0) {
            if (gpt) strlcpy(code, type, sizeof code);
            else strlcpy(code, type + 2, sizeof code);
            if (!tbl_set(s->disk, "--part-type", p->num, code))
                return false;
        }
        if (gpt) {
            char as[200];
            attrs_str(attrs, as, sizeof as);
            if (attrs != p->attrs && !tbl_set(s->disk, "--part-attrs", p->num, as))
                return false;
        } else if ((attrs & 0x80) != (p->attrs & 0x80)) {
            /* --activate switches on the ones named and off the rest. */
            char nb[8][8];
            const char *post[10];
            int k = 0;
            for (int i = 0; i < t.n && k < 8; i++) {
                bool on = t.p[i].num == p->num ? (attrs & 0x80) : (t.p[i].attrs & 0x80);
                if (on && !t.p[i].logical) {
                    numstr(t.p[i].num, nb[k]);
                    post[k] = nb[k];
                    k++;
                }
            }
            if (!k) post[k++] = "-";
            post[k] = NULL;
            char bin[64], dev[64];
            if (!need_tool("sfdisk", "fdisk", bin, sizeof bin) ||
                !dev_path(s->disk, dev, sizeof dev))
                return false;
            char *argv[16];
            int n = 0;
            argv[n++] = bin; argv[n++] = "--no-reread"; argv[n++] = "--no-tell-kernel";
            argv[n++] = "--activate"; argv[n++] = dev;
            for (int i = 0; post[i]; i++) argv[n++] = (char *)post[i];
            argv[n] = NULL;
            if (run(argv) != 0)
                return failed("failed", "sfdisk could not set the boot flag");
        }
        if (!reread(s->disk, &t))
            return false;
        p = entry_of(&t, s);
        if (!p || strcmp(p->type, type) != 0 || p->attrs != attrs)
            return failed("failed", "the flags did not read back");
        return true;
    }

    case S_CHECK:
    case S_REPAIR: {
        pct_lo = 0; pct_hi = 100;
        int r = fsck_on(kn, s->kind == S_REPAIR);
        if (r < 0)
            return false;
        if (r == 1)
            return failed("failed", "the check found errors; repair them with"
                          " \"Check and repair\" (nothing was changed)");
        return true;
    }

    case S_WIPE: {
        char dev[64];
        if (!dev_path(kn, dev, sizeof dev))
            return failed("failed", "the device is gone");
        if (!wipe_sigs(dev))
            return failed("failed", "wipefs failed");
        s->phase = 1;
        if (!s->num && !sync_or_fail(s->disk))
            return false;
        return true;
    }
    }
    return failed("failed", "unknown step");
}

/* What the disk looks like now, as "state" lines: the part of the
 * failure message that says where things stand. */
static void state_table(const char *disk)
{
    static table_t t;
    char line[400], sz[32], st[32];
    if (!table_read(disk, &t)) {
        reply("state", "the partition table could not be read");
        return;
    }
    snprintf(line, sizeof line, "%s now has a %s partition table:", disk,
             !strcmp(t.kind, "mbr") ? "MBR" : !strcmp(t.kind, "gpt") ? "GPT" : "no");
    reply("state", line);
    for (int i = 0; i < t.n; i++) {
        char kn[40];
        probe_t pr;
        part_name(disk, t.p[i].num, kn, sizeof kn);
        probe_dev(kn, &pr);
        human(t.p[i].start * t.lss, st, sizeof st);
        human(t.p[i].size * t.lss, sz, sizeof sz);
        snprintf(line, sizeof line, "  %s  at %s  %s  %s%s%s", kn, st, sz,
                 pr.fs[0] ? pr.fs : "no filesystem", pr.label[0] ? " " : "",
                 pr.label);
        reply("state", line);
    }
}

static void state_report(int failed_at)
{
    char line[600];
    for (int i = 0; i < failed_at - 1; i++) {
        snprintf(line, sizeof line, "done: %d. %s", i + 1, steps[i].desc);
        reply("state", line);
    }
    step_t *s = &steps[failed_at - 1];
    snprintf(line, sizeof line, "stopped: %d. %s", failed_at, s->desc);
    reply("state", line);
    const char *what = "nothing of this step was done";
    switch (s->kind) {
    case S_RESIZE:
        if (s->phase == 1)
            what = "the filesystem was already made smaller, but the partition still"
                   " has its old size. That is safe: the space after the filesystem"
                   " is simply unused. Try again, or grow the filesystem back";
        else if (s->phase == 2)
            what = "the filesystem and the partition entry have their new size; only"
                   " telling the kernel failed. Restart before using the partition";
        else if (s->phase == 3)
            what = "the partition is bigger, the filesystem inside still has its old"
                   " size. That is safe; grow it again to use the space";
        break;
    case S_MOVE:
        if (s->phase == 1)
            what = "the data was being copied. The partition table still points at"
                   " the old place. A move journal is kept: run \"lp-diskctl resume\""
                   " (or open Disks) to finish the move before using the partition";
        else if (s->phase == 2)
            what = "every block was copied but the table was not updated. Run"
                   " \"lp-diskctl resume\" to finish";
        else if (s->phase == 3)
            what = "the move is complete and the table updated; the final check or"
                   " telling the kernel failed. Restart and run a check";
        break;
    case S_CREATE:
        if (s->phase == 1)
            what = "the partition exists but has no filesystem yet; format it";
        break;
    case S_FORMAT:
        if (s->phase == 0)
            what = "formatting did not finish: what was on the partition may be"
                   " partly overwritten. Format it again";
        break;
    case S_MKLABEL:
        if (s->phase == 1)
            what = "the new table was written; only telling the kernel failed."
                   " Restart before using the disk";
        break;
    default:
        break;
    }
    snprintf(line, sizeof line, "  %s.", what);
    reply("state", line);
    for (int i = failed_at; i < nsteps; i++) {
        snprintf(line, sizeof line, "not started: %d. %s", i + 1, steps[i].desc);
        reply("state", line);
    }
    char seen[6][32];
    int ns = 0;
    for (int i = 0; i < failed_at && i < nsteps; i++) {
        bool dup = false;
        for (int k = 0; k < ns; k++) if (!strcmp(seen[k], steps[i].disk)) dup = true;
        if (!dup && ns < 6) {
            strlcpy(seen[ns++], steps[i].disk, 32);
            state_table(steps[i].disk);
        }
    }
}

/* Split the plan's fields into steps and play them all on the model.
 * Returns false (reason set) if any step would fail. */
static bool plan_parse(char **f, int nf, char *why, size_t whyn)
{
    plan_code = "invalid";
    memset(models, 0, sizeof models);
    nsteps = 0;
    int i = 0;
    while (i < nf) {
        int j = i;
        while (j < nf && strcmp(f[j], "|") != 0) j++;
        if (j == i) {
            snprintf(why, whyn, "an empty step");
            return false;
        }
        if (nsteps >= MAX_STEPS) {
            snprintf(why, whyn, "at most %d steps in one plan", MAX_STEPS);
            return false;
        }
        char w[300];
        if (!plan_step(&steps[nsteps], nsteps + 1, f + i, j - i, w, sizeof w)) {
            snprintf(why, whyn, "step %d: %s", nsteps + 1, w);
            return false;
        }
        /* Flags are checked against the model here too. */
        step_t *s = &steps[nsteps];
        if (s->kind == S_FLAGS) {
            table_t *t = model_of(s->disk);
            pent_t *p = t ? table_num(t, s->num) : NULL;
            char type[48];
            u64 attrs;
            if (!p || !flags_compute(t, p, s->flags, type, sizeof type, &attrs, w, sizeof w)) {
                snprintf(why, whyn, "step %d: %s", nsteps + 1, p ? w : "no such partition");
                return false;
            }
            strlcpy(p->type, type, sizeof p->type);
            p->attrs = attrs;
        }
        nsteps++;
        i = j + 1;
    }
    if (nsteps == 0) {
        snprintf(why, whyn, "the plan is empty");
        return false;
    }
    return true;
}

static void plan_run(void)
{
    char line[500];
    snprintf(line, sizeof line, "%d", nsteps);
    reply("plan", line);
    for (int i = 0; i < nsteps; i++) {
        snprintf(line, sizeof line, "%d %d %s", i + 1, nsteps, steps[i].desc);
        reply("describe", line);
    }
    for (int i = 0; i < nsteps; i++) {
        if (cancel_req) {
            snprintf(fail_text, sizeof fail_text, "cancelled before step %d; the"
                     " disk is consistent after step %d", i + 1, i);
            strlcpy(fail_why, "cancelled", sizeof fail_why);
            job_rc = 1;
            state_report(i + 1);
            finish_fail();
            return;
        }
        cur_step = i + 1;
        snprintf(line, sizeof line, "%d %d %s", i + 1, nsteps, steps[i].desc);
        reply("step", line);
        snprintf(line, sizeof line, "step %d/%d: %s", i + 1, nsteps, steps[i].desc);
        audit(line);
        strlcpy(job_desc, line, sizeof job_desc);
        progress_last = -1;
        pct_lo = 0; pct_hi = 100; pct_text = "";
        progress(0, steps[i].desc);
        if (!exec_step(&steps[i])) {
            state_report(i + 1);
            finish_fail();
            return;
        }
        progress(100, "done");
        snprintf(line, sizeof line, "%d", i + 1);
        reply("stepdone", line);
    }
    snprintf(line, sizeof line, "%d step%s finished", nsteps, nsteps == 1 ? "" : "s");
    done(line);
}

/* Finish a move a power cut interrupted, from its journal. */
static void resume_move(void)
{
    journal_t j;
    if (!journal_read(&j)) {
        done("no interrupted move");
        return;
    }
    static table_t t;
    if (!blk_exists(j.disk) || !table_read(j.disk, &t) || strcmp(t.id, j.diskid)) {
        failed("failed", "the disk the move was on is not here (or has a"
               " different partition table now)");
        finish_fail();
        return;
    }
    pent_t *p = table_uuid(&t, j.partuuid);
    u64 lss = t.lss;
    if (!p || (p->start * lss != j.from && p->start * lss != j.to) ||
        p->size * lss != j.len) {
        failed("failed", "the partition in the journal does not match the table;"
               " not touching anything");
        finish_fail();
        return;
    }
    char kn[40];
    part_name(j.disk, p->num, kn, sizeof kn);
    mounts_refresh();
    bool vital;
    char why[300];
    if (in_use(kn, &vital, why, sizeof why)) {
        failed("refused", why);
        finish_fail();
        return;
    }
    char msg[200], a[32];
    human(j.done, a, sizeof a);
    snprintf(msg, sizeof msg, "resuming the move of %s: %s were already copied", kn, a);
    say(msg);
    audit(msg);
    if (j.phase == 0 && p->start * lss == j.from) {
        bool clean;
        pct_lo = 0; pct_hi = 90;
        cancel_req = 0;
        if (!move_copy(&j, &clean)) {
            finish_fail();
            return;
        }
        j.phase = 1;
        journal_write(&j);
    }
    if (p->start * lss != j.to && !tbl_geom(j.disk, p->num, j.to / lss, p->size)) {
        finish_fail();
        return;
    }
    lp_unlink(JOURNAL_PATH);
    if (!sync_or_fail(j.disk)) {
        finish_fail();
        return;
    }
    progress(100, "moved");
    done("the move is finished");
}

/* ═══════════════════════════════════════════════════════════════════
 * What is there: disks, partitions, free space
 *
 * One "disk" record per disk, then one "part" record per table entry
 * (or one for a filesystem written straight onto a disk with no table),
 * then one "free" record per gap a new partition could go into. Every
 * field is key=value and every number is bytes, so the application
 * never has to know a sector size, and a field added later breaks no
 * reader.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    char   s[2300];
    size_t n;
} kv_t;

static void kv_str(kv_t *k, const char *key, const char *val)
{
    size_t cap = sizeof k->s - 1;
    if (k->n && k->n < cap) k->s[k->n++] = '\t';
    for (const char *p = key; *p && k->n < cap; p++) k->s[k->n++] = *p;
    if (k->n < cap) k->s[k->n++] = '=';
    for (const char *p = val ? val : ""; *p && k->n < cap; p++) {
        unsigned char c = (unsigned char)*p;
        k->s[k->n++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    }
    k->s[k->n] = '\0';
}

static void kv_u64(kv_t *k, const char *key, u64 v)
{
    char b[24];
    snprintf(b, sizeof b, "%llu", (unsigned long long)v);
    kv_str(k, key, b);
}

static void kv_int(kv_t *k, const char *key, long v)
{
    char b[24];
    snprintf(b, sizeof b, "%ld", v);
    kv_str(k, key, b);
}

/* The first device built on this one: the open LUKS mapping of a
 * crypto_LUKS partition is its holder, dm-N. */
static bool first_holder(const char *name, char *out, size_t n)
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
        if (nm[0] != '.') {
            strlcpy(out, nm, n);
            return true;
        }
    }
    return false;
}

static void list_part(const table_t *t, const pent_t *p, const char *kn)
{
    static kv_t k;
    k.n = 0;
    k.s[0] = '\0';
    bool exists = blk_exists(kn);
    probe_t pr;
    memset(&pr, 0, sizeof pr);
    if (exists)
        probe_dev(kn, &pr);
    u64 lss = t->lss;
    u64 start = p ? p->start * lss : 0;
    u64 size = p ? p->size * lss : blk_bytes(kn);
    kv_str(&k, "disk", t->disk);
    kv_str(&k, "name", kn);
    kv_int(&k, "num", p ? p->num : 0);
    kv_u64(&k, "start", start);
    kv_u64(&k, "size", size);
    if (p) {
        const ptype_t *ty = ptype_find(p->type);
        char fl[200];
        flags_str(t, p, fl, sizeof fl);
        const char *tag = protected_tag(t, p);
        kv_str(&k, "type", p->type);
        kv_str(&k, "typename", ty ? ty->name : "other");
        kv_str(&k, "typedesc", ty ? ty->desc : p->type);
        kv_str(&k, "partuuid", p->uuid);
        kv_str(&k, "partlabel", p->name);
        kv_str(&k, "flags", fl);
        kv_int(&k, "aligned", start % MIB == 0);
        kv_int(&k, "logical", p->logical);
        kv_str(&k, "protected", tag ? tag : "");
    }
    kv_str(&k, "fs", pr.fs);
    kv_str(&k, "label", pr.label);
    kv_str(&k, "uuid", pr.uuid);
    kv_str(&k, "state", pr.state);
    kv_u64(&k, "fssize", pr.size);
    const char *at = exists ? mounted_at(kn) : NULL;
    u64 used = pr.used;
    bool known = pr.used_known;
    if (at) {
        /* Mounted: the kernel's count is the live one; the superblock's
         * is only brought up to date at unmount. */
        u64 fr = 0, tot = 0;
        if (lp_fs_space(at, &fr, &tot) == 0 && tot >= fr) {
            used = tot - fr;
            known = true;
        }
    }
    if (known)
        kv_u64(&k, "used", used);
    kv_str(&k, "mount", at ? at : "");
    bool vital = false;
    char why[300] = "";
    const char *iu = exists ? in_use(kn, &vital, why, sizeof why) : NULL;
    kv_str(&k, "inuse", iu ? iu : "");
    kv_str(&k, "why", iu ? why : "");
    if (p) {
        char sp[96];
        snprintf(sp, sizeof sp, "/sys/class/block/%s/size", kn);
        kv_int(&k, "kernel", exists && sys_u64(sp) * 512 == size);
    }
    char h[32];
    if (!strcmp(pr.fs, "crypto_LUKS") && exists && first_holder(kn, h, sizeof h)) {
        probe_t in;
        probe_dev(h, &in);
        const char *iat = mounted_at(h);
        kv_str(&k, "inner", h);
        kv_str(&k, "innerfs", in.fs);
        kv_str(&k, "innerlabel", in.label);
        kv_str(&k, "innermount", iat ? iat : "");
    }
    record("part", k.s);
}

/* The gaps between partitions, each starting on the next MiB and
 * running to the sector before the next partition (or the last usable
 * one): exactly the range a "create" step will accept. */
static void list_free(const table_t *t)
{
    if (!strcmp(t->kind, "none") || t->last <= t->first)
        return;
    u64 al = ALIGN_BYTES / t->lss;
    if (al == 0) al = 1;
    int order[MAX_PARTS];
    int n = 0;
    for (int i = 0; i < t->n; i++)
        order[n++] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && t->p[order[j - 1]].start > t->p[order[j]].start; j--) {
            int x = order[j]; order[j] = order[j - 1]; order[j - 1] = x;
        }
    u64 cur = t->first;
    for (int i = 0; i <= n; i++) {
        u64 end = i < n ? t->p[order[i]].start : t->last + 1;   /* exclusive */
        u64 s = align_up(cur, al);
        if (end > s && end - s >= al) {
            kv_t k;
            k.n = 0;
            k.s[0] = '\0';
            kv_str(&k, "disk", t->disk);
            kv_u64(&k, "start", s * t->lss);
            kv_u64(&k, "size", (end - s) * t->lss);
            record("free", k.s);
        }
        if (i < n) {
            u64 pe = t->p[order[i]].start + t->p[order[i]].size;
            if (pe > cur) cur = pe;
        }
    }
}

static void list_disk(const char *disk)
{
    static table_t t;
    static kv_t k;
    char v[128], p[96], tr[16];
    table_read(disk, &t);
    k.n = 0;
    k.s[0] = '\0';
    kv_str(&k, "name", disk);
    kv_u64(&k, "size", blk_bytes(disk));
    blk_model(disk, v, sizeof v);
    kv_str(&k, "model", v);
    blk_transport(disk, tr, sizeof tr);
    kv_str(&k, "transport", tr);
    kv_int(&k, "removable", blk_removable(disk));
    snprintf(p, sizeof p, "/sys/block/%s/queue/rotational", disk);
    kv_int(&k, "rotational", starts(disk, "loop") ? 0 : (long)sys_u64(p));
    snprintf(p, sizeof p, "/sys/block/%s/ro", disk);
    kv_int(&k, "ro", (long)sys_u64(p));
    kv_str(&k, "table", t.kind);
    kv_str(&k, "id", t.id);
    kv_u64(&k, "lss", t.lss);
    kv_u64(&k, "first", t.first * t.lss);
    kv_u64(&k, "end", (t.last + 1) * t.lss);
    kv_int(&k, "system", disk_is_system(disk));
    kv_str(&k, "err", t.err);
    record("disk", k.s);
    if (!strcmp(t.kind, "none")) {
        probe_t pr;
        if (probe_dev(disk, &pr) && pr.fs[0])
            list_part(&t, NULL, disk);
        return;
    }
    for (int i = 0; i < t.n; i++) {
        char kn[40];
        part_name(disk, t.p[i].num, kn, sizeof kn);
        list_part(&t, &t.p[i], kn);
    }
    list_free(&t);
}

static void answer_list(const char *only)
{
    mounts_refresh();
    static char names[256][32];
    int n = blk_all(names, 256);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char x[32];
            strlcpy(x, names[j], 32);
            strlcpy(names[j], names[j - 1], 32);
            strlcpy(names[j - 1], x, 32);
        }
    for (int i = 0; i < n; i++) {
        if (!blk_listable(names[i]))
            continue;
        if (only && strcmp(only, names[i]) != 0)
            continue;
        list_disk(names[i]);
    }
    journal_t j;
    if (journal_read(&j)) {
        kv_t k;
        k.n = 0;
        k.s[0] = '\0';
        kv_str(&k, "disk", j.disk);
        kv_str(&k, "partuuid", j.partuuid);
        kv_u64(&k, "done", j.done);
        kv_u64(&k, "len", j.len);
        record("journal", k.s);
    }
    reply("done", "");
}

/* ═══════════════════════════════════════════════════════════════════
 * The other verbs
 *
 * Each ends the answer itself with exactly one "done" or "fail" line.
 * The ones that only read (minsize, smart) are answered in a process
 * of their own so that a SMART query does not wait for a resize; the
 * ones that write run as the one job at a time.
 * ═══════════════════════════════════════════════════════════════════ */

static void fail_now(const char *why, const char *text)
{
    failed(why, text);
    finish_fail();
}

/* ── How small can it get ─────────────────────────────────────────── */

static void v_minsize(char **a, int n)
{
    (void)n;
    const char *kn = a[0];
    probe_t pr;
    if (!probe_dev(kn, &pr)) {
        fail_now("failed", "cannot read the device");
        return;
    }
    fstype_t f = fs_of_probe(&pr);
    const char *no_shrink = pr.fs[0] && f == FS_NONE ? "unknown filesystem"
                                                     : resize_refusal(f, true);
    const char *no_grow = pr.fs[0] && f == FS_NONE ? "unknown filesystem"
                                                   : resize_refusal(f, false);
    char bin[64];
    if (f == FS_FAT32 && !tool("fatresize", bin, sizeof bin))
        no_shrink = no_grow = "resizing FAT needs fatresize (Debian package"
                              " fatresize), which is not installed";
    u64 min = 0;
    if (!no_shrink && f != FS_NONE && f != FS_SWAP)
        min = fs_min_size(kn, &pr);
    if (f == FS_SWAP || f == FS_NONE)
        min = MIB;
    /* Whatever the tool says, never offer less than it plus a margin
     * for the journal and metadata growth after the next write. */
    if (min && f != FS_NONE && f != FS_SWAP)
        min = align_up(min + min / 50 + 8 * MIB, MIB);
    kv_t k;
    k.n = 0;
    k.s[0] = '\0';
    kv_str(&k, "name", kn);
    kv_str(&k, "fs", pr.fs);
    kv_u64(&k, "fssize", pr.size);
    if (pr.used_known) kv_u64(&k, "used", pr.used);
    kv_u64(&k, "min", min);
    kv_int(&k, "shrink", no_shrink == NULL && (min > 0 || f == FS_NONE));
    kv_int(&k, "grow", no_grow == NULL);
    kv_str(&k, "noshrink", no_shrink ? no_shrink : "");
    kv_str(&k, "nogrow", no_grow ? no_grow : "");
    record("resize", k.s);
    done("");
}

/* ── Health (SMART) ───────────────────────────────────────────────────
 *
 * smartctl knows every kind of drive and is what the numbers come from
 * when it is installed; `nvme smart-log` is the fallback for the NVMe
 * disk. The verdict is ours, in three words a person can act on:
 *
 *   good     the drive reports nothing wrong
 *   warning  it has started to wear or has bad sectors: keep the backup
 *            current, plan to replace it
 *   failing  the drive itself says it is about to fail: copy the files
 *            off now
 *
 * and "unknown" for what does not report (loop devices, most USB
 * adapters, SD cards). The reasons go out as codes the application
 * translates. */

static struct {
    char health[16];             /* PASSED FAILED OK "" */
    long temp, hours, pct_used, realloc, pending, uncorr, media, crit;
    char model[80], serial[64], fw[40];
    bool any;
} sm;

static long num_after(const char *s)
{
    while (*s && (*s < '0' || *s > '9')) s++;
    long v = 0;
    bool seen = false;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); seen = true; }
        else if (*s == ',' && seen) continue;
        else break;
    }
    return seen ? v : -1;
}

static void val_after_colon(const char *s, char *out, size_t n)
{
    const char *c = strchr(s, ':');
    if (!c) return;
    c++;
    while (*c == ' ') c++;
    strlcpy(out, c, n);
}

static void smart_line(char *s)
{
    say(s);
    char *c;
    if ((c = strstr(s, "self-assessment test result:")) != NULL) {
        sm.any = true;
        strlcpy(sm.health, strstr(c, "PASSED") ? "PASSED" :
                           strstr(c, "FAILED") ? "FAILED" : "", sizeof sm.health);
    } else if (starts(s, "SMART Health Status:")) {
        sm.any = true;
        strlcpy(sm.health, strstr(s, "OK") ? "OK" : "FAILED", sizeof sm.health);
    } else if (starts(s, "Device Model:") || starts(s, "Model Number:") ||
               starts(s, "Product:")) {
        val_after_colon(s, sm.model, sizeof sm.model);
    } else if (starts(s, "Serial Number:") || starts(s, "Serial number:")) {
        val_after_colon(s, sm.serial, sizeof sm.serial);
    } else if (starts(s, "Firmware Version:")) {
        val_after_colon(s, sm.fw, sizeof sm.fw);
    } else if (starts(s, "Critical Warning:") || starts(s, "critical_warning")) {
        sm.any = true;
        const char *v = strchr(s, ':');
        sm.crit = v ? strtol(v + 1, NULL, 0) : 0;
    } else if (starts(s, "Temperature:") || starts(s, "temperature ") ||
               starts(s, "Current Drive Temperature:")) {
        sm.temp = num_after(strchr(s, ':') ? strchr(s, ':') : s);
    } else if (starts(s, "Percentage Used:") || starts(s, "percentage_used")) {
        sm.pct_used = num_after(strchr(s, ':'));
    } else if (starts(s, "Media and Data Integrity Errors:") || starts(s, "media_errors")) {
        sm.media = num_after(strchr(s, ':'));
    } else if (starts(s, "Power On Hours:") || starts(s, "power_on_hours")) {
        sm.hours = num_after(strchr(s, ':'));
    } else {
        /* An ATA attribute row: ID NAME FLAG VALUE WORST THRESH TYPE
         * UPDATED WHEN_FAILED RAW - the raw value is the tenth field. */
        char *t = s;
        while (*t == ' ') t++;
        long id = num_after(t);
        if (id <= 0 || id > 255 || t[0] < '0' || t[0] > '9')
            return;
        char *f[12];
        int nf = 0;
        char tmp[300];
        strlcpy(tmp, t, sizeof tmp);
        for (char *q = tmp; *q && nf < 12; ) {
            while (*q == ' ') q++;
            if (!*q) break;
            f[nf++] = q;
            while (*q && *q != ' ') q++;
            if (*q) *q++ = '\0';
        }
        if (nf < 10)
            return;
        long raw = num_after(f[9]);
        sm.any = true;
        switch (id) {
        case 5:   sm.realloc = raw; break;
        case 9:   sm.hours = raw; break;
        case 190: if (sm.temp < 0) sm.temp = raw; break;
        case 194: sm.temp = raw; break;
        case 197: sm.pending = raw; break;
        case 198: sm.uncorr = raw; break;
        default: break;
        }
    }
}

static void v_smart(char **a, int n)
{
    (void)n;
    char disk[32], dev[64], bin[64];
    blk_disk(a[0], disk, sizeof disk);
    memset(&sm, 0, sizeof sm);
    sm.temp = sm.hours = sm.pct_used = sm.realloc = sm.pending = sm.uncorr =
        sm.media = -1;
    if (!dev_path(disk, dev, sizeof dev)) {
        fail_now("failed", "the disk is gone");
        return;
    }
    const char *used = "none";
    if (!starts(disk, "loop") && tool("smartctl", bin, sizeof bin)) {
        char *argv[] = { bin, "-H", "-i", "-A", dev, NULL };
        used = "smartctl";
        capture_run(argv, smart_line);
    }
    if (!sm.any && starts(disk, "nvme") && tool("nvme", bin, sizeof bin)) {
        /* nvme0n1 -> the controller, nvme0. */
        char ctl[64];
        snprintf(ctl, sizeof ctl, "/dev/%s", disk);
        char *pn = strchr(ctl + 9, 'n');
        if (pn) *pn = '\0';
        char *argv[] = { bin, "smart-log", ctl, NULL };
        used = "nvme";
        capture_run(argv, smart_line);
        if (sm.any && !sm.health[0])
            strlcpy(sm.health, sm.crit ? "FAILED" : "PASSED", sizeof sm.health);
    }

    const char *verdict = "unknown";
    char reasons[160] = "";
    #define WHY(s) do { if (reasons[0]) strlcat(reasons, ",", sizeof reasons); strlcat(reasons, s, sizeof reasons); } while (0)
    if (sm.any) {
        verdict = "good";
        if (sm.realloc > 0)  { verdict = "warning"; WHY("realloc"); }
        if (sm.pending > 0)  { verdict = "warning"; WHY("pending"); }
        if (sm.uncorr > 0)   { verdict = "warning"; WHY("uncorrectable"); }
        if (sm.media > 0)    { verdict = "warning"; WHY("media"); }
        if (sm.pct_used >= 90) { verdict = "warning"; WHY("worn"); }
        if (sm.crit > 0 && sm.crit != 2) { verdict = "failing"; WHY("critical"); }
        else if (sm.crit == 2) { WHY("hot"); if (!strcmp(verdict, "good")) verdict = "warning"; }
        if (!strcmp(sm.health, "FAILED")) { verdict = "failing"; WHY("selftest"); }
    } else {
        WHY(starts(disk, "loop") ? "loop" : !strcmp(used, "none") ? "notool" : "noreport");
    }
    #undef WHY
    kv_t k;
    k.n = 0;
    k.s[0] = '\0';
    kv_str(&k, "disk", disk);
    kv_str(&k, "verdict", verdict);
    kv_str(&k, "reasons", reasons);
    kv_str(&k, "tool", used);
    kv_str(&k, "model", sm.model);
    kv_str(&k, "serial", sm.serial);
    kv_str(&k, "firmware", sm.fw);
    kv_int(&k, "temp", sm.temp);
    kv_int(&k, "hours", sm.hours);
    kv_int(&k, "pct_used", sm.pct_used);
    kv_int(&k, "realloc", sm.realloc);
    kv_int(&k, "pending", sm.pending);
    kv_int(&k, "uncorrectable", sm.uncorr);
    kv_int(&k, "media_errors", sm.media);
    kv_int(&k, "critical", sm.crit);
    record("smart", k.s);
    done(verdict);
}

/* ── Benchmark ────────────────────────────────────────────────────────
 *
 * Reads only, never writes, so it is allowed on a mounted partition.
 * O_DIRECT, so the numbers are the disk's and not the page cache's; half
 * the time on 1 MiB sequential reads, half on 4 KiB reads at random
 * places, which is what starting programs and opening folders is. Time
 * boxed, because a benchmark of a 2 TB disk must not read 2 TB. */

static void v_bench(char **a, int n)
{
    const char *kn = a[0];
    u64 secs = 10;
    if (n > 1) parse_u64(a[1], &secs);
    if (secs < 2) secs = 2;
    if (secs > 60) secs = 60;
    char dev[64];
    if (!dev_path(kn, dev, sizeof dev)) {
        fail_now("failed", "the device is gone");
        return;
    }
    u64 size = blk_bytes(kn);
    if (size < 8 * MIB) {
        fail_now("refused", "the device is too small to measure");
        return;
    }
    bool direct = true;
    long fd = lp_open(dev, O_RDONLY | O_DIRECT | O_CLOEXEC, 0);
    if (fd < 0) {
        direct = false;
        fd = lp_open(dev, O_RDONLY | O_CLOEXEC, 0);
        if (fd >= 0) lp_ioctl((int)fd, BLKFLSBUF_, NULL);
    }
    if (fd < 0) {
        fail_now("failed", "cannot open the device");
        return;
    }
    u8 *raw = malloc(MIB + 4096);
    if (!raw) {
        lp_close((int)fd);
        fail_now("failed", "out of memory");
        return;
    }
    u8 *buf = (u8 *)(((unsigned long)raw + 4095) & ~4095ul);
    say(direct ? "reading with O_DIRECT (the page cache is bypassed)"
               : "this device refuses O_DIRECT; the cache was dropped first");

    s64 half = (s64)secs * 500;
    s64 t0 = lp_monotonic_ms(), last = 0;
    u64 seq = 0, off = 0;
    while (lp_monotonic_ms() - t0 < half && !cancel_req) {
        if (off + MIB > size) off = 0;
        if (!read_at((int)fd, off, buf, MIB))
            break;
        off += MIB;
        seq += MIB;
        s64 now = lp_monotonic_ms();
        if (now - last > 250) {
            last = now;
            progress((int)((now - t0) * 50 / half), "sequential read");
        }
    }
    s64 seq_ms = lp_monotonic_ms() - t0;

    u64 x = (u64)lp_monotonic_ms() * 0x9E3779B97F4A7C15ull;
    lp_getrandom(&x, sizeof x, 0);
    x |= 1;
    u64 ops = 0, blocks = size / 4096;
    s64 t1 = lp_monotonic_ms();
    while (lp_monotonic_ms() - t1 < half && !cancel_req) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        if (!read_at((int)fd, (x % blocks) * 4096, buf, 4096))
            break;
        ops++;
        s64 now = lp_monotonic_ms();
        if (now - last > 250) {
            last = now;
            progress(50 + (int)((now - t1) * 50 / half), "random 4 KiB reads");
        }
    }
    s64 rnd_ms = lp_monotonic_ms() - t1;
    lp_close((int)fd);
    free(raw);
    if (cancel_req) {
        fail_now("cancelled", "the benchmark was stopped");
        return;
    }
    kv_t k;
    k.n = 0;
    k.s[0] = '\0';
    kv_str(&k, "name", kn);
    kv_int(&k, "direct", direct);
    kv_u64(&k, "seq_bytes", seq);
    kv_int(&k, "seq_ms", (long)seq_ms);
    kv_u64(&k, "seq_mbps", seq_ms > 0 ? seq * 1000 / (u64)seq_ms / MIB : 0);
    kv_u64(&k, "rand_ops", ops);
    kv_int(&k, "rand_ms", (long)rnd_ms);
    kv_u64(&k, "rand_iops", rnd_ms > 0 ? ops * 1000 / (u64)rnd_ms : 0);
    kv_u64(&k, "rand_lat_us", ops ? (u64)rnd_ms * 1000 / ops : 0);
    record("bench", k.s);
    done("");
}

/* ── Images ───────────────────────────────────────────────────────────
 *
 * The file is the client's: it opened it, with its own permissions, and
 * passed the descriptor with the request (see the top). Only regular
 * files are accepted - a descriptor for a block device or a pipe would
 * turn "save an image" into "copy my disk onto yours". */

static int passed_fd = -1;           /* SCM_RIGHTS, from the request */

static bool passed_file(u64 *size, bool want_write)
{
    if (passed_fd < 0)
        return failed("invalid", "send the image file's descriptor with the"
                      " request"), false;
    char p[40];
    snprintf(p, sizeof p, "/proc/self/fd/%d", passed_fd);
    lp_stat_t st;
    if (lp_stat(p, &st, true) < 0 || (st.mode & LP_S_IFMT) != LP_S_IFREG)
        return failed("invalid", "the image has to be an ordinary file"), false;
    /* A zero-length transfer checks the descriptor's mode and moves
     * nothing: EBADF when it was not opened for that direction. */
    char z = 0;
    long r = want_write ? lp_write(passed_fd, &z, 0) : lp_read(passed_fd, &z, 0);
    if (r < 0)
        return failed("invalid", want_write ? "the image file is not open for writing"
                                            : "the image file is not open for reading"), false;
    *size = (u64)st.size;
    return true;
}

static void v_image(bool save, char **a, int n)
{
    const char *kn = a[0];
    char confirm[40] = "";
    if (n > 1 && starts(a[1], "confirm="))
        strlcpy(confirm, a[1] + 8, sizeof confirm);
    mounts_refresh();
    bool vital;
    char why[300];
    if (in_use(kn, &vital, why, sizeof why)) {
        fail_now("refused", why);
        return;
    }
    u64 fsize = 0;
    if (!passed_file(&fsize, save)) {
        finish_fail();
        return;
    }
    u64 psize = blk_bytes(kn);
    if (!save) {
        if (fsize == 0 || fsize > psize) {
            char msg[160], x[32], y[32];
            human(fsize, x, sizeof x);
            human(psize, y, sizeof y);
            snprintf(msg, sizeof msg, "the image (%s) does not fit on %s (%s)", x, kn, y);
            fail_now("refused", fsize ? msg : "the image file is empty");
            return;
        }
        if (blk_is_part(kn)) {
            static table_t t;
            char disk[32];
            blk_disk(kn, disk, sizeof disk);
            pent_t *p = table_read(disk, &t) ? table_num(&t, blk_partno(kn)) : NULL;
            const char *tag = p ? protected_tag(&t, p) : NULL;
            if (tag && strcmp(confirm, tag)) {
                snprintf(why, sizeof why, "%s is the %s; restoring over it needs the"
                         " typed confirmation", kn, tag);
                fail_now("refused", why);
                return;
            }
        }
    }
    char dev[64];
    if (!dev_path(kn, dev, sizeof dev)) {
        fail_now("failed", "the device is gone");
        return;
    }
    long dfd = lp_open(dev, (save ? O_RDONLY : O_RDWR) | O_CLOEXEC, 0);
    if (dfd < 0) {
        fail_now("failed", "cannot open the device");
        return;
    }
    u64 len = save ? psize : fsize;
    char msg[200], hs[32];
    human(len, hs, sizeof hs);
    snprintf(msg, sizeof msg, "%s %s %s", save ? "saving" : "restoring", hs,
             save ? "into the image file" : "onto the partition");
    say(msg);
    audit(msg);
    if (save) lp_ftruncate(passed_fd, 0);
    lp_lseek(passed_fd, 0, SEEK_SET);
    lp_lseek((int)dfd, 0, SEEK_SET);
    u8 *buf = malloc(4 * MIB);
    if (!buf) {
        lp_close((int)dfd);
        fail_now("failed", "out of memory");
        return;
    }
    static lp_digest_t d;
    lp_digest_init(&d, LP_SHA256);
    int src = save ? (int)dfd : passed_fd, dst = save ? passed_fd : (int)dfd;
    u64 done_b = 0;
    s64 last = 0;
    bool ok = true;
    while (done_b < len) {
        if (cancel_req) {
            ok = false;
            failed("cancelled", save ? "stopped: the image file is incomplete -"
                                       " delete it"
                                     : "stopped: the partition is now partly"
                                       " overwritten - restore again or format it");
            break;
        }
        size_t c = len - done_b < 4 * MIB ? (size_t)(len - done_b) : 4 * MIB;
        size_t got = 0;
        while (got < c) {
            long r = lp_read(src, buf + got, c - got);
            if (r == -EINTR_) continue;
            if (r <= 0) break;
            got += (size_t)r;
        }
        if (got != c) {
            ok = false;
            failed("failed", save ? "a read error on the device stopped the copy"
                                  : "the image file could not be read");
            break;
        }
        size_t put = 0;
        while (put < c) {
            long w = lp_write(dst, buf + put, c - put);
            if (w == -EINTR_) continue;
            if (w <= 0) break;
            put += (size_t)w;
        }
        if (put != c) {
            ok = false;
            failed("failed", save ? "the image file could not be written (is the"
                                    " disk it is on full?)"
                                  : "a write error on the device stopped the restore");
            break;
        }
        lp_digest_update(&d, buf, c);
        done_b += c;
        s64 now = lp_monotonic_ms();
        if (now - last > 250) {
            last = now;
            progress((int)(done_b * 100 / len), save ? "saving the image" : "restoring");
        }
    }
    free(buf);
    lp_fsync(dst);
    if (!save) lp_ioctl((int)dfd, BLKFLSBUF_, NULL);
    lp_close((int)dfd);
    if (!ok) {
        finish_fail();
        return;
    }
    char hex[80];
    lp_digest_final(&d, hex);
    kv_t k;
    k.n = 0;
    k.s[0] = '\0';
    kv_str(&k, "name", kn);
    kv_u64(&k, "bytes", done_b);
    kv_str(&k, "sha256", hex);
    record("image", k.s);
    done(save ? "the image is saved" : "the image is restored");
}

static void v_image_save(char **a, int n)    { v_image(true, a, n); }
static void v_image_restore(char **a, int n) { v_image(false, a, n); }

/* ── Mounting, and the drive going away ───────────────────────────────
 *
 * The same places and rules as automount and lp-privd: /media/<label>,
 * the label cleaned so it cannot climb out of /media, and FAT, exFAT
 * and NTFS - which have no owners - handed to whoever asked. */

static void safe_name(const char *label, const char *fallback, char *out, size_t n)
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
        if (!strcmp(fs, "vfat"))
            snprintf(data, sizeof data, "uid=%u,gid=%u,umask=022,utf8=1,"
                     "shortname=mixed,flush", caller_uid, caller_gid);
        else
            snprintf(data, sizeof data, "uid=%u,gid=%u,umask=022,iocharset=utf8",
                     caller_uid, caller_gid);
        opts = data;
    }
    return lp_mount(dev, point, fs, flags, opts);
}

/* Mount `name` (a partition, or the open mapping of an encrypted one). */
static void v_mount(char **a, int n)
{
    (void)n;
    char name[32];
    strlcpy(name, a[0], sizeof name);
    probe_t p;
    probe_dev(name, &p);
    if (!strcmp(p.fs, "crypto_LUKS")) {
        char h[32];
        if (!first_holder(name, h, sizeof h)) {
            fail_now("refused", "the encrypted partition is locked; unlock it first");
            return;
        }
        strlcpy(name, h, sizeof name);
        probe_dev(name, &p);
    }
    mounts_refresh();
    const char *at = mounted_at(name);
    if (at) {
        done(at);
        return;
    }
    bool vital;
    char why[300];
    const char *iu = in_use(name, &vital, why, sizeof why);
    if (iu) {
        fail_now("refused", why);
        return;
    }
    if (!p.fs[0] || !strcmp(p.fs, "swap")) {
        fail_now("refused", "there is no filesystem here that can be mounted");
        return;
    }
    char dev[64];
    if (!dev_path(name, dev, sizeof dev)) {
        fail_now("failed", "the device is gone");
        return;
    }
    char clean[48], point[96];
    safe_name(p.label, name, clean, sizeof clean);
    if (!pick_point(clean, point, sizeof point)) {
        fail_now("failed", "no free mount point under /media");
        return;
    }
    lp_mkdir("/media", 0755);
    if (lp_mkdir(point, 0755) < 0 && !lp_is_dir(point)) {
        fail_now("failed", "cannot create the mount point");
        return;
    }
    char line[200];
    snprintf(line, sizeof line, "mount %s %s (%s)", dev, point, p.fs);
    audit(line);
    unsigned long flags = MS_NOSUID | MS_NODEV;
    long r = -1;
    const char *used = p.fs;
    if (starts(p.fs, "ext") || !strcmp(p.fs, "btrfs")) {
        r = try_mount(dev, point, p.fs, flags, false);
    } else if (!strcmp(p.fs, "vfat") || !strcmp(p.fs, "exfat")) {
        r = try_mount(dev, point, p.fs, flags, true);
    } else if (!strcmp(p.fs, "ntfs")) {
        used = "ntfs3";
        r = try_mount(dev, point, "ntfs3", flags, true);
        char ntfs3g[64];
        if (r < 0 && tool("ntfs-3g", ntfs3g, sizeof ntfs3g)) {
            char opts[96];
            snprintf(opts, sizeof opts, "uid=%u,gid=%u,umask=022,nosuid,nodev",
                     caller_uid, caller_gid);
            char *argv[] = { ntfs3g, dev, point, "-o", opts, NULL };
            used = "ntfs-3g";
            r = run(argv) == 0 ? 0 : -1;
        }
    } else if (!strcmp(p.fs, "iso9660")) {
        r = try_mount(dev, point, p.fs, flags | MS_RDONLY, false);
    }
    if (r < 0 && strcmp(used, "ntfs-3g")) {
        /* A dirty FAT, or an NTFS Windows left hibernated, often mounts
         * read-only when it will not mount writable - said out loud. */
        bool own = !strcmp(p.fs, "vfat") || !strcmp(p.fs, "exfat") || !strcmp(p.fs, "ntfs");
        r = try_mount(dev, point, !strcmp(p.fs, "ntfs") ? "ntfs3" : p.fs,
                      flags | MS_RDONLY, own);
        if (r == 0)
            say("mounted read-only: the filesystem needs checking");
    }
    if (r < 0) {
        lp_rmdir(point);
        char msg[200];
        snprintf(msg, sizeof msg, "the kernel would not mount it (%s, error %ld)", p.fs, -r);
        fail_now("failed", msg);
        return;
    }
    done(point);
}

/* Unmount every mount of one device, newest first. Busy is reported,
 * not forced: a lazy detach would say nothing and leave the drive
 * half-written when it is pulled. The running system's own mounts are
 * never taken down here. */
static bool unmount_dev(const char *name)
{
    u32 maj, min;
    if (!blk_devnum(name, &maj, &min))
        return true;
    bool vital;
    char why[300];
    if (in_use(name, &vital, why, sizeof why) && vital)
        return failed("refused", why);
    for (int i = nmnts - 1; i >= 0; i--) {
        if (mnts[i].maj != maj || mnts[i].min != min)
            continue;
        char line[300];
        snprintf(line, sizeof line, "umount %s", mnts[i].point);
        audit(line);
        lp_sync();
        long r = lp_umount(mnts[i].point, 0);
        if (r == -EBUSY_)
            return failed("busy", "a program still has files open on it");
        if (r < 0) {
            char msg[96];
            snprintf(msg, sizeof msg, "the kernel would not unmount it (error %ld)", -r);
            return failed("failed", msg);
        }
        if (starts(mnts[i].point, "/media/"))
            lp_rmdir(mnts[i].point);
    }
    return true;
}

/* A partition and whatever is mounted from its open LUKS mapping. */
static bool unmount_all_of(const char *name)
{
    char h[32];
    if (first_holder(name, h, sizeof h) && !unmount_dev(h))
        return false;
    return unmount_dev(name);
}

static void v_unmount(char **a, int n)
{
    (void)n;
    mounts_refresh();
    if (!unmount_all_of(a[0])) {
        finish_fail();
        return;
    }
    done("unmounted");
}

static bool cryptsetup_close(const char *part)
{
    char h[32], nm[80], cs[64];
    if (!first_holder(part, h, sizeof h))
        return true;
    char p[96];
    snprintf(p, sizeof p, "/sys/class/block/%s/dm/name", h);
    if (!sys_read(p, nm, sizeof nm) || !starts(nm, "lp-"))
        return failed("refused", "that encrypted volume was opened by something"
                      " else; close it there");
    if (!need_tool("cryptsetup", "cryptsetup", cs, sizeof cs))
        return false;
    char *argv[] = { cs, "close", nm, NULL };
    if (run(argv) != 0)
        return failed("failed", "cryptsetup would not close it");
    return true;
}

/* Everything on the disk unmounted and closed, the caches written out,
 * and then - where the kernel has the switch - the device removed from
 * it: that is what makes a stick's light go out. A loop device is
 * detached from its file instead. */
static bool eject_disk(const char *disk)
{
    mounts_refresh();
    bool vital;
    char why[300];
    if (disk_busy(disk, &vital, why, sizeof why) && vital)
        return failed("refused", why);
    char parts[128][32];
    int np = blk_parts(disk, parts, 128);
    for (int i = 0; i < np; i++)
        if (!unmount_all_of(parts[i]) || !cryptsetup_close(parts[i]))
            return false;
    if (!unmount_dev(disk))
        return false;
    lp_sync();
    flush_dev(disk);
    if (starts(disk, "loop")) {
        char dev[64];
        if (dev_path(disk, dev, sizeof dev)) {
            long fd = lp_open(dev, O_RDONLY | O_CLOEXEC, 0);
            if (fd >= 0) {
                lp_ioctl((int)fd, 0x4C01 /* LOOP_CLR_FD */, NULL);
                lp_close((int)fd);
                audit("  loop device detached");
            }
        }
        return true;
    }
    char p[96];
    snprintf(p, sizeof p, "/sys/block/%s/device/delete", disk);
    long fd = lp_open(p, O_WRONLY | O_CLOEXEC, 0);
    if (fd >= 0) {
        lp_write((int)fd, "1\n", 2);
        lp_close((int)fd);
        audit("  device removed from the kernel");
    }
    return true;
}

static void v_eject(char **a, int n)
{
    (void)n;
    if (!eject_disk(a[0])) {
        finish_fail();
        return;
    }
    done("safe to remove");
}

/* Power off: eject, then have the USB port let go of the device, the
 * way GNOME Disks' power button does. Only USB has that; anything else
 * is ejected and the answer says so. */
static void v_poweroff(char **a, int n)
{
    (void)n;
    char p[160], link[512], usb[512] = "";
    snprintf(p, sizeof p, "/sys/block/%s", a[0]);
    long r = lp_readlink(p, link, sizeof link - 1);
    link[r > 0 ? r : 0] = '\0';
    /* ../devices/pci0000:00/0000:00:14.0/usb2/2-1/2-1:1.0/host3/...:
     * the USB device is the last component before the first ':' one. */
    char *q = strstr(link, "/usb");
    if (q) {
        char *c = strchr(q + 1, '/');
        while (c) {
            char *next = strchr(c + 1, '/');
            char comp[64];
            size_t l = next ? (size_t)(next - c - 1) : strlen(c + 1);
            if (l >= sizeof comp) break;
            memcpy(comp, c + 1, l);
            comp[l] = '\0';
            if (strchr(comp, ':')) {
                size_t pl = (size_t)(c - link);
                snprintf(usb, sizeof usb, "/sys/block/%.*s/remove", (int)pl, link);
                break;
            }
            c = next;
        }
    }
    if (!eject_disk(a[0])) {
        finish_fail();
        return;
    }
    if (usb[0]) {
        /* /sys/block/<disk> + "/" + "../devices/..." resolves to the
         * USB device directory; its remove file disconnects the port. */
        char path[600];
        strlcpy(path, usb, sizeof path);
        char *rel = strstr(path, "../");
        char full[640];
        if (rel) {
            snprintf(full, sizeof full, "/sys/%s", rel + 3);
            long fd = lp_open(full, O_WRONLY | O_CLOEXEC, 0);
            if (fd >= 0) {
                lp_write((int)fd, "1\n", 2);
                lp_close((int)fd);
                audit("  USB device powered off");
                done("powered off");
                return;
            }
        }
    }
    done("safe to remove (this drive has no power switch the system can use)");
}

/* ── Encrypted partitions ───────────────────────────────────────────── */

static bool key_field(const char *f, char *out, size_t outn, size_t *len)
{
    return starts(f, "k:") && unhex(f + 2, out, outn, len) && *len >= 1;
}

static void v_luks_open(char **a, int n)
{
    (void)n;
    const char *kn = a[0];
    char pass[260], cs[64], dev[64], map[48];
    size_t plen = 0;
    if (!key_field(a[1], pass, sizeof pass, &plen)) {
        fail_now("invalid", "the passphrase is hex after k:");
        return;
    }
    probe_t p;
    probe_dev(kn, &p);
    char h[32];
    if (strcmp(p.fs, "crypto_LUKS")) {
        scrub(pass, sizeof pass);
        fail_now("refused", "that is not an encrypted (LUKS) partition");
        return;
    }
    if (first_holder(kn, h, sizeof h)) {
        scrub(pass, sizeof pass);
        done(h);
        return;
    }
    if (!need_tool("cryptsetup", "cryptsetup", cs, sizeof cs) ||
        !dev_path(kn, dev, sizeof dev)) {
        scrub(pass, sizeof pass);
        finish_fail();
        return;
    }
    snprintf(map, sizeof map, "lp-%s", kn);
    char *argv[] = { cs, "open", "--key-file=-", dev, map, NULL };
    int rc = run_in(argv, STAT_NONE, pass, plen, false, true);
    scrub(pass, sizeof pass);
    if (rc == 2) {
        fail_now("auth", "wrong passphrase");
        return;
    }
    if (rc != 0 || !first_holder(kn, h, sizeof h)) {
        fail_now("failed", "cryptsetup could not open it");
        return;
    }
    done(h);
}

static void v_luks_close(char **a, int n)
{
    (void)n;
    mounts_refresh();
    char h[32];
    if (!first_holder(a[0], h, sizeof h)) {
        done("already locked");
        return;
    }
    if (!unmount_dev(h) || !cryptsetup_close(a[0])) {
        finish_fail();
        return;
    }
    done("locked");
}

/* ── /etc/fstab ───────────────────────────────────────────────────────
 *
 * Shown whole; changed only where it concerns data partitions - an entry
 * whose mount point is /mnt/<name> or /media/<name>, or a swap area
 * named by UUID. The root, the ESP and everything the installer wrote
 * are read-only here: a mistake in those lines is a system that does
 * not boot. Entries go in by UUID with "nofail", so a data disk that is
 * missing at boot costs a line in the log and not the boot. Written the
 * persistence way: the old file kept as fstab.lp-diskd.bak, the new one
 * written beside it, fsynced, renamed over, the directory fsynced. */

static char fstab_buf[32768];

static bool fstab_editable(const char *point, const char *fs, const char *spec)
{
    if ((starts(point, "/mnt/") || starts(point, "/media/")) &&
        !strchr(strchr(point + 1, '/') + 1, '/'))
        return true;
    return !strcmp(fs, "swap") && starts(spec, "UUID=");
}

/* Split one fstab line into its fields (in place). */
static int fstab_fields(char *line, char *f[6])
{
    int n = 0;
    for (char *p = line; *p && n < 6; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') break;
        f[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

/* The kernel name of what an fstab spec names, if it is here. */
static bool fstab_resolve(const char *spec, char *out, size_t n)
{
    out[0] = '\0';
    if (starts(spec, "/dev/")) {
        const char *b = spec + 5;
        if (dev_name_ok(b) && blk_exists(b)) { strlcpy(out, b, n); return true; }
        return false;
    }
    const char *eq = strchr(spec, '=');
    if (!eq) return false;
    static char names[256][32];
    int nn = blk_all(names, 256);
    for (int i = 0; i < nn; i++) {
        if (starts(names[i], "ram") || starts(names[i], "zram") || blk_bytes(names[i]) == 0)
            continue;
        if (starts(spec, "UUID=") || starts(spec, "LABEL=")) {
            probe_t p;
            if (!probe_dev(names[i], &p)) continue;
            const char *v = starts(spec, "UUID=") ? p.uuid : p.label;
            if (v[0] && !ieq_not(v, eq + 1)) { strlcpy(out, names[i], n); return true; }
        } else if ((starts(spec, "PARTUUID=") || starts(spec, "PARTLABEL=")) &&
                   blk_is_part(names[i])) {
            static table_t t;
            char disk[32];
            blk_disk(names[i], disk, sizeof disk);
            if (!table_read(disk, &t)) continue;
            pent_t *p = table_num(&t, blk_partno(names[i]));
            if (!p) continue;
            const char *v = starts(spec, "PARTUUID=") ? p->uuid : p->name;
            if (v[0] && !ieq_not(v, eq + 1)) { strlcpy(out, names[i], n); return true; }
        }
    }
    return false;
}

static void answer_fstab(void)
{
    long got = proc_read(FSTAB_PATH, fstab_buf, sizeof fstab_buf - 1);
    if (got < 0) got = 0;
    fstab_buf[got] = '\0';
    int ln = 0;
    for (char *line = fstab_buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        ln++;
        char copy[1024];
        strlcpy(copy, line, sizeof copy);
        char *f[6];
        int nf = fstab_fields(copy, f);
        if (nf >= 2) {
            char dev[32];
            fstab_resolve(f[0], dev, sizeof dev);
            kv_t k;
            k.n = 0;
            k.s[0] = '\0';
            kv_int(&k, "line", ln);
            kv_str(&k, "spec", f[0]);
            kv_str(&k, "point", f[1]);
            kv_str(&k, "fs", nf > 2 ? f[2] : "");
            kv_str(&k, "opts", nf > 3 ? f[3] : "");
            kv_str(&k, "pass", nf > 5 ? f[5] : "0");
            kv_str(&k, "dev", dev);
            kv_int(&k, "editable", fstab_editable(f[1], nf > 2 ? f[2] : "", f[0]));
            record("fstab", k.s);
        }
        line = nl ? nl + 1 : NULL;
    }
    reply("done", "");
}

/* Rewrite /etc/fstab with every editable line for `uuid` (or at
 * `point`) dropped, and `add` (may be NULL) appended. */
static bool fstab_rewrite(const char *uuid, const char *point, const char *add)
{
    long got = proc_read(FSTAB_PATH, fstab_buf, sizeof fstab_buf - 1);
    if (got < 0) got = 0;
    fstab_buf[got] = '\0';
    static char out[34000];
    size_t k = 0;
    char spec[64];
    snprintf(spec, sizeof spec, "UUID=%s", uuid);
    bool removed = false;
    for (char *line = fstab_buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char copy[1024];
        strlcpy(copy, line, sizeof copy);
        char *f[6];
        int nf = fstab_fields(copy, f);
        bool drop = false;
        if (nf >= 3) {
            bool same = !ieq_not(f[0], spec) || (point && !strcmp(f[1], point));
            if (same && !fstab_editable(f[1], f[2], f[0]))
                return failed("refused", "the system itself uses that entry of"
                              " /etc/fstab; it is not changed here");
            drop = same;
        }
        if (drop) {
            /* ... and the comment this program put above it. */
            static const char mark[] = "# added by Disks (lp-diskd)\n";
            size_t ml = sizeof mark - 1;
            if (k >= ml && !memcmp(out + k - ml, mark, ml))
                k -= ml;
            removed = true;
        } else if (k + strlen(line) + 2 < sizeof out) {
            k += strlcpy(out + k, line, sizeof out - k);
            out[k++] = '\n';
        }
        line = nl ? nl + 1 : NULL;
    }
    if (add) {
        if (k + strlen(add) + 64 >= sizeof out)
            return failed("failed", "/etc/fstab is too long to edit here");
        k += strlcpy(out + k, "# added by Disks (lp-diskd)\n", sizeof out - k);
        k += strlcpy(out + k, add, sizeof out - k);
        out[k++] = '\n';
    } else if (!removed) {
        return failed("invalid", "there is no data-partition entry for that UUID");
    }
    out[k] = '\0';
    /* fstab_buf was cut into lines above; read the file again for the
     * copy of what it was. */
    static char orig[34000];
    long og = proc_read(FSTAB_PATH, orig, sizeof orig);
    if (og > 0 && !lp_write_file_atomic(FSTAB_PATH ".lp-diskd.bak", orig, (size_t)og))
        return failed("failed", "could not keep a copy of the old /etc/fstab");
    if (!lp_write_file_atomic(FSTAB_PATH, out, k))
        return failed("failed", "could not write /etc/fstab");
    audit(add ? add : "  fstab entry removed");
    return true;
}

static bool point_name_ok(const char *s)
{
    return alphabet_ok(s, "_.-", 32, true) && s[0] != '.';
}

static void v_fstab_set(char **a, int n)
{
    (void)n;
    const char *kn = a[0];
    if (!point_name_ok(a[1])) {
        fail_now("invalid", "the mount point name is letters, digits, _ . -");
        return;
    }
    bool automount = !strcmp(a[2], "auto");
    bool ro = !strcmp(a[3], "ro");
    if ((!automount && strcmp(a[2], "noauto")) || (!ro && strcmp(a[3], "rw"))) {
        fail_now("invalid", "fstab-set <part> <name> auto|noauto rw|ro");
        return;
    }
    probe_t p;
    probe_dev(kn, &p);
    if (!p.uuid[0] || !p.fs[0]) {
        fail_now("refused", "there is no filesystem with a UUID on it");
        return;
    }
    if (!strcmp(p.fs, "crypto_LUKS")) {
        fail_now("refused", "an encrypted partition would need /etc/crypttab as"
                 " well; unlock and mount it from Disks instead");
        return;
    }
    char line[400], point[64];
    const char *fs = !strcmp(p.fs, "ntfs") ? "ntfs3" : p.fs;
    if (!strcmp(p.fs, "swap")) {
        strlcpy(point, "none", sizeof point);
        snprintf(line, sizeof line, "UUID=%s none swap sw,nofail 0 0", p.uuid);
    } else {
        snprintf(point, sizeof point, "/mnt/%s", a[1]);
        char extra[80] = "";
        if (!strcmp(p.fs, "vfat") || !strcmp(p.fs, "exfat") || !strcmp(p.fs, "ntfs"))
            snprintf(extra, sizeof extra, ",uid=%u,gid=%u,umask=022", caller_uid, caller_gid);
        snprintf(line, sizeof line, "UUID=%s %s %s %s,%s,nofail,nosuid,nodev%s 0 %d",
                 p.uuid, point, fs, ro ? "ro" : "rw", automount ? "auto" : "noauto",
                 extra, starts(p.fs, "ext") ? 2 : 0);
    }
    if (!fstab_rewrite(p.uuid, strcmp(point, "none") ? point : NULL, line)) {
        finish_fail();
        return;
    }
    if (strcmp(point, "none")) {
        lp_mkdir("/mnt", 0755);
        lp_mkdir(point, 0755);
    }
    done(line);
}

static void v_fstab_remove(char **a, int n)
{
    (void)n;
    if (!alphabet_ok(a[0], "-", 36, true)) {
        fail_now("invalid", "fstab-remove takes the filesystem UUID");
        return;
    }
    if (!fstab_rewrite(a[0], NULL, NULL)) {
        finish_fail();
        return;
    }
    done("removed");
}

/* ── Secure erase ─────────────────────────────────────────────────────
 *
 * Removable media only (USB sticks, SD cards - and loop devices, which
 * are files), after the person typed the disk's name. Every byte is
 * overwritten with zeros, then the device is asked to discard, which on
 * flash also releases the blocks the wear levelling kept aside. One
 * pass is enough for any recovery that does not take the chips out; the
 * application says that flash cannot promise more. */

static void v_erase(char **a, int n)
{
    const char *disk = a[0];
    if (n < 2 || !starts(a[1], "confirm=") || strcmp(a[1] + 8, disk)) {
        fail_now("refused", "erasing needs the disk's name typed as confirmation");
        return;
    }
    if (!blk_removable(disk)) {
        fail_now("refused", "only removable drives (USB, SD cards) are erased here;"
                 " for an internal disk, reinstall from Recovery instead");
        return;
    }
    mounts_refresh();
    bool vital;
    char why[300];
    if (disk_busy(disk, &vital, why, sizeof why)) {
        fail_now("refused", why);
        return;
    }
    char dev[64];
    if (!dev_path(disk, dev, sizeof dev)) {
        fail_now("failed", "the disk is gone");
        return;
    }
    long fd = lp_open(dev, O_WRONLY | O_CLOEXEC, 0);
    if (fd < 0) {
        fail_now("failed", "cannot open the disk for writing");
        return;
    }
    u64 len = blk_bytes(disk), off = 0;
    u8 *z = malloc(4 * MIB);
    if (!z) {
        lp_close((int)fd);
        fail_now("failed", "out of memory");
        return;
    }
    memset(z, 0, 4 * MIB);
    s64 last = 0;
    bool ok = true;
    int since = 0;
    while (off < len) {
        if (cancel_req) {
            char msg[200], x[32];
            human(off, x, sizeof x);
            snprintf(msg, sizeof msg, "stopped: the first %s are erased, the rest"
                     " is not; the partition table is gone", x);
            failed("cancelled", msg);
            ok = false;
            break;
        }
        size_t c = len - off < 4 * MIB ? (size_t)(len - off) : 4 * MIB;
        if (!write_at((int)fd, off, z, c)) {
            failed("failed", "a write error stopped the erase");
            ok = false;
            break;
        }
        off += c;
        if (++since >= 32) { lp_fsync((int)fd); since = 0; }
        s64 now = lp_monotonic_ms();
        if (now - last > 250) {
            last = now;
            progress((int)(off * 100 / len), "overwriting with zeros");
        }
    }
    lp_fsync((int)fd);
    if (ok) {
        u64 range[2] = { 0, len };
        lp_ioctl((int)fd, BLKDISCARD_, range);   /* not every device can */
    }
    lp_ioctl((int)fd, BLKFLSBUF_, NULL);
    lp_close((int)fd);
    free(z);
    char w[300];
    kernel_sync(disk, w, sizeof w);
    if (!ok) {
        finish_fail();
        return;
    }
    done("erased");
}

/* ── Plans, and finishing an interrupted move ──────────────────────── */

static void v_plan(char **a, int n)
{
    char why[400];
    if (!plan_parse(a, n, why, sizeof why)) {
        for (int i = 0; i < MAX_STEPS; i++) scrub(steps[i].pass, sizeof steps[i].pass);
        fail_now(plan_code, why);
        return;
    }
    plan_run();
    for (int i = 0; i < MAX_STEPS; i++) scrub(steps[i].pass, sizeof steps[i].pass);
}

/* "check" for the plan without doing it: the same model, the same
 * refusals, and the plain-words description of every step. */
static void v_preview(char **a, int n)
{
    char why[400], line[500];
    bool ok = plan_parse(a, n, why, sizeof why);
    for (int i = 0; i < MAX_STEPS; i++) scrub(steps[i].pass, sizeof steps[i].pass);
    if (!ok) {
        fail_now(plan_code, why);
        return;
    }
    for (int i = 0; i < nsteps; i++) {
        snprintf(line, sizeof line, "%d %d %s", i + 1, nsteps, steps[i].desc);
        reply("describe", line);
    }
    done("the plan can be applied");
}

/* Make the kernel's partitions match the table on the disk again -
 * after another tool rewrote the table, or a kernel that cannot parse
 * it dropped them (BLKRRPART without the GPT parser removes them all). */
static void v_rescan(char **a, int n)
{
    (void)n;
    mounts_refresh();
    char why[300];
    if (!kernel_sync(a[0], why, sizeof why)) {
        fail_now("failed", why);
        return;
    }
    done("the kernel's view matches the partition table");
}

/* "Do this from Recovery": the partition that holds the running
 * system cannot be changed while it runs, so the application offers to
 * restart into Recovery, where it is not in use. lp-reboot-recovery
 * (the boot-recovery track's) sets the boot menu's one-shot variable
 * and restarts; this only runs it, from a fixed path, with nothing of
 * the caller's in its argv. */
static void v_reboot_recovery(char **a, int n)
{
    (void)a; (void)n;
    char bin[64];
    if (!need_tool("lp-reboot-recovery", "lp-base", bin, sizeof bin)) {
        finish_fail();
        return;
    }
    char *argv[] = { bin, NULL };
    if (run(argv) != 0) {
        fail_now("failed", "lp-reboot-recovery could not set the next boot");
        return;
    }
    done("restarting into Recovery");
}

static void v_resume(char **a, int n)
{
    (void)a; (void)n;
    resume_move();
}

/* ═══════════════════════════════════════════════════════════════════
 * The password
 *
 * The same bargain as sudo and lp-privd: the caller's own password,
 * checked against /etc/shadow with crypt6, kept five minutes per uid in
 * this process's memory only, counted on CLOCK_BOOTTIME so a suspended
 * laptop does not come back authorised. A wrong password makes that uid
 * wait - a time stamp, not a sleep, so nobody else waits with it.
 * ═══════════════════════════════════════════════════════════════════ */

#define KEEP_SLOTS    32
#define WAIT_STEP_MS  2000
#define WAIT_MAX_MS   30000

typedef struct {
    u32 uid;
    s64 until, not_before, used;
    int fails;
} keep_t;

static keep_t keeps[KEEP_SLOTS];

static s64 boottime_ms(void)
{
    s64 ts[2] = { 0, 0 };
    if (sys_call2(SYS_clock_gettime, 7 /* CLOCK_BOOTTIME */, (long)ts) < 0)
        return lp_monotonic_ms();
    return ts[0] * 1000 + ts[1] / 1000000;
}

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

static long keep_left(u32 uid)
{
    keep_t *k = keep_for(uid, false);
    s64 now = boottime_ms();
    if (!k || k->until <= now)
        return 0;
    return (long)((k->until - now + 999) / 1000);
}

/* Even hex, 1..255 bytes, no NUL byte in it (a NUL would cut the
 * password short where the person did not). */
static bool pw_hex_ok(const char *s)
{
    size_t n = strlen(s);
    if (n < 2 || n % 2 || n > 2 * (LP_CRYPT6_PW_MAX - 1))
        return false;
    for (size_t i = 0; i < n; i++)
        if (hexval(s[i]) < 0)
            return false;
    for (size_t i = 0; i < n; i += 2)
        if (s[i] == '0' && s[i + 1] == '0')
            return false;
    return true;
}

static void answer_auth(u32 uid, const char *user, const char *who, char *hex)
{
    keep_t *k = keep_for(uid, true);
    s64 now = boottime_ms();
    char line[300], msg[160];
    k->used = now;
    if (now < k->not_before) {
        long secs = (long)((k->not_before - now + 999) / 1000);
        snprintf(line, sizeof line, "%s: auth *** -> auth (waiting, %lds left)", who, secs);
        audit(line);
        snprintf(msg, sizeof msg, "auth wait %ld s: too soon after a wrong password", secs);
        reply("fail", msg);
        scrub(hex, strlen(hex));
        return;
    }
    char pw[LP_CRYPT6_PW_MAX];
    size_t len = 0;
    bool shaped = unhex(hex, pw, sizeof pw, &len);
    scrub(hex, strlen(hex));
    int r = shaped ? lp_shadow_check(NULL, user, pw) : LP_SHADOW_WRONG;
    scrub(pw, sizeof pw);
    const char *code = "failed", *text = "cannot read /etc/shadow";
    switch (r) {
    case LP_SHADOW_OK:
        k->until = now + AUTH_KEEP_MS;
        k->fails = 0;
        k->not_before = 0;
        snprintf(line, sizeof line, "%s: auth *** -> ok, kept %ds", who, AUTH_KEEP_MS / 1000);
        audit(line);
        snprintf(msg, sizeof msg, "authorised for %d s", AUTH_KEEP_MS / 1000);
        reply("done", msg);
        return;
    case LP_SHADOW_WRONG: {
        k->fails++;
        k->until = 0;
        s64 wait = (s64)WAIT_STEP_MS * k->fails;
        k->not_before = now + (wait > WAIT_MAX_MS ? WAIT_MAX_MS : wait);
        snprintf(line, sizeof line, "%s: auth *** -> wrong password (%d in a row)", who, k->fails);
        audit(line);
        reply("fail", "auth wrong password");
        return;
    }
    case LP_SHADOW_LOCKED:
    case LP_SHADOW_EMPTY:
        code = "denied"; text = "this account has no password to check; set one with passwd";
        break;
    case LP_SHADOW_UNSUPPORTED:
        code = "denied"; text = "this account's password is stored in a form this system"
                                " cannot check; set it again with passwd";
        break;
    case LP_SHADOW_NOUSER:
        code = "denied"; text = "this account has no entry in /etc/shadow";
        break;
    default:
        break;
    }
    snprintf(line, sizeof line, "%s: auth *** -> %s (%s)", who, code, text);
    audit(line);
    snprintf(msg, sizeof msg, "%s %s", code, text);
    reply("fail", msg);
}

/* Root, or a member of group sudo - by /etc/group, not by the peer's
 * supplementary groups: the desktop session is started with none, and
 * the account database is what makes a removal from sudo take effect
 * at once. */
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
            char *f2 = strchr(line + 5, ':');
            char *f3 = f2 ? strchr(f2 + 1, ':') : NULL;
            if (f2 && ((u32)atoi(f2 + 1) == gid || (u32)atoi(f2 + 1) == u.gid))
                return true;
            for (char *m = f3 ? f3 + 1 : NULL; m && *m; ) {
                char *comma = strchr(m, ',');
                if (comma) *comma = '\0';
                if (!strcmp(m, u.name))
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
 * The verbs
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    K_DEV,        /* a block device: disk or partition */
    K_DISK,       /* a whole disk we list */
    K_PART,       /* a partition */
    K_SECS,       /* 1..60 */
    K_HEX,        /* a password, hex */
    K_KEY,        /* k:<hex>, a passphrase */
    K_CONFIRM,    /* confirm=<typed word> */
    K_WORD,       /* a lower-case word the verb checks */
    K_TOKEN,      /* letters, digits, _ . - (a name or a UUID) */
    K_PLAN        /* plan fields: the plan parser checks every one */
} kind_t;

typedef enum { RUN_INLINE, RUN_READER, RUN_JOB } runmode_t;

typedef struct {
    const char *verb;
    bool        admin;
    runmode_t   mode;
    int         min, max;
    kind_t      kind[4];       /* per position; the last one repeats */
    void      (*fn)(char **a, int n);
    const char *help;
} verb_t;

static const verb_t VERBS[] = {
    { "ping",          false, RUN_INLINE, 0, 0,   {K_WORD}, NULL, "who am I, and may I change things" },
    { "status",        false, RUN_INLINE, 0, 0,   {K_WORD}, NULL, "what is running now" },
    { "auth",          false, RUN_INLINE, 1, 1,   {K_HEX}, NULL, "<password as hex>: allow changes for 5 minutes" },
    { "forget",        false, RUN_INLINE, 0, 0,   {K_WORD}, NULL, "end those 5 minutes now" },
    { "list",          false, RUN_INLINE, 0, 1,   {K_DISK}, NULL, "disks, partitions and free space" },
    { "fstab",         false, RUN_INLINE, 0, 0,   {K_WORD}, NULL, "the entries of /etc/fstab" },
    { "minsize",       false, RUN_READER, 1, 1,   {K_DEV}, v_minsize, "how far a filesystem can shrink" },
    { "smart",         false, RUN_READER, 1, 1,   {K_DEV}, v_smart, "the drive's health, in plain words" },
    { "preview",       false, RUN_READER, 1, MAX_FIELDS - 1, {K_PLAN}, v_preview, "check a plan without doing it" },
    { "cancel",        true,  RUN_INLINE, 0, 0,   {K_WORD}, NULL, "stop the running job where that is safe" },
    { "plan",          true,  RUN_JOB,    1, MAX_FIELDS - 1, {K_PLAN}, v_plan, "<step> [args] [| <step> ...]: change the disks" },
    { "resume",        true,  RUN_JOB,    0, 0,   {K_WORD}, v_resume, "finish a move a power cut interrupted" },
    { "rescan",        true,  RUN_JOB,    1, 1,   {K_DISK}, v_rescan, "make the kernel's partitions match the table" },
    { "reboot-recovery", true, RUN_JOB,   0, 0,   {K_WORD}, v_reboot_recovery, "restart into Recovery (lp-reboot-recovery)" },
    { "mount",         true,  RUN_JOB,    1, 1,   {K_PART}, v_mount, "mount a partition under /media" },
    { "unmount",       true,  RUN_JOB,    1, 1,   {K_DEV}, v_unmount, "unmount it (and its unlocked contents)" },
    { "eject",         true,  RUN_JOB,    1, 1,   {K_DISK}, v_eject, "unmount a whole drive so it can be pulled" },
    { "poweroff",      true,  RUN_JOB,    1, 1,   {K_DISK}, v_poweroff, "eject, then power the USB drive down" },
    { "luks-open",     true,  RUN_JOB,    2, 2,   {K_PART, K_KEY}, v_luks_open, "<part> k:<hex>: unlock an encrypted partition" },
    { "luks-close",    true,  RUN_JOB,    1, 1,   {K_PART}, v_luks_close, "lock it again" },
    { "bench",         true,  RUN_JOB,    1, 2,   {K_DEV, K_SECS}, v_bench, "<dev> [seconds]: read speed, read-only" },
    { "image-save",    true,  RUN_JOB,    1, 1,   {K_DEV}, v_image_save, "<part> + a file descriptor: save an image" },
    { "image-restore", true,  RUN_JOB,    1, 2,   {K_PART, K_CONFIRM}, v_image_restore, "<part> [confirm=] + a descriptor: restore it" },
    { "fstab-set",     true,  RUN_JOB,    4, 4,   {K_PART, K_TOKEN, K_WORD, K_WORD}, v_fstab_set, "<part> <name> auto|noauto rw|ro: mount at /mnt/<name> at boot" },
    { "fstab-remove",  true,  RUN_JOB,    1, 1,   {K_TOKEN}, v_fstab_remove, "<fs UUID>: remove that data-partition entry" },
    { "erase",         true,  RUN_JOB,    2, 2,   {K_DISK, K_CONFIRM}, v_erase, "<disk> confirm=<disk>: overwrite a removable drive" },
    { NULL, false, RUN_INLINE, 0, 0, {K_WORD}, NULL, NULL }
};

static bool arg_ok(kind_t k, const char *s, char *why, size_t whyn)
{
    switch (k) {
    case K_DEV:
    case K_DISK:
    case K_PART:
        if (!dev_name_ok(s) || !blk_exists(s)) {
            snprintf(why, whyn, "\"%.32s\" is not a block device here", s);
            return false;
        }
        if (k == K_DISK && (blk_is_part(s) || !blk_listable(s))) {
            snprintf(why, whyn, "%s is not a whole disk", s);
            return false;
        }
        if (k == K_PART && !blk_is_part(s)) {
            snprintf(why, whyn, "%s is not a partition", s);
            return false;
        }
        return true;
    case K_SECS: {
        u64 v;
        if (!parse_u64(s, &v) || v < 1 || v > 60) {
            snprintf(why, whyn, "seconds are 1 to 60");
            return false;
        }
        return true;
    }
    case K_HEX:
        if (!pw_hex_ok(s)) {
            snprintf(why, whyn, "the password is sent as hex");
            return false;
        }
        return true;
    case K_KEY: {
        size_t n = strlen(s);
        bool ok = starts(s, "k:") && n > 2 && n % 2 == 0 && n <= 2 + 512;
        for (size_t i = 2; ok && i < n; i++)
            if (hexval(s[i]) < 0) ok = false;
        if (!ok) snprintf(why, whyn, "a passphrase is k: and its hex");
        return ok;
    }
    case K_CONFIRM:
        if (!starts(s, "confirm=") || !alphabet_ok(s + 8, "-", 32, true)) {
            snprintf(why, whyn, "a confirmation is confirm=<what was typed>");
            return false;
        }
        return true;
    case K_WORD:
        if (!alphabet_ok(s, "", 16, false)) {
            snprintf(why, whyn, "\"%.20s\" is not a word this verb takes", s);
            return false;
        }
        return true;
    case K_TOKEN:
        if (!alphabet_ok(s, "_.-", 40, true)) {
            snprintf(why, whyn, "names are letters, digits, _ . -");
            return false;
        }
        return true;
    case K_PLAN:
        /* Every field is re-checked for its position by plan_step();
         * here only the length, and that nothing starts with a dash
         * except the ones plan_step() knows how to read. */
        if (strlen(s) > 1100) {
            snprintf(why, whyn, "a plan field is too long");
            return false;
        }
        return true;
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════════
 * The daemon
 * ═══════════════════════════════════════════════════════════════════ */

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

/* struct msghdr and cmsghdr as the kernel lays them out for this
 * machine: pointers and size_t are the machine's width, so plain C
 * types give the right layout on all three. */
typedef struct { void *base; size_t len; } iov_t;
typedef struct {
    void  *name;
    u32    namelen;
    iov_t *iov;
    size_t iovlen;
    void  *control;
    size_t controllen;
    int    flags;
} msghdr_t;
typedef struct { size_t len; int level; int type; } cmsg_t;
#define CMSG_HDR_   ((sizeof(cmsg_t) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define MSG_CMSG_CLOEXEC_ 0x40000000

/* Read one request line, at most REQ_TIMEOUT late, with any descriptor
 * that rides along (recvmsg, so SCM_RIGHTS is not silently dropped).
 * Exactly one line: bytes after the newline are a second request trying
 * to ride along, and are refused. */
static long read_request(int fd, char *buf, size_t n, int *fd_out)
{
    size_t got = 0;
    *fd_out = -1;
    s64 deadline = lp_monotonic_ms() + REQ_TIMEOUT;
    while (got < n - 1) {
        s64 left = deadline - lp_monotonic_ms();
        if (left <= 0)
            return -1;
        lp_pollfd_t p = { fd, LP_POLLIN, 0 };
        if (lp_poll(&p, 1, (int)left) <= 0)
            continue;
        iov_t iov = { buf + got, n - 1 - got };
        u64 ctl[16];
        msghdr_t m;
        memset(&m, 0, sizeof m);
        m.iov = &iov;
        m.iovlen = 1;
        m.control = ctl;
        m.controllen = sizeof ctl;
        long r = sys_call3(SYS_recvmsg_, fd, (long)&m, MSG_CMSG_CLOEXEC_);
        if (r == -EINTR_)
            continue;
        if (r <= 0)
            return -1;
        if (m.controllen >= sizeof(cmsg_t)) {
            cmsg_t *c = (cmsg_t *)ctl;
            if (c->level == SOL_SOCKET && c->type == SCM_RIGHTS_ &&
                c->len >= CMSG_HDR_ + sizeof(int)) {
                int *fds = (int *)((u8 *)ctl + CMSG_HDR_);
                size_t nfd = (c->len - CMSG_HDR_) / sizeof(int);
                for (size_t i = 0; i < nfd; i++) {
                    if (*fd_out < 0) *fd_out = fds[i];
                    else lp_close(fds[i]);
                }
            }
        }
        for (long i = 0; i < r; i++) {
            if (buf[got + (size_t)i] == '\n') {
                if (i != r - 1)
                    return -1;
                buf[got + (size_t)i] = '\0';
                return (long)(got + (size_t)i);
            }
        }
        got += (size_t)r;
    }
    return -1;
}

static pid_t worker = 0;           /* the one job that writes, or 0 */
static char  worker_desc[300];
static int   readers;              /* minsize/smart/preview children alive */

static void answer_status(void)
{
    char buf[700];
    long got = proc_read(JOB_FILE, buf, sizeof buf - 1);
    if (got <= 0) {
        reply("done", "idle");
        return;
    }
    buf[got] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) {
        *nl = '\0';
        char *nl2 = strchr(nl + 1, '\n');
        if (nl2) *nl2 = '\0';
        reply("progress", nl + 1);
    }
    reply("done", buf);
}

/* The worker takes this lock for as long as it runs, so a root
 * lp-diskctl in another terminal (which runs without the daemon) and
 * the daemon's job cannot both be rewriting a partition table. */
static bool take_lock(void)
{
    long fd = lp_open(LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return true;                    /* no /run: nothing else can run either */
    return sys_call2(SYS_flock_, fd, LOCK_EX_ | LOCK_NB_) == 0;
}

static void reap(void);

static void handle(int fd)
{
    client = fd;
    u32 pid = 0, uid = 0, gid = 0;
    if (!peer_cred(fd, &pid, &uid, &gid)) {
        reply("fail", "denied could not tell who is asking");
        return;
    }
    static char req[MAX_REQ];
    int pfd = -1;
    long len = read_request(fd, req, sizeof req, &pfd);
    char user[40];
    bool admin = is_admin(uid, gid, user, sizeof user);
    char who[96];
    snprintf(who, sizeof who, "uid=%u(%s) pid=%u", uid, user, pid);
    char line[1400];
    if (len < 0) {
        snprintf(line, sizeof line, "%s: no request line -> invalid", who);
        audit(line);
        reply("fail", "invalid one line of at most 16 KiB, ending in a newline,"
              " within 3 seconds, and nothing after it");
        if (pfd >= 0) lp_close(pfd);
        return;
    }
    for (long i = 0; i < len; i++) {
        unsigned char c = (unsigned char)req[i];
        if (c != '\t' && (c < 0x20 || c > 0x7e)) {
            snprintf(line, sizeof line, "%s: a byte outside the protocol -> invalid", who);
            audit(line);
            reply("fail", "invalid requests are printable ASCII fields separated by tabs");
            if (pfd >= 0) lp_close(pfd);
            return;
        }
    }

    static char *fields[MAX_FIELDS];
    int nf = 0;
    for (char *p = req; p && nf < MAX_FIELDS; ) {
        fields[nf++] = p;
        char *t = strchr(p, '\t');
        if (t) *t++ = '\0';
        p = t;
    }
    /* What the log says was asked: every field, but never a password or
     * a passphrase. */
    static char printable[MAX_REQ];
    size_t pk = 0;
    for (int i = 0; i < nf && pk < sizeof printable - 8; i++) {
        if (i) printable[pk++] = ' ';
        const char *f = fields[i];
        if ((i == 1 && !strcmp(fields[0], "auth")) || starts(f, "k:"))
            f = starts(f, "k:") ? "k:***" : "***";
        pk += strlcpy(printable + pk, f, sizeof printable - pk);
        if (pk >= sizeof printable) pk = sizeof printable - 1;
    }
    printable[pk] = '\0';

    const verb_t *v = NULL;
    for (int i = 0; VERBS[i].verb; i++)
        if (!strcmp(VERBS[i].verb, fields[0]))
            v = &VERBS[i];
    int nargs = nf - 1;
    char **args = fields + 1;
    char why[300] = "";
    const char *code = NULL;
    mounts_refresh();

    if (!v) {
        code = "invalid"; snprintf(why, sizeof why, "unknown verb");
    } else if (uid != 0 && uid < 1000) {
        code = "denied"; snprintf(why, sizeof why, "only a person at this machine may ask");
    } else if ((v->admin || !strcmp(v->verb, "auth")) && !admin) {
        code = "denied";
        snprintf(why, sizeof why, "%s is not an administrator (group sudo)", user);
    } else if (nargs < v->min || nargs > v->max) {
        code = "invalid";
        snprintf(why, sizeof why, "%s takes %d to %d arguments", v->verb, v->min, v->max);
    } else {
        for (int i = 0; i < nargs && !code; i++) {
            kind_t k = v->kind[0] == K_PLAN ? K_PLAN : v->kind[i < 4 ? i : 3];
            if (!arg_ok(k, args[i], why, sizeof why))
                code = "invalid";
        }
    }
    /* The refusals that do not depend on who asks come before the
     * password, so nobody types it for a request that was never going
     * to be allowed: a plan is played through on the model here (the
     * job does it again, fresh, before touching anything), and the
     * running system's own partitions are refused by name. */
    if (!code && !strcmp(v->verb, "plan")) {
        char w[400];
        bool ok = plan_parse(args, nargs, w, sizeof w);
        for (int i = 0; i < MAX_STEPS; i++) scrub(steps[i].pass, sizeof steps[i].pass);
        if (!ok) {
            code = plan_code;
            strlcpy(why, w, sizeof why);
        }
    }
    if (!code && v->mode == RUN_JOB && nargs > 0 && strcmp(v->verb, "plan") &&
        strcmp(v->verb, "bench") && strcmp(v->verb, "fstab-remove") &&
        strcmp(v->verb, "rescan") &&
        dev_name_ok(args[0]) && blk_exists(args[0])) {
        bool vital = false;
        char w[300];
        if (blk_is_part(args[0])) in_use(args[0], &vital, w, sizeof w);
        else disk_busy(args[0], &vital, w, sizeof w);
        if (vital) {
            code = "refused";
            strlcpy(why, w, sizeof why);
        }
    }
    /* Last, and only for what would otherwise go ahead: the password. */
    if (!code && v->admin && uid != 0 && keep_left(uid) == 0) {
        code = "auth"; snprintf(why, sizeof why, "password required");
    }
    if (!code && pfd >= 0 && strcmp(v->verb, "image-save") && strcmp(v->verb, "image-restore")) {
        code = "invalid"; snprintf(why, sizeof why, "this verb takes no file descriptor");
    }
    if (code) {
        snprintf(line, sizeof line, "%s: %s -> %s (%s)", who, printable, code, why);
        audit(line);
        char msg[400];
        snprintf(msg, sizeof msg, "%s %s", code, why);
        reply("fail", msg);
        if (v && !strcmp(v->verb, "auth") && nargs > 0) scrub(args[0], strlen(args[0]));
        for (int i = 0; i < nargs; i++) if (starts(args[i], "k:")) scrub(args[i], strlen(args[i]));
        if (pfd >= 0) lp_close(pfd);
        return;
    }

    if (!strcmp(v->verb, "ping")) {
        char msg[200];
        snprintf(msg, sizeof msg, "uid=%u user=%s admin=%s auth=%ld job=%s", uid, user,
                 admin ? "yes" : "no", uid == 0 ? (long)(AUTH_KEEP_MS / 1000) : keep_left(uid),
                 worker ? "yes" : "no");
        reply("done", msg);
        return;
    }
    if (!strcmp(v->verb, "auth")) {
        if (uid == 0) {
            scrub(args[0], strlen(args[0]));
            reply("done", "root needs no password here");
            return;
        }
        answer_auth(uid, user, who, args[0]);
        return;
    }
    if (!strcmp(v->verb, "forget")) {
        keep_t *k = keep_for(uid, false);
        if (k) k->until = 0;
        snprintf(line, sizeof line, "%s: forget -> done", who);
        audit(line);
        reply("done", "the password will be asked for again");
        return;
    }
    if (!strcmp(v->verb, "status")) { answer_status(); return; }
    if (!strcmp(v->verb, "list"))   { answer_list(nargs ? args[0] : NULL); return; }
    if (!strcmp(v->verb, "fstab"))  { answer_fstab(); return; }
    if (!strcmp(v->verb, "cancel")) {
        if (!worker) {
            reply("done", "nothing is running");
            return;
        }
        snprintf(line, sizeof line, "%s: cancel -> sent to %s", who, worker_desc);
        audit(line);
        lp_kill(worker, SIGUSR1);
        reply("done", "asked the job to stop where that is safe");
        return;
    }

    /* A job that has just sent its last line may not have exited yet;
     * a client that asks again at once must not be told "busy" for the
     * few milliseconds that takes. */
    for (int i = 0; v->mode == RUN_JOB && worker && i < 30; i++) {
        reap();
        if (worker) lp_sleep_ms(10);
    }
    if (v->mode == RUN_JOB && worker) {
        snprintf(line, sizeof line, "%s: %s -> busy (%s)", who, printable, worker_desc);
        audit(line);
        char msg[400];
        snprintf(msg, sizeof msg, "busy another job is running: %s", worker_desc);
        reply("fail", msg);
        if (pfd >= 0) lp_close(pfd);
        return;
    }
    snprintf(line, sizeof line, "%s: %s -> accepted", who, printable);
    audit(line);

    pid_t w = lp_fork();
    if (w < 0) {
        reply("fail", "failed could not start the job");
        if (pfd >= 0) lp_close(pfd);
        return;
    }
    if (w == 0) {
        caller_uid = uid;
        caller_gid = gid;
        passed_fd = pfd;
        cancel_req = 0;
        lp_signal_handler(SIGUSR1, on_cancel);
        s64 t0 = lp_monotonic_ms();
        if (v->mode == RUN_JOB) {
            if (!take_lock()) {
                reply("fail", "busy another lp-diskctl is working on the disks");
                lp_exit(1);
            }
            strlcpy(job_desc, printable, sizeof job_desc);
            long lf = lp_open(LAST_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
            last_fd = lf >= 0 ? (int)lf : -1;
            if (last_fd >= 0) {
                char head[500];
                int k = snprintf(head, sizeof head, "# %s %s\n", who, printable);
                if (k > 0) lp_write(last_fd, head, (size_t)k);
            }
            job_file(0, "starting");
        }
        job_rc = 1;
        v->fn(args, nargs);
        if (v->mode == RUN_JOB) {
            char end[500];
            snprintf(end, sizeof end, "%s: %s -> %s after %lds", who, printable,
                     job_rc == 0 ? "done" : "failed",
                     (long)((lp_monotonic_ms() - t0) / 1000));
            audit(end);
            lp_unlink(JOB_FILE);
        }
        if (client >= 0) lp_close(client);
        lp_exit(job_rc);
    }
    if (pfd >= 0) lp_close(pfd);
    if (v->mode == RUN_JOB) {
        worker = w;
        strlcpy(worker_desc, printable, sizeof worker_desc);
    } else {
        readers++;
    }
    client = -1;                          /* the child has it now */
}

static void reap(void)
{
    int status;
    pid_t p;
    while ((p = lp_waitpid(-1, &status, WNOHANG)) > 0) {
        if (p == worker) {
            worker = 0;
            worker_desc[0] = '\0';
            lp_unlink(JOB_FILE);
        } else if (readers > 0) {
            readers--;
        }
    }
}

static void close_client(int fd)
{
    if (client >= 0) {
        /* Read away whatever was not read - the rest of an over-long
         * line - or the close becomes a reset that overtakes the
         * "fail" saying why. */
        lp_shutdown(client, 1);
        char junk[4096];
        for (int i = 0; i < 16; i++)
            if (lp_recvfrom(client, junk, sizeof junk, MSG_DONTWAIT_, NULL, NULL) <= 0)
                break;
        lp_close(client);
        client = -1;
    } else {
        lp_close(fd);
    }
}

static int serve(void)
{
    lp_signal_ignore(13);                  /* SIGPIPE */
    sys_call1(SYS_umask_, 022);
    lp_chdir("/");
    lp_unlink(sock_path);
    sun_t sa;
    long ls = sun_fill(&sa, sock_path) ? lp_socket(AF_UNIX_, SOCK_STREAM, 0) : -1;
    if (ls < 0 || lp_bind((int)ls, &sa, sizeof sa) < 0 || lp_listen((int)ls, 16) < 0) {
        dprintf(STDERR_FILENO, "lp-diskd: cannot listen on %s\n", sock_path);
        return 1;
    }
    lp_chmod(sock_path, 0666);
    audit("listening");
    journal_t j;
    if (journal_read(&j)) {
        char msg[200];
        snprintf(msg, sizeof msg, "an interrupted move of %s on %s is waiting:"
                 " run lp-diskctl resume", j.partuuid, j.disk);
        audit(msg);
    }
    for (;;) {
        /* Asleep in poll() for good when nothing runs: an idle laptop
         * pays no wakeups for this daemon. */
        lp_pollfd_t p = { (int)ls, LP_POLLIN, 0 };
        lp_poll(&p, 1, (worker || readers) ? 500 : -1);
        reap();
        if (!(p.revents & LP_POLLIN))
            continue;
        long fd = lp_accept((int)ls, NULL, NULL, LP_SOCK_CLOEXEC);
        if (fd < 0)
            continue;
        handle((int)fd);
        close_client((int)fd);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * lp-diskctl - the same thing from a terminal
 *
 * Run as root (the recovery shell, or sudo) it does not need the
 * daemon: it makes a socket pair, a child of its own becomes "the
 * daemon" for exactly one request through exactly the code above, and
 * this side prints the answer. Run as anybody else it asks the daemon,
 * and asks for the password when the daemon says so. Either way the
 * checks, the model, the refusals and the log are the daemon's.
 *
 * Ctrl-C asks the job to stop where stopping is safe (a check, a copy
 * that has not started overwriting); it never kills it half way. In
 * the root case the job runs in a session of its own for that reason,
 * so the terminal's SIGINT cannot reach it.
 * ═══════════════════════════════════════════════════════════════════ */

static volatile int sigint_seen;
static void on_sigint(int sig) { (void)sig; sigint_seen = 1; }

static bool read_secret(const char *prompt, char *out, size_t n)
{
    dprintf(STDERR_FILENO, "%s", prompt);
    lp_termios_t saved;
    bool quiet = lp_term_cbreak(STDIN_FILENO, &saved) == 0;
    size_t used = 0;
    bool ok = true;
    for (;;) {
        char ch;
        long r = lp_read(STDIN_FILENO, &ch, 1);
        if (r == -EINTR_) { ok = false; break; }
        if (r <= 0) { ok = used > 0; break; }
        if (ch == '\n' || ch == '\r') break;
        if (ch == 3) { ok = false; break; }
        if (ch == 0x7f || ch == '\b') { if (used) used--; continue; }
        if (used < n - 1) out[used++] = ch;
    }
    out[used] = '\0';
    if (quiet) lp_term_restore(STDIN_FILENO, &saved);
    dprintf(STDERR_FILENO, "\n");
    return ok;
}

static void to_hex(const char *s, size_t n, char *out, size_t outn)
{
    static const char hx[] = "0123456789abcdef";
    size_t k = 0;
    for (size_t i = 0; i < n && k + 3 < outn; i++) {
        out[k++] = hx[(u8)s[i] >> 4];
        out[k++] = hx[(u8)s[i] & 15];
    }
    out[k] = '\0';
}

/* The value of key in a TAB-separated key=value record. */
static bool rec_get(const char *rec, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    for (const char *p = rec; p && *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            const char *e = strchr(v, '\t');
            size_t l = e ? (size_t)(e - v) : strlen(v);
            if (l >= n) l = n - 1;
            memcpy(out, v, l);
            out[l] = '\0';
            return true;
        }
        p = strchr(p, '\t');
        if (p) p++;
    }
    out[0] = '\0';
    return false;
}

static u64 rec_u64(const char *rec, const char *key)
{
    char v[32];
    u64 x = 0;
    if (rec_get(rec, key, v, sizeof v)) parse_u64(v, &x);
    return x;
}

/* One line of the answer, for a person: records as a table, progress
 * redrawn in place on a terminal. `silent` swallows everything (the
 * answer to the password, which is not news). */
static bool pretty = true, progress_open = false, silent = false;

/* Columns on a terminal are cells, not bytes: Hangul and the other
 * wide scripts take two, so a Korean label must be padded by what it
 * shows, or every column after it slides. */
static int cells(const char *s)
{
    int w = 0;
    for (const u8 *p = (const u8 *)s; *p; ) {
        u32 c = *p;
        int len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
        if (len > 1) {
            c &= 0x3f >> (len - 1);
            for (int k = 1; k < len && p[k]; k++) c = (c << 6) | (p[k] & 0x3f);
        }
        bool wide = (c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) ||
                    (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) ||
                    (c >= 0xfe30 && c <= 0xfe4f) || (c >= 0xff00 && c <= 0xff60) ||
                    (c >= 0xffe0 && c <= 0xffe6);
        w += wide ? 2 : 1;
        for (int k = 0; k < len && *p; k++) p++;
    }
    return w;
}

static void padded(const char *s, int width)
{
    printf("%s", s);
    for (int w = cells(s); w < width; w++) printf(" ");
}

static void show_line(const char *line)
{
    bool tty = lp_isatty(STDOUT_FILENO);
    if (silent)
        return;
    if (!pretty) {
        printf("%s\n", line);
        return;
    }
    if (starts(line, "progress ")) {
        if (tty) {
            char *end;
            long pct = strtol(line + 9, &end, 10);
            printf("\r\033[K  %3ld%%%s", pct, end);
            progress_open = true;
        }
        return;
    }
    if (progress_open) {
        printf("\n");
        progress_open = false;
    }
    char a[64], b[64], c[64], d[64], e[128], f[128], g[200];
    const char *rec = strchr(line, '\t') ? strchr(line, '\t') + 1 : "";
    if (starts(line, "disk\t")) {
        rec_get(rec, "name", a, sizeof a);
        human(rec_u64(rec, "size"), b, sizeof b);
        rec_get(rec, "table", c, sizeof c);
        rec_get(rec, "model", e, sizeof e);
        rec_get(rec, "transport", d, sizeof d);
        upcase(c);
        printf("\n%s  %s  %s  %s (%s)%s%s\n", a, b, !strcmp(c, "NONE") ? "no table" : c, e, d,
               rec_u64(rec, "removable") ? "  removable" : "",
               rec_u64(rec, "system") ? "  - the running system's disk" : "");
        rec_get(rec, "err", g, sizeof g);
        if (g[0]) printf("  ! %s\n", g);
        printf("  %-3s %-12s %10s %10s  %-8s %-16s %s\n", "#", "device", "start", "size",
               "fs", "label", "type / mounted / note");
        return;
    }
    if (starts(line, "part\t")) {
        rec_get(rec, "num", a, sizeof a);
        rec_get(rec, "name", b, sizeof b);
        char st[32], sz[32];
        human(rec_u64(rec, "start"), st, sizeof st);
        human(rec_u64(rec, "size"), sz, sizeof sz);
        rec_get(rec, "fs", c, sizeof c);
        rec_get(rec, "label", e, sizeof e);
        rec_get(rec, "typedesc", f, sizeof f);
        rec_get(rec, "mount", g, sizeof g);
        printf("  %-3s %-12s %10s %10s  %-8s ", a, b, st, sz, c[0] ? c : "-");
        padded(e[0] ? e : "-", 16);
        printf(" %s", f);
        if (g[0]) printf(", mounted at %s", g);
        rec_get(rec, "inuse", d, sizeof d);
        if (!strcmp(d, "vital")) printf(" [system]");
        rec_get(rec, "protected", d, sizeof d);
        if (d[0]) printf(" [%s]", d);
        printf("\n");
        return;
    }
    if (starts(line, "free\t")) {
        char st[32], sz[32];
        human(rec_u64(rec, "start"), st, sizeof st);
        human(rec_u64(rec, "size"), sz, sizeof sz);
        printf("  %-3s %-12s %10s %10s  unallocated\n", "", "", st, sz);
        return;
    }
    if (starts(line, "journal\t")) {
        rec_get(rec, "partuuid", a, sizeof a);
        printf("\n! a move of %s was interrupted: run  lp-diskctl resume\n", a);
        return;
    }
    if (starts(line, "log ")) {
        printf("  %s\n", line + 4);
        return;
    }
    if (starts(line, "smart\t")) {
        rec_get(rec, "verdict", a, sizeof a);
        rec_get(rec, "reasons", g, sizeof g);
        rec_get(rec, "model", e, sizeof e);
        const char *say_ = !strcmp(a, "good") ? "the drive reports no problems" :
                           !strcmp(a, "warning") ? "the drive has started to wear or has bad"
                                                   " sectors: keep your backup current" :
                           !strcmp(a, "failing") ? "the drive expects to fail soon: copy your"
                                                   " files off it now" :
                           "this device does not report its health";
        printf("Health: %s - %s%s%s%s\n", a, say_, g[0] ? " (" : "", g, g[0] ? ")" : "");
        if (e[0]) printf("  model %s\n", e);
        long t = (long)rec_u64(rec, "temp"), h = (long)rec_u64(rec, "hours");
        rec_get(rec, "temp", b, sizeof b);
        if (b[0] && b[0] != '-') printf("  temperature %ld C\n", t);
        rec_get(rec, "hours", b, sizeof b);
        if (b[0] && b[0] != '-') printf("  powered on %ld hours\n", h);
        return;
    }
    if (starts(line, "resize\t")) {
        char mn[32];
        rec_get(rec, "name", a, sizeof a);
        human(rec_u64(rec, "min"), mn, sizeof mn);
        rec_get(rec, "noshrink", g, sizeof g);
        if (rec_u64(rec, "shrink"))
            printf("%s can shrink to %s\n", a, mn);
        else
            printf("%s cannot shrink: %s\n", a, g[0] ? g : "unknown");
        return;
    }
    if (starts(line, "bench\t")) {
        printf("sequential read %llu MB/s, random 4 KiB reads %llu IOPS (%llu us each)%s\n",
               (unsigned long long)rec_u64(rec, "seq_mbps"),
               (unsigned long long)rec_u64(rec, "rand_iops"),
               (unsigned long long)rec_u64(rec, "rand_lat_us"),
               rec_u64(rec, "direct") ? "" : " - cached reads, not the disk's own speed");
        return;
    }
    if (starts(line, "describe ") || starts(line, "plan "))
        return;
    if (starts(line, "step ")) {
        const char *t = strchr(line + 5, ' ');
        t = t ? strchr(t + 1, ' ') : NULL;
        long n1 = strtol(line + 5, NULL, 10);
        const char *sp = strchr(line + 5, ' ');
        long n2 = sp ? strtol(sp + 1, NULL, 10) : 0;
        printf("[%ld/%ld] %s\n", n1, n2, t ? t + 1 : "");
        return;
    }
    if (starts(line, "stepdone "))
        return;
    printf("%s\n", line);
}

/* Send one request (and a descriptor, if fdx >= 0) on fd. */
static bool send_req(int fd, const char *req, size_t k, int fdx)
{
    iov_t iov = { (void *)req, k };
    u64 ctl[4];
    msghdr_t m;
    memset(&m, 0, sizeof m);
    m.iov = &iov;
    m.iovlen = 1;
    if (fdx >= 0) {
        memset(ctl, 0, sizeof ctl);
        cmsg_t *c = (cmsg_t *)ctl;
        c->len = CMSG_HDR_ + sizeof(int);
        c->level = SOL_SOCKET;
        c->type = SCM_RIGHTS_;
        *(int *)((u8 *)ctl + CMSG_HDR_) = fdx;
        m.control = ctl;
        m.controllen = (CMSG_HDR_ + sizeof(int) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1);
    }
    return sys_call3(SYS_sendmsg_, fd, (long)&m, MSG_NOSIGNAL_) == (long)k;
}

/* Read the answer, line by line. Returns 0 done, 1 fail; *auth_needed
 * when the answer was "fail auth password required" (not printed). */
static int read_answer(int fd, bool *auth_needed, char *last, size_t lastn,
                       pid_t cancel_group)
{
    static char buf[8192];
    size_t have = 0;
    int rc = 1;
    for (;;) {
        long r = lp_read(fd, buf + have, sizeof buf - 1 - have);
        if (r == -EINTR_ || (r < 0 && sigint_seen)) {
            if (sigint_seen) {
                sigint_seen = 0;
                dprintf(STDERR_FILENO, "\nasking the job to stop where that is safe...\n");
                if (cancel_group > 0) {
                    lp_kill(-cancel_group, SIGUSR1);
                } else {
                    sun_t sa;
                    long c = sun_fill(&sa, sock_path) ? lp_socket(AF_UNIX_, SOCK_STREAM, 0) : -1;
                    if (c >= 0 && lp_connect((int)c, &sa, sizeof sa) == 0) {
                        lp_write((int)c, "cancel\n", 7);
                        char junk[256];
                        while (lp_read((int)c, junk, sizeof junk) > 0) ;
                    }
                    if (c >= 0) lp_close((int)c);
                }
            }
            continue;
        }
        if (r <= 0)
            break;
        have += (size_t)r;
        buf[have] = '\0';
        char *start = buf;
        char *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            if (starts(start, "fail auth password required") && auth_needed) {
                *auth_needed = true;
            } else {
                show_line(start);
                if (last) strlcpy(last, start, lastn);
            }
            if (starts(start, "done")) rc = 0;
            else if (starts(start, "fail")) rc = 1;
            start = nl + 1;
        }
        have = strlen(start);
        memmove(buf, start, have);
        if (have >= sizeof buf - 1) have = 0;     /* a line too long to be ours */
    }
    if (progress_open) { printf("\n"); progress_open = false; }
    return rc;
}

/* Root: be the daemon for one request, in a child. */
static int ctl_local(const char *req, size_t k, int fdx)
{
    int sv[2];
    if (sys_call4(SYS_socketpair_, AF_UNIX_, SOCK_STREAM, 0, (long)sv) < 0) {
        dprintf(STDERR_FILENO, "lp-diskctl: socketpair failed\n");
        return 2;
    }
    pid_t child = lp_fork();
    if (child == 0) {
        lp_close(sv[1]);
        lp_setsid();
        lp_signal_ignore(SIGUSR1);
        lp_signal_ignore(SIGINT);
        lp_signal_ignore(13);
        sys_call1(SYS_umask_, 022);
        handle(sv[0]);
        close_client(sv[0]);
        int st;
        while (lp_waitpid(-1, &st, 0) > 0 || lp_waitpid(-1, &st, 0) == -EINTR_)
            ;
        lp_exit(0);
    }
    lp_close(sv[0]);
    if (!send_req(sv[1], req, k, fdx)) {
        dprintf(STDERR_FILENO, "lp-diskctl: could not send the request\n");
        return 2;
    }
    int rc = read_answer(sv[1], NULL, NULL, 0, child);
    lp_close(sv[1]);
    int st;
    while (lp_waitpid(child, &st, 0) == -EINTR_)
        ;
    return rc;
}

static int talk(const char *req, size_t k, int fdx, bool *auth_needed, char *last, size_t lastn)
{
    sun_t sa;
    long fd = sun_fill(&sa, sock_path) ? lp_socket(AF_UNIX_, SOCK_STREAM, 0) : -1;
    if (fd < 0 || lp_connect((int)fd, &sa, sizeof sa) < 0) {
        dprintf(STDERR_FILENO, "lp-diskctl: the disk service is not running (%s)\n", sock_path);
        if (fd >= 0) lp_close((int)fd);
        return 2;
    }
    if (!send_req((int)fd, req, k, fdx)) {
        lp_close((int)fd);
        return 2;
    }
    int rc = read_answer((int)fd, auth_needed, last, lastn, 0);
    lp_close((int)fd);
    return rc;
}

/* Ask for the password on the terminal and hand it to the daemon.
 * 0 accepted, 1 not, 2 the daemon cannot be reached. A "wait N s"
 * answer is waited out and the same password sent again once, rather
 * than making the person type it twice for the daemon's pause. */
static int auth_prompt(void)
{
    if (!lp_isatty(STDIN_FILENO))
        return 1;
    lp_user_t u;
    char prompt[80], pw[LP_CRYPT6_PW_MAX];
    snprintf(prompt, sizeof prompt, "[lp-diskd] password for %s: ",
             lp_user_by_uid((uid_t)lp_getuid(), &u) ? u.name : "you");
    if (!read_secret(prompt, pw, sizeof pw))
        return 1;
    static char areq[2 * LP_CRYPT6_PW_MAX + 16];
    size_t ak = strlcpy(areq, "auth\t", sizeof areq);
    to_hex(pw, strlen(pw), areq + ak, sizeof areq - ak - 2);
    scrub(pw, sizeof pw);
    ak = strlen(areq);
    areq[ak++] = '\n';
    char last[300] = "";
    bool dummy = false, was = pretty;
    int ar;
    pretty = false;
    silent = true;
    for (int round = 0; ; round++) {
        ar = talk(areq, ak, -1, &dummy, last, sizeof last);
        long secs = starts(last, "fail auth wait ") ? strtol(last + 15, NULL, 10) : 0;
        if (ar == 1 && round == 0 && secs > 0 && secs <= 30) {
            dprintf(STDERR_FILENO, "lp-diskctl: waiting %ld s after a wrong password\n", secs);
            lp_sleep_ms(secs * 1000 + 100);
            continue;
        }
        break;
    }
    pretty = was;
    silent = false;
    scrub(areq, sizeof areq);
    if (ar == 1 && last[0])
        dprintf(STDERR_FILENO, "lp-diskctl: %s\n", last);
    return ar;
}

static int ctl_remote(const char *req, size_t k, int fdx)
{
    bool need = false;
    int rc = talk(req, k, fdx, &need, NULL, 0);
    /* Three tries, as sudo gives; the daemon makes each wrong one wait. */
    for (int attempt = 0; need && attempt < 3; attempt++) {
        int ar = auth_prompt();
        if (ar == 2)
            return 2;
        if (ar == 0) {
            need = false;
            rc = talk(req, k, fdx, &need, NULL, 0);
            break;
        }
        if (!lp_isatty(STDIN_FILENO))
            break;
    }
    if (need) {
        printf("fail auth password required\n");
        return 1;
    }
    return rc;
}

/* "300G", "512MiB", "1.5T", "4096" (bytes). Binary units. */
static bool parse_size(const char *s, u64 *out)
{
    u64 whole = 0, frac = 0, fdiv = 1;
    const char *p = s;
    if (*p < '0' || *p > '9') return false;
    for (; *p >= '0' && *p <= '9'; p++) whole = whole * 10 + (u64)(*p - '0');
    if (*p == '.') {
        for (p++; *p >= '0' && *p <= '9' && fdiv < 1000000; p++) {
            frac = frac * 10 + (u64)(*p - '0');
            fdiv *= 10;
        }
    }
    u64 mul = 1;
    switch (*p) {
    case 'k': case 'K': mul = 1024ull; p++; break;
    case 'm': case 'M': mul = MIB; p++; break;
    case 'g': case 'G': mul = GIB; p++; break;
    case 't': case 'T': mul = 1024ull * GIB; p++; break;
    case '\0': break;
    default: return false;
    }
    if (mul > 1 && (!strcmp(p, "iB") || !strcmp(p, "B") || !strcmp(p, "ib"))) p += strlen(p);
    if (*p) return false;
    *out = whole * mul + frac * mul / fdiv;
    return true;
}

/* A label as the protocol wants it: itself when it is plain, else x:hex. */
static void label_field(const char *l, char *out, size_t n)
{
    if (!l[0]) { strlcpy(out, "-", n); return; }
    if (alphabet_ok(l, " _.-", 255, true) && l[0] != ' ') { strlcpy(out, l, n); return; }
    strlcpy(out, "x:", n);
    to_hex(l, strlen(l), out + 2, n - 2);
}

static void ctl_usage(void)
{
    printf("usage: lp-diskctl [--raw] [--confirm WORD] <command> [arg...]\n\n"
           "Disks from a terminal: the same checks and refusals as the Disks\n"
           "application. As root (the recovery shell) it works without the\n"
           "service; otherwise it asks lp-diskd, and your password when needed.\n"
           "Sizes take binary units: 300G, 512M, 1.5T; a bare number is bytes.\n\n"
           "  list [disk]                     disks, partitions, free space\n"
           "  smart <disk>                    health, in plain words\n"
           "  minsize <part>                  how far it can shrink\n"
           "  check <part> | repair <part>    fsck (read-only | repair)\n"
           "  resize <part> <size>            grow or shrink (ext4, NTFS, FAT, swap)\n"
           "  move <part> <start>             move it on the disk (slow)\n"
           "  format <part> <fs> [label]      ext4 btrfs fat32 exfat ntfs swap luks-ext4\n"
           "  create <disk> <start> <size> <fs> [label] [type]\n"
           "  delete <part>   wipe <dev>   mklabel <disk> gpt|mbr\n"
           "  label <dev> <label>   name <part> <name>   type <part> <type>\n"
           "  flags <part> <esp,boot,msftdata,lvm,raid,hidden,legacy_boot|none>\n"
           "  mount <part>   unmount <dev>   eject <disk>   poweroff <disk>\n"
           "  luks-open <part>   luks-close <part>\n"
           "  bench <dev> [seconds]   image-save <part> <file>   image-restore <part> <file>\n"
           "  fstab   fstab-set <part> <name> auto|noauto rw|ro   fstab-remove <uuid>\n"
           "  erase <disk>                    overwrite a removable drive\n"
           "  plan <step> [args] [| <step> ...]    several steps as one plan\n"
           "  rescan <disk>                   make the kernel match the table\n"
           "  status   cancel   resume   auth   forget   ping\n\n"
           "Every request is written to %s.\n", LOG_PATH);
}

static int ctl(int argc, char **argv)
{
    char confirm[48] = "";
    int a = 0;
    while (a < argc && argv[a][0] == '-' && argv[a][1] == '-') {
        if (!strcmp(argv[a], "--raw")) { pretty = false; a++; }
        else if (!strcmp(argv[a], "--confirm") && a + 1 < argc) {
            snprintf(confirm, sizeof confirm, "confirm=%s", argv[a + 1]);
            a += 2;
        } else if (!strcmp(argv[a], "--socket") && a + 1 < argc) {
            sock_path = argv[a + 1];
            a += 2;
        } else break;
    }
    if (a >= argc || !strcmp(argv[a], "help") || !strcmp(argv[a], "-h") ||
        !strcmp(argv[a], "--help")) {
        ctl_usage();
        return a >= argc ? 2 : 0;
    }
    const char *cmd = argv[a++];
    int n = argc - a;
    char **x = argv + a;
    static char *f[MAX_FIELDS];
    static char store[MAX_FIELDS][600];
    int nf = 0;
    int fdx = -1;
    char pass[260] = "";
    #define PUT(s) do { if (nf < MAX_FIELDS) { strlcpy(store[nf], (s), sizeof store[nf]); f[nf] = store[nf]; nf++; } } while (0)

    static const char *const PLAN1[] = { "check", "repair", "delete", "wipe", NULL };
    bool is_plan1 = false;
    for (int i = 0; PLAN1[i]; i++) if (!strcmp(cmd, PLAN1[i])) is_plan1 = true;
    char tmp[600];
    u64 v1, v2;

    if (!strcmp(cmd, "auth")) {
        /* `lp-diskctl auth`: ask now, like `sudo -v`. */
        if (lp_getuid() == 0) { printf("done root needs no password\n"); return 0; }
        int ar = auth_prompt();
        if (ar == 0) printf("done authorised for %d s\n", AUTH_KEEP_MS / 1000);
        return ar;
    } else if (is_plan1 && n == 1) {
        PUT("plan"); PUT(cmd); PUT(x[0]);
        if (confirm[0]) PUT(confirm);
    } else if ((!strcmp(cmd, "resize") || !strcmp(cmd, "move")) && n == 2) {
        if (!parse_size(x[1], &v1)) { dprintf(2, "lp-diskctl: \"%s\" is not a size\n", x[1]); return 2; }
        snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)v1);
        PUT("plan"); PUT(cmd); PUT(x[0]); PUT(tmp);
    } else if (!strcmp(cmd, "format") && (n == 2 || n == 3)) {
        PUT("plan"); PUT("format"); PUT(x[0]); PUT(x[1]);
        label_field(n == 3 ? x[2] : "", tmp, sizeof tmp); PUT(tmp);
        if (!strcmp(x[1], "luks-ext4")) {
            char again[260];
            if (!read_secret("passphrase for the new encrypted partition: ", pass, sizeof pass) ||
                !read_secret("the same passphrase again: ", again, sizeof again)) return 1;
            if (strcmp(pass, again) || strlen(pass) < 8) {
                scrub(again, sizeof again);
                dprintf(2, "lp-diskctl: the passphrases differ, or it is shorter than 8\n");
                return 1;
            }
            scrub(again, sizeof again);
            strlcpy(tmp, "k:", sizeof tmp);
            to_hex(pass, strlen(pass), tmp + 2, sizeof tmp - 2);
            PUT(tmp);
        }
        if (confirm[0]) PUT(confirm);
    } else if (!strcmp(cmd, "create") && n >= 4 && n <= 6) {
        if (!parse_size(x[1], &v1) || !parse_size(x[2], &v2)) { dprintf(2, "lp-diskctl: bad start or size\n"); return 2; }
        PUT("plan"); PUT("create"); PUT(x[0]);
        snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)v1); PUT(tmp);
        snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)v2); PUT(tmp);
        PUT(x[3]);
        label_field(n >= 5 ? x[4] : "", tmp, sizeof tmp); PUT(tmp);
        PUT(n == 6 ? x[5] : "auto");
        if (!strcmp(x[3], "luks-ext4")) {
            char again[260];
            if (!read_secret("passphrase for the new encrypted partition: ", pass, sizeof pass) ||
                !read_secret("the same passphrase again: ", again, sizeof again)) return 1;
            bool same = !strcmp(pass, again) && strlen(pass) >= 8;
            scrub(again, sizeof again);
            if (!same) { dprintf(2, "lp-diskctl: the passphrases differ, or it is shorter than 8\n"); return 1; }
            strlcpy(tmp, "k:", sizeof tmp);
            to_hex(pass, strlen(pass), tmp + 2, sizeof tmp - 2);
            PUT(tmp);
        }
    } else if ((!strcmp(cmd, "label") || !strcmp(cmd, "name")) && n == 2) {
        PUT("plan"); PUT(cmd); PUT(x[0]);
        label_field(x[1], tmp, sizeof tmp); PUT(tmp);
    } else if ((!strcmp(cmd, "type") || !strcmp(cmd, "flags") || !strcmp(cmd, "mklabel")) && n == 2) {
        PUT("plan"); PUT(cmd); PUT(x[0]); PUT(x[1]);
        if (confirm[0]) PUT(confirm);
    } else if (!strcmp(cmd, "luks-open") && n == 1) {
        if (!read_secret("passphrase: ", pass, sizeof pass)) return 1;
        strlcpy(tmp, "k:", sizeof tmp);
        to_hex(pass, strlen(pass), tmp + 2, sizeof tmp - 2);
        PUT("luks-open"); PUT(x[0]); PUT(tmp);
    } else if ((!strcmp(cmd, "image-save") || !strcmp(cmd, "image-restore")) && n == 2) {
        bool save = !strcmp(cmd, "image-save");
        long fd = save ? lp_open(x[1], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)
                       : lp_open(x[1], O_RDONLY | O_CLOEXEC, 0);
        if (fd < 0) { dprintf(2, "lp-diskctl: cannot open %s\n", x[1]); return 2; }
        fdx = (int)fd;
        PUT(cmd); PUT(x[0]);
        if (!save && confirm[0]) PUT(confirm);
    } else if (!strcmp(cmd, "erase") && n == 1) {
        char typed[64];
        dprintf(2, "Everything on %s will be overwritten and cannot be recovered.\n", x[0]);
        if (!confirm[0]) {
            if (!lp_isatty(STDIN_FILENO)) { dprintf(2, "lp-diskctl: use --confirm %s\n", x[0]); return 2; }
            dprintf(2, "Type the disk's name (%s) to go ahead: ", x[0]);
            long r = readline(STDIN_FILENO, typed, sizeof typed);
            if (r < 0) return 1;
            snprintf(confirm, sizeof confirm, "confirm=%s", typed);
        }
        PUT("erase"); PUT(x[0]); PUT(confirm);
    } else {
        /* Anything else goes as it is: list, smart, plan ..., fstab-set ... */
        PUT(cmd);
        for (int i = 0; i < n; i++) PUT(x[i]);
        if (confirm[0] && !strcmp(cmd, "image-restore")) PUT(confirm);
    }
    #undef PUT

    static char req[MAX_REQ];
    size_t k = 0;
    for (int i = 0; i < nf; i++) {
        if (i && k < sizeof req - 1) req[k++] = '\t';
        k += strlcpy(req + k, f[i], sizeof req - k);
        if (k >= sizeof req - 2) {
            dprintf(2, "lp-diskctl: the request is too long\n");
            return 2;
        }
    }
    req[k++] = '\n';
    req[k] = '\0';
    scrub(pass, sizeof pass);
    for (int i = 0; i < nf; i++) if (starts(store[i], "k:")) scrub(store[i], sizeof store[i]);

    lp_signal_handler(SIGINT, on_sigint);
    int rc;
    if (lp_getuid() == 0)
        rc = ctl_local(req, k, fdx);
    else
        rc = ctl_remote(req, k, fdx);
    scrub(req, sizeof req);
    if (fdx >= 0) lp_close(fdx);
    return rc;
}

static void daemon_usage(void)
{
    printf("usage: lp-diskd -d [--socket PATH]     run the disk service (root)\n"
           "       lp-diskctl <command> [arg...]   use it (see lp-diskctl help)\n"
           "       lp-diskd ctl <command> [arg...] the same, without the second name\n\n"
           "Verbs the service answers:\n");
    for (int i = 0; VERBS[i].verb; i++)
        printf("  %-14s %s%s\n", VERBS[i].verb, VERBS[i].help,
               VERBS[i].admin ? "  (sudo + password)" : "");
}

int main(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (!strcmp(base, "lp-diskctl"))
        return ctl(argc - 1, argv + 1);
    int a = 1;
    bool daemon = false;
    while (a < argc && argv[a][0] == '-') {
        if (!strcmp(argv[a], "-d")) { daemon = true; a++; }
        else if (!strcmp(argv[a], "--socket") && a + 1 < argc) { sock_path = argv[a + 1]; a += 2; }
        else if (!strcmp(argv[a], "-h") || !strcmp(argv[a], "--help")) { daemon_usage(); return 0; }
        else break;
    }
    if (daemon) {
        if (lp_getuid() != 0) {
            dprintf(STDERR_FILENO, "lp-diskd: the service has to run as root\n");
            return 1;
        }
        return serve();
    }
    if (a < argc && !strcmp(argv[a], "ctl"))
        return ctl(argc - a - 1, argv + a + 1);
    daemon_usage();
    return a < argc ? 2 : 0;
}
