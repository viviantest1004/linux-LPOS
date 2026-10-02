/* lp-ui.h - the drawing both recovery screens share: the UEFI boot menu
 * (boot/efi/lpboot.c) and the recovery menu (recovery/lp-recovery.c).
 *
 * Two programs, two worlds: one runs inside the firmware with no
 * operating system, the other on Linux with our own libc and a
 * framebuffer. What they have in common is a block of 32-bit pixels and
 * the need to look like the same product as the desktop and the boot
 * splash. So this header draws into memory and nothing else - no
 * allocation, no system calls, no libc - and each program supplies the
 * memory and puts the result on the screen its own way (GOP Blt in the
 * firmware, the framebuffer on Linux).
 *
 * The includer defines u8/u16/u32/s16/s32/s64/u64 first (our libc's
 * types.h does; lpboot.c does it itself).
 *
 * ── What is here ──
 *   - the desktop's deep-blue gradient, from the same table the boot
 *     splash uses (userland/splash/logo.h), ordered-dithered so a dark
 *     gradient across 3840 pixels has no contour lines;
 *   - the LP mark from desktop/branding/c/lp-logo-alpha.h, shrunk with an
 *     area filter to whatever size is asked;
 *   - rounded rectangles, filled or stroked, with anti-aliased corners;
 *   - text from lp-glyphs.h: three faces rendered at their 4K size and
 *     shrunk with an area filter on a smaller screen (never grown - see
 *     mkglyphs.py), with measuring and word wrapping, Hangul included;
 *   - a spring (COMMON.md "Motion"): the menu highlight is interruptible
 *     because it carries a velocity, not a timeline;
 *   - a %d/%s formatter, because the firmware has no snprintf.
 *
 * Everything is integer arithmetic. The firmware allows SSE and our libc
 * could use floats too, but the splash set the house rule (no floating
 * point in the boot path) and fixed point is exact enough for pixels.
 *
 * Pixels are 0x00RRGGBB in a u32. That is the byte order of the UEFI Blt
 * pixel (blue, green, red, reserved) and of the usual Linux XRGB8888
 * framebuffer, so the common case copies without converting.
 */
#ifndef LP_UI_H
#define LP_UI_H

#include "../../userland/splash/logo.h"
#include "../../desktop/branding/c/lp-logo-alpha.h"
#include "lp-glyphs.h"

#define LPUI static inline

/* ── Design tokens ────────────────────────────────────────────────── */
#define LPUI_INK        0xffffffu   /* text on the gradient */
#define LPUI_INK2       0xdbe8f0u   /* secondary text: the wordmark's tint */
#define LPUI_ACCENT     0xf28c28u   /* the mark's orange: focus, progress */
#define LPUI_DANGER     0xff5a5au   /* destructive choices, errors */
#define LPUI_CARD_A     26          /* card fill: white at ~10% */
#define LPUI_LINE_A     44          /* card hairline: white at ~17% */
#define LPUI_SEL_A      46          /* selected card fill: white at ~18% */

typedef struct {
    u32 *px;
    int  w, h, stride;          /* stride in pixels */
} lpui_canvas_t;

/* ── Scale ────────────────────────────────────────────────────────────
 * Layout is written in pixels of the owner's 3840x2160 panel and scaled
 * by the smaller of the two ratios, so a 16:10 VM window and a portrait
 * tablet both keep every element on the screen. */
/* The screen scale and the language are the program's, not a source
 * file's: lp-recovery is several files, and each would otherwise have
 * its own copy of a static and draw at the wrong size or in the wrong
 * language (recovery.h defines LPUI_SHARED_STATE and lp-recovery.c owns
 * the variables). The boot menu is one file and keeps them static. */
#ifdef LPUI_SHARED_STATE
extern int lpui_permille;
extern bool lpui_korean;
#else
static int lpui_permille = 1000;
static bool lpui_korean;
#endif

LPUI void lpui_set_screen(int w, int h)
{
    int a = w * 1000 / 3840, b = h * 1000 / 2160;
    lpui_permille = a < b ? a : b;
    if (lpui_permille < 150)
        lpui_permille = 150;
}

