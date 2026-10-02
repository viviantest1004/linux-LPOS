/* unistd.c - thin wrappers over the system calls. */
#include "unistd.h"
#include "syscall.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"

char **environ = NULL;

/* ── Files ─────────────────────────────────────────────────────── */

long lp_open(const char *path, int flags, mode_t mode)
{
    /* AArch64 has no open. Use openat with AT_FDCWD. */
    return sys_call4(SYS_openat, AT_FDCWD, (long)path, flags, (long)mode);
}

long lp_close(int fd)                    { return sys_call1(SYS_close, fd); }
long lp_read(int fd, void *b, size_t n)  { return sys_call3(SYS_read, fd, (long)b, (long)n); }
long lp_write(int fd, const void *b, size_t n) { return sys_call3(SYS_write, fd, (long)b, (long)n); }
/* lseek, with an offset that does not fit in a register on every
 * machine.
 *
 * On a 64-bit machine this is the one syscall. On 32-bit ARM, lseek at
 * 19 takes and returns a 32-bit offset: it works perfectly up to two
 * gigabytes and then stops, which is not an error anybody sees. `dd
 * skip=3000` read from the start of the file instead of 3GB in, and
 * `wc -c` on a 5GB file said 1GB - the top bits, quietly gone.
 *
 * _llseek is the call for it: the offset goes in as two halves and the
 * answer comes back through a pointer, because there is nowhere else to
 * put 64 bits. The return type here is s64 for the same reason - a long
 * on this machine cannot hold the answer. */
s64 lp_lseek(int fd, off_t o, int w)
{
#if defined(__arm__)
    s64 result = 0;
    long r = sys_call5(SYS_llseek, fd,
                       (long)((u64)o >> 32), (long)((u64)o & 0xffffffffu),
                       (long)&result, w);
    if (r < 0)
        return r;
    return result;
#else
    return sys_call3(SYS_lseek, fd, (long)o, w);
#endif
}

long lp_dup(int fd) { return sys_call1(SYS_dup, fd); }

long lp_dup2(int oldfd, int newfd)
{
    if (oldfd == newfd)
        return newfd;                    /* dup3 returns EINVAL when they match */
    return sys_call3(SYS_dup3, oldfd, newfd, 0);
}

long lp_pipe(int fds[2])                 { return sys_call2(SYS_pipe2, (long)fds, 0); }
long lp_unlink(const char *p)            { return sys_call3(SYS_unlinkat, AT_FDCWD, (long)p, 0); }
long lp_rmdir(const char *p)             { return sys_call3(SYS_unlinkat, AT_FDCWD, (long)p, AT_REMOVEDIR); }
long lp_mkdir(const char *p, mode_t m)   { return sys_call3(SYS_mkdirat, AT_FDCWD, (long)p, (long)m); }
long lp_chdir(const char *p)             { return sys_call1(SYS_chdir, (long)p); }
long lp_umask(long m)                    { return sys_call1(SYS_umask, m); }
long lp_getcwd(char *b, size_t n)        { return sys_call2(SYS_getcwd, (long)b, (long)n); }
long lp_access(const char *p, int mode)  { return sys_call4(SYS_faccessat, AT_FDCWD, (long)p, mode, 0); }
long sys_getdents(int fd, void *buf, size_t size) { return sys_call3(SYS_getdents64, fd, (long)buf, (long)size); }

/* ── struct stat, by offset ──
 *
 * Rather than declaring the whole thing we read the handful of fields we
 * use at fixed offsets, so nothing depends on how a compiler pads a
 * struct. The offsets are not the same on both machines: arm64 uses the
 * asm-generic layout, x86-64 carries its own older one where st_nlink
 * is 64 bits and comes before st_mode instead of after it.
 *
 *              arm64   x86-64
 *   st_mode      16      24
 *   st_nlink     20      16   (32-bit on arm64, 64-bit on x86-64)
 *   st_uid       24      28
 *   st_gid       28      32
 *   st_size      48      48
 *   st_mtime     88      88
 *
 * st_size and st_mtime happen to land in the same place on both, which
 * is luck rather than design - they are written out here anyway so that
 * nobody has to rediscover it. */
#if defined(__x86_64__)
#  define STAT_BUF_SIZE  144
#  define STAT_MODE_OFF   24
#  define STAT_NLINK_OFF  16
#  define STAT_UID_OFF    28
#  define STAT_GID_OFF    32
#else
#  define STAT_BUF_SIZE  128
#  define STAT_MODE_OFF   16
#  define STAT_NLINK_OFF  20
#  define STAT_UID_OFF    24
#  define STAT_GID_OFF    28
#endif
#define STAT_MTIME_OFF    88
/* st_blocks: 512-byte units the file actually occupies, which is not
 * the same as its size - a sparse file uses fewer, and a small file on
 * a filesystem with big blocks uses more. `ls -l` prints the sum as
 * "total", so guessing it from the size gave a different number from
 * every other ls. Both architectures put it at 64. */
#define STAT_BLOCKS_OFF   64
/* dev, ino, atime and ctime land in the same place on both; only rdev
 * and the width of st_blksize differ. */
#define STAT_DEV_OFF       0
#define STAT_INO_OFF       8
#define STAT_BLKSIZE_OFF  56
#define STAT_ATIME_OFF    72
#define STAT_CTIME_OFF   104
#if defined(__x86_64__)
#  define STAT_RDEV_OFF   40
#else
#  define STAT_RDEV_OFF   32
#endif
#define S_IFMT          0170000
#define S_IFDIR         0040000

static long try_statx(const char *path, lp_stat_t *out, bool follow_symlink);

static long stat_mode(const char *path, u32 *mode_out)
{
#if defined(__arm__)
    /* 32-bit ARM has no newfstatat at all, and its fstatat64 struct is
     * a different shape again. statx is the same shape on every machine
     * and has been in the kernel since 4.11, which is far older than
     * anything this image ships with, so ARM simply uses it. */
    lp_stat_t st;
    long r = try_statx(path, &st, true);
    if (r < 0)
        return r;
    *mode_out = st.mode;
    return 0;
#else
    u8 buf[STAT_BUF_SIZE];
    long r = sys_call4(SYS_newfstatat, AT_FDCWD, (long)path, (long)buf, 0);
    if (r < 0)
        return r;
    *mode_out = *(u32 *)(buf + STAT_MODE_OFF);
    return 0;
#endif
}

bool lp_exists(const char *path)
{
    u32 mode;
    return stat_mode(path, &mode) == 0;
}

