/* console.c - 프레임버퍼 텍스트 콘솔.
 *
 * 폰트 렌더링이라고 해봐야 비트맵을 픽셀로 펼치는 것이 전부다.
 * 라이브러리도 드라이버 스택도 없다. */
#include "console.h"
#include "fb.h"
#include "font.h"
#include "uart.h"
#include "string.h"

/* 화면 가장자리 여백 (픽셀) */
#define MARGIN_X  8
#define MARGIN_Y  8

static struct {
    bool ready;
    u32  cols, rows;    /* 글자 단위 화면 크기 */
    u32  cx, cy;        /* 커서 위치 (글자 단위) */
    u32  fg, bg;
} con;

bool console_ready(void) { return con.ready; }

bool console_init(void)
{
    con.ready = false;

    if (!fb.ready)
        return false;

    if (fb.width  < MARGIN_X * 2 + FONT_WIDTH ||
        fb.height < MARGIN_Y * 2 + FONT_HEIGHT)
        return false;

    con.cols = (fb.width  - MARGIN_X * 2) / FONT_WIDTH;
    con.rows = (fb.height - MARGIN_Y * 2) / FONT_HEIGHT;
    con.cx = con.cy = 0;

    con.fg = fb_rgb(0xC8, 0xD0, 0xD8);   /* 밝은 회색 */
    con.bg = fb_rgb(0x0A, 0x0E, 0x14);   /* 거의 검정 */

    con.ready = true;
    fb_clear(con.bg);
    return true;
}

void console_set_color(u32 fg, u32 bg)
{
    con.fg = fg;
    con.bg = bg;
}

void console_clear(void)
{
    if (!con.ready)
        return;
    fb_clear(con.bg);
    con.cx = con.cy = 0;
}

/* 글자 하나를 (col,row) 위치에 찍는다. */
static void draw_glyph(char c, u32 col, u32 row)
{
    const u8 *g = font_glyph(c);
    u32 px = MARGIN_X + col * FONT_WIDTH;
    u32 py = MARGIN_Y + row * FONT_HEIGHT;
    u32 stride = fb.pitch / 4;

    for (u32 y = 0; y < FONT_HEIGHT; y++) {
        u8   bits = g[y];
        u32 *line = &fb.pixels[(py + y) * stride + px];
        for (u32 x = 0; x < FONT_WIDTH; x++)
            line[x] = (bits & (0x80u >> x)) ? con.fg : con.bg;
    }
}

/* 한 줄 위로 밀어 올린다. */
static void scroll_up(void)
{
    u32 stride    = fb.pitch / 4;
    u32 line_px   = FONT_HEIGHT * stride;          /* 한 텍스트 줄의 워드 수 */
    u32 top       = MARGIN_Y * stride;
    u32 used_rows = con.rows * FONT_HEIGHT;

    /* 본문을 한 줄 크기만큼 위로 복사 */
    memmove(&fb.pixels[top],
            &fb.pixels[top + line_px],
            (used_rows - FONT_HEIGHT) * stride * sizeof(u32));

    /* 마지막 줄 지우기 */
    fb_fill_rect(0, MARGIN_Y + (con.rows - 1) * FONT_HEIGHT,
                 fb.width, FONT_HEIGHT, con.bg);
}

static void newline(void)
{
    con.cx = 0;
    if (++con.cy >= con.rows) {
        con.cy = con.rows - 1;
        scroll_up();
    }
}

void console_putc(char c)
{
    if (!con.ready)
        return;

    switch (c) {
    case '\n':
        newline();
        return;
    case '\r':
        con.cx = 0;
        return;
    case '\t':
        /* 8칸 탭 정렬 */
        do { console_putc(' '); } while (con.cx % 8);
        return;
    case '\b':
        if (con.cx) {
            con.cx--;
            draw_glyph(' ', con.cx, con.cy);
        }
        return;
    default:
        break;
    }

    if (con.cx >= con.cols)
        newline();

    draw_glyph(c, con.cx, con.cy);
    con.cx++;
}

void console_puts(const char *s)
{
    while (*s)
        console_putc(*s++);
}

void console_draw_text_at(u32 px, u32 py, const char *s, u32 scale, u32 fg)
{
    if (!fb.ready || scale == 0)
        return;

    for (u32 n = 0; s[n]; n++) {
        const u8 *g = font_glyph(s[n]);
        u32 gx = px + n * FONT_WIDTH * scale;

        for (u32 y = 0; y < FONT_HEIGHT; y++) {
            u8 bits = g[y];
            for (u32 x = 0; x < FONT_WIDTH; x++) {
                if (!(bits & (0x80u >> x)))
                    continue;   /* 배경은 건드리지 않는다 (투명) */
                /* 픽셀 하나를 scale x scale 블록으로 확대 */
                fb_fill_rect(gx + x * scale, py + y * scale, scale, scale, fg);
            }
        }
    }
}

/* 커널의 단일 문자 출력 지점.
 * 시리얼은 항상, 화면은 준비됐을 때만. 덕분에 부팅 아주 초반의
 * 메시지도 잃지 않고, 화면이 올라온 뒤에는 양쪽에 다 나온다. */
void kputc(char c)
{
    uart_putc(c);
    console_putc(c);
}
