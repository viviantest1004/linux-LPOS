/* uart.c - PL011 UART0 폴링 드라이버.
 *
 * Pi Zero 2 W 핀아웃 (40핀 헤더):
 *   GPIO14 = TXD0 -> 헤더 8번 핀
 *   GPIO15 = RXD0 -> 헤더 10번 핀
 *   GND           -> 헤더 6번 핀
 * USB-TTL 어댑터를 TX<->RX 교차로 연결하고 115200 8N1 로 연다.
 *
 * 참고: 리눅스에서는 PL011 이 기본적으로 블루투스에 물려 있어서
 * dtoverlay=disable-bt 가 필요하다. 하지만 베어메탈에서는 우리가
 * GPIO 대체기능을 직접 지정하므로 그런 제약이 없다. */
#include "uart.h"
#include "mmio.h"
#include "gpio.h"
#include "mbox.h"
#include "bcm2710.h"

#define UART_BAUD  115200

void uart_init(void)
{
    /* 1. 설정 중에는 UART 를 완전히 끈다 */
    mmio_write32(UART0_CR, 0);

    /* 2. GPIO14/15 를 ALT0 (= TXD0/RXD0) 으로.
     *    시리얼 라인은 외부에서 구동되므로 내부 풀은 끈다. */
    gpio_set_function(14, GPIO_FUNC_ALT0);
    gpio_set_function(15, GPIO_FUNC_ALT0);
    gpio_set_pull(14, GPIO_PULL_NONE);
    gpio_set_pull(15, GPIO_PULL_NONE);

    /* 3. 대기 중인 인터럽트 전부 클리어 */
    mmio_write32(UART0_ICR, 0x7FF);

    /* 4. 보레이트 분주비 계산.
     *    UART 클럭은 config.txt 의 init_uart_clock 이 정한다(보통 48MHz).
     *    하드코딩하면 펌웨어 버전에 따라 깨지므로 GPU 에게 직접 묻는다. */
    u32 clk = mbox_get_clock_rate(MBOX_CLOCK_UART);
    if (clk == 0)
        clk = 48000000;     /* 메일박스 실패 시 폴백 */

    /* PL011: BAUDDIV = clk / (16 * baud)
     *        IBRD = 정수부, FBRD = 소수부 * 64 (반올림)
     * f = 64 * BAUDDIV 를 한 번에 구하면 나눗셈 한 번으로 끝난다. */
    u64 f    = ((u64)clk * 8 + UART_BAUD) / (2ULL * UART_BAUD);
    u32 ibrd = (u32)(f >> 6);
    u32 fbrd = (u32)(f & 0x3F);

    mmio_write32(UART0_IBRD, ibrd);
    mmio_write32(UART0_FBRD, fbrd);

    /* 5. 8N1 + FIFO 사용.
     *    중요: LCRH 쓰기가 IBRD/FBRD 값을 래치한다. 반드시 이 순서. */
    mmio_write32(UART0_LCRH, UART_LCRH_WLEN8 | UART_LCRH_FEN);

    /* 6. 인터럽트는 쓰지 않는다 (Phase 2 에서 추가) */
    mmio_write32(UART0_IMSC, 0);

    /* 7. 송수신 켜기 */
    mmio_write32(UART0_CR, UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);
}

void uart_putc(char c)
{
    /* 터미널이 기대하는 CRLF 로 변환 */
    if (c == '\n')
        uart_putc('\r');

    while (mmio_read32(UART0_FR) & UART_FR_TXFF)
        ;
    mmio_write32(UART0_DR, (u32)(u8)c);
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

bool uart_rx_ready(void)
{
    return (mmio_read32(UART0_FR) & UART_FR_RXFE) == 0;
}

char uart_getc(void)
{
    while (mmio_read32(UART0_FR) & UART_FR_RXFE)
        ;
    return (char)(mmio_read32(UART0_DR) & 0xFF);
}

void uart_flush(void)
{
    while (mmio_read32(UART0_FR) & UART_FR_BUSY)
        ;
}