bool lp_is_dir(const char *path)
{
    u32 mode;
    if (stat_mode(path, &mode) != 0)
        return false;
    return (mode & S_IFMT) == S_IFDIR;
}

/* Offsets of the two struct stat fields we use:
 *   16  st_mode (u32)
 *   48  st_size (s64)
 * everything else is skipped. */
#define STAT_SIZE_OFF       48
#define AT_SYMLINK_NOFOLLOW 0x100

/* statx, laid out the way the kernel writes it. It is here for one
 * field - the creation time - which newfstatat cannot report at all and
 * which `stat` prints on every other Linux. Everything else it returns
 * we already had. Kernels before 4.11 have no statx, so a failure here
 * falls back rather than failing the call. */
#define STATX_BASIC_STATS 0x7ff
#define STATX_BTIME       0x800
#define STATX_MASK_OFF      0
#define STATX_BLKSIZE_OFF   4
#define STATX_NLINK_OFF    16
#define STATX_UID_OFF      20
#define STATX_GID_OFF      24
#define STATX_MODE_OFF     28
#define STATX_INO_OFF      32
#define STATX_SIZE_OFF     40
#define STATX_BLOCKS_OFF   48
#define STATX_ATIME_OFF    64
#define STATX_BTIME_OFF    80
#define STATX_CTIME_OFF    96
#define STATX_MTIME_OFF   112
#define STATX_RDEV_MAJ_OFF 128
#define STATX_RDEV_MIN_OFF 132
#define STATX_DEV_MAJ_OFF 136
#define STATX_DEV_MIN_OFF 140

static u64 makedev(u32 maj, u32 min)
{
    return ((u64)(maj & 0xfff) << 8) | (u64)(min & 0xff) |
           ((u64)(maj & ~0xfffu) << 32) | ((u64)(min & ~0xffu) << 12);
}

static long try_statx(const char *path, lp_stat_t *out, bool follow_symlink)
{
    u8 buf[256];
    memset(buf, 0, sizeof buf);
    long r = sys_call5(SYS_statx, AT_FDCWD, (long)path,
                       follow_symlink ? 0 : AT_SYMLINK_NOFOLLOW,
                       STATX_BASIC_STATS | STATX_BTIME, (long)buf);
    if (r < 0)
        return r;

    u32 mask   = *(u32 *)(buf + STATX_MASK_OFF);
    out->mode  = *(u16 *)(buf + STATX_MODE_OFF);
    out->size  = *(u64 *)(buf + STATX_SIZE_OFF);
    out->nlink = *(u32 *)(buf + STATX_NLINK_OFF);
    out->uid   = *(u32 *)(buf + STATX_UID_OFF);
    out->gid   = *(u32 *)(buf + STATX_GID_OFF);
    out->blocks  = *(u64 *)(buf + STATX_BLOCKS_OFF);
    out->blksize = *(u32 *)(buf + STATX_BLKSIZE_OFF);
    out->ino     = *(u64 *)(buf + STATX_INO_OFF);
    out->atime = *(s64 *)(buf + STATX_ATIME_OFF);
    out->mtime = *(s64 *)(buf + STATX_MTIME_OFF);
    out->ctime = *(s64 *)(buf + STATX_CTIME_OFF);
    out->atime_ns = *(u32 *)(buf + STATX_ATIME_OFF + 8);
    out->mtime_ns = *(u32 *)(buf + STATX_MTIME_OFF + 8);
    out->ctime_ns = *(u32 *)(buf + STATX_CTIME_OFF + 8);
    out->btime    = (mask & STATX_BTIME) ? *(s64 *)(buf + STATX_BTIME_OFF) : 0;
    out->btime_ns = (mask & STATX_BTIME) ? *(u32 *)(buf + STATX_BTIME_OFF + 8) : 0;
    out->has_btime = (mask & STATX_BTIME) != 0;
    out->rdev = makedev(*(u32 *)(buf + STATX_RDEV_MAJ_OFF),
                        *(u32 *)(buf + STATX_RDEV_MIN_OFF));
    out->dev  = makedev(*(u32 *)(buf + STATX_DEV_MAJ_OFF),
                        *(u32 *)(buf + STATX_DEV_MIN_OFF));
    return 0;
}

long lp_stat(const char *path, lp_stat_t *out, bool follow_symlink)
{
    long sx = try_statx(path, out, follow_symlink);
    if (sx == 0)
        return 0;

#if defined(__arm__)
    /* No newfstatat here, and no second layout worth carrying: statx is
     * the one call, so its error is the answer. */
    return sx;
#else
    u8 buf[STAT_BUF_SIZE];
    long r = sys_call4(SYS_newfstatat, AT_FDCWD, (long)path, (long)buf,
                       follow_symlink ? 0 : AT_SYMLINK_NOFOLLOW);
    if (r < 0)
        return r;
    out->mode  = *(u32 *)(buf + STAT_MODE_OFF);
    out->size  = *(u64 *)(buf + STAT_SIZE_OFF);
#if defined(__x86_64__)
    out->nlink = (u32)*(u64 *)(buf + STAT_NLINK_OFF);
#else
    out->nlink = *(u32 *)(buf + STAT_NLINK_OFF);
#endif
    out->uid   = *(u32 *)(buf + STAT_UID_OFF);
    out->gid   = *(u32 *)(buf + STAT_GID_OFF);
    out->mtime = *(s64 *)(buf + STAT_MTIME_OFF);
    out->blocks = *(u64 *)(buf + STAT_BLOCKS_OFF);
    out->dev   = *(u64 *)(buf + STAT_DEV_OFF);
    out->ino   = *(u64 *)(buf + STAT_INO_OFF);
    out->rdev  = *(u64 *)(buf + STAT_RDEV_OFF);
#if defined(__x86_64__)
    out->blksize = *(u64 *)(buf + STAT_BLKSIZE_OFF);
#else
    out->blksize = *(u32 *)(buf + STAT_BLKSIZE_OFF);
#endif
    out->atime = *(s64 *)(buf + STAT_ATIME_OFF);
    out->ctime = *(s64 *)(buf + STAT_CTIME_OFF);
    out->atime_ns = (u32)*(u64 *)(buf + STAT_ATIME_OFF + 8);
    out->mtime_ns = (u32)*(u64 *)(buf + STAT_MTIME_OFF + 8);
    out->ctime_ns = (u32)*(u64 *)(buf + STAT_CTIME_OFF + 8);
    out->btime = 0;
    out->btime_ns = 0;
    out->has_btime = false;
    return 0;
#endif
}

