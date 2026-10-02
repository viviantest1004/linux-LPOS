#define _DEFAULT_SOURCE 1

/*
 * edit.c - LP Photos: the editor half.
 *
 * "Draw on top of the picture" is most of what people want from an
 * editor that comes with a viewer: an arrow at the button, a circle
 * around the thing, a box over a phone number. After that: crop, turn,
 * a little brighter, smaller for e-mail. That is the whole list; layers
 * and selections belong to a real paint program.
 *
 * ── Ink goes where it is seen ──
 *
 * While a stroke is being drawn it lives only as a list of points and is
 * painted over the picture on every frame (edit_draw_overlay). Only on
 * release does it go into the full-resolution surface, once. Drawing
 * each motion event into a 20 MP surface and rescaling it would lag on
 * this machine; drawing a path over a ready-made screen-sized cache does
 * not.
 *
 * Ink does not go into the photo itself but into a transparent layer of
 * the same size (app->ann, see photos.h), which is what lets the eraser
 * rub out a stroke and leave the photo under it as it was. Mosaic, blur,
 * brightness and the like are the opposite: they change the photo, and
 * the eraser does not bring back what they hid - that would defeat the
 * point of hiding it.
 *
 * Widths are chosen in screen pixels and turned into picture pixels at
 * the zoom the stroke began at, so "medium" looks medium however far in
 * or out you are - what you see is what is saved.
 *
 * ── Undo without a gigabyte ──
 *
 * A snapshot of the whole picture per step would be 80 MB each for a
 * phone photo. A stroke only keeps the rectangle it covered; turning and
 * flipping keep nothing (they are undone by turning back); only crop,
 * resize and adjust keep a whole picture. The stack is capped in bytes,
 * dropping the oldest steps first - strokes are small, so dozens fit.
 */

#include "photos.h"
#include <math.h>
#include <string.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <stdlib.h>

#define UNDO_BUDGET ((gsize)160 << 20)

static const double PEN_W[3]    = { 3, 6, 12 };    /* screen pixels */
static const double HL_W[3]     = { 14, 24, 40 };
static const double ERASER_W[3] = { 12, 26, 50 };
static const double TEXT_PX[3]  = { 20, 32, 52 };
static const double MOSAIC_PX[3] = { 8, 14, 24 };
static const double BLUR_PX[3]  = { 5, 9, 16 };     /* box radius, three passes */

#define TEXT_FONT "Pretendard Variable,Pretendard,Noto Sans CJK KR,Sans Bold"


static const double SWATCH[N_SWATCH][3] = {
    { 0.00, 0.00, 0.00 },    /* black  */
    { 1.00, 1.00, 1.00 },    /* white  */
    { 0.90, 0.22, 0.21 },    /* red    */
    { 0.98, 0.55, 0.00 },    /* orange */
    { 0.99, 0.85, 0.21 },    /* yellow */
    { 0.26, 0.63, 0.28 },    /* green  */
    { 0.12, 0.53, 0.90 },    /* blue   */
    { 0.56, 0.14, 0.67 },    /* purple */
};

static void syncing_aspect(app_t *app);
static void crop_reset(app_t *app);

static void after_edit(app_t *app)
{
    title_update(app);
    edit_update_buttons(app);
}

/* ── undo ─────────────────────────────────────────────────────────── */

static gsize surf_bytes(cairo_surface_t *s)
{
    return s ? (gsize)cairo_image_surface_get_stride(s) * cairo_image_surface_get_height(s) : 0;
}

static gsize entry_bytes(const undo_t *e) { return surf_bytes(e->surf) + surf_bytes(e->ann); }

static void undo_free(undo_t *e)
{
    if (e->surf) cairo_surface_destroy(e->surf);
    if (e->ann) cairo_surface_destroy(e->ann);
    e->surf = e->ann = NULL;
}

void ink_drop(app_t *app)
{
    if (app->ann) cairo_surface_destroy(app->ann);
    app->ann = NULL;
}

/* Made on the first stroke. A new cairo surface is all zeros, which is
 * exactly "no ink anywhere". */
static gboolean ink_ensure(app_t *app)
{
    if (app->ann) return TRUE;
    app->ann = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, app->iw, app->ih);
    if (cairo_surface_status(app->ann) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(app->ann);
        app->ann = NULL;
        toast(app, T("Not enough memory to draw on a picture this large.",
                     "사진이 너무 커서 그릴 메모리가 부족합니다."));
        return FALSE;
    }
    return TRUE;
}

void undo_clear(app_t *app)
{
    for (int i = 0; i < app->un; i++) undo_free(&app->u[i]);
    app->un = app->upos = app->usaved = 0;
    app->ubytes = 0;
    if (app->undo_btn) edit_update_buttons(app);
}

/* Unsaved means the picture differs from the file. Turned right and back
 * left, or flipped twice, it does not: the steps in between are all quarter
 * turns and flips, and multiplied together (as the 2x2 matrices they are,
 * y pointing down) they come to nothing - so there is nothing to ask about. */
gboolean edit_dirty(app_t *app)
{
    static const int XM[4][4] = {        /* row-major, indexed by xform_t */
        { 0, 1, -1, 0 },                 /* XF_ROT_L  */
        { 0, -1, 1, 0 },                 /* XF_ROT_R  */
        { -1, 0, 0, 1 },                 /* XF_FLIP_H */
        { 1, 0, 0, -1 },                 /* XF_FLIP_V */
    };
    if (app->upos == app->usaved) return FALSE;
    if (app->usaved < 0) return TRUE;
    int m[4] = { 1, 0, 0, 1 };
    for (int i = MIN(app->upos, app->usaved); i < MAX(app->upos, app->usaved); i++) {
        if (app->u[i].kind != U_XFORM) return TRUE;
        const int *o = XM[app->u[i].op];
        int n[4] = { o[0] * m[0] + o[1] * m[2], o[0] * m[1] + o[1] * m[3],
                     o[2] * m[0] + o[3] * m[2], o[2] * m[1] + o[3] * m[3] };
        memcpy(m, n, sizeof m);
    }
    return !(m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 1);
}

static void undo_push(app_t *app, undo_t e)
{
    for (int i = app->upos; i < app->un; i++) {
        app->ubytes -= entry_bytes(&app->u[i]);
        undo_free(&app->u[i]);
    }
    app->un = app->upos;
    /* The saved state was in the future we just threw away: no amount of
     * undoing gets back to it now. */
    if (app->usaved > app->un) app->usaved = -1;
    if (app->un == app->ucap) {
        app->ucap = app->ucap ? app->ucap * 2 : 32;
        app->u = g_renew(undo_t, app->u, app->ucap);
    }
    app->u[app->un++] = e;
    app->upos = app->un;
    app->ubytes += entry_bytes(&e);

    while (app->ubytes > UNDO_BUDGET && app->un > 1) {
        app->ubytes -= entry_bytes(&app->u[0]);
        undo_free(&app->u[0]);
        memmove(app->u, app->u + 1, sizeof(undo_t) * (app->un - 1));
        app->un--; app->upos--;
        if (app->usaved == 0) app->usaved = -1;
        else if (app->usaved > 0) app->usaved--;
    }
}

static cairo_surface_t *copy_rect(cairo_surface_t *src, int x, int y, int w, int h)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(s);
    cairo_set_source_surface(cr, src, -x, -y);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    return s;
}

/* Keep the pixels a change is about to cover, of the photo or of the ink. */
static void push_region(app_t *app, gboolean ink, int x, int y, int w, int h)
{
    undo_t e = { U_REGION, x, y, copy_rect(ink ? app->ann : app->img, x, y, w, h), 0, ink, NULL };
    undo_push(app, e);
}

/* The new picture replaces the old one; the old one *is* the undo. With
 * `ink` the ink layer is replaced along with it (crop, resize: the ink
 * has to be cut and scaled the same way); without, adjusting colours, it
 * stays as it is. */
static void replace_image(app_t *app, cairo_surface_t *s, gboolean ink, cairo_surface_t *ann)
{
    undo_t e = { U_FULL, 0, 0, app->img, 0, ink, NULL };
    app->img = s;
    if (ink) { e.ann = app->ann; app->ann = ann; }
    undo_push(app, e);
    view_image_changed(app);
}

static void swap_region(app_t *app, undo_t *e)
{
    cairo_surface_t *dst = e->ink ? app->ann : app->img;
    if (!dst) return;
    int w = cairo_image_surface_get_width(e->surf), h = cairo_image_surface_get_height(e->surf);
    cairo_surface_t *cur = copy_rect(dst, e->x, e->y, w, h);
    cairo_t *cr = cairo_create(dst);
    cairo_set_source_surface(cr, e->surf, e->x, e->y);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_rectangle(cr, e->x, e->y, w, h);
    cairo_fill(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(e->surf);
    e->surf = cur;
    view_region_changed(app, e->x, e->y, w, h);
}

static cairo_surface_t *xform_surface(cairo_surface_t *src, xform_t op)
{
    int w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
    gboolean turn = op == XF_ROT_L || op == XF_ROT_R;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, turn ? h : w, turn ? w : h);
    cairo_t *cr = cairo_create(s);
    switch (op) {
    case XF_ROT_L:  cairo_translate(cr, 0, w); cairo_rotate(cr, -G_PI / 2); break;
    case XF_ROT_R:  cairo_translate(cr, h, 0); cairo_rotate(cr, G_PI / 2); break;
    case XF_FLIP_H: cairo_translate(cr, w, 0); cairo_scale(cr, -1, 1); break;
    case XF_FLIP_V: cairo_translate(cr, 0, h); cairo_scale(cr, 1, -1); break;
    }
    cairo_set_source_surface(cr, src, 0, 0);
    /* Exact quarter turns land pixel centres on pixel centres; NEAREST
     * keeps them bit-identical instead of smearing half a pixel. */
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    return s;
}

static void apply_xform(app_t *app, xform_t op)
{
    cairo_surface_t *s = xform_surface(app->img, op);
    cairo_surface_destroy(app->img);
    app->img = s;
    if (app->ann) {
        s = xform_surface(app->ann, op);
        cairo_surface_destroy(app->ann);
        app->ann = s;
    }
    app->fit = TRUE;           /* a turned picture has a new best fit */
    view_image_changed(app);
}

static xform_t xf_inverse(xform_t op)
{
    return op == XF_ROT_L ? XF_ROT_R : op == XF_ROT_R ? XF_ROT_L : op;
}

static void undo_apply(app_t *app, undo_t *e, gboolean undo)
{
    switch (e->kind) {
    case U_REGION: swap_region(app, e); break;
    case U_FULL: {
        app->ubytes -= entry_bytes(e);
        cairo_surface_t *t = app->img;
        app->img = e->surf;
        e->surf = t;
        if (e->ink) { t = app->ann; app->ann = e->ann; e->ann = t; }
        app->ubytes += entry_bytes(e);
        view_image_changed(app);
        break;
    }
    case U_XFORM: apply_xform(app, undo ? xf_inverse(e->op) : e->op); break;
    }
}

/* After the picture changed shape under it, a crop box starts over. */
static void crop_follow(app_t *app)
{
    app->crop_has = FALSE;
    if (app->editing && app->tool == TOOL_CROP) crop_reset(app);
}

void edit_undo(app_t *app)
{
    if (app->upos == 0 || app->adjusting) return;
    undo_apply(app, &app->u[--app->upos], TRUE);
    crop_follow(app);
    after_edit(app);
}

void edit_redo(app_t *app)
{
    if (app->upos >= app->un || app->adjusting) return;
    undo_apply(app, &app->u[app->upos++], FALSE);
    crop_follow(app);
    after_edit(app);
}

void edit_xform(app_t *app, xform_t op)
{
    if (!app->img) return;
    apply_xform(app, op);
    undo_t e = { U_XFORM, 0, 0, NULL, op, FALSE, NULL };
    undo_push(app, e);
    crop_follow(app);
    after_edit(app);
}

/* ── shapes ───────────────────────────────────────────────────────── */

static double shape_lw(app_t *app)
{
    const double *t = app->tool == TOOL_HIGHLIGHT ? HL_W : app->tool == TOOL_ERASER ? ERASER_W : PEN_W;
    return t[app->width_idx] / app->stroke_zoom;
}

static void arrow(cairo_t *cr, double x0, double y0, double x1, double y1, double lw)
{
    double len = hypot(x1 - x0, y1 - y0);
    double ang = atan2(y1 - y0, x1 - x0);
    double hl = MIN(lw * 4 + 4 * lw / 3, len * 0.7), hw = hl * 0.55;
    double bx = x1 - cos(ang) * hl * 0.7, by = y1 - sin(ang) * hl * 0.7;
    cairo_move_to(cr, x0, y0);
    cairo_line_to(cr, bx, by);
    cairo_stroke(cr);
    cairo_move_to(cr, x1, y1);
    cairo_line_to(cr, x1 - cos(ang) * hl + sin(ang) * hw, y1 - sin(ang) * hl - cos(ang) * hw);
    cairo_line_to(cr, x1 - cos(ang) * hl - sin(ang) * hw, y1 - sin(ang) * hl + cos(ang) * hw);
    cairo_close_path(cr);
    cairo_fill(cr);
}

