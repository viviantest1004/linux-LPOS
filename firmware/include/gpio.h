/* gpio.h - BCM2710 GPIO. 핀 0~53. */
#ifndef _GPIO_H
#define _GPIO_H

#include "types.h"

/* GPFSEL 3비트 인코딩. 순서가 직관적이지 않으니 데이터시트 그대로 옮긴다. */
typedef enum {
    GPIO_FUNC_INPUT  = 0,   /* 000 */
    GPIO_FUNC_OUTPUT = 1,   /* 001 */
    GPIO_FUNC_ALT5   = 2,   /* 010 */
    GPIO_FUNC_ALT4   = 3,   /* 011 */
    GPIO_FUNC_ALT0   = 4,   /* 100 */
    GPIO_FUNC_ALT1   = 5,   /* 101 */
    GPIO_FUNC_ALT2   = 6,   /* 110 */
    GPIO_FUNC_ALT3   = 7,   /* 111 */
} gpio_func_t;

typedef enum {
    GPIO_PULL_NONE = 0,
    GPIO_PULL_DOWN = 1,
    GPIO_PULL_UP   = 2,
} gpio_pull_t;

void gpio_set_function(u32 pin, gpio_func_t func);
void gpio_set_pull(u32 pin, gpio_pull_t pull);
void gpio_write(u32 pin, bool high);
bool gpio_read(u32 pin);
void gpio_toggle(u32 pin);

#endif /* _GPIO_H */