/* Set a file's length. truncate and `> file` both want it, and it is
 * the only way to make a sparse file without writing the whole thing. */
long lp_ftruncate(int fd, s64 length)
{
    return sys_call2(SYS_ftruncate, fd, (long)length);
}

/* A hard link: a second name for the same inode. */
long lp_link(const char *from, const char *to)
{
    return sys_call5(SYS_linkat, AT_FDCWD, (long)from, AT_FDCWD, (long)to, 0);
}

/* A named pipe, or any other node the caller has a mode for. */
long lp_mknod(const char *path, mode_t mode, u64 dev)
{
    return sys_call4(SYS_mknodat, AT_FDCWD, (long)path, mode, (long)dev);
}

/* Force one file's contents to the disk. sync() does the whole system
 * and returns before the disk has necessarily finished; this waits for
 * one file, which is what a settings write actually needs. */
long lp_fsync(int fd)
{
    return sys_call1(SYS_fsync, fd);
}

/* ── Writing a setting so that a power cut cannot lose it ─────────────
 *
 * open(O_TRUNC), write, close looks like it saves a file. What it
 * actually does is empty the file first and fill it in afterwards, and
 * between those two the file is zero bytes long. Pull the power there -
 * which on a board with no battery is not a rare event, it is Tuesday -
 * and the setting is not "the old value" or "the new value", it is
 * gone. The timezone comes back as UTC and every timestamp on the
 * machine is wrong by hours until somebody notices.
 *
 * So: write a new file beside it, make sure that reached the disk, then
 * rename over the old one. rename is atomic - a reader sees the old
 * contents or the new ones and never anything in between - and the
 * final fsync on the directory is what makes the rename itself survive
 * the power going, rather than just the bytes it points at.
 */
bool lp_write_file_atomic(const char *path, const void *data, size_t n)
{
    char tmp[1024];
    int  len = snprintf(tmp, sizeof tmp, "%s.new", path);
    if (len <= 0 || (size_t)len >= sizeof tmp)
        return false;

    long fd = lp_open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;

    const u8 *p = data;
    size_t left = n;
    while (left) {
        long w = lp_write((int)fd, p, left);
        if (w <= 0) { lp_close((int)fd); lp_unlink(tmp); return false; }
        p += w;
        left -= (size_t)w;
    }
    if (lp_fsync((int)fd) < 0) { lp_close((int)fd); lp_unlink(tmp); return false; }
    lp_close((int)fd);

    if (lp_rename(tmp, path) < 0) { lp_unlink(tmp); return false; }

    /* The directory entry now points at the new file. Without this the
     * rename can still be sitting in the page cache when the power goes. */
    char dir[1024];
    strlcpy(dir, path, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        long dfd = lp_open(dir[0] ? dir : "/", O_RDONLY, 0);
        if (dfd >= 0) { lp_fsync((int)dfd); lp_close((int)dfd); }
    }
    return true;
}

/* Where a setting belongs on this machine.
 *
 * On a RAM root only /data survives a reboot, so that is where it goes.
 * On a disk root /data does not exist and /etc is an ordinary writable
 * directory, so writing to /data would silently save nothing. Try the
 * writable one and say which it was. */
const char *lp_setting_path(const char *name, char *buf, size_t cap)
{
    snprintf(buf, cap, "/data/%s", name);
    if (lp_is_dir("/data") && lp_access("/data", W_OK) == 0)
        return buf;
    snprintf(buf, cap, "/etc/%s", name);
    return buf;
}

long lp_rename(const char *from, const char *to)
{
    return sys_call4(SYS_renameat, AT_FDCWD, (long)from, AT_FDCWD, (long)to);
}

long lp_chmod(const char *path, mode_t mode)
{
    return sys_call4(SYS_fchmodat, AT_FDCWD, (long)path, (long)mode, 0);
}

long lp_symlink(const char *target, const char *linkpath)
{
    return sys_call3(SYS_symlinkat, (long)target, AT_FDCWD, (long)linkpath);
}

long lp_readlink(const char *path, char *buf, size_t n)
{
    return sys_call4(SYS_readlinkat, AT_FDCWD, (long)path, (long)buf, (long)n);
}

/* ── Processes ─────────────────────────────────────────────────── */

pid_t lp_fork(void)
{
    /* AArch64 has no fork system call. clone's arguments are
     *   clone(flags, stack, parent_tid, tls, child_tid)
     * Passing only the exit signal in flags and zero for the rest is fork.
     * With stack=0 the child inherits the parent's stack copy-on-write. */
    return (pid_t)sys_call5(SYS_clone, SIGCHLD, 0, 0, 0, 0);
}

long lp_execve(const char *path, char *const argv[], char *const envp[])
{
    return sys_call3(SYS_execve, (long)path, (long)argv, (long)envp);
}

pid_t lp_waitpid(pid_t pid, int *status, int options)
{
    return (pid_t)sys_call4(SYS_wait4, pid, (long)status, options, 0);
}

pid_t lp_wait(int *status)
{
    return lp_waitpid(-1, status, 0);
}

pid_t lp_getpid(void)      { return (pid_t)sys_call0(SYS_getpid); }
long  lp_setsid(void)      { return sys_call0(SYS_setsid); }
/* A negative pid is passed straight through, because that is how the
 * kernel is told "the whole process group": kill(-pgid, sig). Against a
 * fork bomb that is the difference between one syscall and one per
 * process. */
long  lp_kill(pid_t p, int s) { return sys_call2(SYS_kill, p, s); }

long lp_setrlimit(int resource, u64 soft, u64 hard)
{
    /* struct rlimit64 { u64 rlim_cur; u64 rlim_max; } */
    u64 lim[2] = { soft, hard };
    /* prlimit64(pid=0 meaning ourselves, resource, new, old) */
    return sys_call4(SYS_prlimit64, 0, resource, (long)lim, 0);
}

void lp_exit(int code)
{
    sys_call1(SYS_exit_group, code);
    __builtin_unreachable();
}

long lp_sleep_ms(long ms)
{
    /* struct timespec { long tv_sec; long tv_nsec; } */
    long ts[2] = { ms / 1000, (ms % 1000) * 1000000L };
    return sys_call2(SYS_nanosleep, (long)ts, 0);
}

/* ── System ────────────────────────────────────────────────────── */

long lp_mount(const char *src, const char *tgt, const char *fstype,
              unsigned long flags, const void *data)
{
    return sys_call5(SYS_mount, (long)src, (long)tgt, (long)fstype,
                     (long)flags, (long)data);
}

