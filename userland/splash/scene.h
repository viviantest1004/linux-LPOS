/* scene.h - the boot screen's picture, as a function of the pixel.
 *
 * Two programs draw this picture and they must agree to the pixel:
 *
 *   splash                          (userland/splash/splash.c) on the
 *                                   framebuffer, from the first second of
 *                                   boot until the desktop takes over;
 *   lp-splash-fade                  (desktop/branding/lp-splash-fade) as
 *                                   the desktop session's first frame,
 *                                   which then fades away into the desktop.
 *
 * If the second one drew the logo half a pixel lower, or dithered the
 * gradient with a different pattern, the hand-off would show as a jump
 * or a shimmer at exactly the moment the boot is meant to look finished.
 * So neither has its own copy: both include this file, and the same
 * integer arithmetic produces the same bytes on both sides.
 *
 * That is why it is a header of static inline functions (inline so that
 * a program using only some of them is not warned about the rest) with
 * no library calls at all. splash is built against our own libc (no floating point, no
 * malloc it could rely on at boot); lp-splash-fade against glibc and
 * GTK. The includer defines u8, s8, u16, s16, u32, s32, u64, s64 and
 * bool first, and nothing else is needed.
 *
 * What the picture is: the dark wallpaper's gradient (logo.h carries its
 * colours, from desktop/branding/src/wallpaper.py), ordered-dithered so
 * it does not band; the LP mark and the system's name, anti-aliased from
 * the distance to their outlines (see splash.c for why shapes and not a
 * bitmap); and, while the system is still starting, a small spinner
 * under them. Everything here is layout and colour; time is the caller's.
 *
 * Positions are in 1/32 of a pixel (SUB), colours in 8.8 fixed point,
 * alpha in 0..256.
 */
#ifndef LP_SPLASH_SCENE_H
#define LP_SPLASH_SCENE_H

#include "logo.h"

#define SUB          32             /* sub-pixel units per pixel */
#define MAXPRIM      96
#define SCENE_MAX_W  8192           /* the widest screen we lay out */
#define LUT_N        4096

enum { G_RING, G_ELL, G_WORD, G_DOT, G_COUNT };

typedef struct {
    u8  kind, quads, group;
    s32 ax, ay, bx, by;             /* SEG: the ends. ARC: centre in a, b unused */
    s32 r, hw;                      /* ARC radius; half the stroke width */
    s32 len;                        /* SEG: length of b - a */
    s32 e0x, e0y, e1x, e1y;         /* ARC: where an open arc ends */
    s32 x0, y0, x1, y1;             /* pixels this can touch, inclusive */
} placed_t;

typedef struct {
    u32      W, H;
    u8       plain;                 /* black, not the gradient: the firmware's logo is up (scene_init_oem) */
    s32      mark_h;                /* the mark's height in pixels */
    placed_t prims[MAXPRIM];
    int      nprims;
    s32      bx0, by0, bx1, by1;    /* every pixel the logo can touch */
    s32      sp_cx, sp_cy;          /* the spinner: centre, sub-pixels */
    s32      sp_r, sp_hw;           /* radius to the stroke's centre; half its width */
    s32      sx0, sy0, sx1, sy1;    /* every pixel the spinner can touch */
    u32      colsq[SCENE_MAX_W];    /* the gradient: squared x distance per column */
    u64      recip;                 /* ...and the scale from squared distance to lut */
    u16      lut[LUT_N][3];         /* the colour at the square root of each index */
} scene_t;

/* The spinner's look. A faint ring all the way round, so it reads as a
 * place where something is happening rather than a mark that moves; and
 * over it one bright stroke with a round head and a tail that fades out
 * over most of a turn. A fading tail is what makes rotation look calm:
 * a hard-ended arc chasing itself round reads as a warning light. */
#define SPIN_TRACK_A   36           /* the ring, alpha /256 (14%) */
#define SPIN_TAIL      51118        /* the tail's length, in 1/65536 turn (0.78) */

