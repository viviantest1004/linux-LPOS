/* fb.h - VideoCore 프레임버퍼.
 *
 * GPU 에게 메일박스로 "이 크기/깊이의 화면 버퍼를 달라"고 요청하면
 * ARM 이 직접 쓸 수 있는 메모리 주소를 돌려준다. 그 다음부터는
 * 그냥 배열에 픽셀 값을 쓰면 HDMI 로 나간다. 드라이버 스택이 없다. */
#ifndef _FB_H
#define _FB_H

#include "types.h"

typedef struct {
    u32   width;        /* 픽셀 */
    u32   height;
    u32   pitch;        /* 한 줄의 바이트 수. width*4 와 다를 수 있다 */
    u32   depth;        /* 비트/픽셀 (우리는 32 고정) */
    u32   pixel_order;  /* 0 = BGR, 1 = RGB */
    u32  *pixels;       /* ARM 물리 주소 */
    u32   size;         /* 버퍼 바이트 수 */
    bool  ready;
} framebuffer_t;

extern framebuffer_t fb;

/* 화면을 요청한다. w/h 가 0 이면 현재 디스플레이 해상도를 따라간다.
 * 모니터가 없거나 gpu_mem 이 부족하면 실패한다. */
bool fb_init(u32 width, u32 height);

/* 색 조합. 실제 바이트 순서는 GPU 가 알려준 pixel_order 에 맞춘다. */
u32  fb_rgb(u8 r, u8 g, u8 b);

void fb_clear(u32 color);
void fb_put_pixel(u32 x, u32 y, u32 color);
void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color);

#endif /* _FB_H */
