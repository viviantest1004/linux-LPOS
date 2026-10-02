/* syscall-x86_64.h - x86-64 Linux system calls.
 *
 * The same job as syscall-arm64.h and almost none of the same numbers.
 * x86-64 has its own syscall table, older than the asm-generic one
 * arm64 uses, so nothing lines up: read is 0 here and 63 there, write
 * is 1 and 64, openat is 257 and 56.
 *
 * The calling convention:
 *   rax      = the system call number
 *   rdi rsi rdx r10 r8 r9 = arguments (note r10, NOT rcx - the syscall
 *              instruction destroys rcx and r11)
 *   syscall
 *   rax      = the result, negative -errno on failure
 *
 * x86-64 does have open / stat / fork / pipe / dup2 as calls of their
 * own, unlike arm64. We deliberately do not use them: sticking to
 * openat / newfstatat / clone / pipe2 / dup3 means the C above this
 * header is identical on both machines, and one behaviour to reason
 * about rather than two.
 */
#ifndef _LP_SYSCALL_X86_64_H
#define _LP_SYSCALL_X86_64_H

#include "types.h"

/* ── System call numbers (arch/x86/entry/syscalls/syscall_64.tbl) ── */
#define SYS_read               0
#define SYS_write              1
#define SYS_close              3
#define SYS_lseek              8
#define SYS_mmap               9
#define SYS_munmap            11
#define SYS_brk               12
#define SYS_rt_sigaction      13
#define SYS_rt_sigprocmask    14
#define SYS_ioctl             16
#define SYS_readv             19
#define SYS_writev            20
#define SYS_sched_yield       24
#define SYS_dup               32
#define SYS_nanosleep         35
#define SYS_getpid            39
#define SYS_socket            41
#define SYS_connect           42
#define SYS_sendto            44
#define SYS_recvfrom          45
#define SYS_shutdown          48
#define SYS_bind              49
#define SYS_listen            50
#define SYS_getsockname       51
#define SYS_setsockopt        54
#define SYS_getsockopt        55
#define SYS_clone             56
#define SYS_execve            59
#define SYS_exit              60
#define SYS_wait4             61
#define SYS_kill              62
#define SYS_uname             63
#define SYS_fcntl             72
#define SYS_getcwd            79
#define SYS_chdir             80
#define SYS_umask             95
#define SYS_statfs           137
#define SYS_getpriority      140
#define SYS_setpriority      141
#define SYS_setsid           112
#define SYS_setpgid          109
#define SYS_getpgid          121
#define SYS_getuid           102
#define SYS_getppid          110
#define SYS_setuid           105
#define SYS_setgid           106
#define SYS_setgroups        116
#define SYS_sync             162
#define SYS_mount            165
#define SYS_umount2          166
#define SYS_chroot           161
#define SYS_swapon           167
#define SYS_swapoff          168
#define SYS_reboot           169
#define SYS_set_tid_address  218
#define SYS_getdents64       217
#define SYS_clock_settime    227
#define SYS_clock_gettime    228
#define SYS_exit_group       231
#define SYS_openat           257
#define SYS_mkdirat          258
#define SYS_fchownat         260
#define SYS_newfstatat       262
/* statx: the only way to ask for a file's creation time. */
#define SYS_statx           332
#define SYS_fstat              5
#define SYS_unlinkat         263
#define SYS_renameat         264
#define SYS_linkat           265
#define SYS_ftruncate         77
#define SYS_shutdown          48
#define SYS_fsync             74
#define SYS_mknodat          259
#define SYS_symlinkat        266
#define SYS_readlinkat       267
#define SYS_fchmodat         268
#define SYS_faccessat        269
#define SYS_accept4          288
/* See the note in syscall-arm64.h: arm64 has no bare poll, so both
 * machines use ppoll and the C above this header stays identical. */
#define SYS_ppoll            271
#define SYS_dup3             292
#define SYS_pipe2            293
#define SYS_prlimit64        302
#define SYS_getrandom        318

#define SYS_utimensat      280     

/* ── Making the call ──
 *
 * "memory" and "cc" on every one: the kernel may read or write anything
 * the pointers reach, and the compiler must not keep a stale copy or
 * assume flags survive. rcx and r11 are destroyed by the syscall
 * instruction itself, so they are always in the clobber list - forgetting
 * them is the classic way to get miscompiled code that works until the
 * optimiser changes its mind.
 */
static inline long sys_call0(long n)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret) : "a"(n)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline long sys_call1(long n, long a)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret) : "a"(n), "D"(a)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline long sys_call2(long n, long a, long b)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret) : "a"(n), "D"(a), "S"(b)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline long sys_call3(long n, long a, long b, long c)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline long sys_call4(long n, long a, long b, long c, long d)
{
    long ret;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall"
                     : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline long sys_call5(long n, long a, long b, long c, long d, long e)
{
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline long sys_call6(long n, long a, long b, long c, long d,
                             long e, long f)
{
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8),
                       "r"(r9)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}

#endif /* _LP_SYSCALL_X86_64_H */
