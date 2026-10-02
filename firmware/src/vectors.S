/* vectors.S - AArch64 EL1 예외 벡터 테이블.
 *
 * ── 표가 이렇게 생긴 이유 ────────────────────────────────────────
 * 아키텍처가 정한 모양이라 우리가 고를 수 있는 게 없다. 칸이 16개이고
 * 칸 하나가 정확히 128바이트(명령 32개)이며, 표 전체는 2KB 경계에
 * 정렬되어야 한다. VBAR_EL1 에 표의 주소를 넣으면 CPU 가 예외 종류에
 * 따라 해당 칸으로 점프한다.
 *
 *   0x000  현재 EL, SP_EL0 사용 중   Sync / IRQ / FIQ / SError
 *   0x200  현재 EL, SP_ELx 사용 중   Sync / IRQ / FIQ / SError   <- 우리
 *   0x400  하위 EL, AArch64          Sync / IRQ / FIQ / SError
 *   0x600  하위 EL, AArch32          Sync / IRQ / FIQ / SError
 *
 * 우리 펌웨어는 EL1h(SP_EL1)로 돌기 때문에 실제로 쓰이는 것은 0x200
 * 무리다. 나머지 열두 칸을 비워두지 않는 이유는, 거기로 들어왔다는
 * 사실 자체가 알아야 할 사건이기 때문이다 - 조용히 죽는 것보다
 * "여기로 왔다"고 말하고 죽는 편이 언제나 낫다.
 *
 * 칸이 128바이트뿐이라 핸들러를 그 안에 다 쓸 수 없다. 그래서 각 칸은
 * "몇 번째 칸인지"만 기록하고 공통 루틴으로 뛴다.
 */

#include "asm-offsets.h"

.section ".text.vectors", "ax"

/* ── 예외 프레임 저장 ─────────────────────────────────────────────
 *
 * exc_frame_t 와 순서가 정확히 같아야 한다. 어긋나면 레지스터 덤프가
 * 조용히 거짓말을 하고, 그건 디버깅에 쓸 수 없는 정도가 아니라 사람을
 * 엉뚱한 데로 보낸다.
 *
 * x30(LR)까지 저장하는 이유: 백트레이스가 x29 체인을 따라가는데,
 * 가장 안쪽 프레임의 복귀 주소는 아직 스택이 아니라 x30 에 있다.
 *
 * ── 칸에서 레지스터를 먼저 저장하는 이유 ─────────────────────────
 * 칸 번호를 어딘가에 담아 와야 하는데, 처음에는 칸에서 x9 에 넣고
 * 여기서 저장했다. 순서가 거꾸로였다 - x9 는 저장되기 전에 이미
 * 덮여 있었으므로, 저장된 것은 인터럽트당한 코드의 x9 가 아니라 칸
 * 번호였고, 복귀할 때 그 값이 x9 로 돌아갔다.
 *
 * 증상은 예외와 아무 상관 없어 보였다. delay_ms(500) 이 8밀리초 만에
 * 돌아왔다 - 루프 변수가 하필 x9 에 있었기 때문이다. 인터럽트가
 * 조용히 남의 레지스터를 바꿔놓는 버그는 원인에서 아무리 멀리
 * 떨어진 곳에서도 터질 수 있다.
 *
 * 그래서 칸이 프레임을 밀고 x0/x1 을 먼저 저장한 다음, 그 x0 에
 * 칸 번호를 담아 온다. 덮이는 레지스터는 이미 저장된 것뿐이다. */