/* One function paints the shape for the preview and for the real thing,
 * so the two cannot disagree. `cr` is in picture coordinates. The eraser
 * brings its own source (the photo, for the preview) or operator (CLEAR,
 * for the ink layer). */
static void paint_shape(app_t *app, cairo_t *cr)
{
    double lw = shape_lw(app);
    const double *c = app->rgba;
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, lw);
    if (app->tool != TOOL_ERASER)
        cairo_set_source_rgba(cr, c[0], c[1], c[2], app->tool == TOOL_HIGHLIGHT ? 0.4 : 1.0);
    double x0 = MIN(app->ax, app->bx), y0 = MIN(app->ay, app->by);
    double w = fabs(app->bx - app->ax), h = fabs(app->by - app->ay);

    switch (app->tool) {
    case TOOL_PEN:
    case TOOL_HIGHLIGHT:
    case TOOL_ERASER: {
        double *p = (double *)app->pts->data;
        guint n = app->pts->len / 2;
        if (n == 0) return;
        cairo_move_to(cr, p[0], p[1]);
        if (n == 1) cairo_line_to(cr, p[0] + 0.01, p[1]);   /* a dot */
        for (guint i = 1; i < n; i++) cairo_line_to(cr, p[2 * i], p[2 * i + 1]);
        /* One path stroked once: a highlighter crossing itself does not
         * get darker where it overlaps, like the real thing. */
        cairo_stroke(cr);
        break;
    }
    case TOOL_LINE:
        cairo_move_to(cr, app->ax, app->ay);
        cairo_line_to(cr, app->bx, app->by);
        cairo_stroke(cr);
        break;
    case TOOL_ARROW:
        arrow(cr, app->ax, app->ay, app->bx, app->by, lw);
        break;
    case TOOL_RECT:
        cairo_rectangle(cr, x0, y0, w, h);
        if (app->fill) cairo_fill_preserve(cr);
        cairo_stroke(cr);
        break;
    case TOOL_ELLIPSE:
        if (w < 0.5 || h < 0.5) return;
        cairo_save(cr);
        cairo_translate(cr, x0 + w / 2, y0 + h / 2);
        cairo_scale(cr, w / 2, h / 2);
        cairo_arc(cr, 0, 0, 1, 0, 2 * G_PI);
        cairo_restore(cr);
        if (app->fill) cairo_fill_preserve(cr);
        cairo_stroke(cr);
        break;
    default:
        break;
    }
}

static gboolean shape_bbox(app_t *app, int *rx, int *ry, int *rw, int *rh)
{
    double lw = shape_lw(app);
    double x0 = G_MAXDOUBLE, y0 = G_MAXDOUBLE, x1 = -G_MAXDOUBLE, y1 = -G_MAXDOUBLE;
    if (app->tool == TOOL_PEN || app->tool == TOOL_HIGHLIGHT || app->tool == TOOL_ERASER) {
        double *p = (double *)app->pts->data;
        for (guint i = 0; i + 1 < app->pts->len; i += 2) {
            x0 = MIN(x0, p[i]); x1 = MAX(x1, p[i]);
            y0 = MIN(y0, p[i + 1]); y1 = MAX(y1, p[i + 1]);
        }
    } else {
        x0 = MIN(app->ax, app->bx); x1 = MAX(app->ax, app->bx);
        y0 = MIN(app->ay, app->by); y1 = MAX(app->ay, app->by);
    }
    double m = lw / 2 + (app->tool == TOOL_ARROW ? lw * 6 : 0) + 2;
    int ix0 = MAX(0, (int)floor(x0 - m)), iy0 = MAX(0, (int)floor(y0 - m));
    int ix1 = MIN(app->iw, (int)ceil(x1 + m)), iy1 = MIN(app->ih, (int)ceil(y1 + m));
    if (ix1 <= ix0 || iy1 <= iy0) return FALSE;
    *rx = ix0; *ry = iy0; *rw = ix1 - ix0; *rh = iy1 - iy0;
    return TRUE;
}

static void commit_shape(app_t *app)
{
    int x, y, w, h;
    if (!shape_bbox(app, &x, &y, &w, &h)) return;
    if (app->tool == TOOL_ERASER && !app->ann) {
        toast(app, T("The eraser removes what you drew. The photo itself stays.",
                     "지우개는 그린 것만 지웁니다. 사진은 그대로 둡니다."));
        return;
    }
    if (!ink_ensure(app)) return;
    push_region(app, TRUE, x, y, w, h);
    cairo_t *cr = cairo_create(app->ann);
    if (app->tool == TOOL_ERASER) cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    paint_shape(app, cr);
    cairo_destroy(cr);
    view_region_changed(app, x, y, w, h);
    after_edit(app);
}

/* ── mosaic ───────────────────────────────────────────────────────── */

static void mosaic(app_t *app, int x, int y, int w, int h, int b)
{
    push_region(app, FALSE, x, y, w, h);
    cairo_surface_flush(app->img);
    guchar *data = cairo_image_surface_get_data(app->img);
    int st = cairo_image_surface_get_stride(app->img);
    for (int by = y; by < y + h; by += b) {
        int bh = MIN(b, y + h - by);
        for (int bx = x; bx < x + w; bx += b) {
            int bw = MIN(b, x + w - bx);
            guint64 s[4] = { 0 };
            for (int yy = by; yy < by + bh; yy++) {
                guint32 *row = (guint32 *)(data + (gsize)yy * st);
                for (int xx = bx; xx < bx + bw; xx++) {
                    guint32 v = row[xx];
                    s[0] += v >> 24; s[1] += (v >> 16) & 255; s[2] += (v >> 8) & 255; s[3] += v & 255;
                }
            }
            guint64 n = (guint64)bw * bh;
            guint32 v = (guint32)((s[0] / n) << 24 | (s[1] / n) << 16 | (s[2] / n) << 8 | (s[3] / n));
            for (int yy = by; yy < by + bh; yy++) {
                guint32 *row = (guint32 *)(data + (gsize)yy * st);
                for (int xx = bx; xx < bx + bw; xx++) row[xx] = v;
            }
        }
    }
    cairo_surface_mark_dirty_rectangle(app->img, x, y, w, h);
    view_region_changed(app, x, y, w, h);
    after_edit(app);
}

/* One row or column of a box blur, `len` pixels `step` apart, averaged
 * over 2r+1 neighbours with the ends repeated. Averaging premultiplied
 * pixels is the right thing to do, so no conversion is needed. */
static void blur_line(guint32 *p, int len, gsize step, int r, guint32 *tmp)
{
    for (int i = 0; i < len; i++) tmp[i] = p[i * step];
    guint32 s[4] = { 0 };
    guint32 win = 2 * r + 1;
    for (int k = -r; k <= r; k++) {
        guint32 v = tmp[CLAMP(k, 0, len - 1)];
        for (int c = 0; c < 4; c++) s[c] += (v >> (c * 8)) & 255;
    }
    for (int i = 0; i < len; i++) {
        guint32 v = 0;
        for (int c = 0; c < 4; c++) v |= ((s[c] + win / 2) / win) << (c * 8);
        p[i * step] = v;
        guint32 o = tmp[CLAMP(i - r, 0, len - 1)], n = tmp[CLAMP(i + r + 1, 0, len - 1)];
        for (int c = 0; c < 4; c++) s[c] += ((n >> (c * 8)) & 255) - ((o >> (c * 8)) & 255);
    }
}

/* Three box blurs in a row are close enough to a gaussian that nobody
 * can tell, and each costs the same whatever the radius. */
static void blur(app_t *app, int x, int y, int w, int h, int r)
{
    push_region(app, FALSE, x, y, w, h);
    cairo_surface_flush(app->img);
    guchar *data = cairo_image_surface_get_data(app->img);
    gsize st = cairo_image_surface_get_stride(app->img) / 4;
    guint32 *base = (guint32 *)data + (gsize)y * st + x;
    guint32 *tmp = g_new(guint32, MAX(w, h));
    r = MIN(r, MAX(w, h));
    for (int pass = 0; pass < 3; pass++) {
        for (int yy = 0; yy < h; yy++) blur_line(base + (gsize)yy * st, w, 1, r, tmp);
        for (int xx = 0; xx < w; xx++) blur_line(base + xx, h, st, r, tmp);
    }
    g_free(tmp);
    cairo_surface_mark_dirty_rectangle(app->img, x, y, w, h);
    view_region_changed(app, x, y, w, h);
    after_edit(app);
}

static gboolean rect_of(app_t *app, double ax, double ay, double bx, double by,
                        int *x, int *y, int *w, int *h)
{
    int x0 = CLAMP((int)floor(MIN(ax, bx)), 0, app->iw);
    int y0 = CLAMP((int)floor(MIN(ay, by)), 0, app->ih);
    int x1 = CLAMP((int)ceil(MAX(ax, bx)), 0, app->iw);
    int y1 = CLAMP((int)ceil(MAX(ay, by)), 0, app->ih);
    if (x1 - x0 < 1 || y1 - y0 < 1) return FALSE;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return TRUE;
}

/* ── text ─────────────────────────────────────────────────────────── */

static PangoLayout *text_layout(app_t *app, cairo_t *cr, const char *text)
{
    PangoLayout *pl = pango_cairo_create_layout(cr);
    /* No hinting: the preview at any zoom and the ink at 100% then lay
     * the glyphs out the same. */
    cairo_font_options_t *fo = cairo_font_options_create();
    cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_NONE);
    cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
    pango_cairo_context_set_font_options(pango_layout_get_context(pl), fo);
    cairo_font_options_destroy(fo);
    pango_layout_context_changed(pl);
    PangoFontDescription *fd = pango_font_description_from_string(TEXT_FONT);
    pango_font_description_set_absolute_size(fd, TEXT_PX[app->width_idx] / app->text_zoom * PANGO_SCALE);
    pango_layout_set_font_description(pl, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(pl, text, -1);
    return pl;
}

/* The click marks the start of the first line, halfway up it. */
static double text_top(app_t *app)
{
    return app->ty - TEXT_PX[app->width_idx] / app->text_zoom * 0.65;
}

static void text_paint(app_t *app, cairo_t *cr, const char *text)
{
    PangoLayout *pl = text_layout(app, cr, text);
    const double *c = app->rgba;
    cairo_set_source_rgba(cr, c[0], c[1], c[2], 1);
    cairo_move_to(cr, app->tx, text_top(app));
    /* Filled as outlines rather than shown as glyphs: fontconfig may ask
     * for subpixel antialiasing whatever the font options say, and its
     * colour fringes would be saved into the picture. A path is always
     * plain grey-level antialiased, the same on screen and in the file. */
    pango_cairo_layout_path(cr, pl);
    cairo_fill(cr);
    g_object_unref(pl);
}

static void text_hide(app_t *app)
{
    app->text_active = FALSE;
    gtk_widget_set_visible(app->text_box, FALSE);
    gtk_widget_grab_focus(app->canvas);
    view_queue(app);
}

void edit_text_commit(app_t *app, const char *text)
{
    if (!app->text_active) return;
    if (!text || !*text || !ink_ensure(app)) { text_hide(app); return; }
    char *t = g_strdup(text);
    /* Measure where the ink will land, keep those pixels, then draw. */
    cairo_t *mcr = cairo_create(app->ann);
    PangoLayout *pl = text_layout(app, mcr, t);
    PangoRectangle ink, logical;
    pango_layout_get_pixel_extents(pl, &ink, &logical);
    g_object_unref(pl);
    cairo_destroy(mcr);
    double top = text_top(app);
    int x, y, w, h;
    double x0 = app->tx + MIN(ink.x, logical.x) - 4, y0 = top + MIN(ink.y, logical.y) - 4;
    double x1 = app->tx + MAX(ink.x + ink.width, logical.x + logical.width) + 4;
    double y1 = top + MAX(ink.y + ink.height, logical.y + logical.height) + 4;
    if (rect_of(app, x0, y0, x1, y1, &x, &y, &w, &h)) {
        push_region(app, TRUE, x, y, w, h);
        cairo_t *cr = cairo_create(app->ann);
        text_paint(app, cr, t);
        cairo_destroy(cr);
        view_region_changed(app, x, y, w, h);
        after_edit(app);
    }
    g_free(t);
    text_hide(app);
}

static void text_begin(app_t *app, double wx, double wy)
{
    if (app->text_active)
        edit_text_commit(app, gtk_editable_get_text(GTK_EDITABLE(app->text_entry)));
    view_w2i(app, wx, wy, &app->tx, &app->ty);
    app->text_zoom = app->zoom;
    app->text_active = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(app->text_entry), "");
    int cwid = gtk_widget_get_width(app->canvas), chei = gtk_widget_get_height(app->canvas);
    gtk_widget_set_margin_start(app->text_box, CLAMP((int)wx - 20, 8, MAX(8, cwid - 340)));
    gtk_widget_set_margin_top(app->text_box,
                              wy + 40 < chei - 60 ? (int)wy + 30 : MAX(8, (int)wy - 90));
    gtk_widget_set_visible(app->text_box, TRUE);
    gtk_widget_grab_focus(app->text_entry);
    view_queue(app);
}

