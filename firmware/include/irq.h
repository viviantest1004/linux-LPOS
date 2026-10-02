/* irq.h - 인터럽트 컨트롤러와 ARM generic timer.
 *
 * Phase 1 은 전부 폴링이었다. UART 를 읽으려면 문자가 올 때까지 돌고,
 * 시간을 재려면 카운터를 계속 읽었다. 그것만으로도 브링업은 되지만,
 * 두 가지를 동시에 기다리는 순간 방법이 없어진다.
 *
 * 인터럽트를 붙이면 CPU 가 알려준다. 그리고 주기적인 타이머 틱이
 * 생기면 "얼마나 지났나" 를 세는 기준이 하나 생긴다 - 뒤에 붙일 SD
 * 드라이버의 타임아웃이 전부 그 위에 선다.
 */
#ifndef _IRQ_H
#define _IRQ_H

#include "types.h"

typedef void (*irq_fn)(void);

/* 컨트롤러를 알려진 상태로 만든다. 모든 소스를 끄고 시작하는데,
 * 부트로더가 켜둔 채로 넘겨준 인터럽트가 우리 핸들러가 준비되기 전에
 * 들어오면 그게 첫 번째 패닉이 되기 때문이다. */
void irq_init(void);

/* 주기적 타이머 틱을 건다. hz 는 초당 틱 수.
 * CNTFRQ_EL0 를 읽어 계산하므로 보드와 QEMU 어느 쪽이든 맞는다. */
void timer_tick_start(u32 hz);
void timer_tick_stop(void);

/* 부팅 후 지나간 틱 수. 타이머가 실제로 도는지 보는 가장 쉬운 증거. */
u64  timer_ticks(void);

/* GPU 페리페럴 인터럽트 하나를 열고 핸들러를 건다.
 * n 은 0..63 (IRQ_PENDING_1/2 의 비트 위치). */
void irq_register(u32 n, irq_fn fn);
void irq_unregister(u32 n);

/* 타이머가 왜 안 우는지 보여준다. */
void timer_tick_debug(void);

#endif /* _IRQ_H */
