/* unistd.h - files and processes.
 * The kernel's -errno is passed straight through; there is no global errno.
 * A negative return is an error, and its negation is the errno. */
#ifndef _LP_UNISTD_H
#define _LP_UNISTD_H

#include "types.h"

/* The special dirfd for openat: "relative to the current directory" */
#define AT_FDCWD            (-100)
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_NOFOLLOW 0x100

/* ── open flags ──
 *
 * The low ones are the same everywhere. O_DIRECTORY is not, and that is
 * a trap worth spelling out: arm and arm64 carry their own fcntl.h that
 * renumbers the upper flags, so O_DIRECTORY is 040000 there and 0200000
 * on x86-64 - where 040000 means O_DIRECT instead. Building the arm64
 * value into an x86-64 program does not fail to compile and does not
 * fail to open; it asks for unbuffered I/O on a directory, and `ls`
 * quietly stops being able to list anything. O_NOFOLLOW and O_DIRECT
 * move for the same reason. */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_EXCL      0200
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_NONBLOCK  04000
#define O_CLOEXEC   02000000

#if defined(__x86_64__)
#  define O_DIRECTORY 0200000
#  define O_NOFOLLOW  0400000
#  define O_DIRECT    040000
#  define O_LARGEFILE 0100000
#else   /* arm64 */
#  define O_DIRECTORY 040000
#  define O_NOFOLLOW  0100000
#  define O_DIRECT    0200000
#  define O_LARGEFILE 0400000
#endif

/* The standard file descriptors */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

/* Signals */
#define SIGHUP   1
#define SIGINT   2
#define SIGKILL  9
#define SIGSEGV 11
#define SIGTERM 15
#define SIGUSR1 10
#define SIGUSR2 12
#define SIGQUIT  3

/* ── Signals ──
 * Only two things are needed here, and neither runs a handler:
 *
 *   ignore    the interactive shell ignores Ctrl-C, so that interrupting
 *             a command does not take the shell down with it
 *   default   a child undoes that before it runs a command, so Ctrl-C
 *             reaches the thing you meant to interrupt
 *
 * Running an actual handler would need a return trampoline and a
 * signal frame, and nothing here has ever wanted one. */
long  lp_signal_ignore(int sig);
long  lp_signal_default(int sig);
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20      /* Ctrl-Z */
#define SIGTTIN 21
#define SIGTTOU 22

/* mount flags */
#define MS_NOSUID   2
#define MS_NODEV    4
#define MS_NOEXEC   8
#define MS_RDONLY   1
/* bind: make an existing directory visible at a second place as well.
 * No filesystem type is given; it keeps the original's. */
#define MS_BIND     4096
#define MS_REMOUNT    32
/* Move a mount somewhere else without unmounting it. The one thing that
 * makes switch_root possible: the real root is mounted under the
 * initramfs, and then moved to / with everything already on it still
 * open. */
#define MS_MOVE     8192

/* reboot magic numbers */
#define LINUX_REBOOT_MAGIC1     0xfee1dead
#define LINUX_REBOOT_MAGIC2     672274793
#define LINUX_REBOOT_CMD_RESTART  0x01234567
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321FEDC

/* ── ioctl numbers ─────────────────────────────────────────────────
 *
 * An ioctl number has the size of its argument encoded in it, and the
 * kernel compares the whole number. So a request whose argument is a
 * size_t or a long is a DIFFERENT NUMBER on a 32-bit machine than on a
 * 64-bit one, and a constant copied from a 64-bit header comes back
 * ENOTTY on the Pi Zero W - the call fails, the caller sees a zero, and
 * nothing says why.
 *
 * That is not hypothetical: BLKGETSIZE64 was written out as 0x80081272,
 * which is its value where size_t is 8 bytes. On ARMv6 it is 0x80041272,
 * so `expandfs` could not read the size of the card and never grew
 * /data past the 124MB it is built with, on any card of any size. `disk`
 * and `lsblk` showed 0 bytes for every device for the same reason.
 *
 * These build the number instead of quoting it. arm, arm64 and x86-64
 * all use the asm-generic encoding below; the architectures that do not
 * (mips, powerpc, sparc, alpha) are not ones we target.
 */