long lp_umount(const char *tgt, int flags)
{
    return sys_call2(SYS_umount2, (long)tgt, flags);
}

long lp_chroot(const char *path)
{
    return sys_call1(SYS_chroot, (long)path);
}

long lp_reboot(int cmd)
{
    return sys_call4(SYS_reboot, (long)LINUX_REBOOT_MAGIC1,
                     (long)LINUX_REBOOT_MAGIC2, cmd, 0);
}

/* ── The terminal ──────────────────────────────────────────────── */

long lp_ioctl(int fd, unsigned long req, void *arg)
{
    return sys_call3(SYS_ioctl, fd, (long)req, (long)arg);
}

#define TCGETS       0x5401
#define TCSETS       0x5402
#define TIOCGWINSZ   0x5413

/* struct termios layout (arm64):
 *    0  c_iflag (u32)
 *    4  c_oflag (u32)
 *    8  c_cflag (u32)
 *   12  c_lflag (u32)
 *   16  c_line  (u8)
 *   17  c_cc[19]
 */
#define T_IFLAG  0
#define T_OFLAG  1
#define T_LFLAG  3
#define T_CC     17
#define VTIME    5
#define VMIN     6
#define VSUSP    10

/* c_lflag */
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0008
#define IEXTEN  0x8000
/* c_iflag */
#define BRKINT  0x0002
#define ISTRIP  0x0020
#define INLCR   0x0040
#define ICRNL   0x0100
#define IXON    0x0400
/* IUTF8: the kernel's line erase treats UTF-8 as whole characters */
#define IUTF8   0x4000
/* c_oflag */
#define OPOST   0x0001
#define ONLCR   0x0004

long lp_term_raw(int fd, lp_termios_t *saved)
{
    long r = lp_ioctl(fd, TCGETS, saved->raw);
    if (r < 0)
        return r;

    lp_termios_t t = *saved;
    u32 *f = (u32 *)t.raw;

    /* Turn off the kernel's line editing, echo and signals - we draw it
     * all ourselves. IXON has to go or Ctrl-S freezes the screen instead
     * of reaching us. ICRNL has to go so Enter arrives as a plain CR.
     * OPOST has to go so \n is not turned into CRLF - we emit that. */
    f[T_LFLAG] &= ~(u32)(ICANON | ECHO | ISIG | IEXTEN);
    f[T_IFLAG] &= ~(u32)(IXON | ICRNL | BRKINT | ISTRIP | INLCR);
    f[T_OFLAG] &= ~(u32)OPOST;

    /* Return as soon as one byte arrives (VMIN=1, VTIME=0). */
    f[T_IFLAG] |= IUTF8;        /* keep telling the terminal it is UTF-8 */

    t.raw[T_CC + VMIN]  = 1;
    t.raw[T_CC + VTIME] = 0;

    return lp_ioctl(fd, TCSETS, t.raw);
}

/* Put a terminal back into the state a person expects.
 *
 * A program that dies while it owns the terminal - killed, or crashed -
 * leaves it however it was: no echo, no line editing, newlines that do
 * not return to column one. The shell then looks broken, and the way
 * out is to type a command you cannot see into a terminal that is not
 * listening properly.
 *
 * So the shell puts it back after every command rather than trusting
 * each program to clean up after itself. This sets only the flags that
 * matter for a usable terminal and leaves the rest - baud rate, control
 * characters - as they were. */
bool lp_isatty(int fd)
{
    lp_termios_t t;
    return lp_ioctl(fd, TCGETS, t.raw) == 0;
}

long lp_term_sane(int fd)
{
    lp_termios_t t;
    if (lp_ioctl(fd, TCGETS, t.raw) < 0)
        return -1;

    u32 *f = (u32 *)t.raw;
    f[T_LFLAG] |= (u32)(ICANON | ECHO | ISIG | IEXTEN);
    f[T_IFLAG] |= (u32)(ICRNL | BRKINT | IXON | IUTF8);
    f[T_OFLAG] |= (u32)(OPOST | ONLCR);

    /* ISIG above is what makes Ctrl-C a signal instead of a byte, and
     * that is the point of this function. It also makes Ctrl-Z one, and
     * Ctrl-Z is a trap here: SIGTSTP stops the foreground process, and
     * nothing in this system can start a stopped process again. There is
     * no job control - no `fg`, no `bg` - so a stopped command would sit
     * there forever with the shell waiting on it, and the terminal would
     * be dead with no key that fixes it.
     *
     * So the suspend key is disabled outright. Ctrl-Z does nothing,
     * which is the honest behaviour for a shell that cannot resume
     * anything, and it leaves Ctrl-C - the one that has to work - as the
     * way out of a command that will not stop. */
    t.raw[T_CC + VSUSP] = 0;

    return lp_ioctl(fd, TCSETS, t.raw);
}

/* The same, minus the output processing. See the header. */
long lp_term_cbreak(int fd, lp_termios_t *saved)
{
    long r = lp_ioctl(fd, TCGETS, saved->raw);
    if (r < 0)
        return r;

    lp_termios_t t = *saved;
    u32 *f = (u32 *)t.raw;

    f[T_LFLAG] &= ~(u32)(ICANON | ECHO | ISIG | IEXTEN);
    f[T_IFLAG] &= ~(u32)(IXON | ICRNL | BRKINT | ISTRIP | INLCR);
    f[T_IFLAG] |= IUTF8;
    /* OPOST is deliberately left alone. */

    t.raw[T_CC + VMIN]  = 1;
    t.raw[T_CC + VTIME] = 0;

    return lp_ioctl(fd, TCSETS, t.raw);
}

long lp_term_restore(int fd, const lp_termios_t *saved)
{
    return lp_ioctl(fd, TCSETS, (void *)saved->raw);
}

long lp_term_set_utf8(int fd)
{
    lp_termios_t t;
    long r = lp_ioctl(fd, TCGETS, t.raw);
    if (r < 0)
        return r;
    u32 *f = (u32 *)t.raw;
    f[T_IFLAG] |= IUTF8;
    return lp_ioctl(fd, TCSETS, t.raw);
}

long lp_term_size(int fd, int *rows, int *cols)
{
    /* struct winsize { u16 row, col, xpixel, ypixel; } */
    u16 ws[4] = { 0, 0, 0, 0 };
    long r = lp_ioctl(fd, TIOCGWINSZ, ws);
    if (r < 0 || ws[0] == 0 || ws[1] == 0) {
        *rows = 24;
        *cols = 80;
        return -1;
    }
    *rows = ws[0];
    *cols = ws[1];
    return 0;
}

