/* string.c - 최소 메모리/문자열 루틴. */
#include "string.h"

void *memset(void *dst, int c, usize n)
{
    u8 *d = (u8 *)dst;
    u8  v = (u8)c;

    /* 8바이트 정렬까지 바이트 단위로 맞춘다 */
    while (n && ((uptr)d & 7u)) { *d++ = v; n--; }

    /* 정렬된 구간은 8바이트씩 */
    u64 w = 0x0101010101010101ULL * v;
    while (n >= 8) { *(u64 *)d = w; d += 8; n -= 8; }

    while (n--) *d++ = v;
    return dst;
}

void *memcpy(void *dst, const void *src, usize n)
{
    u8       *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    /* 양쪽 정렬이 같을 때만 워드 단위로 빠르게 복사 */
    if ((((uptr)d ^ (uptr)s) & 7u) == 0) {
        while (n && ((uptr)d & 7u)) { *d++ = *s++; n--; }
        while (n >= 8) { *(u64 *)d = *(const u64 *)s; d += 8; s += 8; n -= 8; }
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, usize n)
{
    u8       *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    if (d == s || n == 0)
        return dst;

    if (d < s)
        return memcpy(dst, src, n);

    /* 영역이 겹치고 dst 가 뒤쪽이면 뒤에서부터 */
    d += n;
    s += n;
    while (n--) *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, usize n)
{
    const u8 *p = (const u8 *)a, *q = (const u8 *)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

usize strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (usize)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}
