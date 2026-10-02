/* syscall.h - pick the system call table for the machine being built.
 *
 * Everything above this line in the source tree is architecture-neutral:
 * the whole userland goes through sys_callN() and the SYS_* names, and
 * those are the only two things that differ between an arm64 board and
 * an amd64 desktop. Splitting the header was the entire port.
 *
 * The numbers really are unrelated between them - x86-64 and 32-bit ARM
 * each carry their own table from before the asm-generic one existed -
 * so there is no clever shared list, just three of them.
 *
 * arm is the Pi Zero W (ARM1176, ARMv6, 32 bits), which is a different
 * machine from the Zero 2 W in every way that matters here.
 */
#ifndef _LP_SYSCALL_H
#define _LP_SYSCALL_H

#if defined(__aarch64__)
#  include "syscall-arm64.h"
#elif defined(__x86_64__)
#  include "syscall-x86_64.h"
#elif defined(__arm__)
#  include "syscall-arm.h"
#else
#  error "no system call table for this machine - see syscall-arm64.h"
#endif

#endif