static void on_text_activate(GtkEntry *e, gpointer d)
{
    edit_text_commit(d, gtk_editable_get_text(GTK_EDITABLE(e)));
}

static void on_text_add(GtkButton *b, gpointer d)
{
    (void)b;
    app_t *app = d;
    edit_text_commit(app, gtk_editable_get_text(GTK_EDITABLE(app->text_entry)));
}

static void on_text_changed(GtkEditable *e, gpointer d) { (void)e; view_queue(d); }

/* ── crop ─────────────────────────────────────────────────────────── */

/*
 * The crop box starts as the whole picture, with handles on its corners
 * and edges: dragging a handle moves that side, dragging inside moves
 * the box, dragging outside draws a new one. With a fixed shape chosen
 * (1:1, 4:3, ...) only whole corners move, from the opposite corner, so
 * the shape holds; an edge handle then acts as the corner next to it.
 */
static double aspect_value(app_t *app, int i)
{
    switch (i) {
    case 1: return (double)app->iw / app->ih;    /* the picture's own */
    case 2: return 1.0;
    case 3: return 4.0 / 3.0;
    case 4: return 16.0 / 9.0;
    default: return 0;                            /* free */
    }
}

/* The box in whole picture pixels, rounded (a drag lands between pixels). */
static gboolean crop_rect(app_t *app, int *x, int *y, int *w, int *h)
{
    return rect_of(app, round(app->cx0), round(app->cy0), round(app->cx1), round(app->cy1), x, y, w, h);
}

static void crop_update_bar(app_t *app)
{
    gtk_widget_set_visible(app->crop_bar, app->editing && app->tool == TOOL_CROP && !app->adjusting);
    int x, y, w, h;
    gboolean whole = !app->crop_has ||
        (crop_rect(app, &x, &y, &w, &h) && w == app->iw && h == app->ih);
    gtk_widget_set_sensitive(app->crop_apply, !whole);
}

/* The largest box of the chosen shape, in the middle of the picture. */
static void crop_reset(app_t *app)
{
    double r = aspect_value(app, app->crop_aspect);
    double w = app->iw, h = app->ih;
    if (r > 0) {
        if (w / h > r) w = h * r; else h = w / r;
    }
    app->cx0 = (app->iw - w) / 2; app->cx1 = app->cx0 + w;
    app->cy0 = (app->ih - h) / 2; app->cy1 = app->cy0 + h;
    app->crop_has = app->img != NULL;
    crop_update_bar(app);
    view_queue(app);
}

void edit_set_aspect(app_t *app, int idx)
{
    if (idx < 0 || idx >= N_ASPECT) return;
    app->crop_aspect = idx;
    syncing_aspect(app);
    if (app->tool == TOOL_CROP) crop_reset(app);
}

/* Which handle (or the inside) is under the pointer, in screen pixels:
 * handles are for fingers too, so they are generous. */
static int crop_hit(app_t *app, double wx, double wy)
{
    const double R = 16;
    double z = app->zoom;
    double x0 = app->ox + app->cx0 * z, x1 = app->ox + app->cx1 * z;
    double y0 = app->oy + app->cy0 * z, y1 = app->oy + app->cy1 * z;
    gboolean iny = wy > y0 - R && wy < y1 + R, inx = wx > x0 - R && wx < x1 + R;
    int m = 0;
    double dl = fabs(wx - x0), dr = fabs(wx - x1), dt = fabs(wy - y0), db = fabs(wy - y1);
    if (iny && MIN(dl, dr) < R) m |= dl <= dr ? CE_L : CE_R;
    if (inx && MIN(dt, db) < R) m |= dt <= db ? CE_T : CE_B;
    if (!m && wx > x0 && wx < x1 && wy > y0 && wy < y1) m = CE_MOVE;
    return m;
}

static void crop_press(app_t *app, double wx, double wy, double ix, double iy)
{
    int m = app->crop_has ? crop_hit(app, wx, wy) : 0;
    if (!m) {
        /* a new box, from here */
        app->cx0 = app->cx1 = CLAMP(ix, 0, app->iw);
        app->cy0 = app->cy1 = CLAMP(iy, 0, app->ih);
        app->crop_has = FALSE;
        m = CE_R | CE_B;
    } else if (m != CE_MOVE && aspect_value(app, app->crop_aspect) > 0
               && !((m & (CE_L | CE_R)) && (m & (CE_T | CE_B)))) {
        m |= (m & (CE_L | CE_R)) ? CE_B : CE_R;
    }
    app->crop_grab = m;
    if (m == CE_MOVE) {
        app->ax = ix; app->ay = iy;             /* where the grab began */
        app->bx = app->cx0; app->by = app->cy0; /* and where the box was */
    } else {
        /* the fixed corner, opposite the one being dragged */
        app->ax = (m & CE_L) ? app->cx1 : app->cx0;
        app->ay = (m & CE_T) ? app->cy1 : app->cy0;
    }
}

static void crop_drag(app_t *app, double ix, double iy)
{
    int m = app->crop_grab;
    double W = app->iw, H = app->ih;
    if (m == CE_MOVE) {
        double w = app->cx1 - app->cx0, h = app->cy1 - app->cy0;
        double x0 = CLAMP(app->bx + ix - app->ax, 0, W - w);
        double y0 = CLAMP(app->by + iy - app->ay, 0, H - h);
        app->cx0 = x0; app->cy0 = y0; app->cx1 = x0 + w; app->cy1 = y0 + h;
        return;
    }
    gboolean hx = (m & (CE_L | CE_R)) != 0, hy = (m & (CE_T | CE_B)) != 0;
    if (hx && hy) {
        double r = aspect_value(app, app->crop_aspect);
        double dx = ix - app->ax, dy = iy - app->ay;
        double sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
        double maxw = sx > 0 ? W - app->ax : app->ax, maxh = sy > 0 ? H - app->ay : app->ay;
        double w = MIN(fabs(dx), maxw), h = MIN(fabs(dy), maxh);
        if (r > 0) {
            /* follow whichever way the pointer went further */
            if (w > h * r) h = w / r; else w = h * r;
            if (w > maxw) { w = maxw; h = w / r; }
            if (h > maxh) { h = maxh; w = h * r; }
        }
        double x1 = app->ax + sx * w, y1 = app->ay + sy * h;
        app->cx0 = MIN(app->ax, x1); app->cx1 = MAX(app->ax, x1);
        app->cy0 = MIN(app->ay, y1); app->cy1 = MAX(app->ay, y1);
    } else if (hx) {
        if (m & CE_L) app->cx0 = CLAMP(ix, 0, app->cx1 - 1);
        else app->cx1 = CLAMP(ix, app->cx0 + 1, W);
    } else if (hy) {
        if (m & CE_T) app->cy0 = CLAMP(iy, 0, app->cy1 - 1);
        else app->cy1 = CLAMP(iy, app->cy0 + 1, H);
    }
}

static void crop_release(app_t *app)
{
    app->crop_grab = 0;
    /* A click, not a drag, outside the box: start again from the whole
     * picture rather than leave a box too small to see. */
    if ((app->cx1 - app->cx0) * app->zoom < 4 || (app->cy1 - app->cy0) * app->zoom < 4)
        crop_reset(app);
    else
        app->crop_has = TRUE;
    crop_update_bar(app);
}

void edit_crop_apply(app_t *app)
{
    int x, y, w, h;
    if (!app->crop_has || !crop_rect(app, &x, &y, &w, &h))
        return;
    if (w == app->iw && h == app->ih) { crop_update_bar(app); view_queue(app); return; }
    app->fit = TRUE;
    replace_image(app, copy_rect(app->img, x, y, w, h), TRUE,
                  app->ann ? copy_rect(app->ann, x, y, w, h) : NULL);
    crop_reset(app);
    after_edit(app);
}

void edit_crop_cancel(app_t *app)
{
    crop_reset(app);
}

/* ── adjust ───────────────────────────────────────────────────────── */

static void adjust_pixels(cairo_surface_t *s, double b, double c, double sat)
{
    guchar lut[256];
    double bo = b / 100.0 * 0.5;
    double cf = c >= 0 ? 1 + c / 100.0 * 1.5 : 1 + c / 100.0;
    for (int i = 0; i < 256; i++) {
        double v = (i / 255.0 - 0.5) * cf + 0.5 + bo;
        lut[i] = (guchar)lround(CLAMP(v, 0, 1) * 255);
    }
    int sf = (int)lround((1 + sat / 100.0) * 256);
    cairo_surface_flush(s);
    int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    int st = cairo_image_surface_get_stride(s);
    guchar *data = cairo_image_surface_get_data(s);
    for (int y = 0; y < h; y++) {
        guint32 *row = (guint32 *)(data + (gsize)y * st);
        for (int x = 0; x < w; x++) {
            guint32 v = row[x];
            int a = v >> 24;
            if (!a) continue;
            int r = (v >> 16) & 255, g = (v >> 8) & 255, bl = v & 255;
            if (a != 255) {           /* work on the colour, not the premultiplied value */
                r = r * 255 / a; g = g * 255 / a; bl = bl * 255 / a;
                r = MIN(r, 255); g = MIN(g, 255); bl = MIN(bl, 255);
            }
            r = lut[r]; g = lut[g]; bl = lut[bl];
            if (sf != 256) {
                int l = (r * 77 + g * 150 + bl * 29) >> 8;
                r = CLAMP(l + (((r - l) * sf) >> 8), 0, 255);
                g = CLAMP(l + (((g - l) * sf) >> 8), 0, 255);
                bl = CLAMP(l + (((bl - l) * sf) >> 8), 0, 255);
            }
            if (a != 255) { r = r * a / 255; g = g * a / 255; bl = bl * a / 255; }
            row[x] = (guint32)a << 24 | (guint32)r << 16 | (guint32)g << 8 | (guint32)bl;
        }
    }
    cairo_surface_mark_dirty(s);
}

static void adj_values(app_t *app, double *b, double *c, double *s)
{
    *b = gtk_range_get_value(GTK_RANGE(app->adj_scale_w[0]));
    *c = gtk_range_get_value(GTK_RANGE(app->adj_scale_w[1]));
    *s = gtk_range_get_value(GTK_RANGE(app->adj_scale_w[2]));
}

static gboolean adj_recompute(gpointer d)
{
    app_t *app = d;
    app->adj_idle = 0;
    if (!app->adjusting) return G_SOURCE_REMOVE;
    double b, c, s;
    adj_values(app, &b, &c, &s);
    if (app->adj_prev) cairo_surface_destroy(app->adj_prev);
    int w = cairo_image_surface_get_width(app->adj_base), h = cairo_image_surface_get_height(app->adj_base);
    app->adj_prev = copy_rect(app->adj_base, 0, 0, w, h);
    adjust_pixels(app->adj_prev, b, c, s);
    view_queue(app);
    return G_SOURCE_REMOVE;
}

static void on_adj_changed(GtkRange *r, gpointer d)
{
    (void)r;
    app_t *app = d;
    if (!app->adj_idle) app->adj_idle = g_idle_add(adj_recompute, app);
}