#define _LP_IOC_NRBITS    8
#define _LP_IOC_TYPEBITS  8
#define _LP_IOC_SIZEBITS 14
#define _LP_IOC_NRSHIFT   0
#define _LP_IOC_TYPESHIFT (_LP_IOC_NRSHIFT + _LP_IOC_NRBITS)
#define _LP_IOC_SIZESHIFT (_LP_IOC_TYPESHIFT + _LP_IOC_TYPEBITS)
#define _LP_IOC_DIRSHIFT  (_LP_IOC_SIZESHIFT + _LP_IOC_SIZEBITS)

#define _LP_IOC(dir, type, nr, size) \
    (((unsigned long)(dir)  << _LP_IOC_DIRSHIFT)  | \
     ((unsigned long)(type) << _LP_IOC_TYPESHIFT) | \
     ((unsigned long)(nr)   << _LP_IOC_NRSHIFT)   | \
     ((unsigned long)(size) << _LP_IOC_SIZESHIFT))

/* dir: 0 none, 1 write (userland -> kernel), 2 read, 3 both.
 * "read" and "write" are from the caller's point of view. */
#define LP_IO(type, nr)          _LP_IOC(0u, (type), (nr), 0)
#define LP_IOR(type, nr, t)      _LP_IOC(2u, (type), (nr), sizeof(t))
#define LP_IOW(type, nr, t)      _LP_IOC(1u, (type), (nr), sizeof(t))
#define LP_IOWR(type, nr, t)     _LP_IOC(3u, (type), (nr), sizeof(t))

/* The size of a block device in bytes. The argument is always a u64 -
 * the kernel says so in its own header - but the number is built from
 * size_t because that is what the kernel's macro was written with. */
#define LP_BLKGETSIZE64  LP_IOR(0x12, 114, size_t)

extern char **environ;

/* ── Files ── */
long  lp_open(const char *path, int flags, mode_t mode);
long  lp_close(int fd);
long  lp_read(int fd, void *buf, size_t n);
long  lp_write(int fd, const void *buf, size_t n);
/* lp_lseek 의 whence. 리눅스는 어느 아키텍처에서나 같은 값을 쓴다. */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2
/* s64, not long: on a 32-bit machine a long cannot hold a file offset,
 * and the value that comes back would be the low half of one. */
s64   lp_lseek(int fd, off_t off, int whence);
long  lp_dup(int fd);
long  lp_dup2(int oldfd, int newfd);
long  lp_pipe(int fds[2]);
long  lp_unlink(const char *path);
long  lp_mkdir(const char *path, mode_t mode);
long  lp_rmdir(const char *path);
long  lp_chdir(const char *path);
long  lp_umask(long mask);
long  lp_getcwd(char *buf, size_t n);
long  lp_access(const char *path, int mode);
/* Read directory entries. Returns the bytes filled in, 0 at the end. */
long  sys_getdents(int fd, void *buf, size_t size);
long  lp_ioctl(int fd, unsigned long req, void *arg);

/* ── The terminal ──
 *
 * A program that draws its own screen, like the editor, has to put the
 * terminal in raw mode. In the default canonical mode the kernel holds a
 * whole line until Enter and handles backspace itself, which leaves no
 * way to react to a single arrow key.
 *
 * struct termios is 36 bytes on arm64. We do not use every field, so it
 * is treated as an opaque blob for saving and restoring. */
typedef struct { u8 raw[64]; } lp_termios_t;

/* Save the current settings into saved and switch to raw. 0 on success. */
long  lp_term_raw(int fd, lp_termios_t *saved);