/* ── Time ──────────────────────────────────────────────────────── */

/* struct timespec on arm64 is { s64 tv_sec; s64 tv_nsec; }, 16 bytes.
 * We use an array rather than declaring the struct. */
#define CLOCK_REALTIME 0

s64 lp_time(void)
{
    s64 ts[2] = { 0, 0 };
    if (sys_call2(SYS_clock_gettime, CLOCK_REALTIME, (long)ts) < 0)
        return 0;
    return ts[0];
}

/* ── The battery-backed clock ─────────────────────────────────────────
 *
 * A PC and an EC2 instance have one; a Pi Zero 2 W does not. The
 * difference matters more than it sounds: a machine with an RTC comes
 * back from a week powered off knowing a week has passed, and one
 * without it comes back believing no time went by at all. Certificates,
 * log order and cron all depend on which of those is true.
 *
 * The kernel reads it once at boot (RTC_HCTOSYS). These two are for
 * writing it back, because a clock set by hand or by ntp is only worth
 * something if it is still there after the power goes.
 *
 * struct rtc_time is nine ints - the same shape as struct tm - and the
 * kernel keeps it in UTC.
 */
#define RTC_RD_TIME   0x80247009UL
#define RTC_SET_TIME  0x4024700aUL

static const char *RTC_PATHS[] = { "/dev/rtc0", "/dev/rtc", "/dev/misc/rtc", NULL };

static long rtc_open(int flags)
{
    for (int i = 0; RTC_PATHS[i]; i++) {
        long fd = lp_open(RTC_PATHS[i], flags, 0);
        if (fd >= 0)
            return fd;
    }
    return -2;                       /* ENOENT: this board has no RTC */
}

bool lp_rtc_read(s64 *out)
{
    long fd = rtc_open(O_RDONLY);
    if (fd < 0)
        return false;

    int t[9];
    memset(t, 0, sizeof t);
    long r = lp_ioctl((int)fd, RTC_RD_TIME, t);
    lp_close((int)fd);
    if (r < 0)
        return false;

    lp_tm_t tm = { .year = t[5] + 1900, .mon = t[4] + 1, .day = t[3],
                   .hour = t[2], .min = t[1], .sec = t[0], .wday = t[6] };
    *out = lp_timegm(&tm);
    return true;
}

bool lp_rtc_write(s64 unix_seconds)
{
    long fd = rtc_open(O_WRONLY);
    if (fd < 0)
        fd = rtc_open(O_RDONLY);     /* RTC_SET_TIME goes through either */
    if (fd < 0)
        return false;

    lp_tm_t tm;
    lp_gmtime(unix_seconds, &tm);

    int t[9] = { tm.sec, tm.min, tm.hour, tm.day,
                 tm.mon - 1, tm.year - 1900, tm.wday, 0, 0 };
    long r = lp_ioctl((int)fd, RTC_SET_TIME, t);
    lp_close((int)fd);
    return r >= 0;
}

long lp_settime(s64 unix_seconds)
{
    s64 ts[2] = { unix_seconds, 0 };
    return sys_call2(SYS_clock_settime, CLOCK_REALTIME, (long)ts);
}

long lp_sync(void)            { return sys_call0(SYS_sync); }
int  lp_getuid(void)          { return (int)sys_call0(SYS_getuid); }

/* CLOCK_MONOTONIC. Counts from an arbitrary point - only differences
 * between two readings mean anything, which is exactly what timing
 * something needs. */
#define CLOCK_MONOTONIC 1

s64 lp_monotonic_ms(void)
{
    s64 ts[2] = { 0, 0 };           /* { tv_sec, tv_nsec } */
    if (sys_call2(SYS_clock_gettime, CLOCK_MONOTONIC, (long)ts) < 0)
        return 0;
    return ts[0] * 1000 + ts[1] / 1000000;
}

/* ── Signals ──────────────────────────────────────────────────────────
 *
 * The kernel's struct sigaction, which is not the same shape on the two
 * machines:
 *
 *   arm64                     x86-64
 *    0  sa_handler             0  sa_handler
 *    8  sa_flags               8  sa_flags
 *   16  sa_mask               16  sa_restorer
 *   (24 bytes)                24  sa_mask
 *                             (32 bytes)
 *
 * arm64 does not define SA_RESTORER, so that field is simply absent and
 * sa_mask moves up. Getting this wrong is not a compile error: the
 * kernel would read sa_mask from eight bytes past the end of our
 * buffer, and the mask a signal is delivered under would be whatever
 * happened to be on the stack.
 *
 * SIG_DFL and SIG_IGN never run any of our code, so no restorer is
 * needed to get back from them. It is filled in anyway on x86-64, where
 * the kernel refuses to deliver a caught signal without one - so that
 * the day somebody adds a real handler here, it works rather than
 * killing the process in a way that takes an afternoon to explain. */
#if defined(__x86_64__)
#  define SA_SIZE       32
#  define SA_MASK_OFF   24
#  define SA_RESTORER_OFF 16
#  define SA_RESTORER   0x04000000UL
extern void lp_sigreturn_trampoline(void);
#else
#  define SA_SIZE       24
#  define SA_MASK_OFF   16
#endif
#define SA_HANDLER    0
#define SA_FLAGS      8
#define SA_MASK_SIZE  8

static long set_disposition(int sig, unsigned long handler)
{
    u8 act[SA_SIZE];
    memset(act, 0, sizeof(act));
    *(unsigned long *)(act + SA_HANDLER) = handler;

#if defined(__x86_64__)
    *(unsigned long *)(act + SA_FLAGS)        = SA_RESTORER;
    *(unsigned long *)(act + SA_RESTORER_OFF) =
        (unsigned long)&lp_sigreturn_trampoline;
#endif

    return sys_call4(SYS_rt_sigaction, (long)sig, (long)act, 0, SA_MASK_SIZE);
}

/* TIOCSCTTY: "make this terminal mine". The argument is 0 - 1 would
 * mean "steal it from whoever has it", which needs CAP_SYS_ADMIN and is
 * never what we want: if something else owns the console, taking it is
 * how you end up with two shells reading the same keystrokes. */
#define TIOCSCTTY 0x540E

long lp_term_make_controlling(int fd)
{
    return lp_ioctl(fd, TIOCSCTTY, 0);
}

