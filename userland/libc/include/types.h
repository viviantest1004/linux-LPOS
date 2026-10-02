/* types.h - base types for our own libc. No system headers.
 *
 * Two word sizes now. The Pi Zero 2 W and a PC are LP64: long is 64
 * bits and a pointer is 64 bits. The Pi Zero W is ILP32: long is 32
 * bits. So the fixed-width names below cannot all be spelled "long",
 * and the ones that must stay 64 bits everywhere - a file offset, a
 * time - are spelled s64 rather than long on purpose.
 *
 * A file bigger than 2GB and a date past 2038 both fit in 32 bits only
 * if you are willing to be wrong about them, and this machine is meant
 * to be sitting in a cupboard working in 2040.
 */
#ifndef _LP_TYPES_H
#define _LP_TYPES_H

typedef unsigned char       u8;
typedef signed char         s8;
typedef unsigned short      u16;
typedef signed short        s16;
typedef unsigned int        u32;
typedef signed int          s32;

#if defined(__LP64__) || defined(_LP64)
typedef unsigned long       u64;
typedef signed long         s64;
typedef unsigned long       size_t;
typedef signed long         ssize_t;
typedef unsigned long       uintptr_t;
#else
typedef unsigned long long  u64;
typedef signed long long    s64;
typedef unsigned int        size_t;
typedef signed int          ssize_t;
typedef unsigned int        uintptr_t;
#endif

/* 64 bits on every machine, deliberately. */
typedef s64                 off_t;
typedef s64                 time_t;

typedef signed int          pid_t;
typedef unsigned int        mode_t;
typedef unsigned int        uid_t;
typedef unsigned int        gid_t;

/* Guarded because the compiler's own stddef.h defines it identically,
 * and anything that includes both - BearSSL's headers do - would
 * otherwise fail the build on a redefinition that changes nothing. */
#ifndef NULL
#define NULL ((void *)0)
#endif

typedef _Bool bool;
#define true  1
#define false 0

_Static_assert(sizeof(u64) == 8, "u64 must be 64 bits");
_Static_assert(sizeof(size_t) == sizeof(void *), "size_t must hold a pointer");

#endif /* _LP_TYPES_H */