void edit_adjust_begin(app_t *app)
{
    if (app->adjusting || !app->img || !app->editing) return;
    if (app->text_active) text_hide(app);
    app->crop_has = FALSE;
    /* The live preview works on a screen-sized copy: dragging a slider
     * must not mean touching 20 million pixels per step. */
    double s = MIN(1.0, app->zoom);
    s = MIN(s, sqrt(3e6 / ((double)app->iw * app->ih)));
    int w = MAX(1, (int)lround(app->iw * s)), h = MAX(1, (int)lround(app->ih * s));
    app->adj_base = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(app->adj_base);
    cairo_scale(cr, (double)w / app->iw, (double)h / app->ih);
    cairo_set_source_surface(cr, app->img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    app->adj_scale = (double)w / app->iw;
    app->adjusting = TRUE;
    for (int i = 0; i < 3; i++) gtk_range_set_value(GTK_RANGE(app->adj_scale_w[i]), 0);
    adj_recompute(app);
    gtk_widget_set_visible(app->adj_bar, TRUE);
    crop_update_bar(app);
    view_cursor(app);
    edit_update_buttons(app);
}

void edit_adjust_set(app_t *app, double b, double c, double s)
{
    gtk_range_set_value(GTK_RANGE(app->adj_scale_w[0]), b);
    gtk_range_set_value(GTK_RANGE(app->adj_scale_w[1]), c);
    gtk_range_set_value(GTK_RANGE(app->adj_scale_w[2]), s);
    if (app->adj_idle) { g_source_remove(app->adj_idle); app->adj_idle = 0; }
    adj_recompute(app);
}

static void adjust_end(app_t *app)
{
    app->adjusting = FALSE;
    if (app->adj_idle) { g_source_remove(app->adj_idle); app->adj_idle = 0; }
    if (app->adj_base) cairo_surface_destroy(app->adj_base);
    if (app->adj_prev) cairo_surface_destroy(app->adj_prev);
    app->adj_base = app->adj_prev = NULL;
    gtk_widget_set_visible(app->adj_bar, FALSE);
    if (app->editing && app->tool == TOOL_CROP) crop_reset(app);
    crop_update_bar(app);
    view_cursor(app);
    edit_update_buttons(app);
    view_queue(app);
}

void edit_adjust_apply(app_t *app)
{
    if (!app->adjusting) return;
    double b, c, s;
    adj_values(app, &b, &c, &s);
    adjust_end(app);
    if (b == 0 && c == 0 && s == 0) return;
    cairo_surface_t *n = copy_rect(app->img, 0, 0, app->iw, app->ih);
    adjust_pixels(n, b, c, s);
    replace_image(app, n, FALSE, NULL);
    after_edit(app);
}

void edit_adjust_cancel(app_t *app) { if (app->adjusting) adjust_end(app); }

static void b_adj_reset(GtkButton *b, gpointer d)
{
    (void)b;
    app_t *app = d;
    for (int i = 0; i < 3; i++) gtk_range_set_value(GTK_RANGE(app->adj_scale_w[i]), 0);
}

/* ── resize ───────────────────────────────────────────────────────── */

static cairo_surface_t *scale_surface(cairo_surface_t *src, int w, int h)
{
    int sw = cairo_image_surface_get_width(src), sh = cairo_image_surface_get_height(src);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return NULL;
    }
    cairo_t *cr = cairo_create(s);
    cairo_scale(cr, (double)w / sw, (double)h / sh);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr),
                             w < sw ? CAIRO_FILTER_GOOD : CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    return s;
}

void edit_resize(app_t *app, int w, int h)
{
    if (!app->img || w < 1 || h < 1 || (w == app->iw && h == app->ih)) return;
    cairo_surface_t *s = scale_surface(app->img, w, h);
    cairo_surface_t *a = app->ann ? scale_surface(app->ann, w, h) : NULL;
    if (!s || (app->ann && !a)) {
        if (s) cairo_surface_destroy(s);
        if (a) cairo_surface_destroy(a);
        toast(app, T("That size is too large.", "너무 큰 크기입니다."));
        return;
    }
    app->fit = TRUE;
    replace_image(app, s, TRUE, a);
    if (app->tool == TOOL_CROP) crop_reset(app);
    after_edit(app);
}

typedef struct { app_t *app; GtkWidget *w, *h, *keep; gboolean busy; } resize_ui_t;

static void rs_w_changed(GtkSpinButton *sb, gpointer d)
{
    resize_ui_t *r = d;
    if (r->busy || !gtk_check_button_get_active(GTK_CHECK_BUTTON(r->keep))) return;
    r->busy = TRUE;
    double w = gtk_spin_button_get_value(sb);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->h), MAX(1, lround(w * r->app->ih / r->app->iw)));
    r->busy = FALSE;
}

static void rs_h_changed(GtkSpinButton *sb, gpointer d)
{
    resize_ui_t *r = d;
    if (r->busy || !gtk_check_button_get_active(GTK_CHECK_BUTTON(r->keep))) return;
    r->busy = TRUE;
    double h = gtk_spin_button_get_value(sb);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->w), MAX(1, lround(h * r->app->iw / r->app->ih)));
    r->busy = FALSE;
}

static void rs_ok(GtkButton *b, gpointer d)
{
    (void)b;
    resize_ui_t *r = d;
    gtk_spin_button_update(GTK_SPIN_BUTTON(r->w));
    gtk_spin_button_update(GTK_SPIN_BUTTON(r->h));
    edit_resize(r->app, gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->w)),
                gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->h)));
}

void edit_resize_dialog(app_t *app)
{
    if (!app->img || app->adjusting) return;
    char *body = g_strdup_printf(T("Now %d × %d pixels.", "지금 %d × %d 픽셀입니다."), app->iw, app->ih);
    GtkWidget *dlg = dialog_new(app, T("Resize", "크기 조절"), body);
    g_free(body);
    resize_ui_t *r = g_new0(resize_ui_t, 1);
    r->app = app;
    g_object_set_data_full(G_OBJECT(dlg), "rs", r, g_free);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    const char *names[2] = { T("Width", "너비"), T("Height", "높이") };
    for (int i = 0; i < 2; i++) {
        GtkWidget *l = gtk_label_new(names[i]);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_grid_attach(GTK_GRID(grid), l, 0, i, 1, 1);
        GtkWidget *sb = gtk_spin_button_new_with_range(1, 16000, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(sb), i == 0 ? app->iw : app->ih);
        gtk_widget_set_hexpand(sb, TRUE);
        gtk_grid_attach(GTK_GRID(grid), sb, 1, i, 1, 1);
        GtkWidget *px = gtk_label_new(T("pixels", "픽셀"));
        gtk_widget_add_css_class(px, "dim-label");
        gtk_grid_attach(GTK_GRID(grid), px, 2, i, 1, 1);
        if (i == 0) r->w = sb; else r->h = sb;
    }
    r->keep = gtk_check_button_new_with_label(T("Keep proportions", "비율 유지"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(r->keep), TRUE);
    gtk_grid_attach(GTK_GRID(grid), r->keep, 0, 2, 3, 1);
    g_signal_connect(r->w, "value-changed", G_CALLBACK(rs_w_changed), r);
    g_signal_connect(r->h, "value-changed", G_CALLBACK(rs_h_changed), r);
    gtk_box_append(GTK_BOX(g_object_get_data(G_OBJECT(dlg), "extra")), grid);

    dialog_add_button(dlg, T("Cancel", "취소"), NULL, NULL, NULL);
    GtkWidget *ok = dialog_add_button(dlg, T("Resize", "크기 바꾸기"), "suggested-action",
                                      G_CALLBACK(rs_ok), r);
    gtk_window_set_default_widget(GTK_WINDOW(dlg), ok);
    gtk_window_present(GTK_WINDOW(dlg));
}

/* ── saving ───────────────────────────────────────────────────────── */

static gboolean format_writable(const char *name)
{
    gboolean ok = FALSE;
    GSList *fmts = gdk_pixbuf_get_formats();
    for (GSList *l = fmts; l; l = l->next) {
        char *n = gdk_pixbuf_format_get_name(l->data);
        if (!g_strcmp0(n, name) && gdk_pixbuf_format_is_writable(l->data)) ok = TRUE;
        g_free(n);
    }
    g_slist_free(fmts);
    return ok;
}

/* The format is the extension, as people expect: photo.jpg is a JPEG. */
static const char *type_for_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) return NULL;
    char *e = g_ascii_strdown(dot + 1, -1);
    const char *t = NULL;
    if (!strcmp(e, "jpg") || !strcmp(e, "jpeg") || !strcmp(e, "jpe")) t = "jpeg";
    else if (!strcmp(e, "png")) t = "png";
    else if (!strcmp(e, "bmp")) t = "bmp";
    else if (!strcmp(e, "tif") || !strcmp(e, "tiff")) t = "tiff";
    else if (!strcmp(e, "webp")) t = "webp";
    else if (!strcmp(e, "ico")) t = "ico";
    g_free(e);
    return (t && format_writable(t)) ? t : NULL;
}

/* The part of saving that may run in a thread: it touches nothing but
 * its arguments. Write next to the original and rename over it: a failed
 * save (disk full, card pulled) must not leave half a photo where a whole
 * one was. */
static gboolean write_pixbuf(GdkPixbuf *pb, const char *path, const char *type, int quality, GError **err)
{
    char *tmp = g_strdup_printf("%s.lp-photos-%d", path, (int)getpid());
    char q[8];
    g_snprintf(q, sizeof q, "%d", quality > 0 ? CLAMP(quality, 10, 100) : 92);
    gboolean ok = !strcmp(type, "jpeg")
        ? gdk_pixbuf_save(pb, tmp, type, err, "quality", q, NULL)
        : gdk_pixbuf_save(pb, tmp, type, err, NULL);
    if (ok) {
        GStatBuf st;
        if (g_stat(path, &st) == 0) g_chmod(tmp, st.st_mode & 07777);
        if (g_rename(tmp, path) != 0) {
            g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", g_strerror(errno));
            ok = FALSE;
        }
    }
    if (!ok) g_unlink(tmp);
    g_free(tmp);
    return ok;
}

/* The part that must run here: the picture as it is now, ink laid on. */
static GdkPixbuf *save_pixels(app_t *app, const char *path, const char **type, GError **err)
{
    *type = type_for_path(path);
    if (!*type) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "%s", T("This file type cannot be written. Use .png or .jpg.",
                            "이 형식으로는 저장할 수 없습니다. .png 나 .jpg 를 쓰십시오."));
        return NULL;
    }
    gboolean alpha = strcmp(*type, "jpeg") && strcmp(*type, "bmp");
    GdkPixbuf *pb = surface_to_pixbuf(app->img, app->ann, alpha);
    if (!pb) g_set_error(err, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "out of memory");
    return pb;
}

gboolean edit_save_to(app_t *app, const char *path, GError **err)
{
    const char *type;
    GdkPixbuf *pb = save_pixels(app, path, &type, err);
    if (!pb) return FALSE;
    gboolean ok = write_pixbuf(pb, path, type, app->jpeg_quality, err);
    g_object_unref(pb);
    return ok;
}

void cont_run(app_t *app)
{
    cont_fn f = app->cont;
    gpointer d = app->cont_data;
    GDestroyNotify fr = app->cont_free;
    app->cont = NULL; app->cont_data = NULL; app->cont_free = NULL;
    if (f) f(app, d);
    if (fr && d) fr(d);
}

void cont_drop(app_t *app)
{
    if (app->cont_free && app->cont_data) app->cont_free(app->cont_data);
    app->cont = NULL; app->cont_data = NULL; app->cont_free = NULL;
}

void cont_leave_editor(app_t *app, gpointer data)
{
    (void)data;
    edit_leave_now(app);
}

static void save_error(app_t *app, GError *e)
{
    char *m = g_strdup_printf(T("Could not save: %s", "저장하지 못했습니다: %s"),
                              e ? e->message : "?");
    toast(app, m);
    g_free(m);
}

/*
 * Encoding a 20 MP photo as JPEG takes a second or more here, so the
 * file is written in a thread while the window shows a spinner. The
 * pixels are taken first, on this thread, so what is saved is what was
 * on screen when Save was pressed; the window takes no edits until the
 * file is written, so "saved" still means that state when it lands.
 */
typedef struct {
    app_t *app;
    GdkPixbuf *pb;
    char *path;
    const char *type;
    int quality;
    GError *err;
} save_job_t;

static void save_job_free(gpointer p)
{
    save_job_t *j = p;
    g_object_unref(j->pb);
    g_free(j->path);
    g_clear_error(&j->err);
    g_free(j);
}

static void save_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    save_job_t *j = data;
    g_task_return_boolean(task, write_pixbuf(j->pb, j->path, j->type, j->quality, &j->err));
}

static void saving_set(app_t *app, gboolean on)
{
    app->saving = on;
    gtk_widget_set_sensitive(app->stack, !on);
    gtk_widget_set_sensitive(app->hb_edit_start, !on);
    gtk_widget_set_sensitive(app->hb_edit_end, !on);
    gtk_widget_set_visible(app->spinner, on);
    if (on) gtk_spinner_start(GTK_SPINNER(app->spinner));
    else gtk_spinner_stop(GTK_SPINNER(app->spinner));
}

static void save_done(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)src; (void)d;
    save_job_t *j = g_task_get_task_data(G_TASK(res));
    app_t *app = j->app;
    saving_set(app, FALSE);
    if (g_task_propagate_boolean(G_TASK(res), NULL)) {
        app->usaved = app->upos;
        after_save_as(app, j->path);       /* name, size and date changed */
        after_edit(app);
        toast(app, T("Saved", "저장했습니다"));
        if (app->st_cmds) g_print("selftest: saved %s in a thread\n", j->path);
        cont_run(app);
    } else {
        save_error(app, j->err);
        cont_drop(app);
    }
}

static void save_start(app_t *app, const char *path)
{
    GError *e = NULL;
    const char *type;
    GdkPixbuf *pb = app->saving ? NULL : save_pixels(app, path, &type, &e);
    if (!pb) {
        if (e) save_error(app, e);
        g_clear_error(&e);
        cont_drop(app);
        return;
    }
    save_job_t *j = g_new0(save_job_t, 1);
    j->app = app;
    j->pb = pb;
    j->path = g_strdup(path);
    j->type = type;
    j->quality = app->jpeg_quality;
    saving_set(app, TRUE);
    GTask *task = g_task_new(NULL, NULL, save_done, NULL);
    g_task_set_task_data(task, j, save_job_free);
    g_task_run_in_thread(task, save_thread);
    g_object_unref(task);
}