/* Like lp_term_raw, but the kernel still turns \n into a carriage
 * return and a line feed on the way out (OPOST stays on).
 *
 * Full raw mode is right for a program that positions every character
 * itself - an editor. It is wrong for one that prints ordinary lines and
 * only wants keys without waiting for Enter: there, \n moves down a row
 * and leaves the column alone, so every line starts where the last one
 * ended and the screen turns into a staircase. */
long  lp_term_cbreak(int fd, lp_termios_t *saved);

/* Put the terminal back to something usable - echo on, line editing on,
 * newlines that return to column one. For after a program that owned the
 * terminal died without tidying up. */
long  lp_term_sane(int fd);
/* Restore from saved. This must run before the program exits, or the
 * shell is left unable to read input. */
long  lp_term_restore(int fd, const lp_termios_t *saved);

/* ── Making a terminal the controlling terminal ──
 *
 * setsid() alone leaves a process with NO controlling terminal, and a
 * terminal with no foreground process group generates no signals at
 * all: the line discipline has nobody to send them to, so Ctrl-C is
 * silently nothing. Reopening the device is not enough either -
 * /dev/console specifically can never become a controlling terminal,
 * because the kernel forces O_NOCTTY for major 5 minor 1. So it has to
 * be asked for, explicitly, on the real device.
 *
 * Call this in the child, after setsid() and after the device is open.
 * Returns the kernel's -errno; -EPERM means somebody else already owns
 * this terminal. */
long  lp_term_make_controlling(int fd);
/* Terminal size. Falls back to 80x24 and returns -1 if unknown. */
long  lp_term_size(int fd, int *rows, int *cols);

/* Is this file descriptor a terminal? Asking the kernel for the
 * terminal settings is the test: it succeeds on a terminal and fails
 * with ENOTTY on anything else. Used to tell "somebody is typing at me"
 * from "I am in a pipe", which changes what a program should say. */
bool  lp_isatty(int fd);

/* Tell the kernel's line editor that input is UTF-8 (IUTF8), so
 * backspace erases a whole Hangul character rather than one byte. */
long  lp_term_set_utf8(int fd);
bool  lp_exists(const char *path);
bool  lp_is_dir(const char *path);
long  lp_fsync(int fd);
/* Save a whole file so a power cut leaves the old contents or the new
 * ones and never half of either: write beside it, fsync, rename over.
 * Read the comment in libc/src/unistd.c before replacing this with an
 * open(O_TRUNC) - the failure it avoids is a setting that vanishes. */
bool  lp_write_file_atomic(const char *path, const void *data, size_t n);
/* "/data/<name>" on a RAM root, "/etc/<name>" on a disk root - whichever
 * of the two actually survives a reboot on this machine. */
const char *lp_setting_path(const char *name, char *buf, size_t cap);
long  lp_ftruncate(int fd, s64 length);
long  lp_link(const char *from, const char *to);
#define LP_S_IFIFO_MODE 0010000
long  lp_mknod(const char *path, mode_t mode, u64 dev);
long  lp_rename(const char *from, const char *to);
long  lp_chmod(const char *path, mode_t mode);
long  lp_symlink(const char *target, const char *linkpath);
long  lp_readlink(const char *path, char *buf, size_t n);

/* The kernel's struct stat is 128 bytes on arm64, 144 on x86-64, and
 * the layouts differ. This is the same fields in one shape, so nothing
 * above here has to know which machine it is on.
 *
 * `stat` wants all of it - the whole point of the command is to show
 * what the kernel actually recorded, so leaving fields out would make
 * it a worse `ls -l` rather than a stat. */
