/* syscall-arm.h - ARM (32-bit, EABI) Linux system calls.
 *
 * The Raspberry Pi Zero W is an ARM1176 - ARMv6, one core, 32 bits.
 * Not a slower Zero 2 W: a different instruction set entirely, which is
 * why it needs its own file rather than a flag in the other one.
 *
 * The calling convention:
 *   r7       = the system call number
 *   r0..r5   = arguments
 *   svc #0
 *   r0       = the result, negative -errno on failure
 *
 * Three things here are not like the 64-bit machines, and each of them
 * is a place where a number silently comes back wrong rather than a
 * call failing loudly:
 *
 *   The table is the old ARM one, not asm-generic. read is 3, not 63.
 *   Every number below had to be looked up separately.
 *
 *   `long` is 32 bits, so a file offset does not fit in a register.
 *   lseek is _llseek: the offset arrives split in two and the result
 *   comes back through a pointer. Using plain lseek would work up to
 *   2GB and then quietly wrap.
 *
 *   time_t is 32 bits in the old calls, and runs out in 2038. The
 *   _time64 versions exist on every kernel this image ships with, so
 *   those are what we call - a machine that will still be sitting in a
 *   cupboard in 2038 should not stop working in it.
 */
#ifndef _LP_SYSCALL_ARM_H
#define _LP_SYSCALL_ARM_H

#include "types.h"

/* ── System call numbers (arch/arm/tools/syscall.tbl) ── */
#define SYS_exit              1
#define SYS_read              3
#define SYS_write             4
#define SYS_close             6
#define SYS_unlink           10
#define SYS_execve           11
#define SYS_chdir            12
#define SYS_umask            60
#define SYS_mknod            14
#define SYS_chmod            15
#define SYS_lseek            19      /* 32-bit only; _llseek is the real one */
#define SYS_getpid           20
#define SYS_mount            21
#define SYS_setuid           23
#define SYS_getuid           24
#define SYS_kill             37
#define SYS_rename           38
#define SYS_mkdir            39
#define SYS_rmdir            40
#define SYS_dup              41
#define SYS_pipe             42
#define SYS_brk              45
#define SYS_setgid           46
#define SYS_getgid           47
#define SYS_ioctl            54
#define SYS_setpgid          57
#define SYS_umount2          52
#define SYS_dup2             63
#define SYS_getppid          64
#define SYS_setsid           66
#define SYS_symlink          83
#define SYS_readlink         85
#define SYS_swapon           87
#define SYS_reboot           88
#define SYS_munmap           91
#define SYS_ftruncate        93
#define SYS_fchmod           94
#define SYS_setpriority      97
#define SYS_getpriority      96
#define SYS_statfs           99
#define SYS_syslog          103
#define SYS_setgroups       81
#define SYS_wait4           114
#define SYS_swapoff         115
#define SYS_sysinfo         116
#define SYS_fsync           118
#define SYS_clone           120
#define SYS_uname           122
#define SYS_getpgid         132
#define SYS_fchdir          133
#define SYS_llseek          140     /* _llseek: 64-bit offsets on a 32-bit machine */
#define SYS_getdents        141
#define SYS_writev          146
#define SYS_readv           145
#define SYS_sched_yield     158
#define SYS_nanosleep       162
#define SYS_poll            168
#define SYS_prctl           172
#define SYS_rt_sigaction    174
#define SYS_rt_sigprocmask  175
#define SYS_getcwd          183
#define SYS_chroot           61
#define SYS_truncate64      193
#define SYS_ftruncate64     194
#define SYS_stat64          195
#define SYS_lstat64         196
#define SYS_fstat64         197
#define SYS_getdents64      217
#define SYS_fcntl64         221
#define SYS_statfs64        266
/* ppoll_time64, not the bare ppoll at 336.
 *
 * The two take different structures: 336 wants a pair of 32-bit words
 * and 414 wants a pair of 64-bit ones. This libc builds every timespec
 * as two s64 - the same shape on all three machines - so 336 would read
 * the seconds, take the top half of that s64 as the nanoseconds, and
 * get zero. Every wait with a timeout under a second became a wait of
 * no time at all, which is not an error: it is a program that returns
 * immediately, for ever. logd sat at 100% of the one core.
 *
 * There is no downside. A relative timeout does not care about 2038,
 * and 414 has been in every kernel since 5.1. */
