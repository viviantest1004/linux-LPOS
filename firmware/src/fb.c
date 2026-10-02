/* fb.c - 메일박스를 통한 프레임버퍼 할당과 기본 그리기. */
#include "fb.h"
#include "mbox.h"
#include "mmio.h"
#include "string.h"

framebuffer_t fb;

/* 프레임버퍼 관련 태그 */
#define TAG_FB_GET_PHYSICAL_WH  0x00040003
#define TAG_FB_ALLOCATE_BUFFER  0x00040001
#define TAG_FB_GET_PITCH        0x00040008
#define TAG_FB_SET_PHYSICAL_WH  0x00048003
#define TAG_FB_SET_VIRTUAL_WH   0x00048004
#define TAG_FB_SET_DEPTH        0x00048005
#define TAG_FB_SET_PIXEL_ORDER  0x00048006
#define TAG_FB_SET_VIRTUAL_OFF  0x00048009

#define FB_DEPTH        32
#define FB_PIXEL_ORDER  1       /* 1 = RGB */

/* 모니터가 없을 때 쓸 기본 해상도 */
#define FB_FALLBACK_W   800
#define FB_FALLBACK_H   600

/* GPU 가 돌려주는 주소는 VideoCore 버스 주소다. 상위 별칭 비트를
 * 떼어내야 ARM 이 쓸 수 있는 물리 주소가 된다. */
#define BUS_TO_PHYS(a)  ((a) & 0x3FFFFFFFu)

u32 fb_rgb(u8 r, u8 g, u8 b)
{
    /* 리틀엔디언 u32 에 바이트를 어떻게 배치할지는 pixel_order 가 정한다.
     *   order 0 (BGR): 메모리 바이트 = B,G,R,A  ->  u32 = (r<<16)|(g<<8)|b
     *   order 1 (RGB): 메모리 바이트 = R,G,B,A  ->  u32 = (b<<16)|(g<<8)|r  */
    if (fb.pixel_order == 0)
        return ((u32)r << 16) | ((u32)g << 8) | (u32)b;
    return ((u32)b << 16) | ((u32)g << 8) | (u32)r;
}

bool fb_init(u32 width, u32 height)
{
    fb.ready = false;

    /* 해상도를 안 정해줬으면 현재 디스플레이 크기를 물어본다 */
    if (width == 0 || height == 0) {
        u32 wh[2] = { 0, 0 };
        if (mbox_prop(TAG_FB_GET_PHYSICAL_WH, NULL, 0, wh, 2)
            && wh[0] && wh[1]) {
            width  = wh[0];
            height = wh[1];
        } else {
            width  = FB_FALLBACK_W;
            height = FB_FALLBACK_H;
        }
    }

    /* 프레임버퍼 설정은 반드시 한 메시지 안에서 원자적으로 해야 한다.
     * 크기와 깊이를 따로 보내면 그 사이에 GPU 가 버퍼를 재할당해버린다. */
    u32 msg[36];
    u32 i = 0;

    msg[i++] = 0;                       /* 크기는 마지막에 채운다 */
    msg[i++] = 0;                       /* 요청 코드 */

    msg[i++] = TAG_FB_SET_PHYSICAL_WH;
    msg[i++] = 8; msg[i++] = 8;
    u32 off_phys = i;
    msg[i++] = width;  msg[i++] = height;

    msg[i++] = TAG_FB_SET_VIRTUAL_WH;
    msg[i++] = 8; msg[i++] = 8;
    msg[i++] = width;  msg[i++] = height;

    msg[i++] = TAG_FB_SET_VIRTUAL_OFF;
    msg[i++] = 8; msg[i++] = 8;
    msg[i++] = 0;  msg[i++] = 0;

    msg[i++] = TAG_FB_SET_DEPTH;
    msg[i++] = 4; msg[i++] = 4;
    u32 off_depth = i;
    msg[i++] = FB_DEPTH;

    msg[i++] = TAG_FB_SET_PIXEL_ORDER;
    msg[i++] = 4; msg[i++] = 4;
    u32 off_order = i;
    msg[i++] = FB_PIXEL_ORDER;

    msg[i++] = TAG_FB_ALLOCATE_BUFFER;
    msg[i++] = 8; msg[i++] = 8;
    u32 off_alloc = i;
    msg[i++] = 4096;                    /* 정렬 요청 */
    msg[i++] = 0;                       /* 여기에 크기가 돌아온다 */

    msg[i++] = TAG_FB_GET_PITCH;
    msg[i++] = 4; msg[i++] = 4;
    u32 off_pitch = i;
    msg[i++] = 0;

    msg[i++] = 0;                       /* 끝 태그 */
    msg[0] = i * 4;

    if (!mbox_send(msg, i))
        return false;

    u32 base = msg[off_alloc];
    if (base == 0)
        return false;

    fb.width       = msg[off_phys];
    fb.height      = msg[off_phys + 1];
    fb.depth       = msg[off_depth];
    fb.pixel_order = msg[off_order];
    fb.size        = msg[off_alloc + 1];
    fb.pitch       = msg[off_pitch];
    fb.pixels      = (u32 *)(uptr)BUS_TO_PHYS(base);

    /* 32bpp 가 아니면 우리 그리기 코드가 맞지 않는다 */
    if (fb.depth != 32 || fb.pitch == 0 || fb.width == 0 || fb.height == 0)
        return false;

    fb.ready = true;
    return true;
}

void fb_put_pixel(u32 x, u32 y, u32 color)
{
    if (!fb.ready || x >= fb.width || y >= fb.height)
        return;
    /* pitch 는 바이트 단위라 4로 나눠 워드 인덱스로 쓴다 */
    fb.pixels[y * (fb.pitch / 4) + x] = color;
}

void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color)
{
    if (!fb.ready)
        return;

    if (x >= fb.width || y >= fb.height)
        return;
    if (x + w > fb.width)  w = fb.width - x;
    if (y + h > fb.height) h = fb.height - y;

    u32 stride = fb.pitch / 4;
    for (u32 row = 0; row < h; row++) {
        u32 *p = &fb.pixels[(y + row) * stride + x];
        for (u32 col = 0; col < w; col++)
            p[col] = color;
    }
}

void fb_clear(u32 color)
{
    if (!fb.ready)
        return;
    fb_fill_rect(0, 0, fb.width, fb.height, color);
}