typedef struct {
    u32 mode;        /* file type (S_IF*) plus permissions */
    u64 size;
    u32 nlink;
    uid_t uid;
    gid_t gid;
    s64 mtime;       /* seconds since 1970 */
    u64 blocks;      /* 512-byte units actually allocated */
    u64 dev;         /* the device the file lives on */
    u64 ino;
    u64 rdev;        /* which device it *is*, for a device node */
    u64 blksize;     /* the I/O size the filesystem prefers */
    s64 atime, ctime;
    u32 mtime_ns, atime_ns, ctime_ns;
    s64 btime;       /* when it was created; statx only */
    u32 btime_ns;
    bool has_btime;  /* false on a filesystem that does not record it */
} lp_stat_t;

#define LP_S_IFMT   0170000
#define LP_S_IFDIR  0040000
#define LP_S_IFREG  0100000
#define LP_S_IFLNK  0120000
#define LP_S_IFCHR  0020000
#define LP_S_IFBLK  0060000
#define LP_S_IFIFO  0010000
#define LP_S_IFSOCK 0140000

/* With follow_symlink=false this looks at the link itself (lstat). */
long  lp_stat(const char *path, lp_stat_t *out, bool follow_symlink);

/* ── Processes ── */
pid_t lp_fork(void);                 /* built on clone(SIGCHLD) */
long  lp_execve(const char *path, char *const argv[], char *const envp[]);
pid_t lp_wait(int *status);
pid_t lp_waitpid(pid_t pid, int *status, int options);
#define WNOHANG 1        /* return at once when the child is still running */
pid_t lp_getpid(void);
/* There is one user on this system and it is root, so this is a
 * formality - but a script asking "am I root" deserves an answer. */
int   lp_getuid(void);
int   lp_getgid(void);
long  lp_setuid(uid_t uid);
long  lp_setgid(gid_t gid);
long  lp_setgroups(int n, const gid_t *list);
/* Owner and group of a file. -1 for either leaves it alone, which is
 * how chgrp is chown with the user left out. */
long  lp_chown(const char *path, uid_t uid, gid_t gid);

/* ── Users ──
 *
 * /etc/passwd and /etc/group, read straight off the disk each time.
 * There is no name service, no cache and no getpwnam: one file, a few
 * lines, and the cost of reading it is a page.
 *
 * /etc is in RAM and is rebuilt from the kernel image at every boot, so
 * a user added on the machine lives in /data/users and is merged back
 * in by /etc/rc. That is the same trick the SSH keys use, and for the
 * same reason. */
typedef struct {
    char name[32];
    uid_t uid;
    gid_t gid;
    char home[64];
    char shell[48];
} lp_user_t;

/* Look one up by name or by number. false when there is no such user. */
bool lp_user_by_name(const char *name, lp_user_t *out);
bool lp_user_by_uid(uid_t uid, lp_user_t *out);
/* The name for a group id, or the number as text when it has none.
 * `out` needs 32 bytes. */
void lp_group_name(gid_t gid, char *out, size_t n);
/* A group id by name. false when there is no such group. */
bool lp_group_by_name(const char *name, gid_t *out);
long  lp_setsid(void);
long  lp_kill(pid_t pid, int sig);
void  lp_exit(int code) __attribute__((noreturn));
long  lp_sleep_ms(long ms);

/* ── Time ── */
/* Seconds since 1970-01-01. 0 on failure. */
s64   lp_time(void);
/* Set the system clock. Root only. */
long  lp_settime(s64 unix_seconds);

/* The battery-backed clock, when the board has one. A PC and an EC2
 * instance do; a Pi Zero 2 W does not, and lp_rtc_read returns false
 * there rather than pretending. The kernel keeps it in UTC. */
bool  lp_rtc_read(s64 *out);
bool  lp_rtc_write(s64 unix_seconds);

/* Milliseconds since the machine started. Unlike lp_time this never
 * jumps: ntp setting the clock does not move it, so it is the one to
 * measure an interval with. */
s64   lp_monotonic_ms(void);

/* Broken-down time (lp_tm_t), UTC and local time, time zones and
 * strftime - all in tz.h, which includes nothing but types.h so that it
 * can be pulled in here without a cycle. */