/* ── Integer helpers ──────────────────────────────────────────────── */

static inline u32 sc_isqrt32(u32 v)
{
    u32 r = 0, b = 1u << 30;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return r;
}

static inline s32 sc_iabs(s32 v) { return v < 0 ? -v : v; }

static inline s32 sc_clamp(s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ── The shapes, placed on this screen ────────────────────────────────
 * logo.h has them in design units; place() turns each into sub-pixel
 * coordinates for this screen once, with its pixel bounding box, so the
 * per-pixel work is only the distance itself. */

/* Where the start of an arc's run of quadrants is: the quadrant whose
 * clockwise neighbour is not in the run. */
static inline void sc_arc_ends(placed_t *p)
{
    int start = 0, count = 0;
    for (int q = 0; q < 4; q++)
        if (p->quads & (1u << q)) {
            count++;
            if (!(p->quads & (1u << ((q + 3) & 3))))
                start = q;
        }
    /* quadrant q starts at angle 90*q: east, north, west, south */
    static const s8 dx[4] = { 1, 0, -1, 0 }, dy[4] = { 0, -1, 0, 1 };
    int end = (start + count) & 3;
    p->e0x = p->ax + dx[start] * p->r;
    p->e0y = p->ay + dy[start] * p->r;
    p->e1x = p->ax + dx[end] * p->r;
    p->e1y = p->ay + dy[end] * p->r;
}

/* Add one primitive: design coordinates (1/LP_UNIT units) scaled by
 * num/den into sub-pixels and moved to (ox, oy) sub-pixels. */
static inline void sc_place(scene_t *s, const lp_prim_t *d, int group,
                     s32 ox, s32 oy, s32 num, s32 den)
{
    if (s->nprims >= MAXPRIM)
        return;
    placed_t *p = &s->prims[s->nprims++];
    p->kind  = d->kind;
    p->quads = d->quads;
    p->group = (u8)group;
    p->ax = ox + d->a * num / den;
    p->ay = oy + d->b * num / den;
    p->hw = d->w * num / den / 2;

    s32 lo_x, lo_y, hi_x, hi_y;
    if (d->kind == LP_SEG) {
        p->bx = ox + d->c * num / den;
        p->by = oy + d->d * num / den;
        s32 dx = p->bx - p->ax, dy = p->by - p->ay;
        p->len = (s32)sc_isqrt32((u32)(dx * dx + dy * dy));
        lo_x = p->ax < p->bx ? p->ax : p->bx;
        hi_x = p->ax < p->bx ? p->bx : p->ax;
        lo_y = p->ay < p->by ? p->ay : p->by;
        hi_y = p->ay < p->by ? p->by : p->ay;
        lo_x -= p->hw; hi_x += p->hw; lo_y -= p->hw; hi_y += p->hw;
    } else {
        p->r = d->c * num / den;
        sc_arc_ends(p);
        lo_x = p->ax - p->r - p->hw; hi_x = p->ax + p->r + p->hw;
        lo_y = p->ay - p->r - p->hw; hi_y = p->ay + p->r + p->hw;
    }
    /* one pixel of margin for the anti-aliased fringe */
    p->x0 = lo_x / SUB - 1; p->x1 = hi_x / SUB + 1;
    p->y0 = lo_y / SUB - 1; p->y1 = hi_y / SUB + 1;
}

/* Distance from (px, py) to the primitive's centreline, in sub-pixels. */
static inline s32 sc_distance(const placed_t *p, s32 px, s32 py)
{
    if (p->kind == LP_SEG) {
        s32 dx = p->bx - p->ax, dy = p->by - p->ay;
        s32 qx = px - p->ax, qy = py - p->ay;
        s32 dot = qx * dx + qy * dy;
        if (p->len > 0 && dot > 0 && dot < p->len * p->len)
            return sc_iabs(qx * dy - qy * dx) / p->len;     /* beside it */
        if (p->len > 0 && dot >= p->len * p->len) {         /* past the far end */
            qx = px - p->bx;
            qy = py - p->by;
        }
        return (s32)sc_isqrt32((u32)(qx * qx + qy * qy));   /* round end */
    }

    s32 dx = px - p->ax, dy = py - p->ay;
    int q = dy <= 0 ? (dx >= 0 ? 0 : 1) : (dx <= 0 ? 2 : 3);
    if (p->quads & (1u << q))
        return sc_iabs((s32)sc_isqrt32((u32)(dx * dx + dy * dy)) - p->r);
    /* outside the swept quadrants: the nearest point is a round end */
    s32 ax = px - p->e0x, ay = py - p->e0y, bx = px - p->e1x, by = py - p->e1y;
    u32 da = (u32)(ax * ax + ay * ay), db = (u32)(bx * bx + by * by);
    return (s32)sc_isqrt32(da < db ? da : db);
}

/* ── Layout ───────────────────────────────────────────────────────────
 * The mark is LP_LAYOUT_MARK_H% of the screen's height, so it is the same
 * size to the eye on every screen, but never more than MARK_MAXW% of the
 * width, for a portrait panel. Its centre sits a little above the middle
 * - a shape placed dead centre reads as slightly low - and the name
 * hangs under it. The spinner is further down, well clear of both, so
 * that when it goes the logo is left exactly as the desktop's first
 * frame draws it. */
/* The mark's height on this screen at `pct` per cent of its height. */
static inline s32 sc_mark_h(u32 W, u32 H, u32 pct)
{
    s32 mark_h = (s32)(H * pct / 100);
    if (mark_h > (s32)(W * LP_LAYOUT_MARK_MAXW / 100)) mark_h = (s32)(W * LP_LAYOUT_MARK_MAXW / 100);
    if (mark_h < LP_LAYOUT_MARK_MIN) mark_h = LP_LAYOUT_MARK_MIN;
    return mark_h;
}

static inline void sc_layout(scene_t *s, u32 W, u32 H, const char *name,
                             s32 mark_h, s32 cy, s32 spin_mark_h, s32 spin_cy);

static inline void scene_init(scene_t *s, u32 W, u32 H, const char *name)
{
    s32 mark_h = sc_mark_h(W, H, LP_LAYOUT_MARK_H);
    s->plain = 0;
    sc_layout(s, W, H, name, mark_h, (s32)H * SUB * LP_LAYOUT_CENTRE_Y / 100,
              mark_h, (s32)H * SUB * LP_LAYOUT_SPIN_Y / 100);
}

/* The screen when the firmware's own logo is up (ACPI BGRT - the
 * maker's logo a PC shows from power-on): that logo stays where it is,
 * on black, as Ubuntu leaves it; ours goes small near the bottom edge,
 * and the spinner sits between the two. `oem_bottom` is the row under
 * the firmware's logo, or -1 when it is not known. The spinner keeps the
 * size it has on the full-screen splash. */
static inline void scene_init_oem(scene_t *s, u32 W, u32 H, const char *name, s32 oem_bottom)
{
    s32 mark_h = sc_mark_h(W, H, LP_LAYOUT_OEM_MARK_H);
    s->plain = 1;
    /* the spinner's place depends on where the logo lands: lay out once
     * to find the logo's top, then again with the spinner there */
    sc_layout(s, W, H, name, mark_h, (s32)H * SUB * LP_LAYOUT_OEM_CENTRE_Y / 100,
              sc_mark_h(W, H, LP_LAYOUT_MARK_H), (s32)H * SUB * LP_LAYOUT_OEM_SPIN_Y / 100);
    if (oem_bottom >= 0 && oem_bottom < s->by0) {
        s32 mid = (oem_bottom + s->by0) / 2;
        sc_layout(s, W, H, name, mark_h, (s32)H * SUB * LP_LAYOUT_OEM_CENTRE_Y / 100,
                  sc_mark_h(W, H, LP_LAYOUT_MARK_H), mid * SUB);
    }
}

static inline void sc_layout(scene_t *s, u32 W, u32 H, const char *name,
                             s32 mark_h, s32 cy, s32 spin_mark_h, s32 spin_cy)
{
    s->W = W;
    s->H = H;
    s->nprims = 0;
    s->mark_h = mark_h;
    s32 cx = (s32)W * SUB / 2;

    s32 m_num = mark_h * SUB, m_den = LP_MARK_Y1 - LP_MARK_Y0;
    s32 m_ox = cx - (LP_MARK_X0 + LP_MARK_X1) * m_num / m_den / 2;
    s32 m_oy = cy - (LP_MARK_Y0 + LP_MARK_Y1) * m_num / m_den / 2;
    for (u32 i = 0; i < sizeof(LP_MARK) / sizeof(LP_MARK[0]); i++)
        sc_place(s, &LP_MARK[i], LP_MARK[i].accent ? G_RING : G_ELL, m_ox, m_oy, m_num, m_den);

    /* The name, in the wordmark's letters: first measured, then placed
     * centred. A letter the wordmark does not have is left as a gap the
     * width of an n rather than guessed at. */
    s32 w_num = mark_h * SUB * LP_LAYOUT_WORD_CAP / 100, w_den = LP_CAP;
    const lp_glyph_t *gl[64];
    int n = 0;
    s32 width = 0;
    for (const char *c = name; *c && n < 64; c++, n++) {
        gl[n] = 0;
        for (u32 k = 0; k < sizeof(LP_LETTERS) / sizeof(LP_LETTERS[0]); k++)
            if (LP_LETTERS[k].ch == *c)
                gl[n] = &LP_LETTERS[k];
        if (n && gl[n]) width += gl[n]->kern;
        width += (gl[n] ? gl[n]->adv : 4 * LP_UNIT) + LP_TRACK;
    }
    if (n)
        width -= LP_TRACK;
    s32 pen = cx - width * w_num / w_den / 2;
    s32 base = cy + mark_h * SUB / 2 + mark_h * SUB * LP_LAYOUT_WORD_GAP / 100;
    s32 x_units = 0;
    for (int i = 0; i < n; i++) {
        if (i && gl[i]) x_units += gl[i]->kern;
        if (gl[i])
            for (int k = 0; k < gl[i]->count; k++) {
                const lp_prim_t *p = &LP_LETTER_PRIMS[gl[i]->first + k];
                sc_place(s, p, p->accent ? G_DOT : G_WORD,
                         pen + x_units * w_num / w_den, base, w_num, w_den);
            }
        x_units += (gl[i] ? gl[i]->adv : 4 * LP_UNIT) + LP_TRACK;
    }

    /* the box every shape lives in: nothing outside it needs a look */
    s->bx0 = (s32)W; s->by0 = (s32)H; s->bx1 = -1; s->by1 = -1;
    for (int i = 0; i < s->nprims; i++) {
        const placed_t *p = &s->prims[i];
        if (p->x0 < s->bx0) s->bx0 = p->x0;
        if (p->y0 < s->by0) s->by0 = p->y0;
        if (p->x1 > s->bx1) s->bx1 = p->x1;
        if (p->y1 > s->by1) s->by1 = p->y1;
    }
    s->bx0 = sc_clamp(s->bx0, 0, (s32)W - 1); s->bx1 = sc_clamp(s->bx1, 0, (s32)W - 1);
    s->by0 = sc_clamp(s->by0, 0, (s32)H - 1); s->by1 = sc_clamp(s->by1, 0, (s32)H - 1);

    /* The spinner: sized from the mark, so it scales with it, but never
     * thinner than a pixel and a half - below that a turning stroke
     * flickers instead of moving. */
    s->sp_cx = cx;
    s->sp_cy = spin_cy;
    s->sp_r  = spin_mark_h * SUB * LP_LAYOUT_SPIN_R / 1000;
    s->sp_hw = spin_mark_h * SUB * LP_LAYOUT_SPIN_W / 1000 / 2;
    if (s->sp_hw < SUB * 3 / 4) s->sp_hw = SUB * 3 / 4;
    if (s->sp_r < s->sp_hw * 3) s->sp_r = s->sp_hw * 3;
    s32 reach = s->sp_r + s->sp_hw;
    s->sx0 = sc_clamp((s->sp_cx - reach) / SUB - 1, 0, (s32)W - 1);
    s->sx1 = sc_clamp((s->sp_cx + reach) / SUB + 1, 0, (s32)W - 1);
    s->sy0 = sc_clamp((s->sp_cy - reach) / SUB - 1, 0, (s32)H - 1);
    s->sy1 = sc_clamp((s->sp_cy + reach) / SUB + 1, 0, (s32)H - 1);

    /* ── The gradient ──
     * The colour depends only on the distance from the focus, so it is
     * looked up rather than computed: the table is indexed by the squared
     * distance (no square root per pixel) and each entry holds the colour
     * at the square root of its index, interpolated from logo.h's 65
     * samples. Positions are in 1/4096ths of the width and height,
     * measured from the focus, so the light stretches to the screen's
     * shape exactly as wallpaper.py's field() does. */
    for (u32 i = 0; i < LUT_N; i++) {
        /* t = sqrt(i / (LUT_N-1)) in 0.16 fixed point: the square root of
         * the ratio scaled by 2^32, which for the last entry is exactly
         * 2^32 and has to be held just under it to fit. */
        u64 ratio = ((u64)i << 32) / (LUT_N - 1);
        if (ratio > 0xFFFFFFFFull)
            ratio = 0xFFFFFFFFull;
        u32 t = sc_isqrt32((u32)ratio);
        u32 pos = t * (LP_GRAD_N - 1);          /* 16.16, 0 .. 64.0 */
        u32 k = pos >> 16, f = pos & 0xFFFF;
        if (k >= LP_GRAD_N - 1) { k = LP_GRAD_N - 2; f = 0xFFFF; }
        for (int c = 0; c < 3; c++) {
            s32 a = LP_GRAD[k][c], b = LP_GRAD[k + 1][c];
            s->lut[i][c] = (u16)(a + (s32)(((s64)(b - a) * f) >> 16));
        }
    }
    for (u32 x = 0; x < W && x < SCENE_MAX_W; x++) {
        s32 ux = (s32)((2 * x + 1) * 2048 / W) - LP_FOCUS_X;
        s->colsq[x] = (u32)(ux * ux);
    }
    u32 fx = 4096 - LP_FOCUS_X, fy = 4096 - LP_FOCUS_Y;
    s->recip = ((u64)(LUT_N - 1) << 32) / (u64)(fx * fx + fy * fy);
}

/* The background at a pixel, 8.8 per channel. The row's share of the
 * distance is worked out once per row (scene_rowsq) - it costs a
 * division, and a 4K logo box is half a million pixels a fade. */
static inline u32 scene_rowsq(const scene_t *s, u32 y)
{
    s32 uy = (s32)((2 * y + 1) * 2048 / s->H) - LP_FOCUS_Y;
    return (u32)(uy * uy);
}

static inline void scene_bg_row(const scene_t *s, u32 x, u32 rowsq, u32 c[3])
{
    if (s->plain) {
        c[0] = c[1] = c[2] = 0;
        return;
    }
    u32 idx = (u32)(((u64)(s->colsq[x] + rowsq) * s->recip) >> 32);
    if (idx >= LUT_N) idx = LUT_N - 1;
    c[0] = s->lut[idx][0];
    c[1] = s->lut[idx][1];
    c[2] = s->lut[idx][2];
}

static inline void scene_bg(const scene_t *s, u32 x, u32 y, u32 c[3])
{
    scene_bg_row(s, x, scene_rowsq(s, y), c);
}

/* Lay the logo over c at full strength. Each colour's shapes are one
 * union (the nearest edge wins), laid down in drawing order: the ring,
 * the L over its west side, the name, the i's dot. */
static inline void scene_logo(const scene_t *s, u32 x, u32 y, u32 c[3])
{
    static const u8 colour[G_COUNT][3] = {
        LP_RGB_ACCENT, LP_RGB_INK, LP_RGB_WORD, LP_RGB_ACCENT,
    };
    if ((s32)x < s->bx0 || (s32)x > s->bx1 || (s32)y < s->by0 || (s32)y > s->by1)
        return;
    s32 px = (s32)x * SUB + SUB / 2, py = (s32)y * SUB + SUB / 2;
    s32 best[G_COUNT];
    for (int g = 0; g < G_COUNT; g++)
        best[g] = 1 << 30;
    for (int i = 0; i < s->nprims; i++) {
        const placed_t *p = &s->prims[i];
        if ((s32)x < p->x0 || (s32)x > p->x1 || (s32)y < p->y0 || (s32)y > p->y1)
            continue;
        s32 sd = sc_distance(p, px, py) - p->hw;
        if (sd < best[p->group])
            best[p->group] = sd;
    }
    /* half a pixel inside the edge is solid, half a pixel outside is
     * clear, and in between is the anti-aliased fringe */
    for (int g = 0; g < G_COUNT; g++) {
        s32 cov = SUB / 2 - best[g];
        if (cov <= 0)
            continue;
        if (cov > SUB) cov = SUB;
        for (int k = 0; k < 3; k++) {
            s32 fg = (s32)colour[g][k] << 8;
            c[k] = (u32)((s32)c[k] + (fg - (s32)c[k]) * cov / SUB);
        }
    }
}

/* ── Angles without floating point ────────────────────────────────────
 * A turn is 65536. The spinner needs two things: the angle of a pixel
 * round its centre (atan through logo.h's table and the octant), and the
 * position of the stroke's head (cos through LP_WAVE, which is
 * (1 - cos) / 2 over half a turn). Both are exact to well under a
 * hundredth of a pixel at the sizes drawn. */

/* Clockwise from north, as a clock hand turns. */
static inline u32 scene_angle(s32 dx, s32 dy)
{
    u32 ax = (u32)sc_iabs(dx), ay = (u32)sc_iabs(dy);
    if (ax == 0 && ay == 0)
        return 0;
    u32 lo = ax < ay ? ax : ay, hi = ax < ay ? ay : ax;
    u32 r = (u32)(((u64)lo << 16) / hi);        /* 0..65536 */
    u32 i = r >> 10, f = r & 1023;              /* 64 steps */
    if (i >= 64) { i = 63; f = 1024; }
    u32 a = LP_ATAN[i] + (((u32)(LP_ATAN[i + 1] - LP_ATAN[i]) * f) >> 10);
    u32 q = ax >= ay ? a : 16384 - a;           /* from the x axis, 0..16384 */
    u32 east;                                   /* from east, clockwise on screen */
    if (dx >= 0 && dy >= 0)      east = q;
    else if (dx < 0 && dy >= 0)  east = 32768 - q;
    else if (dx < 0)             east = 32768 + q;
    else                         east = 65536 - q;
    return (east + 16384) & 0xFFFF;
}

/* cos of a turn angle, in 1/65536. */
static inline s32 scene_cos(u32 a)
{
    a &= 0xFFFF;
    if (a > 32768)
        a = 65536 - a;
    u32 pos = a * 64;                           /* 0 .. 64 << 15 */
    u32 i = pos >> 15, f = pos & 0x7FFF;
    if (i >= 64) { i = 63; f = 0x8000; }
    s32 w = LP_WAVE[i] + (s32)((((s32)LP_WAVE[i + 1] - (s32)LP_WAVE[i]) * (s32)f) >> 15);
    return 65536 - 2 * w;
}

/* Where the head of the stroke is when it points at `head`. */
static inline void scene_spin_head(const scene_t *s, u32 head, s32 *hx, s32 *hy)
{
    /* clockwise from north: x = sin, y = -cos */
    *hx = s->sp_cx + (s32)(((s64)s->sp_r * scene_cos(head - 16384)) >> 16);
    *hy = s->sp_cy - (s32)(((s64)s->sp_r * scene_cos(head)) >> 16);
}

/* The spinner's alpha (0..256) at a pixel. `ring` draws only the ring,
 * evenly all the way round - reduced motion's version, which breathes
 * instead of turning. */
static inline u32 scene_spin(const scene_t *s, u32 x, u32 y, u32 head, s32 hx, s32 hy, bool ring)
{
    s32 px = (s32)x * SUB + SUB / 2, py = (s32)y * SUB + SUB / 2;
    s32 dx = px - s->sp_cx, dy = py - s->sp_cy;
    s32 d  = (s32)sc_isqrt32((u32)(dx * dx + dy * dy));
    s32 band = SUB / 2 - (sc_iabs(d - s->sp_r) - s->sp_hw);
    band = sc_clamp(band, 0, SUB);
    if (ring)
        return (u32)(band * 256 / SUB);

    u32 a = 0;
    if (band > 0) {
        u32 behind = (head - scene_angle(dx, dy)) & 0xFFFF;
        u32 arc = 0;
        if (behind < SPIN_TAIL) {
            u32 u = (SPIN_TAIL - behind) * 256u / SPIN_TAIL;     /* 256 at the head */
            arc = u * u >> 8;
        }
        if (arc < SPIN_TRACK_A)
            arc = SPIN_TRACK_A;
        a = arc * (u32)band / SUB;
    }
    /* the head is a round end: a disc the stroke's width */
    s32 ex = px - hx, ey = py - hy;
    s32 cap = SUB / 2 - ((s32)sc_isqrt32((u32)(ex * ex + ey * ey)) - s->sp_hw);
    cap = sc_clamp(cap, 0, SUB) * 256 / SUB;
    return (u32)cap > a ? (u32)cap : a;
}

/* ── Dither ───────────────────────────────────────────────────────────
 * A dark gradient across 3840 pixels spends only a few dozen 8-bit
 * steps, so rounding each pixel paints contour lines. Each pixel is
 * rounded against an 8x8 ordered-dither threshold instead, so every 8x8
 * block averages to the true colour and the steps dissolve, at whatever
 * depth the framebuffer has. The channels read the pattern at different
 * offsets so the dither does not line up into a grey grid of its own. */
static const u8 SC_BAYER[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 },
};