static void save_here(app_t *app)
{
    save_start(app, app->path);
}

static void save_as_response(GtkNativeDialog *nd, int resp, gpointer d)
{
    app_t *app = d;
    char *path = NULL;
    if (resp == GTK_RESPONSE_ACCEPT) {
        GFile *f = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(nd));
        if (f) { path = g_file_get_path(f); g_object_unref(f); }
    }
    /* The format box decides the type and the name follows it: typing
     * "photo.png" and choosing JPEG saves photo.jpg, not a PNG with the
     * wrong name. An extension the box does not offer (.webp, .bmp) is
     * taken as meant. */
    const char *fmt = gtk_file_chooser_get_choice(GTK_FILE_CHOOSER(nd), "type");
    const char *qs = gtk_file_chooser_get_choice(GTK_FILE_CHOOSER(nd), "quality");
    gboolean jpeg = !g_strcmp0(fmt, "jpeg");
    if (app->st_cmds) g_print("selftest: save-as choice type=%s quality=%s\n", fmt, qs);
    if (qs) app->jpeg_quality = atoi(qs);
    if (path) {
        const char *t = type_for_path(path);
        if (!t || !strcmp(t, "png") || !strcmp(t, "jpeg")) {
            char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
            if (t && dot && (!slash || dot > slash)) *dot = 0;
            char *p2 = g_strconcat(path, jpeg ? ".jpg" : ".png", NULL);
            g_free(path);
            path = p2;
        }
    }
    g_object_unref(nd);
    if (!path) { cont_drop(app); return; }
    save_start(app, path);
    g_free(path);
}

void edit_save_as(app_t *app)
{
    if (!app->img) { cont_drop(app); return; }
    GtkFileChooserNative *nd = gtk_file_chooser_native_new(
        T("Save As", "다른 이름으로 저장"), GTK_WINDOW(app->win),
        GTK_FILE_CHOOSER_ACTION_SAVE, T("Save", "저장"), T("Cancel", "취소"));
    if (app->dir) {
        GFile *df = g_file_new_for_path(app->dir);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(nd), df, NULL);
        g_object_unref(df);
    }
    /* Suggest "name-edited" in a format we can write, so the original is
     * not replaced by accident. */
    char *base = app->path ? g_path_get_basename(app->path) : g_strdup("picture.png");
    char *dot = strrchr(base, '.');
    const char *ext = app->path && type_for_path(app->path) ? strrchr(app->path, '.') : ".png";
    if (dot) *dot = 0;
    char *name = g_strdup_printf("%s-%s%s", base, T("edited", "편집"), ext);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(nd), name);
    /* Format and JPEG quality are two choices under the file list. The
     * one to start on goes first: GTK 4.8's own file chooser ignores
     * gtk_file_chooser_set_choice (it keeps showing the first option), so
     * the order is what picks the default - and the portal agrees. */
    gboolean png_first = g_str_has_suffix(name, ".png") || !app->path;
    const char *png_l = T("PNG — exact, larger", "PNG — 손실 없음, 큰 파일");
    const char *jpg_l = T("JPEG — photos, smaller", "JPEG — 사진용, 작은 파일");
    const char *tids[] = { png_first ? "png" : "jpeg", png_first ? "jpeg" : "png", NULL };
    const char *tlab[] = { png_first ? png_l : jpg_l, png_first ? jpg_l : png_l, NULL };
    gtk_file_chooser_add_choice(GTK_FILE_CHOOSER(nd), "type", T("Format", "형식"), tids, tlab);
    static const char *QV[5] = { "95", "90", "80", "70", "50" };
    const char *QL[5] = { T("Best (95)", "최고 (95)"), T("High (90)", "높음 (90)"),
                          T("Good (80)", "좋음 (80)"), T("Medium (70)", "보통 (70)"),
                          T("Smallest (50)", "가장 작게 (50)") };
    int q = app->jpeg_quality > 0 ? app->jpeg_quality : 90;
    int qi = q >= 93 ? 0 : q >= 85 ? 1 : q >= 75 ? 2 : q >= 60 ? 3 : 4;
    const char *qids[6], *qlab[6];
    qids[0] = QV[qi]; qlab[0] = QL[qi];
    for (int i = 0, k = 1; i < 5; i++)
        if (i != qi) { qids[k] = QV[i]; qlab[k] = QL[i]; k++; }
    qids[5] = qlab[5] = NULL;
    gtk_file_chooser_add_choice(GTK_FILE_CHOOSER(nd), "quality", T("JPEG quality", "JPEG 품질"), qids, qlab);
    /* Without a filter GTK's chooser shows an empty "(None)" box next to
     * the two choices; one filter makes it say what the list holds. */
    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, T("Pictures", "사진"));
    gtk_file_filter_add_pixbuf_formats(ff);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(nd), ff);
    g_object_unref(ff);
    g_free(name); g_free(base);
    g_signal_connect(nd, "response", G_CALLBACK(save_as_response), app);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(nd));
}

static void dlg_mark(GtkWidget *w) { g_object_set_data(G_OBJECT(gtk_widget_get_root(w)), "handled", GINT_TO_POINTER(1)); }

static void dlg_destroyed(GtkWidget *w, gpointer d)
{
    if (!g_object_get_data(G_OBJECT(w), "handled")) cont_drop(d);
}

static void ow_ok(GtkButton *b, gpointer d) { dlg_mark(GTK_WIDGET(b)); save_here(d); }

void edit_save(app_t *app)
{
    if (!app->img) { cont_drop(app); return; }
    if (!app->path || !type_for_path(app->path)) {
        if (app->path)
            toast(app, T("This format cannot be saved; choose a new name.",
                         "이 형식으로는 저장할 수 없어 새 이름을 고릅니다."));
        edit_save_as(app);
        return;
    }
    char *base = g_path_get_basename(app->path);
    char *body = g_strdup_printf(T("“%s” will be replaced by the edited picture. This cannot be undone.",
                                   "“%s” 을(를) 편집한 사진으로 덮어씁니다. 되돌릴 수 없습니다."), base);
    GtkWidget *dlg = dialog_new(app, T("Replace the original?", "원본을 덮어쓸까요?"), body);
    g_free(body); g_free(base);
    dialog_add_button(dlg, T("Cancel", "취소"), NULL, NULL, NULL);
    GtkWidget *ok = dialog_add_button(dlg, T("Replace", "덮어쓰기"), "suggested-action",
                                      G_CALLBACK(ow_ok), app);
    g_signal_connect(dlg, "destroy", G_CALLBACK(dlg_destroyed), app);
    gtk_window_set_default_widget(GTK_WINDOW(dlg), ok);
    gtk_window_present(GTK_WINDOW(dlg));
    gtk_widget_grab_focus(ok);
}

static void un_save(GtkButton *b, gpointer d)
{
    app_t *app = d;
    dlg_mark(GTK_WIDGET(b));
    /* The question already named the file; asking "replace it?" again
     * would be the same question twice. */
    if (app->path && type_for_path(app->path)) save_here(app);
    else edit_save_as(app);
}

static void un_discard(GtkButton *b, gpointer d)
{
    app_t *app = d;
    dlg_mark(GTK_WIDGET(b));
    if (app->cont == cont_leave_editor || app->cont == NULL) {
        /* Staying on this picture: bring back the file as it is on disk. */
        edit_leave_now(app);
        undo_clear(app);
        reload_current(app);
    }
    app->usaved = app->upos;   /* nothing left to ask about */
    cont_run(app);
}

void ask_unsaved(app_t *app, cont_fn cont, gpointer data, GDestroyNotify fr)
{
    cont_drop(app);
    app->cont = cont; app->cont_data = data; app->cont_free = fr;
    char *base = app->path ? g_path_get_basename(app->path) : g_strdup("");
    char *title = g_strdup_printf(T("Save changes to “%s”?", "“%s” 의 변경 내용을 저장할까요?"), base);
    GtkWidget *dlg = dialog_new(app, title,
        T("If you don't save, your edits will be lost.",
          "저장하지 않으면 편집한 내용이 사라집니다."));
    g_free(title); g_free(base);
    dialog_add_button(dlg, T("Cancel", "취소"), NULL, NULL, NULL);
    dialog_add_button(dlg, T("Don't Save", "저장 안 함"), "destructive-action",
                      G_CALLBACK(un_discard), app);
    GtkWidget *ok = dialog_add_button(dlg, T("Save", "저장"), "suggested-action",
                                      G_CALLBACK(un_save), app);
    g_signal_connect(dlg, "destroy", G_CALLBACK(dlg_destroyed), app);
    gtk_window_set_default_widget(GTK_WINDOW(dlg), ok);
    gtk_window_present(GTK_WINDOW(dlg));
    gtk_widget_grab_focus(ok);
}

/* ── pointer ──────────────────────────────────────────────────────── */

void edit_press(app_t *app, double wx, double wy)
{
    if (!app->img || app->adjusting) return;
    double ix, iy;
    view_w2i(app, wx, wy, &ix, &iy);
    app->stroke_zoom = app->zoom;
    switch (app->tool) {
    case TOOL_PEN:
    case TOOL_HIGHLIGHT:
    case TOOL_ERASER:
        g_array_set_size(app->pts, 0);
        g_array_append_val(app->pts, ix);
        g_array_append_val(app->pts, iy);
        app->stroking = TRUE;
        break;
    case TOOL_TEXT:
        text_begin(app, wx, wy);
        break;
    case TOOL_CROP:
        crop_press(app, wx, wy, ix, iy);
        app->stroking = TRUE;
        break;
    default:
        app->ax = app->bx = ix;
        app->ay = app->by = iy;
        app->stroking = TRUE;
        break;
    }
    view_queue(app);
}

void edit_motion(app_t *app, double wx, double wy)
{
    if (!app->stroking) return;
    double ix, iy;
    view_w2i(app, wx, wy, &ix, &iy);
    switch (app->tool) {
    case TOOL_PEN:
    case TOOL_HIGHLIGHT:
    case TOOL_ERASER: {
        double *p = (double *)app->pts->data;
        guint n = app->pts->len;
        if (hypot(ix - p[n - 2], iy - p[n - 1]) * app->zoom < 1.0) return;
        g_array_append_val(app->pts, ix);
        g_array_append_val(app->pts, iy);
        break;
    }
    case TOOL_CROP:
        crop_drag(app, ix, iy);
        break;
    default:
        app->bx = ix; app->by = iy;
        break;
    }
    view_queue(app);
}

void edit_release(app_t *app, double wx, double wy)
{
    if (!app->stroking) return;
    edit_motion(app, wx, wy);
    app->stroking = FALSE;
    /* Screen distance, so a click that wobbled a pixel is not a shape. */
    double moved = hypot(app->bx - app->ax, app->by - app->ay) * app->zoom;
    switch (app->tool) {
    case TOOL_PEN:
    case TOOL_HIGHLIGHT:
    case TOOL_ERASER:
        commit_shape(app);
        g_array_set_size(app->pts, 0);
        break;
    case TOOL_LINE: case TOOL_ARROW: case TOOL_RECT: case TOOL_ELLIPSE:
        if (moved >= 3) commit_shape(app);
        break;
    case TOOL_MOSAIC:
    case TOOL_BLUR: {
        int x, y, w, h;
        if (moved < 3 || !rect_of(app, app->ax, app->ay, app->bx, app->by, &x, &y, &w, &h))
            break;
        if (app->tool == TOOL_MOSAIC)
            mosaic(app, x, y, w, h, MAX(2, (int)lround(MOSAIC_PX[app->width_idx] / app->zoom)));
        else
            blur(app, x, y, w, h, MAX(2, (int)lround(BLUR_PX[app->width_idx] / app->zoom)));
        break;
    }
    case TOOL_CROP:
        crop_release(app);
        break;
    default:
        break;
    }
    view_queue(app);
}

/* ── overlay ──────────────────────────────────────────────────────── */