/* 4K pixels to this screen's pixels, never below 1 for a non-zero size. */
LPUI int lpui_px(int v4k)
{
    int v = (v4k * lpui_permille + 500) / 1000;
    return (v == 0 && v4k > 0) ? 1 : v;
}

/* A text size for this screen: scaled, but with a floor, because an
 * 11-pixel line of Hangul in a VM window is unreadable however correct
 * the arithmetic is. */
LPUI int lpui_text_px(int v4k)
{
    int v = lpui_px(v4k);
    int floor = v4k >= 90 ? 30 : v4k >= 56 ? 20 : 15;
    return v < floor ? floor : v;
}

/* ── Small helpers ────────────────────────────────────────────────── */
LPUI u64 lpui_isqrt(u64 n)
{
    u64 r = 0, bit = (u64)1 << 62;
    while (bit > n)
        bit >>= 2;
    while (bit) {
        if (n >= r + bit) {
            n -= r + bit;
            r = (r >> 1) + bit;
        } else
            r >>= 1;
        bit >>= 2;
    }
    return r;
}

LPUI u32 lpui_mix(u32 dst, u32 src, u32 a /* 0..256 */)
{
    if (a >= 256)
        return src;
    if (a == 0)
        return dst;
    u32 rb = dst & 0xff00ffu, g = dst & 0x00ff00u;
    u32 srb = src & 0xff00ffu, sg = src & 0x00ff00u;
    rb = (rb + (((srb - rb) * a) >> 8)) & 0xff00ffu;
    g = (g + (((sg - g) * a) >> 8)) & 0x00ff00u;
    return rb | g;
}

LPUI void lpui_copy_rect(lpui_canvas_t *dst, const lpui_canvas_t *src,
                         int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > dst->w) w = dst->w - x;
    if (y + h > dst->h) h = dst->h - y;
    for (int j = 0; j < h; j++) {
        u32 *d = dst->px + (u64)(y + j) * dst->stride + x;
        const u32 *s = src->px + (u64)(y + j) * src->stride + x;
        for (int i = 0; i < w; i++)
            d[i] = s[i];
    }
}

/* The whole canvas at `a`/256 of its brightness: the fade in and out. */
LPUI void lpui_dim(lpui_canvas_t *dst, const lpui_canvas_t *src, u32 a)
{
    for (int y = 0; y < dst->h; y++) {
        u32 *d = dst->px + (u64)y * dst->stride;
        const u32 *s = src->px + (u64)y * src->stride;
        for (int x = 0; x < dst->w; x++) {
            u32 v = s[x];
            u32 rb = ((v & 0xff00ffu) * a >> 8) & 0xff00ffu;
            u32 g = ((v & 0x00ff00u) * a >> 8) & 0x00ff00u;
            d[x] = rb | g;
        }
    }
}

/* dst = a at weight wa/256 over b. */
LPUI void lpui_crossfade(lpui_canvas_t *dst, const lpui_canvas_t *a,
                         const lpui_canvas_t *b, u32 wa)
{
    for (int y = 0; y < dst->h; y++) {
        u32 *d = dst->px + (u64)y * dst->stride;
        const u32 *s = a->px + (u64)y * a->stride;
        const u32 *t = b->px + (u64)y * b->stride;
        for (int x = 0; x < dst->w; x++)
            d[x] = lpui_mix(t[x], s[x], wa);
    }
}

/* ── The gradient ─────────────────────────────────────────────────────
 * The dark wallpaper exactly: light from LP_FOCUS, colour by distance,
 * the same 65 samples the splash and the desktop use (logo.h), so boot
 * menu, splash and desktop are one continuous surface. A 4096-entry
 * table indexed by squared distance avoids a square root per pixel. */
#define LPUI_LUT_N 4096
static u16 lpui_lut[LPUI_LUT_N][3];
static const u8 LPUI_BAYER[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 },
};

