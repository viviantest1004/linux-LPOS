/* gpio.c - BCM2710 GPIO 드라이버. */
#include "gpio.h"
#include "mmio.h"
#include "bcm2710.h"
#include "timer.h"

#define GPIO_MAX_PIN  53

void gpio_set_function(u32 pin, gpio_func_t func)
{
    if (pin > GPIO_MAX_PIN)
        return;

    /* GPFSEL 레지스터 하나가 핀 10개를 3비트씩 담당한다 */
    uptr reg   = GPFSEL0 + (uptr)(pin / 10) * 4;
    u32  shift = (pin % 10) * 3;

    u32 v = mmio_read32(reg);
    v &= ~(7u << shift);
    v |= ((u32)func & 7u) << shift;
    mmio_write32(reg, v);
}

void gpio_set_pull(u32 pin, gpio_pull_t pull)
{
    if (pin > GPIO_MAX_PIN)
        return;

    /* BCM2835/2710 의 풀 설정은 상태머신이라 순서를 지켜야 한다:
     *   GPPUD 에 값 쓰기 -> 150사이클 -> GPPUDCLK 에 핀 비트 -> 150사이클
     *   -> 양쪽 클리어.
     * (Pi 4 의 BCM2711 은 이 방식이 아니라 전용 레지스터를 쓴다) */
    uptr clk_reg = (pin < 32) ? GPPUDCLK0 : GPPUDCLK1;
    u32  bit     = 1u << (pin % 32);

    mmio_write32(GPPUD, (u32)pull);
    delay_cycles(150);
    mmio_write32(clk_reg, bit);
    delay_cycles(150);
    mmio_write32(GPPUD, 0);
    mmio_write32(clk_reg, 0);
}

void gpio_write(u32 pin, bool high)
{
    if (pin > GPIO_MAX_PIN)
        return;

    /* SET/CLR 은 write-1-to-act 이라 read-modify-write 가 필요 없다.
     * 덕분에 인터럽트와 경쟁하지 않는다. */
    uptr reg;
    if (high) reg = (pin < 32) ? GPSET0 : GPSET1;
    else      reg = (pin < 32) ? GPCLR0 : GPCLR1;

    mmio_write32(reg, 1u << (pin % 32));
}

bool gpio_read(u32 pin)
{
    if (pin > GPIO_MAX_PIN)
        return false;

    uptr reg = (pin < 32) ? GPLEV0 : GPLEV1;
    return (mmio_read32(reg) >> (pin % 32)) & 1u;
}

void gpio_toggle(u32 pin)
{
    gpio_write(pin, !gpio_read(pin));
}