#include "tz.h"

/* ── System ── */
/* lp_access 의 mode. 존재만 보려면 F_OK. */
#define F_OK  0
#define X_OK  1
#define W_OK  2
#define R_OK  4

/* 마운트를 푼다. flags 에 MNT_DETACH(2) 를 주면 쓰는 사람이 있어도
 * 트리에서 떼어낸다 - 뽑힌 드라이브를 정리할 때 필요하다. */
#define MNT_DETACH  2
long  lp_umount(const char *tgt, int flags);

/* 루트를 옮긴다. apt 가 데비안 트리 안에서 돌 때 쓴다 - dpkg 는
 * /var/lib/dpkg 같은 절대경로를 코드에 박고 있어서, 그 트리를 진짜
 * 루트로 보여주는 것 말고는 방법이 없다. */
long  lp_chroot(const char *path);

long  lp_mount(const char *src, const char *tgt, const char *fstype,
               unsigned long flags, const void *data);
long  lp_reboot(int cmd);

/* Run fn() when this signal arrives.
 *
 * The only other dispositions here are SIG_DFL and SIG_IGN, which need
 * nothing from us because they never run our code. A real handler does:
 * it has to return somewhere, and on x86-64 the kernel will not deliver
 * one without a restorer to return through. See set_disposition.
 *
 * SA_RESTART is deliberately NOT set. init waits in wait() with no
 * timeout, and the point of sending it a signal is to make that wait
 * return so the main loop can look at what changed. */
long  lp_signal_handler(int sig, void (*fn)(int));
long  lp_swapon(const char *path, int flags);
long  lp_swapoff(const char *path);

/* Read a one-line value out of /proc. Returns the bytes read. */
long  proc_read(const char *path, char *buf, size_t size);
/* Find a value in the "name:   number kB" format /proc/meminfo uses.
 * -1 if it is not there. */
long  proc_find_kv(const char *text, const char *key);
long  lp_sync(void);

/* SHA-256 of a file, written to `hex` as 64 characters and a NUL.
 * false when the file cannot be read. */
bool  lp_sha256_file(const char *path, char *hex);

/* ── Message digests ──────────────────────────────────────────────────
 *
 * One front end for MD5, SHA-1, SHA-256 and SHA-512, because md5sum and
 * its three siblings are one program under four names and pkg wants the
 * same code the checksum commands use.
 *
 * MD5 and SHA-1 are here to read what other people published, not to
 * decide whether to trust it. Nothing in this system verifies a
 * signature with either.
 */
typedef enum { LP_MD5 = 0, LP_SHA1, LP_SHA256, LP_SHA512 } lp_algo_t;

typedef struct {
    int  algo;
    int  block;      /* 64, or 128 for SHA-512 */
    int  used;
    u64  len;
    u32  h32[8];
    u64  h64[8];
    u8   buf[128];
} lp_digest_t;

void lp_digest_init(lp_digest_t *d, int algo);
void lp_digest_update(lp_digest_t *d, const void *data, size_t n);
/* Writes 2*bytes hex characters and a NUL. */
void lp_digest_final(lp_digest_t *d, char *hex);
int  lp_digest_bits(int algo);
bool lp_digest_fd(int fd, int algo, char *hex);
bool lp_digest_file(const char *path, int algo, char *hex);

/* Scheduling priority ("nice"): -20 gets the CPU first, 19 last, 0 is
 * the default. Lowering it needs root. */
long  lp_setpriority(pid_t pid, int nice_value);
/* The nice value of a process, or 0 when it cannot be read. */
int   lp_getpriority(pid_t pid);