long lp_signal_handler(int sig, void (*fn)(int))
{
    u8 act[SA_SIZE];
    memset(act, 0, sizeof(act));
    *(unsigned long *)(act + SA_HANDLER) = (unsigned long)fn;

#if defined(__x86_64__)
    *(unsigned long *)(act + SA_FLAGS)        = SA_RESTORER;
    *(unsigned long *)(act + SA_RESTORER_OFF) =
        (unsigned long)&lp_sigreturn_trampoline;
#endif

    return sys_call4(SYS_rt_sigaction, (long)sig, (long)act, 0, SA_MASK_SIZE);
}

long lp_signal_ignore(int sig)  { return set_disposition(sig, 1); }
long lp_signal_default(int sig) { return set_disposition(sig, 0); }

/* ── Scheduling priority ──────────────────────────────────────────────
 * PRIO_PROCESS = 0. The kernel does not hand the nice value back as it
 * went in: getpriority returns 20 - nice, so that a valid result is
 * never negative and cannot be mistaken for an error code. We undo
 * that here, so callers see the nice value they set. */
#define PRIO_PROCESS 0

long lp_setpriority(pid_t pid, int nice_value)
{
    return sys_call3(SYS_setpriority, PRIO_PROCESS, (long)pid,
                     (long)nice_value);
}

int lp_getpriority(pid_t pid)
{
    long rc = sys_call2(SYS_getpriority, PRIO_PROCESS, (long)pid);
    if (rc < 0)
        return 0;
    return (int)(20 - rc);
}

/* ── Filesystem space ─────────────────────────────────────────────────
 *
 * Two different calls and two different structures, because the kernel
 * has one shape for 64-bit machines and another for 32-bit ones.
 *
 * On a 64-bit machine, statfs fills a struct whose every field is 64
 * bits. On a 32-bit one, statfs (99) fills a struct whose every field
 * is 32 bits - a 16GB card does not fit in it - and statfs64 (266) is
 * the one with 64-bit block counts. statfs64 takes the size of the
 * structure as its second argument, because more than one version of
 * it has shipped, and on ARM the structure is packed to four bytes, so
 * the field offsets are not the 64-bit ones either.
 *
 * Reading the 64-bit layout on the 32-bit machine is what `df` did
 * before this: it printed 17650703386236652 1K-blocks for a 186MB
 * ramdisk, and guard reported "/data has -327968716MB left". Neither
 * looked like a type error. They looked like a broken filesystem.
 *
 * f_bfree includes the few percent ext4 keeps back for root; f_bavail
 * is what an ordinary process may still write, and is the honest one. */
#if defined(__arm__)
/* struct statfs64, packed to 4 bytes: 84 bytes total. The u64 fields
 * all land on multiples of 8 anyway, but they are read through memcpy
 * because nothing in the definition promises that. */
#define STATFS_SIZE        84
#define STATFS_BUF_SIZE    88
#define STATFS_CALL(p, b)  sys_call3(SYS_statfs64, (long)(p), STATFS_SIZE, (long)(b))
#define SFS_TYPE(b)        fs_u32((b),  0)
#define SFS_BSIZE(b)       fs_u32((b),  4)
#define SFS_BLOCKS(b)      fs_u64((b),  8)
#define SFS_BFREE(b)       fs_u64((b), 16)
#define SFS_BAVAIL(b)      fs_u64((b), 24)
#define SFS_FILES(b)       fs_u64((b), 32)
#define SFS_FFREE(b)       fs_u64((b), 40)
#define SFS_NAMELEN(b)     fs_u32((b), 56)
#define SFS_FRSIZE(b)      fs_u32((b), 60)
#else
#define STATFS_BUF_SIZE    120
#define STATFS_CALL(p, b)  sys_call2(SYS_statfs, (long)(p), (long)(b))
#define SFS_TYPE(b)        fs_u64((b),  0)
#define SFS_BSIZE(b)       fs_u64((b),  8)
#define SFS_BLOCKS(b)      fs_u64((b), 16)
#define SFS_BFREE(b)       fs_u64((b), 24)
#define SFS_BAVAIL(b)      fs_u64((b), 32)
#define SFS_FILES(b)       fs_u64((b), 40)
#define SFS_FFREE(b)       fs_u64((b), 48)
#define SFS_NAMELEN(b)     fs_u64((b), 64)
#define SFS_FRSIZE(b)      fs_u64((b), 72)
#endif

static inline u64 fs_u64(const u8 *b, unsigned off)
{
    u64 v;
    memcpy(&v, b + off, sizeof v);
    return v;
}

#if defined(__arm__)
/* Only the 32-bit layout has 32-bit fields, and an unused function is
 * an error in this build. */
static inline u64 fs_u32(const u8 *b, unsigned off)
{
    u32 v;
    memcpy(&v, b + off, sizeof v);
    return v;
}
#endif

long lp_statfs(const char *path, lp_statfs_t *out)
{
    u8 buf[STATFS_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    long rc = STATFS_CALL(path, buf);
    if (rc < 0)
        return rc;

    out->type    = SFS_TYPE(buf);
    out->bsize   = SFS_BSIZE(buf);
    out->blocks  = SFS_BLOCKS(buf);
    out->bfree   = SFS_BFREE(buf);
    out->bavail  = SFS_BAVAIL(buf);
    out->files   = SFS_FILES(buf);
    out->ffree   = SFS_FFREE(buf);
    out->namelen = SFS_NAMELEN(buf);
    out->frsize  = SFS_FRSIZE(buf);
    if (out->frsize == 0) out->frsize = out->bsize;
    return 0;
}

long lp_fs_space(const char *path, u64 *free_bytes, u64 *total_bytes)
{
    u8 buf[STATFS_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    long rc = STATFS_CALL(path, buf);
    if (rc < 0)
        return rc;

    u64 bsize  = SFS_BSIZE(buf);
    u64 blocks = SFS_BLOCKS(buf);
    u64 avail  = SFS_BAVAIL(buf);

    if (free_bytes)  *free_bytes  = avail * bsize;
    if (total_bytes) *total_bytes = blocks * bsize;
    return 0;
}

long lp_swapon(const char *path, int flags)
{
    return sys_call2(SYS_swapon, (long)path, flags);
}

long lp_swapoff(const char *path)
{
    return sys_call1(SYS_swapoff, (long)path);
}

/* /proc files report a size of 0, so stat tells us nothing in advance.
 * Just read until the buffer is full. */
/* The pgid, session, parent and tty of a process. See unistd.h for why
 * this is not three lines of strtok at each call site. */
bool lp_proc_ids(pid_t pid, pid_t *ppid, pid_t *pgid, pid_t *sid,
                 int *tty_nr)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);

    char buf[512];
    if (proc_read(path, buf, sizeof buf) <= 0)
        return false;

    /* Field 2 is "(name)" and the name may contain ')' and spaces, so
     * the only safe anchor is the last ')' in the whole line. */
    char *p = NULL;
    for (char *c = buf; *c; c++)
        if (*c == ')')
            p = c;
    if (!p)
        return false;
    p++;

    /* After the ')': state, ppid, pgrp, session, tty_nr, ... */
    long v[5] = { 0, 0, 0, 0, 0 };
    int got = 0;
    while (got < 5 && *p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        if (got == 0) {                 /* state is a letter, not a number */
            while (*p && *p != ' ')
                p++;
            got++;
            continue;
        }
        char *end = p;
        long n = strtol(p, &end, 10);
        if (end == p)
            return false;               /* not a number where one must be */
        v[got++] = n;
        p = end;
    }
    if (got < 5)
        return false;

    if (ppid)   *ppid   = (pid_t)v[1];
    if (pgid)   *pgid   = (pid_t)v[2];
    if (sid)    *sid    = (pid_t)v[3];
    if (tty_nr) *tty_nr = (int)v[4];
    return true;
}

