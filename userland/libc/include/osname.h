/* osname.h - what this system calls itself.
 *
 * One place, because the name appears at boot, in `uname`, in `top`'s
 * header, in `sysinfo` and in `help`, and a name that is right in five
 * of those and stale in the sixth is worse than either.
 *
 * The Makefile sets it: the arm64 build is LP-zero, the machine this
 * was written for; the amd64 build is linux-LP, which is the same
 * system on an ordinary desktop and says so rather than claiming to be
 * a Raspberry Pi. */
#ifndef _LP_OSNAME_H
#define _LP_OSNAME_H

#ifndef LP_OS_NAME
#  define LP_OS_NAME "LP-zero"
#endif

/* The line under the name at boot and on the splash: what this is
 * running on. The arm64 build says the board it was written for; the
 * amd64 build must not, because it is not one. */
#ifndef LP_OS_MACHINE
#  define LP_OS_MACHINE "Raspberry Pi Zero 2 W  -  built from scratch"
#endif
#ifndef LP_OS_TITLE
#  define LP_OS_TITLE "test_a_123_LPzero2W_img"
#endif

#endif
