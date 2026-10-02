/* uart.h - PL011 UART0 폴링 드라이버 (GPIO14=TX, GPIO15=RX, 115200 8N1) */
#ifndef _UART_H
#define _UART_H

#include "types.h"

void uart_init(void);
void uart_putc(char c);          /* '\n' 은 "\r\n" 으로 변환 */
void uart_puts(const char *s);
char uart_getc(void);            /* 블로킹 */
bool uart_rx_ready(void);
void uart_flush(void);           /* 송신 FIFO 가 빌 때까지 대기 */

#endif /* _UART_H */