/* One 8.8 channel value to `len` bits, rounded against the dither
 * threshold of channel k at (x, y). */
static inline u32 scene_quantise(u32 c, u32 len, u32 x, u32 y, int k)
{
    u32 thr = SC_BAYER[(y + 3 * (u32)k) & 7][(x + 5 * (u32)k) & 7] * 4u + 2u;
    if (len >= 8) {
        u32 v = (c + thr) >> 8;
        if (v > 255) v = 255;
        return v << (len - 8);
    }
    u32 max = (1u << len) - 1;
    u32 v = (((c * max * 257) >> 16) + thr) >> 8;  /* c * max / 255, 8.8 */
    return v > max ? max : v;
}

/* ── The PC maker's logo (ACPI BGRT) ────────────────────────────────
 * /sys/firmware/acpi/bgrt/image is the BMP the firmware showed at
 * power-on, xoffset and yoffset where it showed it. Parsing and placing
 * it are here, not in splash.c, for the reason everything else is: the
 * session's first frame (lp-splash-fade) must put it on the same pixels.
 *
 * The offsets are for the mode the firmware drew in. When the screen
 * now has another size (a virtual machine's card replacing the
 * firmware's), the logo goes where the firmware convention puts it:
 * centred across, its middle 38.2% of the way down. */