LPUI void lpui_gradient(lpui_canvas_t *c)
{
    for (u32 i = 0; i < LPUI_LUT_N; i++) {
        u64 ratio = ((u64)i << 32) / (LPUI_LUT_N - 1);
        if (ratio > 0xFFFFFFFFull)
            ratio = 0xFFFFFFFFull;
        u32 t = (u32)lpui_isqrt(ratio);            /* 0.16 */
        u32 pos = t * (LP_GRAD_N - 1);
        u32 k = pos >> 16, f = pos & 0xFFFF;
        if (k >= LP_GRAD_N - 1) { k = LP_GRAD_N - 2; f = 0xFFFF; }
        for (int ch = 0; ch < 3; ch++) {
            s32 a = LP_GRAD[k][ch], b = LP_GRAD[k + 1][ch];
            lpui_lut[i][ch] = (u16)(a + (s32)(((s64)(b - a) * f) >> 16));
        }
    }
    u32 fx = 4096 - LP_FOCUS_X, fy = 4096 - LP_FOCUS_Y;
    u64 recip = ((u64)(LPUI_LUT_N - 1) << 32) / (u64)(fx * fx + fy * fy);
    for (int y = 0; y < c->h; y++) {
        s32 uy = (s32)((2 * y + 1) * 2048 / c->h) - LP_FOCUS_Y;
        u32 rowsq = (u32)(uy * uy);
        u32 *row = c->px + (u64)y * c->stride;
        for (int x = 0; x < c->w; x++) {
            s32 ux = (s32)((2 * x + 1) * 2048 / c->w) - LP_FOCUS_X;
            u32 idx = (u32)(((u64)((u32)(ux * ux) + rowsq) * recip) >> 32);
            if (idx >= LPUI_LUT_N)
                idx = LPUI_LUT_N - 1;
            u32 thr = (u32)LPUI_BAYER[y & 7][x & 7] * 4;
            u32 r = (lpui_lut[idx][0] + thr) >> 8;
            u32 g = (lpui_lut[idx][1] + thr) >> 8;
            u32 b = (lpui_lut[idx][2] + thr) >> 8;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            row[x] = (r << 16) | (g << 8) | b;
        }
    }
}

/* ── Rectangles ───────────────────────────────────────────────────────
 * Edges sit on whole pixels, so only the corners need anti-aliasing:
 * coverage there is the distance from the pixel's centre to the corner
 * circle, in 1/16 px. */
LPUI u32 lpui_rr_cov(int px, int py, int x0, int y0, int x1, int y1, int r)
{
    if (px < x0 || px >= x1 || py < y0 || py >= y1)
        return 0;
    if (r <= 0)
        return 256;
    s32 cx = px * 16 + 8, cy = py * 16 + 8;
    s32 dx = 0, dy = 0;
    if (cx < (x0 + r) * 16) dx = (x0 + r) * 16 - cx;
    else if (cx > (x1 - r) * 16) dx = cx - (x1 - r) * 16;
    if (cy < (y0 + r) * 16) dy = (y0 + r) * 16 - cy;
    else if (cy > (y1 - r) * 16) dy = cy - (y1 - r) * 16;
    if (dx == 0 || dy == 0) {
        s32 d = dx > dy ? dx : dy;
        s32 cov = r * 16 - d + 8;
        return cov >= 16 ? 256 : cov <= 0 ? 0 : (u32)cov * 16;
    }
    s32 d = (s32)lpui_isqrt((u64)((s64)dx * dx + (s64)dy * dy));
    s32 cov = r * 16 - d + 8;
    return cov >= 16 ? 256 : cov <= 0 ? 0 : (u32)cov * 16;
}

/* Filled rounded rectangle, rgb at alpha a (0..256). */
LPUI void lpui_rrect(lpui_canvas_t *c, int x, int y, int w, int h, int r,
                     u32 rgb, u32 a)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int ya = y < 0 ? 0 : y, yb = y + h > c->h ? c->h : y + h;
    int xa = x < 0 ? 0 : x, xb = x + w > c->w ? c->w : x + w;
    for (int py = ya; py < yb; py++) {
        u32 *row = c->px + (u64)py * c->stride;
        bool corner_row = py < y + r || py >= y + h - r;
        for (int px = xa; px < xb; px++) {
            u32 cov = 256;
            if (corner_row || px < x + r || px >= x + w - r)
                cov = lpui_rr_cov(px, py, x, y, x + w, y + h, r);
            if (cov)
                row[px] = lpui_mix(row[px], rgb, cov * a >> 8);
        }
    }
}

