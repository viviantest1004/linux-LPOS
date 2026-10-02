/* console.h - 프레임버퍼 위의 텍스트 콘솔.
 * 8x16 폰트로 글자를 직접 픽셀에 찍고, 아래로 넘치면 스크롤한다. */
#ifndef _CONSOLE_H
#define _CONSOLE_H

#include "types.h"

/* 프레임버퍼가 준비된 뒤에 호출한다. 실패하면 이후 출력은 조용히 무시된다. */
bool console_init(void);
bool console_ready(void);

void console_putc(char c);
void console_puts(const char *s);
void console_clear(void);
void console_set_color(u32 fg, u32 bg);

/* 임의 위치에 확대 텍스트를 그린다 (커서와 무관, 배경 투명).
 * scale=2 면 글자가 16x32 픽셀이 된다. 제목용. */
void console_draw_text_at(u32 px, u32 py, const char *s, u32 scale, u32 fg);

/* 커널 전체의 문자 출력 지점. UART 와 화면 양쪽으로 나간다.
 * printf 가 이걸 쓴다. */
void kputc(char c);

#endif /* _CONSOLE_H */