typedef struct {
    bool       ok, top_down;
    const u8  *bmp;
    u32        n, w, h, bpp, stride, data;
    s32        x, y;                /* on this screen */
} scene_oem_t;

static inline u32 sc_le16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static inline u32 sc_le32(const u8 *p) { return sc_le16(p) | sc_le16(p + 2) << 16; }

/* An uncompressed 24- or 32-bit BMP, bottom-up or top-down. */
static inline bool scene_oem_parse(scene_oem_t *o, const u8 *buf, u32 n)
{
    o->ok = false;
    if (n < 54 || buf[0] != 'B' || buf[1] != 'M' || sc_le32(buf + 14) < 40)
        return false;
    s32 w = (s32)sc_le32(buf + 18), h = (s32)sc_le32(buf + 22);
    u32 bpp = sc_le16(buf + 28), comp = sc_le32(buf + 30);
    o->top_down = h < 0;
    if (h < 0)
        h = -h;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || sc_le16(buf + 26) != 1 ||
        (bpp != 24 && bpp != 32) || (comp != 0 && !(comp == 3 && bpp == 32)))
        return false;
    o->bmp = buf;
    o->n = n;
    o->w = (u32)w;
    o->h = (u32)h;
    o->bpp = bpp;
    o->stride = ((u32)w * bpp + 31) / 32 * 4;
    o->data = sc_le32(buf + 10);
    if (o->data >= n || (u64)o->stride * o->h > (u64)(n - o->data))
        return false;
    o->ok = true;
    return true;
}