long proc_read(const char *path, char *buf, size_t size)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return fd;

    size_t total = 0;
    while (total < size - 1) {
        long n = lp_read((int)fd, buf + total, size - 1 - total);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    lp_close((int)fd);

    buf[total] = '\0';
    return (long)total;
}

/* Pull the number out of a line like "MemAvailable:  483252 kB". */
long proc_find_kv(const char *text, const char *key)
{
    size_t klen = strlen(key);

    for (const char *p = text; *p; ) {
        /* Match only at the start of a line, so a substring cannot fool us
         * (looking for "SwapFree" must not match inside "MemFree"). */
        if (strncmp(p, key, klen) == 0 && p[klen] == ':') {
            p += klen + 1;
            while (*p == ' ' || *p == '\t') p++;

            long v = 0;
            if (*p < '0' || *p > '9')
                return -1;
            while (*p >= '0' && *p <= '9')
                v = v * 10 + (*p++ - '0');
            return v;
        }
        /* on to the next line */
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return -1;
}
long lp_uname(void *buf)      { return sys_call1(SYS_uname, (long)buf); }
long lp_getrandom(void *buf, size_t n, unsigned flags)
{
    return sys_call3(SYS_getrandom, (long)buf, (long)n, (long)flags);
}

/* ── Users and groups ─────────────────────────────────────────────────
 *
 * /etc/passwd is "name:x:uid:gid:comment:home:shell" and /etc/group is
 * "name:x:gid:members". Both are read line by line every time. There is
 * no cache because there is nothing to cache: the files have a handful
 * of lines and live in RAM already. */

int lp_getgid(void)
{
    /* There is no getgid in the asm-generic table under a different
     * name; 176 is getgid on arm64. */
    return (int)sys_call0(176);
}

long lp_setuid(uid_t uid)  { return sys_call1(SYS_setuid, (long)uid); }
long lp_setgid(gid_t gid)  { return sys_call1(SYS_setgid, (long)gid); }

long lp_setgroups(int n, const gid_t *list)
{
    return sys_call2(SYS_setgroups, (long)n, (long)list);
}

long lp_chown(const char *path, uid_t uid, gid_t gid)
{
    return sys_call5(SYS_fchownat, AT_FDCWD, (long)path,
                     (long)uid, (long)gid, 0);
}

/* Split "a:b:c" in place, returning how many fields were found. */
static int split_colons(char *line, char **fields, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        fields[n++] = p;
        char *colon = strchr(p, ':');
        if (!colon)
            break;
        *colon = '\0';
        p = colon + 1;
    }
    return n;
}

static bool passwd_scan(const char *want_name, int want_uid, lp_user_t *out)
{
    long fd = lp_open("/etc/passwd", O_RDONLY, 0);
    if (fd < 0)
        return false;

    char line[256];
    bool found = false;

    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (line[0] == '#' || line[0] == '\0')
            continue;

        char *f[8];
        int n = split_colons(line, f, 8);
        if (n < 7)
            continue;

        int uid = atoi(f[2]);
        if (want_name ? (strcmp(f[0], want_name) != 0) : (uid != want_uid))
            continue;

        strlcpy(out->name,  f[0], sizeof(out->name));
        out->uid = (uid_t)uid;
        out->gid = (gid_t)atoi(f[3]);
        strlcpy(out->home,  f[5], sizeof(out->home));
        strlcpy(out->shell, f[6], sizeof(out->shell));
        found = true;
        break;
    }

    lp_close((int)fd);
    return found;
}

bool lp_user_by_name(const char *name, lp_user_t *out)
{
    return passwd_scan(name, 0, out);
}

bool lp_user_by_uid(uid_t uid, lp_user_t *out)
{
    return passwd_scan(NULL, (int)uid, out);
}

void lp_group_name(gid_t gid, char *out, size_t n)
{
    snprintf(out, n, "%d", (int)gid);      /* the fallback is the number */

    long fd = lp_open("/etc/group", O_RDONLY, 0);
    if (fd < 0)
        return;

    char line[256];
    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (line[0] == '#' || line[0] == '\0')
            continue;
        char *f[6];
        if (split_colons(line, f, 6) < 3)
            continue;
        if ((gid_t)atoi(f[2]) == gid) {
            strlcpy(out, f[0], n);
            break;
        }
    }
    lp_close((int)fd);
}

bool lp_group_by_name(const char *name, gid_t *out)
{
    long fd = lp_open("/etc/group", O_RDONLY, 0);
    if (fd < 0)
        return false;

    char line[256];
    bool found = false;
    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (line[0] == '#' || line[0] == '\0')
            continue;
        char *f[6];
        if (split_colons(line, f, 6) < 3)
            continue;
        if (strcmp(f[0], name) == 0) {
            *out = (gid_t)atoi(f[2]);
            found = true;
            break;
        }
    }
    lp_close((int)fd);
    return found;
}

