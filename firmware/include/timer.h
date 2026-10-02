/* timer.h - BCM2710 시스템 타이머 (1MHz 고정 64비트 프리러닝 카운터) */
#ifndef _TIMER_H
#define _TIMER_H

#include "types.h"

u64  timer_get_us(void);          /* 부팅 후 경과 마이크로초 */
void delay_us(u64 us);
void delay_ms(u64 ms);

/* 사이클 단위 짧은 지연. GPIO 풀업/풀다운처럼 "N 사이클 대기"가
 * 명세에 박혀 있는 경우에 쓴다. 최적화로 사라지지 않도록 인라인 asm. */
static inline void delay_cycles(u64 n)
{
    __asm__ volatile("1: subs %0, %0, #1\n\t"
                     "   bne  1b\n\t"
                     : "+r"(n) :: "cc");
}

#endif /* _TIMER_H */