#define SYS_ppoll           414

/* utimensat_time64, not the utimensat at 348, for the same reason as
 * every other _time64 call here: this libc's timespec is two 64-bit
 * words on every architecture, and handing that to the 32-bit call
 * makes the kernel read a seconds field that is half a timestamp. */
#define SYS_utimensat       412
#define SYS_openat          322
#define SYS_mkdirat         323
#define SYS_mknodat         324
#define SYS_fchownat        325
#define SYS_fstatat64       327
#define SYS_unlinkat        328
#define SYS_renameat        329
#define SYS_linkat          330
#define SYS_symlinkat       331
#define SYS_readlinkat      332
#define SYS_fchmodat        333
#define SYS_faccessat       334
#define SYS_pipe2           359
#define SYS_dup3            358
#define SYS_prlimit64       369
#define SYS_getrandom       384
#define SYS_statx           397
#define SYS_exit_group      248
#define SYS_set_tid_address 256
#define SYS_sync             36
#define SYS_mmap2           192

/* Time.
 *
 * SYS_clock_gettime and SYS_clock_settime are the _time64 calls, not
 * the 32-bit ones at 263 and 264. The names have no suffix because the
 * shared code in unistd.c calls SYS_clock_gettime and must get the same
 * structure on every machine.
 *
 * The 32-bit pair is left here named for what it is, unused. Calling it
 * with this libc's 16-byte timespec does not fail: the kernel writes
 * eight bytes, the seconds come back with the nanoseconds sitting in
 * their top half, and the clock reads as some hundreds of billions of
 * years from now. `date` spun in its calendar loop rather than
 * printing anything.
 *
 * The 2038 argument for these is the real one, though. A board that
 * will still be in a cupboard then should not stop working in it. */
#define SYS_clock_gettime32 263
#define SYS_clock_settime32 264
#define SYS_clock_gettime   403
#define SYS_clock_settime   404

/* Sockets. ARM has the individual calls as well as socketcall, and the
 * individual ones are simpler to get right. */
#define SYS_socket          281
#define SYS_bind            282
#define SYS_connect         283
#define SYS_listen          284
#define SYS_accept4         366
#define SYS_getsockname     286
#define SYS_sendto          290
#define SYS_recvfrom        292
#define SYS_shutdown        293
#define SYS_setsockopt      294
#define SYS_getsockopt      295

/* ── Raw system call wrappers ──
 *
 * r7 holds the number and must be named explicitly or the compiler
 * picks its own register. The "memory" clobber says the call may change
 * memory behind the compiler's back.
 */
static inline long sys_call0(long n)
{
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0");
    __asm__ volatile("svc #0" : "=r"(r0) : "r"(r7) : "memory", "cc");
    return r0;
}

static inline long sys_call1(long n, long a)
{
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r7) : "memory", "cc");
    return r0;
}

static inline long sys_call2(long n, long a, long b)
{
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r7), "r"(r1) : "memory", "cc");
    return r0;
}

static inline long sys_call3(long n, long a, long b, long c)
{
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2)
                     : "memory", "cc");
    return r0;
}

static inline long sys_call4(long n, long a, long b, long c, long d)
{
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2), "r"(r3)
                     : "memory", "cc");
    return r0;
}

static inline long sys_call5(long n, long a, long b, long c, long d, long e)
{
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r4 __asm__("r4") = e;
    __asm__ volatile("svc #0" : "+r"(r0)
                     : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4)
                     : "memory", "cc");
    return r0;
}

static inline long sys_call6(long n, long a, long b, long c,
                             long d, long e, long f)
{
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r4 __asm__("r4") = e;
    register long r5 __asm__("r5") = f;
    __asm__ volatile("svc #0" : "+r"(r0)
                     : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
                     : "memory", "cc");
    return r0;
}

#endif /* _LP_SYSCALL_ARM_H */