/* A rounded outline of thickness t, drawn inside the box. */
LPUI void lpui_rrect_stroke(lpui_canvas_t *c, int x, int y, int w, int h,
                            int r, int t, u32 rgb, u32 a)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int ri = r - t < 0 ? 0 : r - t;
    int ya = y < 0 ? 0 : y, yb = y + h > c->h ? c->h : y + h;
    int xa = x < 0 ? 0 : x, xb = x + w > c->w ? c->w : x + w;
    for (int py = ya; py < yb; py++) {
        u32 *row = c->px + (u64)py * c->stride;
        bool band = py < y + r + t || py >= y + h - r - t;
        for (int px = xa; px < xb; px++) {
            if (!band && px >= x + r + t && px < x + w - r - t)
                continue;           /* the hollow middle */
            u32 o = lpui_rr_cov(px, py, x, y, x + w, y + h, r);
            u32 i = lpui_rr_cov(px, py, x + t, y + t, x + w - t, y + h - t, ri);
            u32 cov = o > i ? o - i : 0;
            if (cov)
                row[px] = lpui_mix(row[px], rgb, cov * a >> 8);
        }
    }
}

/* ── The mark ─────────────────────────────────────────────────────────
 * lp-logo-alpha.h holds the ring and the L as run-length planes at 256
 * and 512 px. The caller lends two planes' worth of scratch (2 x 512 x
 * 512 bytes) since this header allocates nothing. The larger master is
 * used whenever the target is over 256, and both are only ever shrunk. */
#define LPUI_LOGO_SCRATCH (2u * 512u * 512u)

LPUI void lpui_logo(lpui_canvas_t *c, int cx, int cy, int size, u8 *scratch)
{
    int m = size > 256 ? 512 : 256;
    u8 *ring = scratch, *ink = scratch + 512 * 512;
    if (m == 512) {
        lp_logo_unpack(LP_LOGO_512_RING, LP_LOGO_512_RING_LEN, ring, 512 * 512);
        lp_logo_unpack(LP_LOGO_512_INK, LP_LOGO_512_INK_LEN, ink, 512 * 512);
    } else {
        lp_logo_unpack(LP_LOGO_256_RING, LP_LOGO_256_RING_LEN, ring, 256 * 256);
        lp_logo_unpack(LP_LOGO_256_INK, LP_LOGO_256_INK_LEN, ink, 256 * 256);
    }
    if (size > m)
        size = m;
    int x0 = cx - size / 2, y0 = cy - size / 2;
    for (int ty = 0; ty < size; ty++) {
        int py = y0 + ty;
        if (py < 0 || py >= c->h)
            continue;
        /* source rows [sa, sb) in 1/256 px */
        u32 sa = (u32)ty * m * 256 / size, sb = (u32)(ty + 1) * m * 256 / size;
        u32 *row = c->px + (u64)py * c->stride;
        for (int tx = 0; tx < size; tx++) {
            int px = x0 + tx;
            if (px < 0 || px >= c->w)
                continue;
            u32 ta = (u32)tx * m * 256 / size, tb = (u32)(tx + 1) * m * 256 / size;
            u64 sr = 0, si = 0;
            for (u32 j = sa >> 8; j < ((sb + 255) >> 8) && j < (u32)m; j++) {
                u32 lo = j * 256 > sa ? j * 256 : sa;
                u32 hi = (j + 1) * 256 < sb ? (j + 1) * 256 : sb;
                u32 wy = hi - lo;
                for (u32 i = ta >> 8; i < ((tb + 255) >> 8) && i < (u32)m; i++) {
                    u32 l2 = i * 256 > ta ? i * 256 : ta;
                    u32 h2 = (i + 1) * 256 < tb ? (i + 1) * 256 : tb;
                    u64 wgt = (u64)wy * (h2 - l2);
                    sr += wgt * ring[j * m + i];
                    si += wgt * ink[j * m + i];
                }
            }
            u64 area = (u64)(sb - sa) * (tb - ta);
            u32 ar = (u32)(sr * 256 / (area * 255));
            u32 ai = (u32)(si * 256 / (area * 255));
            u32 v = row[px];
            if (ar) v = lpui_mix(v, LP_LOGO_RGB_ACCENT, ar);
            if (ai) v = lpui_mix(v, LP_LOGO_RGB_INK, ai);
            row[px] = v;
        }
    }
}