.macro SAVE_REST
    /* x0 와 x1 은 이미 칸에서 저장했다. x0 에는 칸 번호가 들어 있다. */
    str     x0,  [sp, #FRAME_KIND]

    stp     x2,  x3,  [sp, #(8 * 2)]
    stp     x4,  x5,  [sp, #(8 * 4)]
    stp     x6,  x7,  [sp, #(8 * 6)]
    stp     x8,  x9,  [sp, #(8 * 8)]
    stp     x10, x11, [sp, #(8 * 10)]
    stp     x12, x13, [sp, #(8 * 12)]
    stp     x14, x15, [sp, #(8 * 14)]
    stp     x16, x17, [sp, #(8 * 16)]
    stp     x18, x19, [sp, #(8 * 18)]
    stp     x20, x21, [sp, #(8 * 20)]
    stp     x22, x23, [sp, #(8 * 22)]
    stp     x24, x25, [sp, #(8 * 24)]
    stp     x26, x27, [sp, #(8 * 26)]
    stp     x28, x29, [sp, #(8 * 28)]

    /* 예외 직전의 SP 는 칸에서 프레임을 밀기 전의 값이다. */
    add     x1,  sp,  #FRAME_SIZE
    stp     x30, x1,  [sp, #(8 * 30)]
.endm

/* IRQ 핸들러는 돌아온다. 그래서 범용 레지스터를 되돌린다.
 *
 * ELR/SPSR 은 건드리지 않는다. 핸들러가 그 값을 고치지 않고, 핸들러
 * 안에서 다시 예외가 나면 어차피 패닉으로 끝나기 때문이다. 되돌리려면
 * 우리가 EL1 에 있는지 EL2 에 있는지에 따라 다른 레지스터를 써야 하는데,
 * 그 분기를 어셈블리에 넣느니 아예 필요를 없애는 편이 낫다. */
.macro RESTORE_FRAME
    ldp     x0,  x1,  [sp, #(8 * 0)]
    ldp     x2,  x3,  [sp, #(8 * 2)]
    ldp     x4,  x5,  [sp, #(8 * 4)]
    ldp     x6,  x7,  [sp, #(8 * 6)]
    ldp     x8,  x9,  [sp, #(8 * 8)]
    ldp     x10, x11, [sp, #(8 * 10)]
    ldp     x12, x13, [sp, #(8 * 12)]
    ldp     x14, x15, [sp, #(8 * 14)]
    ldp     x16, x17, [sp, #(8 * 16)]
    ldp     x18, x19, [sp, #(8 * 18)]
    ldp     x20, x21, [sp, #(8 * 20)]
    ldp     x22, x23, [sp, #(8 * 22)]
    ldp     x24, x25, [sp, #(8 * 24)]
    ldp     x26, x27, [sp, #(8 * 26)]
    ldp     x28, x29, [sp, #(8 * 28)]
    ldr     x30,      [sp, #(8 * 30)]
    add     sp,  sp,  #FRAME_SIZE
.endm

/* 칸 하나. 프레임을 밀고 x0/x1 을 저장한 뒤, 저장이 끝난 x0 에 칸
 * 번호를 담아 공통 루틴으로 뛴다.
 * .align 7 = 128바이트 = 칸 하나의 크기. 네 명령이면 넉넉히 들어간다. */
.macro VECTOR_ENTRY kind, target
    .align  7
    sub     sp,  sp,  #FRAME_SIZE
    stp     x0,  x1,  [sp, #(8 * 0)]
    mov     x0,  #\kind
    b       \target
.endm

/* ── 표 본체 ─────────────────────────────────────────────────────
 * 2KB 정렬은 VBAR_EL1 이 하위 11비트를 무시하기 때문에 선택이 아니다. */
    .align  11
    .global vector_table
vector_table:
    /* 현재 EL, SP_EL0 */
    VECTOR_ENTRY 0,  handle_sync_fatal
    VECTOR_ENTRY 1,  handle_irq_fatal
    VECTOR_ENTRY 2,  handle_sync_fatal
    VECTOR_ENTRY 3,  handle_sync_fatal

    /* 현재 EL, SP_ELx - 우리가 실제로 도는 모드 */
    VECTOR_ENTRY 4,  handle_sync
    VECTOR_ENTRY 5,  handle_irq
    VECTOR_ENTRY 6,  handle_irq        /* FIQ 도 같은 경로로 */
    VECTOR_ENTRY 7,  handle_sync_fatal /* SError - 복구 불가 */

    /* 하위 EL, AArch64 - 아직 하위 EL 로 내려보내는 것이 없다 */
    VECTOR_ENTRY 8,  handle_sync_fatal
    VECTOR_ENTRY 9,  handle_irq_fatal
    VECTOR_ENTRY 10, handle_irq_fatal
    VECTOR_ENTRY 11, handle_sync_fatal

    /* 하위 EL, AArch32 - 32비트로 내려보내는 것도 없다 */
    VECTOR_ENTRY 12, handle_sync_fatal
    VECTOR_ENTRY 13, handle_irq_fatal
    VECTOR_ENTRY 14, handle_irq_fatal
    VECTOR_ENTRY 15, handle_sync_fatal

/* ── 공통 진입점 ─────────────────────────────────────────────────
 *
 * 들어올 때 프레임은 이미 밀려 있고 x0/x1 이 저장돼 있으며, x0 에
 * 칸 번호가 들어 있다. */

.type   handle_sync, %function
handle_sync:
    SAVE_REST
    mov     x0, sp
    bl      exception_handler
    /* 동기 예외는 돌아오지 않는다. exception_handler 가 패닉으로 끝낸다.
     * 그래도 혹시 돌아오면 멈춘다 - 깨진 상태로 계속 도는 것보다 낫다. */
1:  wfe
    b       1b
.size   handle_sync, . - handle_sync

.type   handle_sync_fatal, %function
handle_sync_fatal:
    SAVE_REST
    mov     x0, sp
    bl      exception_handler
1:  wfe
    b       1b
.size   handle_sync_fatal, . - handle_sync_fatal

.type   handle_irq, %function
handle_irq:
    SAVE_REST
    mov     x0, sp
    bl      irq_handler
    RESTORE_FRAME
    eret
.size   handle_irq, . - handle_irq

/* 올 수 없는 자리로 온 IRQ. 돌려보내지 않는다 - 어떻게 여기 왔는지
 * 모르는 채로 복귀하면 다음에 어디서 죽을지도 모른다. */
.type   handle_irq_fatal, %function
handle_irq_fatal:
    SAVE_REST
    mov     x0, sp
    bl      exception_handler
1:  wfe
    b       1b
.size   handle_irq_fatal, . - handle_irq_fatal