void edit_draw_overlay(app_t *app, cairo_t *cr)
{
    double z = app->zoom;
    if (app->stroking && app->tool == TOOL_ERASER && app->ann) {
        /* The photo, drawn along the stroke: what will be left once the
         * ink there is gone. */
        cairo_save(cr);
        cairo_translate(cr, app->ox, app->oy);
        cairo_scale(cr, z, z);
        if (app->has_alpha) cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        else {
            cairo_set_source_surface(cr, app->img, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), z < 1 ? CAIRO_FILTER_FAST : CAIRO_FILTER_BILINEAR);
        }
        paint_shape(app, cr);
        cairo_restore(cr);
    } else if (app->stroking && app->tool >= TOOL_PEN && app->tool <= TOOL_ELLIPSE
               && app->tool != TOOL_ERASER) {
        cairo_save(cr);
        cairo_translate(cr, app->ox, app->oy);
        cairo_scale(cr, z, z);
        paint_shape(app, cr);
        cairo_restore(cr);
    }
    if (app->stroking && (app->tool == TOOL_MOSAIC || app->tool == TOOL_BLUR)) {
        double x = app->ox + MIN(app->ax, app->bx) * z, y = app->oy + MIN(app->ay, app->by) * z;
        double w = fabs(app->bx - app->ax) * z, h = fabs(app->by - app->ay) * z;
        cairo_rectangle(cr, x + 0.5, y + 0.5, w, h);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.18);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1.5);
        double dash[] = { 5, 4 };
        cairo_set_dash(cr, dash, 2, 0);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }
    if (app->tool == TOOL_CROP && (app->crop_has || app->stroking)) {
        double x = app->ox + MIN(app->cx0, app->cx1) * z, y = app->oy + MIN(app->cy0, app->cy1) * z;
        double w = fabs(app->cx1 - app->cx0) * z, h = fabs(app->cy1 - app->cy0) * z;
        /* Dim what will be cut away; the part kept stays as it is. */
        cairo_rectangle(cr, app->ox, app->oy, app->iw * z, app->ih * z);
        cairo_rectangle(cr, x, y, w, h);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
        cairo_fill(cr);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
        cairo_set_line_width(cr, 1);
        for (int i = 1; i < 3; i++) {
            cairo_move_to(cr, x + w * i / 3, y); cairo_line_to(cr, x + w * i / 3, y + h);
            cairo_move_to(cr, x, y + h * i / 3); cairo_line_to(cr, x + w, y + h * i / 3);
        }
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
        cairo_set_line_width(cr, 1.5);
        cairo_rectangle(cr, x, y, w, h);
        cairo_stroke(cr);
        cairo_set_line_width(cr, 4);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        double k = MIN(18, MIN(w, h) / 3);
        double cx[4] = { x, x + w, x, x + w }, cy[4] = { y, y, y + h, y + h };
        for (int i = 0; i < 4; i++) {
            double sx = i % 2 ? -1 : 1, sy = i < 2 ? 1 : -1;
            cairo_move_to(cr, cx[i] + sx * k, cy[i]);
            cairo_line_to(cr, cx[i], cy[i]);
            cairo_line_to(cr, cx[i], cy[i] + sy * k);
        }
        /* edge handles, where a side alone can be moved */
        if (aspect_value(app, app->crop_aspect) <= 0 && w > 3 * k && h > 3 * k) {
            cairo_move_to(cr, x + w / 2 - k / 2, y); cairo_line_to(cr, x + w / 2 + k / 2, y);
            cairo_move_to(cr, x + w / 2 - k / 2, y + h); cairo_line_to(cr, x + w / 2 + k / 2, y + h);
            cairo_move_to(cr, x, y + h / 2 - k / 2); cairo_line_to(cr, x, y + h / 2 + k / 2);
            cairo_move_to(cr, x + w, y + h / 2 - k / 2); cairo_line_to(cr, x + w, y + h / 2 + k / 2);
        }
        cairo_stroke(cr);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
        /* the size it will be, in picture pixels */
        int px, py, pw, ph;
        if (crop_rect(app, &px, &py, &pw, &ph)) {
            char buf[48];
            g_snprintf(buf, sizeof buf, "%d × %d", pw, ph);
            PangoLayout *pl = gtk_widget_create_pango_layout(app->canvas, buf);
            int lw, lh;
            pango_layout_get_pixel_size(pl, &lw, &lh);
            double tx = x + w / 2 - lw / 2.0, ty = y + 10;
            cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
            cairo_rectangle(cr, tx - 8, ty - 3, lw + 16, lh + 6);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_move_to(cr, tx, ty);
            pango_cairo_show_layout(cr, pl);
            g_object_unref(pl);
        }
    }
    if (app->text_active) {
        const char *t = gtk_editable_get_text(GTK_EDITABLE(app->text_entry));
        cairo_save(cr);
        cairo_translate(cr, app->ox, app->oy);
        cairo_scale(cr, z, z);
        if (t && *t) {
            text_paint(app, cr, t);
        } else {
            /* an empty entry still shows where the text will start */
            double s = TEXT_PX[app->width_idx] / app->text_zoom;
            cairo_set_source_rgba(cr, app->rgba[0], app->rgba[1], app->rgba[2], 0.9);
            cairo_set_line_width(cr, 2 / z);
            cairo_move_to(cr, app->tx, text_top(app));
            cairo_line_to(cr, app->tx, text_top(app) + s * 1.2);
            cairo_stroke(cr);
        }
        cairo_restore(cr);
    }
}

/* ── tool bar ─────────────────────────────────────────────────────── */

/* The drawing tools get their own little pictures. Icon themes disagree
 * about what a "highlighter" or "mosaic" icon is called, when they have
 * one at all; these look the same under any theme. */
static void icon_draw(GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer d)
{
    int id = GPOINTER_TO_INT(d);
    GdkRGBA fg;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_style_context_get_color(gtk_widget_get_style_context(GTK_WIDGET(da)), &fg);
    G_GNUC_END_IGNORE_DEPRECATIONS
    cairo_translate(cr, (w - 20) / 2.0, (h - 20) / 2.0);
    gdk_cairo_set_source_rgba(cr, &fg);
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    switch (id) {
    case TOOL_MOVE:   /* four-way arrow */
        cairo_move_to(cr, 10, 2); cairo_line_to(cr, 10, 18);
        cairo_move_to(cr, 2, 10); cairo_line_to(cr, 18, 10);
        cairo_stroke(cr);
        cairo_move_to(cr, 7, 5); cairo_line_to(cr, 10, 2); cairo_line_to(cr, 13, 5);
        cairo_move_to(cr, 7, 15); cairo_line_to(cr, 10, 18); cairo_line_to(cr, 13, 15);
        cairo_move_to(cr, 5, 7); cairo_line_to(cr, 2, 10); cairo_line_to(cr, 5, 13);
        cairo_move_to(cr, 15, 7); cairo_line_to(cr, 18, 10); cairo_line_to(cr, 15, 13);
        cairo_stroke(cr);
        break;
    case TOOL_PEN:    /* pencil */
        cairo_move_to(cr, 3, 17); cairo_line_to(cr, 4, 13); cairo_line_to(cr, 14, 3);
        cairo_line_to(cr, 17, 6); cairo_line_to(cr, 7, 16); cairo_close_path(cr);
        cairo_stroke(cr);
        cairo_move_to(cr, 12, 5); cairo_line_to(cr, 15, 8); cairo_stroke(cr);
        break;
    case TOOL_HIGHLIGHT:
        cairo_save(cr);
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.35);
        cairo_set_line_width(cr, 6);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
        cairo_move_to(cr, 2, 15); cairo_line_to(cr, 18, 15); cairo_stroke(cr);
        cairo_restore(cr);
        cairo_move_to(cr, 6, 13); cairo_line_to(cr, 8, 8); cairo_line_to(cr, 14, 2);
        cairo_line_to(cr, 17, 5); cairo_line_to(cr, 11, 11); cairo_close_path(cr);
        cairo_stroke(cr);
        break;
    case TOOL_ERASER:  /* a block eraser, tilted, over a rubbed-out line */
        cairo_save(cr);
        cairo_translate(cr, 10.5, 8.5);
        cairo_rotate(cr, -G_PI / 4);
        cairo_rectangle(cr, -7, -3.5, 14, 7);
        cairo_stroke(cr);
        cairo_rectangle(cr, -7, -3.5, 5, 7);
        cairo_fill(cr);
        cairo_restore(cr);
        cairo_move_to(cr, 8, 18); cairo_line_to(cr, 18, 18); cairo_stroke(cr);
        break;
    case TOOL_LINE:
        cairo_move_to(cr, 3, 17); cairo_line_to(cr, 17, 3); cairo_stroke(cr);
        break;
    case TOOL_ARROW:
        cairo_move_to(cr, 3, 17); cairo_line_to(cr, 15, 5); cairo_stroke(cr);
        cairo_move_to(cr, 17, 3); cairo_line_to(cr, 16.5, 10); cairo_line_to(cr, 10, 3.5);
        cairo_close_path(cr); cairo_fill(cr);
        break;
    case TOOL_RECT:
        cairo_rectangle(cr, 3, 5, 14, 10); cairo_stroke(cr);
        break;
    case TOOL_ELLIPSE:
        cairo_save(cr); cairo_translate(cr, 10, 10); cairo_scale(cr, 7.5, 5.5);
        cairo_arc(cr, 0, 0, 1, 0, 2 * G_PI); cairo_restore(cr); cairo_stroke(cr);
        break;
    case TOOL_TEXT:
        cairo_set_line_width(cr, 2.2);
        cairo_move_to(cr, 4, 4.5); cairo_line_to(cr, 16, 4.5);
        cairo_move_to(cr, 10, 4.5); cairo_line_to(cr, 10, 17);
        cairo_stroke(cr);
        break;
    case TOOL_MOSAIC:
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                double a = ((x + y) % 2) ? 0.9 : ((x * 3 + y) % 3 ? 0.45 : 0.2);
                cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, a);
                cairo_rectangle(cr, 2 + x * 4, 2 + y * 4, 4, 4);
                cairo_fill(cr);
            }
        break;
    case TOOL_BLUR:    /* a drop, fading out */
        cairo_move_to(cr, 10, 2);
        cairo_curve_to(cr, 13, 6, 16, 9.5, 16, 12.5);
        cairo_arc(cr, 10, 12.5, 6, 0, G_PI);
        cairo_curve_to(cr, 4, 9.5, 7, 6, 10, 2);
        cairo_close_path(cr);
        cairo_stroke(cr);
        for (int i = 0; i < 3; i++) {
            cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.8 - i * 0.25);
            cairo_arc(cr, 7.5 + i * 2.5, 13.5 - (i % 2) * 2, 1.1, 0, 2 * G_PI);
            cairo_fill(cr);
        }
        break;
    case TOOL_CROP:
        cairo_move_to(cr, 5, 1); cairo_line_to(cr, 5, 15); cairo_line_to(cr, 19, 15);
        cairo_move_to(cr, 1, 5); cairo_line_to(cr, 15, 5); cairo_line_to(cr, 15, 19);
        cairo_stroke(cr);
        break;
    case 100: case 101: case 102: {   /* stroke widths */
        double r[3] = { 1.6, 3.0, 5.0 };
        cairo_arc(cr, 10, 10, r[id - 100], 0, 2 * G_PI);
        cairo_fill(cr);
        break;
    }
    case 110:   /* fill */
        cairo_rectangle(cr, 3, 3, 14, 14);
        cairo_stroke_preserve(cr);
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.6);
        cairo_fill(cr);
        break;
    case 120:   /* resize: small box in big box with an arrow */
        cairo_rectangle(cr, 2, 2, 16, 16);
        cairo_stroke(cr);
        cairo_rectangle(cr, 2, 11, 7, 7);
        cairo_fill(cr);
        cairo_move_to(cr, 11, 9); cairo_line_to(cr, 15.5, 4.5); cairo_stroke(cr);
        cairo_move_to(cr, 11.5, 4.5); cairo_line_to(cr, 15.5, 4.5); cairo_line_to(cr, 15.5, 8.5);
        cairo_stroke(cr);
        break;
    case 121: { /* adjust: half-filled sun */
        cairo_arc(cr, 10, 10, 4.2, 0, 2 * G_PI); cairo_stroke(cr);
        cairo_arc(cr, 10, 10, 4.2, G_PI / 2, 3 * G_PI / 2); cairo_fill(cr);
        for (int i = 0; i < 8; i++) {
            double a = i * G_PI / 4;
            cairo_move_to(cr, 10 + cos(a) * 6.8, 10 + sin(a) * 6.8);
            cairo_line_to(cr, 10 + cos(a) * 8.6, 10 + sin(a) * 8.6);
        }
        cairo_stroke(cr);
        break;
    }
    }
}

static GtkWidget *drawn_icon(int id)
{
    GtkWidget *da = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(da), 20);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(da), 20);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), icon_draw, GINT_TO_POINTER(id), NULL);
    return da;
}

static void swatch_draw(GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer d)
{
    (void)da;
    int i = GPOINTER_TO_INT(d);
    cairo_arc(cr, w / 2.0, h / 2.0, MIN(w, h) / 2.0 - 1.5, 0, 2 * G_PI);
    cairo_set_source_rgb(cr, SWATCH[i][0], SWATCH[i][1], SWATCH[i][2]);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.8);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}