/* A per-process resource ceiling. `resource` is one of LP_RLIMIT_*.
 * Returns the kernel's -errno.
 *
 * Worth knowing before relying on this: root is exempt from
 * RLIMIT_NPROC. A process with CAP_SYS_ADMIN or CAP_SYS_RESOURCE - which
 * every root process here has - passes the fork check regardless of the
 * limit. So this bounds what a user can do, not what root can do. The
 * bound on root is kernel.pid_max, which /etc/rc sets. */
long  lp_setrlimit(int resource, u64 soft, u64 hard);
#define LP_RLIMIT_CPU     0
#define LP_RLIMIT_FSIZE   1
#define LP_RLIMIT_DATA    2
#define LP_RLIMIT_STACK   3
#define LP_RLIMIT_NPROC   6
#define LP_RLIMIT_NOFILE  7

/* The pgid, session id, parent and controlling terminal of a process,
 * read from /proc/<pid>/stat. Any out pointer may be NULL.
 *
 * The reason this is a libc function and not three lines at each call
 * site: field 2 of that file is the process name in parentheses, and the
 * name may itself contain spaces and parentheses - a process can call
 * itself ") 1 (" if it wants to. Splitting on spaces gets the wrong
 * fields for such a process, which is exactly the process you are
 * looking at when you need this. Parsing starts after the LAST ')'.
 *
 * false when the process is gone or the file cannot be parsed. */
bool  lp_proc_ids(pid_t pid, pid_t *ppid, pid_t *pgid, pid_t *sid,
                  int *tty_nr);

/* Free and total bytes of the filesystem holding `path`.
 * "free" is what an unprivileged process may still use. */
/* Everything statfs reports, for df. */
typedef struct {
    u64 type;        /* the magic number that names the filesystem */
    u64 bsize;       /* the block size for I/O */
    u64 blocks, bfree, bavail;
    u64 files, ffree;
    u64 namelen;
    u64 frsize;      /* the block size the counts above are in */
} lp_statfs_t;

long  lp_statfs(const char *path, lp_statfs_t *out);
long  lp_fs_space(const char *path, u64 *free_bytes, u64 *total_bytes);
long  lp_uname(void *buf);
long  lp_getrandom(void *buf, size_t n, unsigned flags);

/* Put one line in the system log, so it survives the reboot. Console
 * output does not: it goes out the serial port and is gone. Silent on
 * failure - see the note by the definition. */
void  lp_log(const char *tag, const char *msg);           /* struct utsname is 390 bytes */

/* ── "there is nothing for me to do here" ──
 *
 * A supervised service exits with this when the hardware or the setting
 * it exists to work with is absent on this machine - as opposed to
 * failing at something it should have managed. init does not restart it
 * and does not treat it as a fault.
 *
 * The case that made this necessary: the watchdog service on a virtual
 * machine, where there is no watchdog device at all. Exiting 1 made init
 * restart it, back off, restart it again and print the same complaint a
 * dozen times before giving up - a fault report, repeated, about
 * something that is not a fault and will never change while the machine
 * is running. */
#define LP_EXIT_NO_HARDWARE  78

/* Decoding a wait status */
#define LP_WIFEXITED(s)    (((s) & 0x7F) == 0)
#define LP_WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define LP_WIFSIGNALED(s)  ((((s) & 0x7F) + 1) >> 1 > 0)
#define LP_WTERMSIG(s)     ((s) & 0x7F)


/* ── Which voice the commands speak in ────────────────────────────────
 *
 * Every command here was written from scratch, and each one invented its
 * own wording for "that file is not there". They were clear, and they
 * were all different from GNU:
 *
 *     ours   cat: nosuch: cannot open (2)
 *     GNU    cat: nosuch: No such file or directory
 *
 * For somebody learning on this machine that is worse than a missing
 * command. A missing command teaches nothing; a command that answers
 * differently teaches something false, and they carry it to Ubuntu and
 * find out there. So the default is GNU's wording, exactly.
 *
 * The other voice is kept because it says more: it names the errno, and
 * several of these tools know things GNU's do not. `voice lp` turns it
 * on, `voice gnu` turns it back, and nothing else changes.
 */