/* ── UTF-8 ────────────────────────────────────────────────────────── */
LPUI u32 lpui_utf8(const char **sp)
{
    const u8 *s = (const u8 *)*sp;
    u32 c = s[0];
    int n = 0;
    if (c < 0x80) n = 1;
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 3; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 4; }
    else { *sp += 1; return 0xFFFD; }
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *sp += i; return 0xFFFD; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *sp += n;
    return c;
}

/* ── Text ─────────────────────────────────────────────────────────── */
LPUI const lpg_glyph_t *lpui_glyph(const lpg_face_t *f, u32 cp)
{
    int lo = 0, hi = f->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->g[mid].cp == cp)
            return &f->g[mid];
        if (f->g[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

/* Advance of one code point at `px`, in 1/64 px. */
LPUI s32 lpui_adv64(const lpg_face_t *f, int px, u32 cp)
{
    const lpg_glyph_t *g = lpui_glyph(f, cp);
    s32 adv = g ? g->adv64 : f->px * 32;
    return (s32)((s64)adv * px / f->px);
}

LPUI int lpui_text_width_n(int face, int px, const char *s, int nbytes)
{
    const lpg_face_t *f = &LPG_FACES[face];
    const char *end = s + nbytes;
    s64 pen = 0;
    while (s < end && *s)
        pen += lpui_adv64(f, px, lpui_utf8(&s));
    return (int)((pen + 63) / 64);
}

LPUI int lpui_text_width(int face, int px, const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return lpui_text_width_n(face, px, s, n);
}

LPUI int lpui_ascent(int face, int px)  { return LPG_FACES[face].ascent * px / LPG_FACES[face].px; }
LPUI int lpui_line(int face, int px)    { return LPG_FACES[face].line * px / LPG_FACES[face].px; }

/* One glyph with its pen at (pen64 in 1/64 px, baseline by). The source
 * bitmap is the glyph at the face's own size; each target pixel takes the
 * area-weighted mean of the source pixels under it. */
LPUI void lpui_glyph_draw(lpui_canvas_t *c, const lpg_face_t *f, const lpg_glyph_t *g,
                          int px, s64 pen64, int by, u32 rgb, u32 alpha)
{
    if (!g->w || !g->h)
        return;
    int P = f->px;
    int rowb = (g->w + 1) / 2;
    const u8 *bits = f->data + g->off;
    /* The glyph's box in target pixels relative to the pen's whole pixel. */
    s64 penx = pen64 >> 6;
    s32 frac = (s32)(pen64 & 63);           /* sub-pixel start, 1/64 */
    /* Q8 source coordinates: target t maps to source (t*256 - frac*4) * P / px. */
    s32 gx0 = g->x0 * 256, gy0 = g->y0 * 256;
    int tx0 = (int)((((s64)g->x0 * px * 256 / P) + frac * 4) >> 8) - 1;
    int tx1 = (int)((((s64)(g->x0 + g->w) * px * 256 / P) + frac * 4) >> 8) + 2;
    int ty0 = (int)(((s64)g->y0 * px) / P) - 1;
    int ty1 = (int)(((s64)(g->y0 + g->h) * px) / P) + 2;
    for (int ty = ty0; ty < ty1; ty++) {
        int py = by + ty;
        if (py < 0 || py >= c->h)
            continue;
        s32 sa = (s32)((s64)ty * P * 256 / px) - gy0;
        s32 sb = (s32)((s64)(ty + 1) * P * 256 / px) - gy0;
        s32 ya = sa < 0 ? 0 : sa, yb = sb > g->h * 256 ? g->h * 256 : sb;
        if (yb <= ya)
            continue;
        u32 *row = c->px + (u64)py * c->stride;
        for (int tx = tx0; tx < tx1; tx++) {
            s64 pxl = penx + tx;
            if (pxl < 0 || pxl >= c->w)
                continue;
            s32 xa0 = (s32)(((s64)tx * 256 - frac * 4) * P / px) - gx0;
            s32 xb0 = (s32)(((s64)(tx + 1) * 256 - frac * 4) * P / px) - gx0;
            s32 xa = xa0 < 0 ? 0 : xa0, xb = xb0 > g->w * 256 ? g->w * 256 : xb0;
            if (xb <= xa)
                continue;
            u64 sum = 0;
            for (s32 j = ya >> 8; j < (yb + 255) >> 8; j++) {
                s32 lo = j * 256 > ya ? j * 256 : ya;
                s32 hi = (j + 1) * 256 < yb ? (j + 1) * 256 : yb;
                const u8 *srow = bits + j * rowb;
                for (s32 i = xa >> 8; i < (xb + 255) >> 8; i++) {
                    s32 l2 = i * 256 > xa ? i * 256 : xa;
                    s32 h2 = (i + 1) * 256 < xb ? (i + 1) * 256 : xb;
                    u32 a = (srow[i >> 1] >> ((i & 1) * 4)) & 15;
                    sum += (u64)a * (u32)(hi - lo) * (u32)(h2 - l2);
                }
            }
            u64 area = (u64)(sb - sa) * (u64)(xb0 - xa0);
            u32 cov = (u32)(sum * 256 / (area * 15));
            if (cov > 256)
                cov = 256;
            cov = cov * alpha >> 8;
            if (cov)
                row[pxl] = lpui_mix(row[pxl], rgb, cov);
        }
    }
}

/* Draw `nbytes` of s with its pen starting at x on baseline y. Returns
 * the width drawn. */
LPUI int lpui_text_n(lpui_canvas_t *c, int face, int px, int x, int y,
                     u32 rgb, u32 alpha, const char *s, int nbytes)
{
    const lpg_face_t *f = &LPG_FACES[face];
    const char *end = s + nbytes;
    s64 pen = (s64)x * 64;
    while (s < end && *s) {
        u32 cp = lpui_utf8(&s);
        const lpg_glyph_t *g = lpui_glyph(f, cp);
        if (g)
            lpui_glyph_draw(c, f, g, px, pen, y, rgb, alpha);
        pen += lpui_adv64(f, px, cp);
    }
    return (int)((pen + 63) / 64) - x;
}

LPUI int lpui_text(lpui_canvas_t *c, int face, int px, int x, int y,
                   u32 rgb, u32 alpha, const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return lpui_text_n(c, face, px, x, y, rgb, alpha, s, n);
}

LPUI void lpui_text_center(lpui_canvas_t *c, int face, int px, int cx, int y,
                           u32 rgb, u32 alpha, const char *s)
{
    lpui_text(c, face, px, cx - lpui_text_width(face, px, s) / 2, y, rgb, alpha, s);
}

/* Word-wrapped text in a column `maxw` wide, first baseline at y. Breaks
 * at spaces (Korean is spaced too); a word longer than the column is
 * broken between characters. `c` may be NULL to only measure. Returns
 * the number of lines. */
LPUI int lpui_text_wrap(lpui_canvas_t *c, int face, int px, int x, int y,
                        int maxw, u32 rgb, u32 alpha, bool center, const char *s)
{
    int lines = 0, lh = lpui_line(face, px);
    while (*s) {
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        /* Take words while they fit. */
        int n = 0, best = 0;
        for (;;) {
            int e = n;
            while (s[e] == ' ')
                e++;
            while (s[e] && s[e] != ' ' && s[e] != '\n')
                e++;
            if (lpui_text_width_n(face, px, s, e) > maxw && best > 0)
                break;
            best = e;
            n = e;
            if (!s[e] || s[e] == '\n')
                break;
        }
        if (lpui_text_width_n(face, px, s, best) > maxw) {
            /* One word wider than the column: cut it between characters. */
            const char *p = s;
            int cut = 0;
            while (p < s + best) {
                const char *q = p;
                lpui_utf8(&q);
                if (lpui_text_width_n(face, px, s, (int)(q - s)) > maxw && cut > 0)
                    break;
                cut = (int)(q - s);
                p = q;
            }
            best = cut;
        }
        if (c) {
            int w = lpui_text_width_n(face, px, s, best);
            lpui_text_n(c, face, px, center ? x + (maxw - w) / 2 : x,
                        y + lines * lh, rgb, alpha, s, best);
        }
        lines++;
        s += best;
        if (*s == '\n')
            s++;
    }
    return lines;
}

/* ── The spring (COMMON.md "Motion", design/feel.md 2) ─────────────────
 * a = -k(x - target) - c v, stepped 1 ms at a time. Position is 16.16
 * (1.0 = 65536), velocity in the same units per second. Changing the
 * target mid-flight keeps position AND velocity - which is the whole
 * reason for a spring over a timeline. k and c come from feel.md's
 * table; c is given in tenths. */
typedef struct {
    s64 x, v, target;
    s32 k, c10;
} lpui_spring_t;

#define LPUI_SPRING_MENU   1599, 736     /* 180 ms, zeta 0.92 */
#define LPUI_SPRING_SHEET   856, 556     /* 260 ms, zeta 0.95 */
#define LPUI_SPRING_PRESS  5005, 1203    /* 130 ms, zeta 0.85 */

LPUI void lpui_spring_init(lpui_spring_t *s, s32 k, s32 c10, s64 at)
{
    s->x = s->target = at;
    s->v = 0;
    s->k = k;
    s->c10 = c10;
}

LPUI bool lpui_spring_resting(const lpui_spring_t *s)
{
    s64 d = s->x - s->target, v = s->v;
    return d > -64 && d < 64 && v > -2000 && v < 2000;
}

/* Advance by ms milliseconds. Returns true while still moving. */
LPUI bool lpui_spring_step(lpui_spring_t *s, int ms)
{
    if (ms > 64)
        ms = 64;                /* a stall is not a reason to jump */
    for (int i = 0; i < ms; i++) {
        s64 a = -(s64)s->k * (s->x - s->target) - (s64)s->c10 * s->v / 10;
        s->v += a / 1000;
        s->x += s->v / 1000;
    }
    if (lpui_spring_resting(s)) {
        s->x = s->target;
        s->v = 0;
        return false;
    }
    return true;
}

/* ── %d and %s ─────────────────────────────────────────────────────── */
#include <stdarg.h>

LPUI int lpui_vfmt(char *out, int n, const char *fmt, va_list ap)
{
    int o = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p == '%' && p[1] == 'd') {
            long v = va_arg(ap, int);
            char tmp[24];
            int t = 0;
            bool neg = v < 0;
            unsigned long u = neg ? (unsigned long)-v : (unsigned long)v;
            do { tmp[t++] = (char)('0' + u % 10); u /= 10; } while (u);
            if (neg && o < n - 1) out[o++] = '-';
            while (t && o < n - 1) out[o++] = tmp[--t];
            p++;
        } else if (*p == '%' && p[1] == 's') {
            const char *s = va_arg(ap, const char *);
            while (s && *s && o < n - 1) out[o++] = *s++;
            p++;
        } else if (*p == '%' && p[1] == '%') {
            if (o < n - 1) out[o++] = '%';
            p++;
        } else if (o < n - 1)
            out[o++] = *p;
    }
    if (n > 0)
        out[o] = 0;
    return o;
}

LPUI int lpui_fmt(char *out, int n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = lpui_vfmt(out, n, fmt, ap);
    va_end(ap);
    return r;
}

/* ── The string catalog ───────────────────────────────────────────── */
enum {
#define LPS(id, cls, en, ko) LPS_##id,
#include "lp-strings.h"
#undef LPS
    LPS_COUNT
};

static const char *const LPUI_EN[LPS_COUNT] = {
#define LPS(id, cls, en, ko) en,
#include "lp-strings.h"
#undef LPS
};
static const char *const LPUI_KO[LPS_COUNT] = {
#define LPS(id, cls, en, ko) ko,
#include "lp-strings.h"
#undef LPS
};

LPUI const char *lpui_s(int id) { return (lpui_korean ? LPUI_KO : LPUI_EN)[id]; }

#endif
