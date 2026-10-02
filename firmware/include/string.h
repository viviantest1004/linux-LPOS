/* string.h - 컴파일러가 자동 호출할 수 있는 최소 메모리/문자열 함수.
 * -ffreestanding 이어도 GCC/Clang 은 구조체 복사 등에서 memcpy/memset 을
 * 부를 수 있으므로 반드시 제공해야 한다. */
#ifndef _STRING_H
#define _STRING_H

#include "types.h"

void  *memset(void *dst, int c, usize n);
void  *memcpy(void *dst, const void *src, usize n);
void  *memmove(void *dst, const void *src, usize n);
int    memcmp(const void *a, const void *b, usize n);
usize  strlen(const char *s);
int    strcmp(const char *a, const char *b);

#endif /* _STRING_H */
