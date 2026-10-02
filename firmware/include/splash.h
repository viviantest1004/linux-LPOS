/* splash.h - 부팅할 때 화면에 뜨는 글씨.
 *
 * ★ 여기만 고치면 부팅 화면 문구가 바뀐다. ★
 *
 * 폰트는 ASCII 0x20~0x7E 만 구워져 있다. 한글을 넣으려면
 * tools/mkfont.py 의 문자 범위를 넓혀서 다시 구워야 한다. */
#ifndef _SPLASH_H
#define _SPLASH_H

/* 큰 글씨로 나오는 제목.
 * 화면 폭에 맞춰 아래 SPLASH_TITLE_MAX_SCALE 부터 1배까지 자동으로
 * 줄어드니 길이를 신경 쓸 필요는 없다. */
#define SPLASH_TITLE     "test_a_123_LPzero2W_img"

/* 제목 확대 배율의 상한. 화면이 좁으면 자동으로 더 낮아진다. */
#define SPLASH_TITLE_MAX_SCALE  3

/* 제목 바로 아래 한 줄 */
#define SPLASH_SUBTITLE  "Raspberry Pi Zero 2 W  /  BCM2710A1"

/* 그 아래로 원하는 만큼. 마지막은 반드시 NULL. */
#define SPLASH_LINES { \
    "bare-metal firmware, built from scratch", \
    "", \
    NULL \
}

/* 스플래시를 몇 밀리초 보여준 뒤 부팅 로그로 넘어갈지.
 * 0 으로 두면 기다리지 않고 바로 로그가 뜬다 (부팅이 가장 빠름). */
#define SPLASH_DWELL_MS  2500

/* 화면 색 (R, G, B) */
#define SPLASH_COLOR_BG      0x0A, 0x0E, 0x14   /* 배경 */
#define SPLASH_COLOR_BAR     0x1E, 0x88, 0xE5   /* 상단 띠 */
#define SPLASH_COLOR_TITLE   0xFF, 0xFF, 0xFF   /* 제목 */
#define SPLASH_COLOR_TEXT    0x8A, 0x9A, 0xA8   /* 본문 */

#endif /* _SPLASH_H */
