/* crt0.S - 프로세스 진입점.
 *
 * 커널은 execve 직후 스택을 이렇게 만들어두고 여기로 점프한다:
 *
 *   sp + 0                  argc
 *   sp + 8                  argv[0]
 *   ...                     argv[argc-1]
 *   sp + 8 + argc*8         NULL
 *   sp + 8 + argc*8 + 8     envp[0]
 *   ...                     NULL
 *   ...                     auxv[]
 *
 * 동적 링커가 없으므로(정적 링크) 우리가 직접 argc/argv/envp 를 꺼내
 * main 으로 넘긴다. */

#if defined(__x86_64__)

/* ── x86-64 ──
 *
 * Same stack layout - the kernel builds it the same way on both - and a
 * different way to reach it. The System V ABI wants rdi/rsi/rdx for the
 * first three arguments, and it wants rsp 16-byte aligned AT THE CALL,
 * which means rsp+8 aligned on entry to the callee because the call
 * pushed a return address. The kernel hands us rsp already 16-aligned
 * with argc on top, so `and $-16` then a plain call gets it right.
 */
.section .text._start, "ax"
.global _start
.type _start, @function

_start:
    xorq    %rbp, %rbp              /* stop stack unwinding here      */

    movq    (%rsp), %rdi            /* rdi = argc                     */
    leaq    8(%rsp), %rsi           /* rsi = argv                     */

    /* envp = argv + argc*8 + 8, just past argv's terminating NULL */
    leaq    8(%rsi,%rdi,8), %rdx    /* rdx = envp                     */

    movq    %rdx, environ(%rip)     /* so getenv() can find it        */

    andq    $-16, %rsp              /* ABI alignment                  */

    call    main

    movl    %eax, %edi              /* main's return is the exit code */
    movl    $231, %eax              /* SYS_exit_group                 */
    syscall

1:  jmp     1b                      /* not reached                    */

.size _start, . - _start

/* ── The signal trampoline ──
 *
 * On arm64 the kernel supplies the code that returns from a signal
 * handler. On x86-64 it does not: rt_sigaction must carry SA_RESTORER
 * and a pointer to something that calls rt_sigreturn, or the handler
 * returns to nowhere and the process dies. This is that something, and
 * it is why lp_signal has an #ifdef in it.
 */
.global lp_sigreturn_trampoline
.type lp_sigreturn_trampoline, @function
lp_sigreturn_trampoline:
    movq    $15, %rax               /* SYS_rt_sigreturn               */
    syscall
.size lp_sigreturn_trampoline, . - lp_sigreturn_trampoline

#elif defined(__arm__)

/* ── ARM, 32-bit (the Pi Zero W) ──
 *
 * The same stack the kernel builds everywhere, in words half the size:
 * argc at sp, argv from sp+4, envp past argv's NULL. AAPCS wants the
 * first three arguments in r0-r2 and the stack 8-byte aligned at a
 * call, which is a weaker rule than the 64-bit one but still a rule.
 *
 * environ is loaded through a literal pool rather than an adrp/add
 * pair - ARMv6 has no adrp, and the pool is what the assembler gives
 * you instead.
 */
.section .text._start, "ax"
.global _start
.type _start, %function

_start:
    mov     fp, #0                  /* stop stack unwinding here      */
    mov     lr, #0

    ldr     r0, [sp]                /* r0 = argc                      */
    add     r1, sp, #4              /* r1 = argv                      */

    /* envp = argv + (argc+1)*4, just past argv's terminating NULL */
    add     r2, r0, #1
    add     r2, r1, r2, lsl #2      /* r2 = envp                      */

    ldr     r3, =environ
    str     r2, [r3]                /* so getenv() can find it        */

    bic     sp, sp, #7              /* AAPCS: 8-byte aligned at a call */

    bl      main

    mov     r7, #248                /* SYS_exit_group                 */
    svc     #0

1:  b       1b                      /* not reached                    */

.size _start, . - _start
.ltorg

#else /* __aarch64__ */

.section .text._start, "ax"
.global _start
.type _start, %function

_start:
    /* 스택 되감기(unwind)가 여기서 멈추도록 프레임/링크 레지스터를 0 으로 */
    mov     x29, xzr
    mov     x30, xzr

    ldr     x0, [sp]                /* x0 = argc                       */
    add     x1, sp, #8              /* x1 = argv                       */

    /* envp = argv + (argc+1)*8   ... argv 종료 NULL 다음 */
    add     x2, x0, #1
    add     x2, x1, x2, lsl #3      /* x2 = envp                       */

    /* environ 전역에 저장해 getenv 등이 쓸 수 있게 한다 */
    adrp    x3, environ
    add     x3, x3, :lo12:environ
    str     x2, [x3]

    /* AAPCS64 는 호출 시점 sp 가 16바이트 정렬일 것을 요구한다.
     * AArch64 는 SP 를 논리연산의 피연산자로 못 쓰므로 레지스터를 경유한다.
     * (커널이 이미 정렬해서 주지만 방어적으로 맞춘다) */
    mov     x9, sp
    and     x9, x9, #-16
    mov     sp, x9

    bl      main

    /* main 의 반환값이 종료 코드. exit_group 은 스레드 그룹 전체를 끝낸다. */
    mov     x8, #94                 /* SYS_exit_group */
    svc     #0

    /* 도달하지 않지만 방어적으로 */
1:  b       1b

.size _start, . - _start

#endif
