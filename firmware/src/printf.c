/* printf.c - 최소 printf. libc 가 없으므로 직접 만든다.
 * 부동소수점은 지원하지 않는다 (커널 초기화 구간에서 FP 를 쓰면
 * 컨텍스트 저장 문제가 생기고, 코드도 훨씬 커진다). */
#include "printf.h"
#include "console.h"
#include "string.h"

#include <stdarg.h>

/* 부호 없는 정수를 base 진수로 출력. 폭/제로패딩/왼쪽정렬 지원.
 *
 * 왼쪽 정렬('-')이 없던 시절, 패닉 핸들러의 레지스터 덤프가
 * "x%-2d 0000..." 을 그대로 찍었다. 모르는 지정자를 글자 그대로
 * 내보내는 동작 자체는 옳지만, 하필 그게 실제로 쓰인 곳이 예외가
 * 난 뒤 딱 한 번 볼 수 있는 출력이라 - 레지스터 번호가 사라졌다.
 * 값 서른 개가 어느 레지스터의 것인지 모르는 덤프는 덤프가 아니다. */
static void print_uint(u64 val, u32 base, bool upper, u32 width,
                       bool zero_pad, bool left)
{
    static const char lower[] = "0123456789abcdef";
    static const char upperc[] = "0123456789ABCDEF";
    const char *digits = upper ? upperc : lower;

    /* 64비트 2진수 최대 64자리지만 우리는 8~16진수만 쓰므로 24면 충분 */
    char buf[24];
    u32  n = 0;

    if (val == 0) {
        buf[n++] = '0';
    } else {
        while (val && n < sizeof(buf)) {
            buf[n++] = digits[val % base];
            val /= base;
        }
    }

    /* 왼쪽 정렬이면 숫자를 먼저 내보내고 뒤를 공백으로 채운다.
     * (왼쪽 정렬에 '0' 패딩은 의미가 없어 무시한다 - printf 도 그렇다.) */
    if (left) {
        u32 printed = n;
        while (n--)
            kputc(buf[n]);
        while (printed < width) { kputc(' '); printed++; }
        return;
    }

    /* 패딩 */
    while (n < width) {
        if (n >= sizeof(buf)) break;
        buf[n++] = zero_pad ? '0' : ' ';
    }

    /* 역순으로 쌓았으니 뒤에서부터 출력 */
    while (n--)
        kputc(buf[n]);
}

static void print_int(s64 val, u32 width, bool zero_pad, bool left)
{
    if (val < 0) {
        kputc('-');
        /* -(-2^63) 은 오버플로다. 캐스팅 후 부호 반전으로 피한다. */
        u64 mag = (u64)(-(val + 1)) + 1;
        print_uint(mag, 10, false, width ? width - 1 : 0, zero_pad, left);
    } else {
        print_uint((u64)val, 10, false, width, zero_pad, left);
    }
}

/* 본체는 va_list 를 받는 쪽이다.
 *
 * panic() 처럼 스스로 가변 인자를 받는 함수가 그것을 그대로 넘겨야
 * 하는데, kprintf(fmt, ...) 만 있으면 그럴 방법이 없다. C 에서 가변
 * 인자를 재전달하는 유일한 길이 va_list 다. */
void kvprintf(const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            kputc(*fmt++);
            continue;
        }
        fmt++;                          /* '%' 소비 */

        /* 플래그: 왼쪽 정렬('-')과 '0' 패딩. 순서는 상관없다. */
        bool zero_pad = false, left = false;
        for (;;) {
            if (*fmt == '-')      { left = true;     fmt++; }
            else if (*fmt == '0') { zero_pad = true; fmt++; }
            else break;
        }

        /* 폭 */
        u32 width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (u32)(*fmt++ - '0');

        /* 길이 수식어: l, ll (AArch64 에서 long 도 64비트) */
        int longness = 0;
        while (*fmt == 'l') { longness++; fmt++; }

        switch (*fmt) {
        case 'c':
            kputc((char)va_arg(ap, int));
            break;

        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            usize len = strlen(s);
            if (left) {
                while (*s) kputc(*s++);
                while (len < width) { kputc(' '); width--; }
            } else {
                while (len < width) { kputc(' '); width--; }
                while (*s) kputc(*s++);
            }
            break;
        }

        case 'd':
        case 'i':
            print_int(longness ? va_arg(ap, s64) : (s64)va_arg(ap, s32),
                      width, zero_pad, left);
            break;

        case 'u':
            print_uint(longness ? va_arg(ap, u64) : (u64)va_arg(ap, u32),
                       10, false, width, zero_pad, left);
            break;

        case 'x':
            print_uint(longness ? va_arg(ap, u64) : (u64)va_arg(ap, u32),
                       16, false, width, zero_pad, left);
            break;

        case 'X':
            print_uint(longness ? va_arg(ap, u64) : (u64)va_arg(ap, u32),
                       16, true, width, zero_pad, left);
            break;

        case 'p':
                    kputc('0');
            kputc('x');
            print_uint((u64)(uptr)va_arg(ap, void *), 16, false, 16, true, false);
            break;

        case '%':
            kputc('%');
            break;

        case '\0':                      /* 형식 문자열이 '%' 로 끝남 */
            return;

        default:                        /* 모르는 지정자는 그대로 출력 */
            kputc('%');
            kputc(*fmt);
            break;
        }
        fmt++;
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}