static const char *tool_tip(tool_t t)
{
    switch (t) {
    case TOOL_MOVE: return T("Move around (drag to pan)", "이동 (끌어서 화면 옮기기)");
    case TOOL_PEN: return T("Pen", "펜");
    case TOOL_HIGHLIGHT: return T("Highlighter", "형광펜");
    case TOOL_ERASER: return T("Eraser — rubs out drawing, not the photo", "지우개 — 그린 것만 지웁니다");
    case TOOL_LINE: return T("Line", "직선");
    case TOOL_ARROW: return T("Arrow", "화살표");
    case TOOL_RECT: return T("Rectangle", "사각형");
    case TOOL_ELLIPSE: return T("Ellipse", "타원");
    case TOOL_TEXT: return T("Text — click where it should go", "글자 — 넣을 곳을 누르십시오");
    case TOOL_MOSAIC: return T("Mosaic — drag over what to hide", "모자이크 — 가릴 곳을 끌어서 고르십시오");
    case TOOL_BLUR: return T("Blur — drag over what to hide", "흐리게 — 가릴 곳을 끌어서 고르십시오");
    case TOOL_CROP: return T("Crop", "자르기");
    default: return "";
    }
}

static gboolean syncing;

static void on_tool_toggled(GtkToggleButton *b, gpointer d)
{
    app_t *app = d;
    if (syncing || !gtk_toggle_button_get_active(b)) return;
    for (int i = 0; i < N_TOOLS; i++)
        if (app->tool_btn[i] == GTK_WIDGET(b)) edit_set_tool(app, i);
}

void edit_set_tool(app_t *app, tool_t t)
{
    if (t >= N_TOOLS) return;
    if (app->text_active && t != TOOL_TEXT)
        edit_text_commit(app, gtk_editable_get_text(GTK_EDITABLE(app->text_entry)));
    app->stroking = FALSE;
    gboolean was_crop = app->tool == TOOL_CROP;
    app->tool = t;
    if (t != TOOL_CROP) app->crop_has = FALSE;
    else if (!was_crop || !app->crop_has) crop_reset(app);
    syncing = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->tool_btn[t]), TRUE);
    syncing = FALSE;
    crop_update_bar(app);
    view_cursor(app);
    view_queue(app);
}

static void on_swatch(GtkToggleButton *b, gpointer d)
{
    app_t *app = d;
    if (syncing || !gtk_toggle_button_get_active(b)) return;
    for (int i = 0; i < N_SWATCH; i++)
        if (app->swatch[i] == GTK_WIDGET(b)) edit_set_color(app, i);
}

void edit_set_color(app_t *app, int i)
{
    if (i < 0 || i >= N_SWATCH) return;
    app->swatch_sel = i;
    app->rgba[0] = SWATCH[i][0]; app->rgba[1] = SWATCH[i][1]; app->rgba[2] = SWATCH[i][2];
    app->rgba[3] = 1;
    syncing = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->swatch[i]), TRUE);
    syncing = FALSE;
    GdkRGBA c = { app->rgba[0], app->rgba[1], app->rgba[2], 1 };
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(app->color_btn), &c);
    G_GNUC_END_IGNORE_DEPRECATIONS
    view_queue(app);
}

static void on_color_set(GtkColorButton *cb, gpointer d)
{
    app_t *app = d;
    GdkRGBA c;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(cb), &c);
    G_GNUC_END_IGNORE_DEPRECATIONS
    app->rgba[0] = c.red; app->rgba[1] = c.green; app->rgba[2] = c.blue; app->rgba[3] = 1;
    app->swatch_sel = -1;
    syncing = TRUE;
    for (int i = 0; i < N_SWATCH; i++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->swatch[i]), FALSE);
    syncing = FALSE;
    view_queue(app);
}

static void syncing_aspect(app_t *app)
{
    if (!app->aspect_btn[0]) return;
    syncing = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->aspect_btn[app->crop_aspect]), TRUE);
    syncing = FALSE;
}

static void on_aspect(GtkToggleButton *b, gpointer d)
{
    app_t *app = d;
    if (syncing || !gtk_toggle_button_get_active(b)) return;
    for (int i = 0; i < N_ASPECT; i++)
        if (app->aspect_btn[i] == GTK_WIDGET(b)) edit_set_aspect(app, i);
}

static void on_width(GtkToggleButton *b, gpointer d)
{
    app_t *app = d;
    if (syncing || !gtk_toggle_button_get_active(b)) return;
    for (int i = 0; i < 3; i++)
        if (app->width_btn[i] == GTK_WIDGET(b)) edit_set_width(app, i);
}

void edit_set_width(app_t *app, int idx)
{
    if (idx < 0 || idx > 2) return;
    app->width_idx = idx;
    syncing = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->width_btn[idx]), TRUE);
    syncing = FALSE;
    view_queue(app);
}

static void on_fill(GtkToggleButton *b, gpointer d)
{
    app_t *app = d;
    if (syncing) return;
    app->fill = gtk_toggle_button_get_active(b);
}

void edit_toggle_fill(app_t *app)
{
    app->fill = !app->fill;
    syncing = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->fill_btn), app->fill);
    syncing = FALSE;
}

void edit_update_buttons(app_t *app)
{
    if (!app->undo_btn) return;
    gtk_widget_set_sensitive(app->undo_btn, app->upos > 0 && !app->adjusting);
    gtk_widget_set_sensitive(app->redo_btn, app->upos < app->un && !app->adjusting);
}

static void b_undo(GtkButton *b, gpointer d) { (void)b; edit_undo(d); }
static void b_redo(GtkButton *b, gpointer d) { (void)b; edit_redo(d); }
static void b_save(GtkButton *b, gpointer d) { (void)b; cont_drop(d); edit_save(d); }
static void b_saveas(GtkButton *b, gpointer d) { (void)b; cont_drop(d); edit_save_as(d); }
static void b_done(GtkButton *b, gpointer d) { (void)b; edit_leave(d); }
static void b_copy(GtkButton *b, gpointer d) { (void)b; surface_to_clipboard(d); }
static void b_rotl(GtkButton *b, gpointer d) { (void)b; app_t *a = d; if (!a->adjusting) edit_xform(a, XF_ROT_L); }
static void b_rotr(GtkButton *b, gpointer d) { (void)b; app_t *a = d; if (!a->adjusting) edit_xform(a, XF_ROT_R); }
static void b_fliph(GtkButton *b, gpointer d) { (void)b; app_t *a = d; if (!a->adjusting) edit_xform(a, XF_FLIP_H); }
static void b_flipv(GtkButton *b, gpointer d) { (void)b; app_t *a = d; if (!a->adjusting) edit_xform(a, XF_FLIP_V); }
static void b_adjust(GtkButton *b, gpointer d) { (void)b; edit_adjust_begin(d); }
static void b_resize(GtkButton *b, gpointer d) { (void)b; edit_resize_dialog(d); }
static void b_crop_apply(GtkButton *b, gpointer d) { (void)b; edit_crop_apply(d); }
static void b_crop_cancel(GtkButton *b, gpointer d) { (void)b; app_t *a = d; edit_crop_cancel(a); edit_set_tool(a, TOOL_PEN); }
static void b_adj_apply(GtkButton *b, gpointer d) { (void)b; edit_adjust_apply(d); }
static void b_adj_cancel(GtkButton *b, gpointer d) { (void)b; edit_adjust_cancel(d); }

static GtkWidget *ibtn(const char *icon, const char *tip, GCallback cb, gpointer d)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_focus_on_click(b, FALSE);
    gtk_widget_add_css_class(b, "flat");
    g_signal_connect(b, "clicked", cb, d);
    return b;
}

static GtkWidget *dbtn(int id, const char *tip, GCallback cb, gpointer d)
{
    GtkWidget *b = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(b), drawn_icon(id));
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_focus_on_click(b, FALSE);
    gtk_widget_add_css_class(b, "flat");
    g_signal_connect(b, "clicked", cb, d);
    return b;
}

static GtkWidget *sep(void)
{
    GtkWidget *s = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    return s;
}