/* ── Writing to the log ───────────────────────────────────────────────
 *
 * logd collects two sources: the kernel's ring buffer and a datagram
 * socket at /dev/log. Nothing in this system ever wrote to either, so
 * /data/log/messages held kernel lines and nothing else - every message
 * from init, from rc and from guard went to the console and was gone
 * the moment it scrolled. A board that had been broken into and one
 * that had not produced identical logs, and guard's record of what it
 * killed and why did not survive the reboot that followed.
 *
 * /dev/kmsg rather than the socket: it is one write with no connection
 * to set up, it works before logd is running, and it puts the line in
 * `dmesg` as well. The fd is kept open because the callers are daemons
 * that will use it again.
 *
 * Failure is silent on purpose. This is called from the paths that
 * handle a machine already in trouble, and a logger that complains
 * about not being able to log would only make the console worse. */
void lp_log(const char *tag, const char *msg)
{
    static int kfd = -2;              /* -2 = not tried yet, -1 = no good */

    if (kfd == -2) {
        long fd = lp_open("/dev/kmsg", O_WRONLY, 0);
        kfd = (fd < 0) ? -1 : (int)fd;
    }
    if (kfd < 0)
        return;

    char line[512];
    int  n = snprintf(line, sizeof line, "%s: %s", tag, msg);
    if (n <= 0)
        return;
    if (n >= (int)sizeof line - 1)
        n = (int)sizeof line - 2;   /* keep a byte for the newline */

    /* One write per record - /dev/kmsg splits on write boundaries - and
     * the record has to end in a newline.
     *
     * This is not cosmetic. A printk whose text does not end in '\n' is
     * a *continuation*: the kernel commits it unfinalised, so that a
     * later write can append to it, and nothing - not dmesg, not a
     * reader of /dev/kmsg, not the console - shows it until something
     * else finalises it. Written without the newline, every message
     * here appeared only once the *next* one was logged, and the last
     * message before a quiet spell was never seen at all. That is how
     * guard's "held a core for 30s" line went missing from dmesg while
     * the same text, printed to the console, was right there on screen.
     *
     * The kernel strips the newline again before storing the text, so
     * it is a terminator, not part of the message. */
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        n--;
    if (n <= 0)
        return;
    line[n++] = '\n';
    lp_write(kfd, line, (size_t)n);
}

/* ── Local time ───────────────────────────────────────────────────────
 *
 * Moved to tz.c, with lp_gmtime and lp_timegm, when the zone started to
 * come from tzdata rather than from four rules written out here. */

/* ── Which voice the commands speak in ────────────────────────────────
 *
 * See unistd.h. The default is GNU's wording because this machine is
 * used to learn on, and a command that answers differently from the one
 * on Ubuntu teaches something that has to be unlearned later.
 */
static bool       voice_loaded = false;
static lp_voice_t voice_value  = LP_VOICE_GNU;

lp_voice_t lp_voice(void)
{
    if (voice_loaded)
        return voice_value;
    voice_loaded = true;

    char buf[32];
    const char *paths[2] = { "/data/voice", "/etc/voice" };
    for (int i = 0; i < 2; i++) {
        long fd = lp_open(paths[i], O_RDONLY, 0);
        if (fd < 0)
            continue;
        long n = lp_read((int)fd, buf, sizeof buf - 1);
        lp_close((int)fd);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        if (buf[0] == 'l' || buf[0] == 'L')      /* "lp" */
            voice_value = LP_VOICE_LP;
        return voice_value;
    }
    return voice_value;
}

/* The errno names people actually see. Not the whole table: the ones
 * missing here print as a number, which is still more than "failed". */
const char *lp_strerror(int err)
{
    if (err < 0) err = -err;
    switch (err) {
    case 0:   return "Success";
    case 1:   return "Operation not permitted";
    case 2:   return "No such file or directory";
    case 3:   return "No such process";
    case 4:   return "Interrupted system call";
    case 5:   return "Input/output error";
    case 6:   return "No such device or address";
    case 7:   return "Argument list too long";
    case 8:   return "Exec format error";
    case 9:   return "Bad file descriptor";
    case 10:  return "No child processes";
    case 11:  return "Resource temporarily unavailable";
    case 12:  return "Cannot allocate memory";
    case 13:  return "Permission denied";
    case 14:  return "Bad address";
    case 16:  return "Device or resource busy";
    case 17:  return "File exists";
    case 18:  return "Invalid cross-device link";
    case 19:  return "No such device";
    case 20:  return "Not a directory";
    case 21:  return "Is a directory";
    case 22:  return "Invalid argument";
    case 23:  return "Too many open files in system";
    case 24:  return "Too many open files";
    case 25:  return "Inappropriate ioctl for device";
    case 26:  return "Text file busy";
    case 27:  return "File too large";
    case 28:  return "No space left on device";
    case 29:  return "Illegal seek";
    case 30:  return "Read-only file system";
    case 31:  return "Too many links";
    case 32:  return "Broken pipe";
    case 33:  return "Numerical argument out of domain";
    case 34:  return "Numerical result out of range";
    case 36:  return "File name too long";
    case 38:  return "Function not implemented";
    case 39:  return "Directory not empty";
    case 40:  return "Too many levels of symbolic links";
    case 61:  return "No data available";
    case 62:  return "Timer expired";
    case 71:  return "Protocol error";
    case 88:  return "Socket operation on non-socket";
    case 91:  return "Protocol wrong type for socket";
    case 95:  return "Operation not supported";
    case 97:  return "Address family not supported by protocol";
    case 98:  return "Address already in use";
    case 99:  return "Cannot assign requested address";
    case 101: return "Network is unreachable";
    case 104: return "Connection reset by peer";
    case 110: return "Connection timed out";
    case 111: return "Connection refused";
    case 113: return "No route to host";
    default:  return NULL;
    }
}

void lp_diag(const char *prog, const char *gnu_before, const char *gnu_after,
             const char *lp_phrase, const char *path, int err)
{
    if (err < 0) err = -err;
    const char *msg = lp_strerror(err);
    char unknown[32];
    if (!msg) {
        snprintf(unknown, sizeof unknown, "Unknown error %d", err);
        msg = unknown;
    }

    if (lp_voice() == LP_VOICE_GNU) {
        if (gnu_before)
            dprintf(STDERR_FILENO, "%s: %s '%s'%s%s: %s\n",
                    prog, gnu_before, path,
                    gnu_after ? " " : "", gnu_after ? gnu_after : "", msg);
        else
            dprintf(STDERR_FILENO, "%s: %s: %s\n", prog, path, msg);
        return;
    }

    /* This system's own voice: the errno number is kept, because it is
     * the thing you look up when the sentence is not enough. */
    dprintf(STDERR_FILENO, "%s: %s: %s (%d)\n",
            prog, path, lp_phrase ? lp_phrase : (msg ? msg : "failed"), err);
}
