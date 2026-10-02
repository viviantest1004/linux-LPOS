/* syscall.h - AArch64 Linux system calls.
 *
 * The calling convention:
 *   x8       = the system call number
 *   x0..x5   = arguments
 *   svc #0
 *   x0       = the result. Errors come back as a negative -errno,
 *              not as -1 plus a separate errno the way glibc does it.
 *
 * AArch64 uses the asm-generic syscall table, so the calls x86 has for
 * open / stat / fork / pipe / dup2 do not exist at all. Instead:
 *   open  -> openat(AT_FDCWD, ...)
 *   stat  -> newfstatat
 *   fork  -> clone(SIGCHLD, ...)
 *   pipe  -> pipe2
 *   dup2  -> dup3
 */
#ifndef _LP_SYSCALL_ARM64_H
#define _LP_SYSCALL_ARM64_H

#include "types.h"

/* ── System call numbers (arch/arm64 = asm-generic/unistd.h) ── */
#define SYS_getcwd          17
#define SYS_dup             23
#define SYS_dup3            24
#define SYS_fcntl           25
#define SYS_ioctl           29
#define SYS_mkdirat         34
#define SYS_unlinkat        35
#define SYS_linkat          37
#define SYS_ftruncate       46
#define SYS_shutdown       210
#define SYS_fsync           82
#define SYS_mknodat         33
#define SYS_symlinkat       36
#define SYS_renameat        38
#define SYS_umount2         39
#define SYS_chroot          51
#define SYS_mount           40
#define SYS_statfs          43
#define SYS_faccessat       48
#define SYS_chdir           49
#define SYS_umask           166
#define SYS_fchmodat        53
#define SYS_fchownat        54
#define SYS_openat          56
#define SYS_close           57
#define SYS_pipe2           59
#define SYS_getdents64      61
#define SYS_lseek           62
#define SYS_read            63
#define SYS_write           64
#define SYS_readv           65
#define SYS_writev          66
#define SYS_readlinkat      78
#define SYS_newfstatat      79
/* statx: the only way to ask for a file's creation time. */
#define SYS_statx           291
#define SYS_fstat           80
#define SYS_exit            93
#define SYS_exit_group      94
#define SYS_set_tid_address 96
#define SYS_nanosleep       101
#define SYS_clock_settime   112
#define SYS_clock_gettime   113
#define SYS_sched_yield     124
#define SYS_kill            129
#define SYS_rt_sigaction    134
#define SYS_rt_sigprocmask  135
#define SYS_reboot          142
#define SYS_setpriority     140
#define SYS_getpriority     141
#define SYS_setgid          144
#define SYS_setuid          146
#define SYS_setgroups       159
#define SYS_setpgid         154
#define SYS_getpgid         155
#define SYS_setsid          157
/* prlimit64 rather than the older setrlimit: aarch64 never had the
 * 32-bit setrlimit syscall, so this is the only one there is. */
#define SYS_prlimit64       261
#define SYS_uname           160
#define SYS_getpid          172
#define SYS_getppid         173
#define SYS_getuid          174
#define SYS_sync            81
/* Sockets (asm-generic numbers) */
#define SYS_socket          198
#define SYS_bind            200
#define SYS_listen          201
#define SYS_connect         203
#define SYS_getsockname     204
#define SYS_sendto          206
#define SYS_recvfrom        207
#define SYS_setsockopt      208
#define SYS_getsockopt      209
#define SYS_shutdown        210
#define SYS_accept4         242

/* ppoll, not poll: the asm-generic table aarch64 uses never had a bare
 * poll syscall, only the version that also takes a signal mask. x86-64
 * has both, so we call ppoll on both machines and there is one path to
 * reason about. */
#define SYS_ppoll            73

#define SYS_swapon          224
#define SYS_swapoff         225

#define SYS_brk             214
#define SYS_munmap          215
#define SYS_clone           220
#define SYS_execve          221
#define SYS_mmap            222
#define SYS_wait4           260
#define SYS_getrandom       278

#define SYS_utimensat      88      

/* ── Raw system call wrappers ──
 * The registers must be named explicitly, or the compiler picks its own.
 * The "memory" clobber says the call may change memory. */

static inline long sys_call0(long n)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0");
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static inline long sys_call1(long n, long a)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static inline long sys_call2(long n, long a, long b)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory", "cc");
    return x0;
}

static inline long sys_call3(long n, long a, long b, long c)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2)
                     : "memory", "cc");
    return x0;
}

static inline long sys_call4(long n, long a, long b, long c, long d)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                     : "memory", "cc");
    return x0;
}

static inline long sys_call5(long n, long a, long b, long c, long d, long e)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
                     : "memory", "cc");
    return x0;
}

static inline long sys_call6(long n, long a, long b, long c,
                             long d, long e, long f)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory", "cc");
    return x0;
}

#endif /* _LP_SYSCALL_ARM64_H */
