/* font.h - 8x16 비트맵 콘솔 폰트.
 *
 * 베어메탈에는 폰트 렌더러가 없다. 글리프가 이미 비트맵이어야 화면에
 * 찍을 수 있다. tools/mkfont.py 가 TTF 에서 미리 구워 넣는다. */
#ifndef _FONT_H
#define _FONT_H

#include "types.h"

#define FONT_WIDTH        8
#define FONT_HEIGHT       16
#define FONT_FIRST_CHAR   0x20      /* 공백 */
#define FONT_LAST_CHAR    0x7E      /* ~ */
#define FONT_GLYPH_COUNT  (FONT_LAST_CHAR - FONT_FIRST_CHAR + 1)

/* [글자][행]. 한 바이트가 한 행이고 MSB 가 왼쪽 픽셀. */
extern const u8 font_8x16[FONT_GLYPH_COUNT][FONT_HEIGHT];

/* 출력 가능 범위 밖은 '?' 글리프로 대체해 돌려준다. */
static inline const u8 *font_glyph(char c)
{
    u8 u = (u8)c;
    if (u < FONT_FIRST_CHAR || u > FONT_LAST_CHAR)
        u = '?';
    return font_8x16[u - FONT_FIRST_CHAR];
}

#endif /* _FONT_H */
