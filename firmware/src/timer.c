/* timer.c - BCM2710 시스템 타이머.
 * 1MHz 로 고정된 64비트 카운터라 ARM 클럭이 바뀌어도 영향이 없다.
 * 그래서 부팅 초반 지연 루프의 기준으로 쓰기에 가장 안전하다. */
#include "timer.h"
#include "mmio.h"
#include "bcm2710.h"

u64 timer_get_us(void)
{
    u32 hi, lo, hi2;

    /* CHI/CLO 는 32비트씩 따로 읽어야 한다. 읽는 도중 하위가
     * 넘어가면 값이 깨지므로 상위가 안 변할 때까지 다시 읽는다. */
    do {
        hi  = mmio_read32(SYSTIMER_CHI);
        lo  = mmio_read32(SYSTIMER_CLO);
        hi2 = mmio_read32(SYSTIMER_CHI);
    } while (hi != hi2);

    return ((u64)hi << 32) | lo;
}

void delay_us(u64 us)
{
    u64 start = timer_get_us();
    /* 뺄셈으로 비교하면 64비트 랩어라운드에도 안전하다
     * (1MHz 기준 58만 년이라 실제로는 볼 일 없지만) */
    while ((timer_get_us() - start) < us)
        ;
}

void delay_ms(u64 ms)
{
    delay_us(ms * 1000ULL);
}
