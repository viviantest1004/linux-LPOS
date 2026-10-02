/* types.h - 자유 표준(freestanding) 환경용 기본 타입.
 * libc가 없으므로 stdint.h 대신 직접 정의한다. */
#ifndef _TYPES_H
#define _TYPES_H

typedef unsigned char       u8;
typedef signed char         s8;
typedef unsigned short      u16;
typedef signed short        s16;
typedef unsigned int        u32;
typedef signed int          s32;
typedef unsigned long long  u64;
typedef signed long long    s64;

typedef unsigned long       uptr;   /* AArch64: 64bit */
typedef unsigned long       usize;

#define NULL  ((void *)0)

typedef _Bool bool;
#define true  1
#define false 0

/* 컴파일 타임 단언 - 구조체 레이아웃 실수를 빌드에서 잡는다 */
#define STATIC_ASSERT(c, m) _Static_assert(c, m)

STATIC_ASSERT(sizeof(u32) == 4, "u32 must be 4 bytes");
STATIC_ASSERT(sizeof(u64) == 8, "u64 must be 8 bytes");
STATIC_ASSERT(sizeof(uptr) == 8, "AArch64 pointer must be 8 bytes");

#endif /* _TYPES_H */