/* Where it goes on a W x H screen, from where the firmware put it;
 * false if it does not fit. */
static inline bool scene_oem_place(scene_oem_t *o, u32 W, u32 H, s32 fx, s32 fy)
{
    if (!o->ok || o->w > W || o->h > H)
        return false;
    s32 x = fx, y = fy;
    s32 centred = (s32)(W - o->w) / 2;
    bool same_mode = x >= 0 && y >= 0 && x + (s32)o->w <= (s32)W &&
                     y + (s32)o->h <= (s32)H && sc_iabs(centred - x) <= (s32)W / 50 + 2;
    if (!same_mode) {
        x = centred;
        y = (s32)(H * 382 / 1000) - (s32)o->h / 2;
        if (y < 0)
            y = 0;
    }
    o->x = x;
    o->y = y;
    return true;
}

/* Its colour at (px, py), 8.8 per channel; false outside it. */
static inline bool scene_oem_pixel(const scene_oem_t *o, u32 px, u32 py, u32 c[3])
{
    s32 dx = (s32)px - o->x, dy = (s32)py - o->y;
    if (dx < 0 || dy < 0 || dx >= (s32)o->w || dy >= (s32)o->h)
        return false;
    u32 r = o->top_down ? (u32)dy : o->h - 1 - (u32)dy;
    const u8 *p = o->bmp + o->data + r * o->stride + (u32)dx * (o->bpp / 8);
    c[0] = (u32)p[2] << 8;
    c[1] = (u32)p[1] << 8;
    c[2] = (u32)p[0] << 8;
    return true;
}

/* Blend fg over bg at alpha (0..256), 8.8 per channel. */
static inline void scene_mix(u32 c[3], const u32 bg[3], const u32 fg[3], u32 alpha)
{
    for (int k = 0; k < 3; k++)
        c[k] = (u32)((s32)bg[k] + (((s32)fg[k] - (s32)bg[k]) * (s32)alpha) / 256);
}

#endif
