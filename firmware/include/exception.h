/* exception.h - AArch64 EL1 예외 처리.
 *
 * Phase 1 에서 무언가 잘못되면 보드는 그냥 멈췄다. 널 포인터를 쓰든,
 * 정렬 안 된 주소를 읽든, 없는 명령을 실행하든 결과가 똑같았다 -
 * UART 가 조용해지고 끝. 어디서 죽었는지 알 방법이 없었다.
 *
 * 예외 벡터를 걸면 그 순간이 보인다. CPU 가 우리 코드로 점프해 오고,
 * ESR_EL1 에 무슨 일인지, FAR_EL1 에 어느 주소인지, ELR_EL1 에 어느
 * 명령에서인지가 들어 있다. 그걸 읽어서 사람이 읽을 수 있는 형태로
 * 뱉는 것이 이 모듈이다.
 */
#ifndef _EXCEPTION_H
#define _EXCEPTION_H

#include "types.h"

/* 예외가 났을 때 스택에 쌓이는 것. vectors.S 와 정확히 같은 순서여야
 * 한다 - 어긋나면 레지스터 덤프가 조용히 거짓말을 한다. */
typedef struct {
    u64 x[31];      /* x0 - x30 */
    u64 sp;         /* 예외 직전의 스택 포인터 */
    u64 elr;        /* 어느 명령에서 났나 */
    u64 spsr;       /* 그때의 PSTATE */
    u64 esr;        /* 무슨 예외인가 */
    u64 far;        /* 어느 주소를 건드리다가 */
    u64 kind;       /* 벡터 테이블의 몇 번째 칸으로 들어왔나 */
} exc_frame_t;

/* VBAR_EL1 을 우리 벡터 테이블로 돌린다. boot.S 가 아주 이른 시점에
 * 부르고, kernel_main 에서 다시 불러도 해가 없다. */
void exception_init(void);

/* IRQ 를 열고 닫는다. DAIF 의 I 비트. */
void irq_enable(void);
void irq_disable(void);

/* vectors.S 가 부르는 것들. 직접 부를 일은 없다. */
void exception_handler(exc_frame_t *f);
void irq_handler(exc_frame_t *f);

/* 되돌아오지 않는다. 이유를 찍고 보드를 세운다.
 * 프레임이 없으면(우리가 스스로 부른 패닉) NULL 을 준다. */
void panic(const exc_frame_t *f, const char *fmt, ...)
    __attribute__((format(printf, 2, 3), noreturn));

#endif /* _EXCEPTION_H */
