/* printf.h - libc 없는 환경용 최소 printf.
 * 지원: %c %s %d %i %u %x %X %p %% , 길이 l/ll, 폭 지정과 '0' 패딩 */
#ifndef _PRINTF_H
#define _PRINTF_H

#include "types.h"

#include <stdarg.h>

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);

#endif /* _PRINTF_H */
