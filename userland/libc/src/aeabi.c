/* aeabi.c - the arithmetic ARMv6 has no instructions for.
 *
 * ARM1176 has no divide instruction. Not "a slow one" - none at all,
 * for 32-bit or 64-bit, signed or unsigned. Every `/` and `%` in the
 * whole userland becomes a call to one of the functions below, and
 * without them the link fails with a name nobody recognises:
 *
 *     ld.lld: error: undefined symbol: __aeabi_uldivmod
 *
 * A freestanding ARM project normally links the compiler's own libgcc
 * for these. This one does not link anything, so here they are. They
 * are the plain restoring shift-and-subtract algorithm, which is what
 * libgcc does too on a chip with no divider - about 32 iterations, and
 * it is exact.
 *
 * The four ...divmod entry points cannot be written in C at all: the
 * ABI says the quotient comes back in r0 (or r0:r1) and the remainder
 * in r1 (or r2:r3), and C has no way to say that. They are in
 * aeabi.S, wrapped around these.
 */
#include "types.h"

#if defined(__arm__)

u32 lp_udivmod32(u32 n, u32 d, u32 *rem)
{
    if (d == 0) {                    /* the caller has a bug; do not trap */
        if (rem) *rem = 0;
        return 0;
    }

    u32 q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (u32)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}

u64 lp_udivmod64(u64 n, u64 d, u64 *rem)
{
    if (d == 0) {
        if (rem) *rem = 0;
        return 0;
    }
    if (d > n) {
        if (rem) *rem = n;
        return 0;
    }

    /* Shift the divisor up until it is just below the dividend, then
     * subtract on the way back down. Starting from the top bit every
     * time would be 64 iterations for every division, including the
     * "12345 / 10" that printf does for each digit. */
    int shift = 0;
    u64 dd = d;
    while (dd <= n >> 1 && shift < 63) { dd <<= 1; shift++; }

    u64 q = 0, r = n;
    for (int i = shift; i >= 0; i--) {
        if (r >= dd) { r -= dd; q |= (u64)1 << i; }
        dd >>= 1;
    }
    if (rem) *rem = r;
    return q;
}

/* The plain quotient-only entry points are ordinary C. */
u32 __aeabi_uidiv(u32 n, u32 d);
u32 __aeabi_uidiv(u32 n, u32 d) { return lp_udivmod32(n, d, 0); }

s32 __aeabi_idiv(s32 n, s32 d);
s32 __aeabi_idiv(s32 n, s32 d)
{
    bool neg = (n < 0) != (d < 0);
    u32 un = (n < 0) ? (u32)(-(s64)n) : (u32)n;
    u32 ud = (d < 0) ? (u32)(-(s64)d) : (u32)d;
    u32 q = lp_udivmod32(un, ud, 0);
    return neg ? -(s32)q : (s32)q;
}

/* Signed 64-bit, for the .S wrapper. C's / and % truncate toward zero
 * and the sign of the remainder follows the dividend; matching that
 * here is what keeps `-7 % 2` equal to -1 on this machine and on the
 * other two. */
s64 lp_divmod64(s64 n, s64 d, s64 *rem);
s64 lp_divmod64(s64 n, s64 d, s64 *rem)
{
    bool nneg = n < 0, dneg = d < 0;
    u64 un = nneg ? (u64)(-(u64)n) : (u64)n;
    u64 ud = dneg ? (u64)(-(u64)d) : (u64)d;

    u64 r;
    u64 q = lp_udivmod64(un, ud, &r);

    if (rem) *rem = nneg ? -(s64)r : (s64)r;
    return (nneg != dneg) ? -(s64)q : (s64)q;
}

/* 64-bit shifts by a variable amount. clang emits calls to these when
 * the shift count is not a constant. */
u64 __aeabi_llsl(u64 v, int n);
u64 __aeabi_llsl(u64 v, int n) { return n ? (v << n) : v; }

u64 __aeabi_llsr(u64 v, int n);
u64 __aeabi_llsr(u64 v, int n) { return n ? (v >> n) : v; }

s64 __aeabi_lasr(s64 v, int n);
s64 __aeabi_lasr(s64 v, int n) { return n ? (v >> n) : v; }

#endif /* __arm__ */