static GtkWidget *osd_bar(void)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(bar, "lp-photos-osd");
    gtk_widget_set_halign(bar, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(bar, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(bar, 18);
    gtk_widget_set_visible(bar, FALSE);
    return bar;
}

void edit_build_ui(app_t *app)
{
    app->pts = g_array_new(FALSE, FALSE, sizeof(double));

    /* header halves */
    app->hb_edit_start = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *done = gtk_button_new_with_label(T("Done", "완료"));
    gtk_widget_set_tooltip_text(done, T("Leave the editor (Esc)", "편집 끝내기 (Esc)"));
    g_signal_connect(done, "clicked", G_CALLBACK(b_done), app);
    gtk_box_append(GTK_BOX(app->hb_edit_start), done);
    app->undo_btn = ibtn("edit-undo-symbolic", T("Undo (Ctrl+Z)", "실행 취소 (Ctrl+Z)"), G_CALLBACK(b_undo), app);
    app->redo_btn = ibtn("edit-redo-symbolic", T("Redo (Ctrl+Shift+Z)", "다시 실행 (Ctrl+Shift+Z)"), G_CALLBACK(b_redo), app);
    gtk_widget_remove_css_class(app->undo_btn, "flat");
    gtk_widget_remove_css_class(app->redo_btn, "flat");
    GtkWidget *ur = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(ur), app->undo_btn);
    gtk_box_append(GTK_BOX(ur), app->redo_btn);
    gtk_box_append(GTK_BOX(app->hb_edit_start), ur);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(app->header), app->hb_edit_start);
    gtk_widget_set_visible(app->hb_edit_start, FALSE);

    app->hb_edit_end = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *sa = ibtn("document-save-as-symbolic", T("Save as a new file (Ctrl+Shift+S)",
                                                       "다른 이름으로 저장 (Ctrl+Shift+S)"), G_CALLBACK(b_saveas), app);
    gtk_widget_remove_css_class(sa, "flat");
    /* what is on screen, ink and all, as a PNG for pasting elsewhere */
    GtkWidget *cp = ibtn("edit-copy-symbolic", T("Copy the edited picture (Ctrl+C)", "편집한 사진 복사 (Ctrl+C)"),
                         G_CALLBACK(b_copy), app);
    gtk_widget_remove_css_class(cp, "flat");
    gtk_box_append(GTK_BOX(app->hb_edit_end), cp);
    app->save_btn = gtk_button_new_with_label(T("Save", "저장"));
    gtk_widget_add_css_class(app->save_btn, "suggested-action");
    gtk_widget_set_tooltip_text(app->save_btn, T("Save over the original (Ctrl+S)", "원본에 저장 (Ctrl+S)"));
    g_signal_connect(app->save_btn, "clicked", G_CALLBACK(b_save), app);
    gtk_box_append(GTK_BOX(app->hb_edit_end), sa);
    gtk_box_append(GTK_BOX(app->hb_edit_end), app->save_btn);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(app->header), app->hb_edit_end);
    gtk_widget_set_visible(app->hb_edit_end, FALSE);

    /* the tool bar under the header */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(bar, "toolbar");
    gtk_widget_add_css_class(bar, "lp-photos-editbar");
    gtk_widget_set_halign(bar, GTK_ALIGN_CENTER);

    GtkToggleButton *first = NULL;
    for (int i = 0; i < N_TOOLS; i++) {
        GtkWidget *b = gtk_toggle_button_new();
        gtk_button_set_child(GTK_BUTTON(b), drawn_icon(i));
        gtk_widget_set_tooltip_text(b, tool_tip(i));
        gtk_widget_set_focus_on_click(b, FALSE);
        gtk_widget_add_css_class(b, "flat");
        if (first) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), first);
        else first = GTK_TOGGLE_BUTTON(b);
        g_signal_connect(b, "toggled", G_CALLBACK(on_tool_toggled), app);
        app->tool_btn[i] = b;
        gtk_box_append(GTK_BOX(bar), b);
        if (i == TOOL_MOVE || i == TOOL_TEXT) gtk_box_append(GTK_BOX(bar), sep());
    }
    gtk_box_append(GTK_BOX(bar), sep());

    const char *cnames[N_SWATCH] = {
        T("Black", "검정"), T("White", "흰색"), T("Red", "빨강"), T("Orange", "주황"),
        T("Yellow", "노랑"), T("Green", "초록"), T("Blue", "파랑"), T("Purple", "보라"),
    };
    first = NULL;
    for (int i = 0; i < N_SWATCH; i++) {
        GtkWidget *b = gtk_toggle_button_new();
        GtkWidget *da = gtk_drawing_area_new();
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(da), 20);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(da), 20);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), swatch_draw, GINT_TO_POINTER(i), NULL);
        gtk_button_set_child(GTK_BUTTON(b), da);
        gtk_widget_set_tooltip_text(b, cnames[i]);
        gtk_widget_set_focus_on_click(b, FALSE);
        gtk_widget_add_css_class(b, "flat");
        gtk_widget_add_css_class(b, "lp-photos-swatch");
        if (first) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), first);
        else first = GTK_TOGGLE_BUTTON(b);
        g_signal_connect(b, "toggled", G_CALLBACK(on_swatch), app);
        app->swatch[i] = b;
        gtk_box_append(GTK_BOX(bar), b);
    }
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    app->color_btn = gtk_color_button_new();
    gtk_color_button_set_title(GTK_COLOR_BUTTON(app->color_btn), T("Choose a Colour", "색 고르기"));
    G_GNUC_END_IGNORE_DEPRECATIONS
    gtk_widget_set_tooltip_text(app->color_btn, T("Other colour…", "다른 색…"));
    gtk_widget_set_valign(app->color_btn, GTK_ALIGN_CENTER);
    g_signal_connect(app->color_btn, "color-set", G_CALLBACK(on_color_set), app);
    gtk_box_append(GTK_BOX(bar), app->color_btn);
    gtk_box_append(GTK_BOX(bar), sep());

    /* One size control for every tool: line width, text size, how
     * coarse the mosaic and how strong the blur. */
    const char *wnames[3] = { T("Small — thin line, small text", "작게 — 가는 선, 작은 글자"),
                              T("Medium", "보통"),
                              T("Large — thick line, big text", "크게 — 굵은 선, 큰 글자") };
    first = NULL;
    for (int i = 0; i < 3; i++) {
        GtkWidget *b = gtk_toggle_button_new();
        gtk_button_set_child(GTK_BUTTON(b), drawn_icon(100 + i));
        gtk_widget_set_tooltip_text(b, wnames[i]);
        gtk_widget_set_focus_on_click(b, FALSE);
        gtk_widget_add_css_class(b, "flat");
        if (first) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), first);
        else first = GTK_TOGGLE_BUTTON(b);
        g_signal_connect(b, "toggled", G_CALLBACK(on_width), app);
        app->width_btn[i] = b;
        gtk_box_append(GTK_BOX(bar), b);
    }
    app->fill_btn = gtk_toggle_button_new();
    gtk_button_set_child(GTK_BUTTON(app->fill_btn), drawn_icon(110));
    gtk_widget_set_tooltip_text(app->fill_btn, T("Fill rectangles and ellipses", "사각형과 타원 채우기"));
    gtk_widget_set_focus_on_click(app->fill_btn, FALSE);
    gtk_widget_add_css_class(app->fill_btn, "flat");
    g_signal_connect(app->fill_btn, "toggled", G_CALLBACK(on_fill), app);
    gtk_box_append(GTK_BOX(bar), app->fill_btn);
    gtk_box_append(GTK_BOX(bar), sep());

    gtk_box_append(GTK_BOX(bar), ibtn("object-rotate-left-symbolic", T("Rotate left (Shift+R)", "왼쪽으로 돌리기 (Shift+R)"), G_CALLBACK(b_rotl), app));
    gtk_box_append(GTK_BOX(bar), ibtn("object-rotate-right-symbolic", T("Rotate right (R)", "오른쪽으로 돌리기 (R)"), G_CALLBACK(b_rotr), app));
    gtk_box_append(GTK_BOX(bar), ibtn("object-flip-horizontal-symbolic", T("Flip horizontally (H)", "좌우 뒤집기 (H)"), G_CALLBACK(b_fliph), app));
    gtk_box_append(GTK_BOX(bar), ibtn("object-flip-vertical-symbolic", T("Flip vertically (V)", "상하 뒤집기 (V)"), G_CALLBACK(b_flipv), app));
    gtk_box_append(GTK_BOX(bar), sep());
    gtk_box_append(GTK_BOX(bar), dbtn(121, T("Brightness, contrast, saturation", "밝기, 대비, 채도"), G_CALLBACK(b_adjust), app));
    gtk_box_append(GTK_BOX(bar), dbtn(120, T("Resize…", "크기 조절…"), G_CALLBACK(b_resize), app));

    app->edit_bar = gtk_revealer_new();
    gtk_revealer_set_child(GTK_REVEALER(app->edit_bar), bar);
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->edit_bar), FALSE);
    gtk_revealer_set_transition_duration(GTK_REVEALER(app->edit_bar), 150);

    /* crop bar: the shape presets, then cancel / crop */
    app->crop_bar = osd_bar();
    GtkWidget *cl = gtk_label_new(T("Shape", "비율"));
    gtk_widget_set_margin_start(cl, 10);
    gtk_widget_set_margin_end(cl, 2);
    gtk_widget_set_tooltip_text(cl, T("Drag the corners or edges; drag inside to move the box",
                                      "모서리나 변을 끌어 조절하고, 안쪽을 끌어 옮깁니다"));
    gtk_box_append(GTK_BOX(app->crop_bar), cl);
    const char *asp[N_ASPECT] = { T("Free", "자유"), T("Original", "원본"), "1:1", "4:3", "16:9" };
    GtkToggleButton *afirst = NULL;
    for (int i = 0; i < N_ASPECT; i++) {
        GtkWidget *b = gtk_toggle_button_new_with_label(asp[i]);
        gtk_widget_set_focus_on_click(b, FALSE);
        gtk_widget_add_css_class(b, "text-button");
        if (afirst) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), afirst);
        else afirst = GTK_TOGGLE_BUTTON(b);
        g_signal_connect(b, "toggled", G_CALLBACK(on_aspect), app);
        app->aspect_btn[i] = b;
        gtk_box_append(GTK_BOX(app->crop_bar), b);
    }
    gtk_box_append(GTK_BOX(app->crop_bar), sep());
    GtkWidget *cc = gtk_button_new_with_label(T("Cancel", "취소"));
    g_signal_connect(cc, "clicked", G_CALLBACK(b_crop_cancel), app);
    gtk_box_append(GTK_BOX(app->crop_bar), cc);
    app->crop_apply = gtk_button_new_with_label(T("Crop", "자르기"));
    gtk_widget_add_css_class(app->crop_apply, "suggested-action");
    gtk_widget_set_tooltip_text(app->crop_apply, T("Crop to the box (Enter)", "상자대로 자르기 (Enter)"));
    g_signal_connect(app->crop_apply, "clicked", G_CALLBACK(b_crop_apply), app);
    gtk_box_append(GTK_BOX(app->crop_bar), app->crop_apply);
    gtk_overlay_add_overlay(GTK_OVERLAY(app->overlay), app->crop_bar);

    /* adjust bar */
    app->adj_bar = osd_bar();
    gtk_orientable_set_orientation(GTK_ORIENTABLE(app->adj_bar), GTK_ORIENTATION_VERTICAL);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_margin_start(grid, 12);
    gtk_widget_set_margin_end(grid, 12);
    gtk_widget_set_margin_top(grid, 6);
    const char *an[3] = { T("Brightness", "밝기"), T("Contrast", "대비"), T("Saturation", "채도") };
    for (int i = 0; i < 3; i++) {
        GtkWidget *l = gtk_label_new(an[i]);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_grid_attach(GTK_GRID(grid), l, 0, i, 1, 1);
        GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -100, 100, 1);
        gtk_scale_add_mark(GTK_SCALE(s), 0, GTK_POS_BOTTOM, NULL);
        gtk_scale_set_draw_value(GTK_SCALE(s), TRUE);
        gtk_scale_set_value_pos(GTK_SCALE(s), GTK_POS_RIGHT);
        gtk_widget_set_size_request(s, 280, -1);
        g_signal_connect(s, "value-changed", G_CALLBACK(on_adj_changed), app);
        gtk_grid_attach(GTK_GRID(grid), s, 1, i, 1, 1);
        app->adj_scale_w[i] = s;
    }
    gtk_box_append(GTK_BOX(app->adj_bar), grid);
    GtkWidget *ar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(ar, GTK_ALIGN_END);
    gtk_widget_set_margin_end(ar, 6);
    gtk_widget_set_margin_bottom(ar, 4);
    GtkWidget *rs = gtk_button_new_with_label(T("Reset", "초기화"));
    g_signal_connect(rs, "clicked", G_CALLBACK(b_adj_reset), app);
    GtkWidget *ac = gtk_button_new_with_label(T("Cancel", "취소"));
    g_signal_connect(ac, "clicked", G_CALLBACK(b_adj_cancel), app);
    GtkWidget *aa = gtk_button_new_with_label(T("Apply", "적용"));
    gtk_widget_add_css_class(aa, "suggested-action");
    g_signal_connect(aa, "clicked", G_CALLBACK(b_adj_apply), app);
    gtk_box_append(GTK_BOX(ar), rs);
    gtk_box_append(GTK_BOX(ar), ac);
    gtk_box_append(GTK_BOX(ar), aa);
    gtk_box_append(GTK_BOX(app->adj_bar), ar);
    gtk_overlay_add_overlay(GTK_OVERLAY(app->overlay), app->adj_bar);

    /* text entry, floated next to where the text goes */
    app->text_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(app->text_box, "lp-photos-textbox");
    gtk_widget_set_halign(app->text_box, GTK_ALIGN_START);
    gtk_widget_set_valign(app->text_box, GTK_ALIGN_START);
    app->text_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->text_entry), T("Type text, then Enter", "글자를 쓰고 Enter"));
    gtk_widget_set_size_request(app->text_entry, 240, -1);
    g_signal_connect(app->text_entry, "activate", G_CALLBACK(on_text_activate), app);
    g_signal_connect(app->text_entry, "changed", G_CALLBACK(on_text_changed), app);
    GtkWidget *tadd = gtk_button_new_with_label(T("Add", "넣기"));
    gtk_widget_add_css_class(tadd, "suggested-action");
    g_signal_connect(tadd, "clicked", G_CALLBACK(on_text_add), app);
    gtk_box_append(GTK_BOX(app->text_box), app->text_entry);
    gtk_box_append(GTK_BOX(app->text_box), tadd);
    gtk_widget_set_visible(app->text_box, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(app->overlay), app->text_box);

    /* initial selections */
    syncing_aspect(app);
    edit_set_tool(app, app->tool);
    edit_set_color(app, app->swatch_sel >= 0 ? app->swatch_sel : 2);
    edit_set_width(app, app->width_idx);
    edit_update_buttons(app);
}

/* ── entering and leaving ─────────────────────────────────────────── */

void edit_enter(app_t *app)
{
    if (!app->img || app->editing || app->loading) return;
    viewer_prepare_edit(app);
    app->editing = TRUE;
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->edit_bar), TRUE);
    gtk_widget_set_visible(app->hb_view_start, FALSE);
    gtk_widget_set_visible(app->hb_view_end, FALSE);
    gtk_widget_set_visible(app->hb_edit_start, TRUE);
    gtk_widget_set_visible(app->hb_edit_end, TRUE);
    if (app->tool == TOOL_CROP) crop_reset(app);
    crop_update_bar(app);
    edit_update_buttons(app);
    title_update(app);
    view_cursor(app);
    view_queue(app);
}

void edit_leave_now(app_t *app)
{
    if (!app->editing) return;
    if (app->text_active) text_hide(app);
    if (app->adjusting) adjust_end(app);
    app->crop_has = FALSE;
    app->stroking = FALSE;
    app->editing = FALSE;
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->edit_bar), FALSE);
    gtk_widget_set_visible(app->crop_bar, FALSE);
    gtk_widget_set_visible(app->hb_edit_start, FALSE);
    gtk_widget_set_visible(app->hb_edit_end, FALSE);
    gtk_widget_set_visible(app->hb_view_start, TRUE);
    gtk_widget_set_visible(app->hb_view_end, TRUE);
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->navbar), TRUE);
    title_update(app);
    view_cursor(app);
    view_queue(app);
}

void edit_leave(app_t *app)
{
    if (!app->editing) return;
    if (app->text_active)
        edit_text_commit(app, gtk_editable_get_text(GTK_EDITABLE(app->text_entry)));
    if (edit_dirty(app)) ask_unsaved(app, cont_leave_editor, NULL, NULL);
    else edit_leave_now(app);
}

gboolean edit_key(app_t *app, guint kv, GdkModifierType mods)
{
    gboolean ctrl = (mods & GDK_CONTROL_MASK) != 0, shift = (mods & GDK_SHIFT_MASK) != 0;
    guint low = gdk_keyval_to_lower(kv);
    if (ctrl) {
        switch (low) {
        case GDK_KEY_z: if (shift) edit_redo(app); else edit_undo(app); return TRUE;
        case GDK_KEY_y: edit_redo(app); return TRUE;
        case GDK_KEY_s: cont_drop(app); if (shift) edit_save_as(app); else edit_save(app); return TRUE;
        default: return FALSE;
        }
    }
    switch (kv) {
    case GDK_KEY_Escape:
        if (app->text_active) text_hide(app);
        else if (app->adjusting) edit_adjust_cancel(app);
        else if (app->stroking) { app->stroking = FALSE; g_array_set_size(app->pts, 0); view_queue(app); }
        else if (app->tool == TOOL_CROP && gtk_widget_get_sensitive(app->crop_apply))
            edit_crop_cancel(app);
        else edit_leave(app);
        return TRUE;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter:
        if (app->adjusting) edit_adjust_apply(app);
        else if (app->crop_has) edit_crop_apply(app);
        return TRUE;
    /* keys that mean something in the viewer but not here */
    case GDK_KEY_Delete: case GDK_KEY_KP_Delete: case GDK_KEY_F5: case GDK_KEY_F11:
    case GDK_KEY_e: case GDK_KEY_E:
        return TRUE;
    default:
        return FALSE;
    }
}