typedef enum { LP_VOICE_GNU = 0, LP_VOICE_LP = 1 } lp_voice_t;

/* Read once from /data/voice, then /etc/voice. Default GNU. */
lp_voice_t lp_voice(void);

/* "No such file or directory" for 2, and so on. Takes a positive errno
 * or the negative return of a syscall; both are understood, because
 * half the callers have one and half the other. */
const char *lp_strerror(int err);

/* One diagnostic line, in whichever voice is set.
 *
 *   before NULL   ->  prog: path: No such file or directory
 *   before set    ->  prog: cannot access 'path': No such file or directory
 *   before+after  ->  prog: cannot open 'path' for reading: No such file...
 *   lp voice      ->  prog: path: <lp_phrase> (errno)
 *
 * GNU uses all three shapes and is not consistent about which: cat takes
 * the first, ls and rm the second, head and tail the third. The caller
 * says which, because matching it exactly is the whole point.
 */
void lp_diag(const char *prog, const char *gnu_before, const char *gnu_after,
             const char *lp_phrase, const char *path, int err);

/* ── Option parsing ───────────────────────────────────────────────────
 *
 * The grammar every GNU tool shares: -n5, -n 5, -abc bundled, --lines=5,
 * --lines 5, "--" to end options, "-" as a filename, and operands that
 * may appear before options. Implemented once in libc/src/getopt.c so
 * the commands cannot each get a different subset of it right.
 *
 *   lp_getopt_t   g;
 *   lp_getopt_init(&g, argc, argv, "n:vq", longopts);
 *   for (int c; (c = lp_getopt(&g)) != -1; )
 *       switch (c) {
 *       case 'n': limit = atoi(g.arg); break;
 *       case '?': lp_getopt_err("head", &g); return 1;
 *       }
 *   for (int i = g.ind; i < argc; i++)  ... operands ...
 *
 * shortopts: "n" takes no value, "n:" requires one, "n::" takes one only
 * when attached. longopts is a table ended by a zero name; has_arg uses
 * the same 0/1/2, and val is what lp_getopt returns for it (use a value
 * above 255 for a long option with no short spelling).
 */
typedef struct {
    const char *name;
    int         has_arg;   /* 0 none, 1 required, 2 optional */
    int         val;
} lp_lopt_t;

typedef struct {
    int          ind;            /* first operand, once lp_getopt returns -1 */
    const char  *arg;            /* value of the option just returned */
    const char  *lname;          /* long name it matched, else NULL */
    char         badchar;        /* the letter, on '?' */
    const char  *badlong;        /* the word, on '?' from a long option */
    int          ambig;          /* the long option was an ambiguous prefix */
    int          missing;        /* the option needs a value and got none */
    /* internal */
    int          argc;
    char       **argv;
    const char  *shortopts;
    const lp_lopt_t *longopts;
    const char  *cur;
    int          pos;
    int          first_operand;
    int          flags;
} lp_getopt_t;

/* -3 is an operand, not three flags. seq, sort and tail need this. */
#define LP_GETOPT_NEGNUM  1
/* Everything after the first operand belongs to whatever this program
 * is about to run. timeout, watch, nice and env work this way. */
#define LP_GETOPT_STOP_AT_OPERAND 2

void lp_getopt_init(lp_getopt_t *st, int argc, char **argv,
                    const char *shortopts, const lp_lopt_t *longopts);
void lp_getopt_init_ex(lp_getopt_t *st, int argc, char **argv,
                       const char *shortopts, const lp_lopt_t *longopts,
                       int flags);
int  lp_getopt(lp_getopt_t *st);
/* GNU's two lines: "prog: invalid option -- 'x'" and the --help hint. */
void lp_getopt_err(const char *prog, const lp_getopt_t *st);

#endif /* _LP_UNISTD_H */
