/* aeabi.S - the two division entry points that C cannot express.
 *
 * The ARM ABI says __aeabi_uidivmod hands back the quotient in r0 and
 * the remainder in r1, and __aeabi_uldivmod the quotient in r0:r1 and
 * the remainder in r2:r3. There is no way to write "returns two values
 * in these registers" in C, so these small wrappers call the C helpers
 * in aeabi.c and move the results where the ABI wants them.
 *
 * The compiler emits calls to these for every `/` and `%` on a chip
 * with no divide instruction, which is every ARMv6 - so a mistake in
 * eight instructions here is a wrong number in a hundred places.
 */
#if defined(__arm__)

.syntax unified
.arm
.section .text, "ax"

/* u32 quotient in r0, u32 remainder in r1 */
.global __aeabi_uidivmod
.type __aeabi_uidivmod, %function
__aeabi_uidivmod:
    push    {r4, lr}
    sub     sp, sp, #8              /* room for the remainder, 8-aligned */
    mov     r2, sp                  /* third argument: u32 *rem          */
    bl      lp_udivmod32
    ldr     r1, [sp]
    add     sp, sp, #8
    pop     {r4, pc}
.size __aeabi_uidivmod, . - __aeabi_uidivmod

/* s32 quotient in r0, s32 remainder in r1 */
.global __aeabi_idivmod
.type __aeabi_idivmod, %function
__aeabi_idivmod:
    push    {r4, r5, r6, lr}
    mov     r4, r0
    mov     r5, r1
    bl      __aeabi_idiv            /* r0 = quotient                     */
    mov     r6, r0
    mul     r0, r6, r5              /* quotient * divisor                */
    sub     r1, r4, r0              /* remainder = dividend - that       */
    mov     r0, r6
    pop     {r4, r5, r6, pc}
.size __aeabi_idivmod, . - __aeabi_idivmod

/* u64 quotient in r0:r1, u64 remainder in r2:r3.
 *
 * n arrives in r0:r1 and d in r2:r3, which is already what
 * lp_udivmod64 wants; the only work is finding somewhere for the
 * remainder and passing its address as the fifth argument, which the
 * ABI puts on the stack. */
.global __aeabi_uldivmod
.type __aeabi_uldivmod, %function
__aeabi_uldivmod:
    push    {r4, lr}
    sub     sp, sp, #16             /* [sp]=arg5, [sp+8]=the remainder   */
    add     r4, sp, #8
    str     r4, [sp]
    bl      lp_udivmod64
    ldr     r2, [sp, #8]
    ldr     r3, [sp, #12]
    add     sp, sp, #16
    pop     {r4, pc}
.size __aeabi_uldivmod, . - __aeabi_uldivmod

/* s64 quotient in r0:r1, s64 remainder in r2:r3 */
.global __aeabi_ldivmod
.type __aeabi_ldivmod, %function
__aeabi_ldivmod:
    push    {r4, lr}
    sub     sp, sp, #16
    add     r4, sp, #8
    str     r4, [sp]
    bl      lp_divmod64
    ldr     r2, [sp, #8]
    ldr     r3, [sp, #12]
    add     sp, sp, #16
    pop     {r4, pc}
.size __aeabi_ldivmod, . - __aeabi_ldivmod

#endif /* __arm__ */
