/*
 * ui.c - the keyboard on the screen: one layer-shell surface, the keys
 * drawn with cairo, and every touch.
 *
 * ── One surface, never resized while it moves ──
 *
 * The keyboard is a bottom-anchored, full-width wlr-layer-shell surface
 * with an exclusive zone, so windows shrink to sit above it instead of
 * disappearing under it. It slides by cairo_translate inside a surface
 * whose size does not change: moving a layer surface by its margin costs
 * a configure round-trip per frame, and on this machine's CPU-only
 * compositor that is a stutter you can see. The exclusive zone is set
 * once at the start of a show (the application resizes once, while the
 * keyboard slides over the space it left) and cleared once at the end
 * of a hide.
 *
 * The surface is taller than the keyboard by one row: the key preview
 * bubble and the long-press menu of the top row have to go above the
 * keyboard, and a layer surface cannot draw outside itself. That strip
 * is transparent and is never in the input region, so it does not eat
 * touches meant for the window under it. The input region is always
 * exactly what is visible of the keyboard, and it is emptied the moment
 * a hide starts: the person has decided, and the application underneath
 * gets the next touch even while the keyboard is still on its way down.
 *
 * The surface is created the first time the keyboard is shown, not at
 * start-up, and unmapped (not destroyed) after every hide, so a keyboard
 * that is not on the screen costs the compositor nothing to draw.
 *
 * ── The close key ──
 *
 * The owner's complaint about on-screen keyboards is that closing one is
 * always forgotten, so here it cannot be missed: the top-right corner of
 * the keyboard is an accent-coloured "Close" key, far larger than 48
 * logical pixels, and it is in the same place on every page, in every
 * language, during every animation. Its hit area runs to the screen's
 * corner (Fitts's law: a corner is the easiest target there is). The
 * top strip can also be dragged down - it follows the finger, and a
 * flick or a drag past half way closes - and `lp-osk hide` does it from
 * anywhere.
 *
 * ── Touch ──
 *
 * GTK 3 hands us each finger as its own touch sequence, and every one is
 * tracked separately, so two thumbs typing fast both register. A key
 * lights on touch-down with no delay; only the fade back is animated. A
 * character is typed on release, which lets a finger that landed on the
 * wrong key slide onto the right one - unless another finger goes down
 * first, which types it at once, because fast typists lift the first
 * finger after the second has landed and the order must survive that.
 * GDK also turns the first finger into emulated pointer events; those
 * are ignored, or every tap would type twice. Nothing depends on hover.
 *
 * The test mode (`lp-osk test ...`, only when started with LP_OSK_TEST=1)
 * goes through ui_test_touch into exactly the functions a real finger
 * reaches, so what the tests type is what a finger would type.
 *
 * ── Drawing ──
 *
 * The keys are rendered once per layout into an image surface at the
 * output's scale, and each frame paints that image and whatever is lit
 * on top of it. Pango text for sixty keys at 4K is several milliseconds;
 * painting one image is about one. Frames that only light a key redraw
 * that key's rectangle and nothing else.
 */
#include "osk.h"

#include <gtk-layer-shell.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <string.h>

#include "lp-i18n.h"
#include "lp-motion.h"

/* ── the keys ─────────────────────────────────────────────────────── */

typedef enum {
    K_TEXT,      /* types t (or st when shifted); a..z are jamo in Korean */
    K_BKSP, K_ENTER, K_SPACE, K_TAB, K_ESC, K_CODE,
    K_SHIFT, K_CAPS, K_CTRL, K_ALT, K_LANG, K_PAGE,
    K_CLOSE, K_GRIP, K_CHIP, K_STRIP,   /* the top strip */
} Kind;

typedef struct {
    const char *name;    /* what tests and `describe` call it */
    Kind        kind;
    const char *t, *st;  /* K_TEXT: text plain / shifted; others: label */
    double      w;       /* width in key units; a row is 15 */
    uint16_t    code;    /* K_CODE: the evdev key sent */
    const char *alts;    /* long-press alternates, space separated */
} KeyDef;

#define END { NULL, K_TEXT, NULL, NULL, 0, 0, NULL }
#define L(c, C, alts) { c, K_TEXT, c, C, 1, 0, alts }
#define S(n, c, C, alts) { n, K_TEXT, c, C, 1, 0, alts }
#define ARROW(n, code) { n, K_CODE, NULL, NULL, 1, code, NULL }

/* A laptop keyboard's rows, because the terminal is where this machine
 * gets used and a phone layout has no Esc, Tab, Ctrl, Alt or arrows.
 * Every row is 15 units, so a key is the same size on every page and a
 * page switch never moves anything that stays. */
static const KeyDef LETTERS[] = {
    { "esc", K_ESC, "Esc", NULL, 1, 0, NULL },
    S("1", "1", "!", "¹ ½ ⅓ ¼"), S("2", "2", "@", "² ⅔"),
    S("3", "3", "#", "³ ¾"), S("4", "4", "$", "₩ € £ ¥ ¢"),
    S("5", "5", "%", "‰"), S("6", "6", "^", NULL), S("7", "7", "&", NULL),
    S("8", "8", "*", "•"), S("9", "9", "(", NULL), S("0", "0", ")", "° ∅"),
    S("minus", "-", "_", "– — ·"), S("equal", "=", "+", "≠ ± ≈"),
    { "backspace", K_BKSP, NULL, NULL, 2, 0, NULL }, END,

    { "tab", K_TAB, "Tab", NULL, 1.5, 0, NULL },
    L("q", "Q", NULL), L("w", "W", NULL), L("e", "E", "é è ê ë ē ę"),
    L("r", "R", NULL), L("t", "T", "þ"), L("y", "Y", "ý ÿ"),
    L("u", "U", "ú ù û ü ū"), L("i", "I", "í ì î ï ī"),
    L("o", "O", "ó ò ô ö õ ø œ ō"), L("p", "P", NULL),
    S("lbracket", "[", "{", NULL), S("rbracket", "]", "}", NULL),
    { "backslash", K_TEXT, "\\", "|", 1.5, 0, NULL }, END,

    { "caps", K_CAPS, "Caps", NULL, 1.75, 0, NULL },
    L("a", "A", "á à â ä ã å æ ā"), L("s", "S", "ß ś š"),
    L("d", "D", "ð"), L("f", "F", NULL), L("g", "G", "ğ"),
    L("h", "H", NULL), L("j", "J", NULL), L("k", "K", NULL),
    L("l", "L", "ł"), S("semicolon", ";", ":", NULL),
    S("apostrophe", "'", "\"", "’ ‘ ” “ `"),
    { "enter", K_ENTER, NULL, NULL, 2.25, 0, NULL }, END,

    { "shift", K_SHIFT, NULL, NULL, 2.25, 0, NULL },
    L("z", "Z", "ž ź ż"), L("x", "X", NULL), L("c", "C", "ç ć č"),
    L("v", "V", NULL), L("b", "B", NULL), L("n", "N", "ñ ń"),
    L("m", "M", NULL), S("comma", ",", "<", NULL),
    S("period", ".", ">", "… ! ?"), S("slash", "/", "?", "\\ ¿"),
    ARROW("up", KEY_UP),
    { "rshift", K_SHIFT, NULL, NULL, 1.75, 0, NULL }, END,

    { "ctrl", K_CTRL, "Ctrl", NULL, 1.5, 0, NULL },
    { "alt", K_ALT, "Alt", NULL, 1.5, 0, NULL },
    { "sym", K_PAGE, "123", NULL, 1.5, 0, NULL },
    { "lang", K_LANG, NULL, NULL, 1.5, 0, NULL },
    { "space", K_SPACE, NULL, NULL, 6, 0, NULL },
    ARROW("left", KEY_LEFT), ARROW("down", KEY_DOWN),
    ARROW("right", KEY_RIGHT), END,
    END,
};

/* The second page: what a laptop keyboard reaches with Shift or Fn. */
static const KeyDef SYMBOLS[] = {
    { "esc", K_ESC, "Esc", NULL, 1, 0, NULL },
    S("exclam", "!", "!", "¡"), S("at", "@", "@", NULL),
    S("numbersign", "#", "#", NULL), S("dollar", "$", "$", "₩ € £ ¥"),
    S("percent", "%", "%", "‰"), S("asciicircum", "^", "^", NULL),
    S("ampersand", "&", "&", NULL), S("asterisk", "*", "*", NULL),
    S("parenleft", "(", "(", NULL), S("parenright", ")", ")", NULL),
    S("underscore", "_", "_", NULL), S("plus", "+", "+", "±"),
    { "backspace", K_BKSP, NULL, NULL, 2, 0, NULL }, END,

    { "tab", K_TAB, "Tab", NULL, 1.5, 0, NULL },
    { "f1", K_CODE, "F1", NULL, 1, KEY_F1, NULL },
    { "f2", K_CODE, "F2", NULL, 1, KEY_F2, NULL },
    { "f3", K_CODE, "F3", NULL, 1, KEY_F3, NULL },
    { "f4", K_CODE, "F4", NULL, 1, KEY_F4, NULL },
    { "f5", K_CODE, "F5", NULL, 1, KEY_F5, NULL },
    { "f6", K_CODE, "F6", NULL, 1, KEY_F6, NULL },
    { "f7", K_CODE, "F7", NULL, 1, KEY_F7, NULL },
    { "f8", K_CODE, "F8", NULL, 1, KEY_F8, NULL },
    { "f9", K_CODE, "F9", NULL, 1, KEY_F9, NULL },
    { "f10", K_CODE, "F10", NULL, 1, KEY_F10, NULL },
    { "f11", K_CODE, "F11", NULL, 1, KEY_F11, NULL },
    { "f12", K_CODE, "F12", NULL, 1, KEY_F12, NULL },
    { "delete", K_CODE, "Del", NULL, 1.5, KEY_DELETE, NULL }, END,

    { "home", K_CODE, "Home", NULL, 1.75, KEY_HOME, NULL },
    S("grave", "`", "`", NULL), S("asciitilde", "~", "~", NULL),
    S("bar", "|", "|", "¦"), S("backslash", "\\", "\\", NULL),
    S("braceleft", "{", "{", NULL), S("braceright", "}", "}", NULL),
    S("less", "<", "<", "« ≤"), S("greater", ">", ">", "» ≥"),
    S("euro", "€", "€", NULL), S("sterling", "£", "£", NULL),
    S("won", "₩", "₩", NULL),
    { "enter", K_ENTER, NULL, NULL, 2.25, 0, NULL }, END,

    { "end", K_CODE, "End", NULL, 2.25, KEY_END, NULL },
    S("degree", "°", "°", NULL), S("multiply", "×", "×", NULL),
    S("division", "÷", "÷", NULL), S("plusminus", "±", "±", NULL),
    S("ellipsis", "…", "…", NULL), S("bullet", "•", "•", NULL),
    S("middot", "·", "·", NULL), S("questiondown", "¿", "¿", NULL),
    S("exclamdown", "¡", "¡", NULL),
    { "pgup", K_CODE, "PgUp", NULL, 1, KEY_PAGEUP, NULL },
    ARROW("up", KEY_UP),
    { "pgdn", K_CODE, "PgDn", NULL, 1.75, KEY_PAGEDOWN, NULL }, END,

    { "ctrl", K_CTRL, "Ctrl", NULL, 1.5, 0, NULL },
    { "alt", K_ALT, "Alt", NULL, 1.5, 0, NULL },
    { "abc", K_PAGE, "ABC", NULL, 1.5, 0, NULL },
    { "lang", K_LANG, NULL, NULL, 1.5, 0, NULL },
    { "space", K_SPACE, NULL, NULL, 6, 0, NULL },
    ARROW("left", KEY_LEFT), ARROW("down", KEY_DOWN),
    ARROW("right", KEY_RIGHT), END,
    END,
};

static const KeyDef STRIP_DEFS[] = {
    { "chip", K_CHIP, NULL, NULL, 0, 0, NULL },
    { "grip", K_GRIP, NULL, NULL, 0, 0, NULL },
    { "close", K_CLOSE, NULL, NULL, 0, 0, NULL },
    { "strip", K_STRIP, NULL, NULL, 0, 0, NULL },
};
enum { S_CHIP, S_GRIP, S_CLOSE, S_STRIP, N_STRIP };

#define ROWS    5
#define UNITS   15.0
#define MAXKEYS 80

typedef struct {
    const KeyDef *d;
    double   x, y, w, h;   /* keyboard coordinates, logical px */
    LpSpring hl;           /* 1 lit, 0 not: lit instantly, fades out */
    gboolean drawn_lit;    /* lit in the last frame drawn */
    gint64   last_up;      /* for the chatter filter */
} Key;

enum { PAGE_LETTERS, PAGE_SYMBOLS, N_PAGES };
static Key keys[N_PAGES][MAXKEYS];
static int nkeys[N_PAGES];
static Key strip[N_STRIP];
static int page = PAGE_LETTERS;

/* ── state ────────────────────────────────────────────────────────── */

enum { M_OFF, M_ONCE, M_LOCK };
typedef struct {
    int      mode;       /* M_* */
    int      held;       /* fingers on the key */
    gboolean used;       /* something was typed while it was held */
    gint64   tap_us;     /* last tap, for double-tap lock */
} Mod;
static Mod m_shift, m_ctrl, m_alt;

typedef struct {
    gboolean used;
    gintptr  id;          /* GdkEventSequence, test id, or the mouse */
    Key     *key;         /* under the finger now; NULL if none */
    double   x0, y0, x, y;/* surface coordinates */
    gint64   t0;          /* monotonic us of the down */
    gboolean committed;   /* its character has been typed already */
    gboolean shift, caps, ctrl, alt;   /* modifiers as they were at down */
    guint    timer;       /* long-press or repeat */
    gboolean repeating;
    gboolean drag;        /* this finger is dragging the keyboard down */
    gboolean resize;      /* ...or its height */
    int      pv;          /* preview slot, -1 */
    LpVelocity vt;
} Touch;
#define MAXTOUCH 10
static Touch touches[MAXTOUCH];

/* Key preview bubbles. A slot outlives its finger while it fades. */
typedef struct {
    Key     *key;
    char     text[24];
    LpSpring s;
    gboolean used;
    gboolean drawn;
    double   bx, by, bw, bh;   /* last drawn rectangle, surface coords */
} Preview;
static Preview previews[MAXTOUCH];

static struct {
    gboolean open;        /* taking input (the spring may still be fading) */
    gintptr  id;
    Key     *key;
    char     items[12][24];
    int      n, sel;
    double   x, y, cw, ch;   /* keyboard coordinates */
    LpSpring s;
    gboolean drawn;
} menu;

static GtkWidget *win, *area;
static LpMotion  *motion;
static LpSpring   slide;       /* 0 hidden, 1 shown */
static LpSpring   xfade;       /* 0 old layout, 1 new layout */
static gboolean   want_shown, mapped;
static double     kb_h = 360;  /* keyboard height, logical px */
static double     body_h;      /* drawn height: kb_h, or the resize in progress */
static double     strip_h, row_h, unit, pad = 6, gap = 5;
static int        alloc_w, alloc_h;
static double     mon_h = 1080;
static cairo_surface_t *cache, *old_cache;
static int        cache_w, cache_h, cache_scale;
static uint32_t   purpose;
static int        last_ir_y = -2;       /* input region last sent */
static gboolean   drag_on, resize_on;
static double     drag_x0;
static double     resize_h0;

#define TEXT_FONT "Pretendard, Noto Sans CJK KR, Noto Sans, DejaVu Sans"

static void redraw_all(void);
static void relabel(gboolean crossfade);
static void finish_hide(void);
static void apply_size(void);

/* ── small things ─────────────────────────────────────────────────── */

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void rgb(cairo_t *cr, uint32_t c, double a)
{
    cairo_set_source_rgba(cr, ((c >> 16) & 255) / 255.0,
                          ((c >> 8) & 255) / 255.0, (c & 255) / 255.0, a);
}

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    r = MIN(r, MIN(w, h) / 2);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

/* The design system's dark greys (desktop/theme/shell.css) and accent. */
#define C_BG      0x1a1a1a
#define C_LINE    0x333333
#define C_KEY     0x363636
#define C_SPECIAL 0x262626
#define C_T1      0xe8e8e8
#define C_T2      0xa8a8a8
#define C_T3      0x6a6a6a
#define C_ACCENT  0xe95420
#define C_BUBBLE  0x4a4a4a

static gboolean is_letter(const KeyDef *d)
{
    return d->kind == K_TEXT && d->t && d->t[0] >= 'a' && d->t[0] <= 'z' &&
           !d->t[1];
}

static gboolean shifted(void)
{
    return m_shift.held > 0 || m_shift.mode != M_OFF;
}

static gboolean korean(void)
{
    return type_lang() == LANG_KO;
}

static gboolean secret_field(void)
{
    return purpose == 8 || purpose == 9;   /* password, pin */
}

/* What a key shows (and a K_TEXT key types) in the current state. */
static void key_text(const KeyDef *d, gboolean sh, gboolean caps, char *out,
                     size_t n)
{
    out[0] = 0;
    if (d->kind != K_TEXT)
        return;
    if (is_letter(d) && korean()) {
        uint32_t j = type_ko_jamo(d->t[0], sh);
        out[g_unichar_to_utf8(j, out)] = 0;
        return;
    }
    gboolean up = is_letter(d) ? (sh != caps) : sh;
    g_strlcpy(out, up && d->st ? d->st : d->t, n);
}

/* In Korean the letter keys hold one alternate: the other half of the
 * Shift pair (ㄱ/ㄲ, ㅐ/ㅒ...), as Korean phone keyboards do, so a double
 * consonant never needs the Shift key. The Latin accents mean nothing
 * there. Returns the alternate, 0 when the key has none. */
static uint32_t ko_alt(const KeyDef *d, gboolean sh)
{
    if (!is_letter(d) || !korean())
        return 0;
    uint32_t plain = type_ko_jamo(d->t[0], sh), other = type_ko_jamo(d->t[0], !sh);
    return plain != other ? other : 0;
}

static gboolean has_alts(const KeyDef *d)
{
    if (secret_field())
        return FALSE;
    if (is_letter(d) && korean())
        return ko_alt(d, FALSE) != 0;
    return d->alts != NULL;
}

static gboolean repeats(const KeyDef *d)
{
    if (d->kind == K_BKSP)
        return TRUE;
    if (d->kind != K_CODE)
        return FALSE;
    switch (d->code) {
    case KEY_UP: case KEY_DOWN: case KEY_LEFT: case KEY_RIGHT:
    case KEY_DELETE: case KEY_PAGEUP: case KEY_PAGEDOWN:
        return TRUE;
    }
    return FALSE;
}

/* Characters are typed on release; these act on touch-down, because
 * waiting for the finger to lift before deleting feels broken. */
static gboolean acts_on_down(const KeyDef *d)
{
    return repeats(d);
}

/* ── geometry ─────────────────────────────────────────────────────── */

static double body_top(void);

static void layout_page(int p, const KeyDef *defs)
{
    int n = 0, row = 0;
    double y = strip_h + pad;
    const KeyDef *d = defs;
    while (row < ROWS && d->name) {
        double x = pad;
        for (; d->name; d++) {
            Key *k = &keys[p][n];
            if (k->d != d) {
                memset(k, 0, sizeof *k);
                k->d = d;
                lp_spring_init(&k->hl, LP_SPRING_PRESS, 0);
                if (motion)
                    lp_motion_add(motion, &k->hl);
            }
            k->x = x + gap / 2;
            k->y = y + gap / 2;
            k->w = d->w * unit - gap;
            k->h = row_h - gap;
            x += d->w * unit;
            n++;
        }
        d++;        /* the row's END */
        y += row_h;
        row++;
    }
    nkeys[p] = n;
}

static void layout(void)
{
    double W = alloc_w > 0 ? alloc_w : 1280;
    strip_h = MAX(56.0, round(kb_h * 0.15));
    row_h = (kb_h - strip_h - 2 * pad) / ROWS;
    unit = (W - 2 * pad) / UNITS;
    layout_page(PAGE_LETTERS, LETTERS);
    layout_page(PAGE_SYMBOLS, SYMBOLS);

    for (int i = 0; i < N_STRIP; i++) {
        if (strip[i].d != &STRIP_DEFS[i]) {
            strip[i].d = &STRIP_DEFS[i];
            lp_spring_init(&strip[i].hl, LP_SPRING_PRESS, 0);
            if (motion)
                lp_motion_add(motion, &strip[i].hl);
        }
    }
    /* Close: the top-right corner. At least 1.6 key units or 132px wide
     * and the strip's height less a margin - on this machine about 64
     * logical px tall, well over the 48 a fingertip needs. */
    double cw = MAX(132.0, round(1.6 * unit));
    Key *c = &strip[S_CLOSE];
    c->w = cw; c->h = strip_h - 8; c->x = W - pad - cw; c->y = 4;
    Key *chip = &strip[S_CHIP];
    chip->w = MAX(96.0, round(1.2 * unit)); chip->h = strip_h - 8;
    chip->x = pad; chip->y = 4;
    Key *g = &strip[S_GRIP];
    g->w = 200; g->h = strip_h; g->x = (W - g->w) / 2; g->y = 0;
    Key *s = &strip[S_STRIP];
    s->x = 0; s->y = 0; s->w = W; s->h = strip_h;
}

static double headroom(void)
{
    return MAX(64.0, round(row_h * 1.35));
}

/* Where the keyboard's top edge is, in surface coordinates, this frame. */
static double slide_offset(void)
{
    if (lp_motion_reduced() && !drag_on)
        return 0;
    return round((1.0 - clampd(slide.x, -0.2, 1.0)) * (body_h + 2));
}

static double body_alpha(void)
{
    if (lp_motion_reduced() && !drag_on)
        return clampd(slide.x, 0, 1);
    return 1.0;
}

static double body_top(void)
{
    return alloc_h - body_h + slide_offset();
}

/* ── the cached image of the keys ─────────────────────────────────── */

static void text_at(cairo_t *cr, const char *s, const char *markup,
                    double cx, double cy, double px, gboolean bold,
                    uint32_t color)
{
    PangoLayout *pl = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(TEXT_FONT);
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    if (bold)
        pango_font_description_set_weight(fd, PANGO_WEIGHT_SEMIBOLD);
    pango_layout_set_font_description(pl, fd);
    if (markup)
        pango_layout_set_markup(pl, markup, -1);
    else
        pango_layout_set_text(pl, s, -1);
    int w, h;
    pango_layout_get_pixel_size(pl, &w, &h);
    rgb(cr, color, 1);
    cairo_move_to(cr, round(cx - w / 2.0), round(cy - h / 2.0));
    pango_cairo_show_layout(cr, pl);
    pango_font_description_free(fd);
    g_object_unref(pl);
}

/* Icons are drawn, not typed: whether a font has U+232B is a question
 * about the font, and a keyboard whose backspace is a box is broken. */
static void icon_shift(cairo_t *cr, double cx, double cy, double s,
                       gboolean filled, uint32_t color)
{
    cairo_new_path(cr);
    cairo_move_to(cr, cx, cy - s * 0.55);
    cairo_line_to(cr, cx + s * 0.5, cy);
    cairo_line_to(cr, cx + s * 0.22, cy);
    cairo_line_to(cr, cx + s * 0.22, cy + s * 0.45);
    cairo_line_to(cr, cx - s * 0.22, cy + s * 0.45);
    cairo_line_to(cr, cx - s * 0.22, cy);
    cairo_line_to(cr, cx - s * 0.5, cy);
    cairo_close_path(cr);
    rgb(cr, color, 1);
    cairo_set_line_width(cr, 2);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (filled)
        cairo_fill_preserve(cr);
    cairo_stroke(cr);
}

static void icon_backspace(cairo_t *cr, double cx, double cy, double s,
                           uint32_t color)
{
    double w = s * 1.3, h = s * 0.8, x = cx - w / 2, y = cy - h / 2;
    cairo_new_path(cr);
    cairo_move_to(cr, x + h * 0.45, y);
    cairo_line_to(cr, x + w, y);
    cairo_line_to(cr, x + w, y + h);
    cairo_line_to(cr, x + h * 0.45, y + h);
    cairo_line_to(cr, x, cy);
    cairo_close_path(cr);
    double m = h * 0.28, bx = x + w * 0.6;
    cairo_move_to(cr, bx - m, cy - m);
    cairo_line_to(cr, bx + m, cy + m);
    cairo_move_to(cr, bx + m, cy - m);
    cairo_line_to(cr, bx - m, cy + m);
    rgb(cr, color, 1);
    cairo_set_line_width(cr, 2);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cr);
}

static void icon_enter(cairo_t *cr, double cx, double cy, double s,
                       uint32_t color)
{
    double w = s * 1.1, h = s * 0.6;
    cairo_new_path(cr);
    cairo_move_to(cr, cx + w / 2, cy - h / 2);
    cairo_line_to(cr, cx + w / 2, cy + h / 4);
    cairo_line_to(cr, cx - w / 2, cy + h / 4);
    cairo_move_to(cr, cx - w / 2 + h * 0.4, cy + h / 4 - h * 0.4);
    cairo_line_to(cr, cx - w / 2, cy + h / 4);
    cairo_line_to(cr, cx - w / 2 + h * 0.4, cy + h / 4 + h * 0.4);
    rgb(cr, color, 1);
    cairo_set_line_width(cr, 2.2);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cr);
}

/* A chevron pointing dir: 0 up, 1 right, 2 down, 3 left. */
static void icon_chevron(cairo_t *cr, double cx, double cy, double s, int dir,
                         double lw, uint32_t color)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, dir * G_PI / 2);
    cairo_new_path(cr);
    cairo_move_to(cr, -s / 2, s / 4);
    cairo_line_to(cr, 0, -s / 4);
    cairo_line_to(cr, s / 2, s / 4);
    rgb(cr, color, 1);
    cairo_set_line_width(cr, lw);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/* "Hide the keyboard": a small keyboard outline over a down chevron. */
static void icon_hide(cairo_t *cr, double cx, double cy, double s,
                      uint32_t color)
{
    double w = s * 1.2, h = s * 0.62, x = cx - w / 2, y = cy - s * 0.55;
    rgb(cr, color, 1);
    cairo_set_line_width(cr, 2);
    rrect(cr, x, y, w, h, 3);
    cairo_stroke(cr);
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 5; c++) {
            cairo_rectangle(cr, x + w * (0.12 + c * 0.17), y + h * (0.22 + r * 0.3),
                            w * 0.08, h * 0.14);
        }
    cairo_rectangle(cr, x + w * 0.3, y + h * 0.78 - h * 0.1, w * 0.4, h * 0.1);
    cairo_fill(cr);
    icon_chevron(cr, cx, cy + s * 0.38, s * 0.5, 2, 2.4, color);
}

static uint32_t mod_color(const Mod *m)
{
    return m->mode == M_LOCK ? 0xffffff : m->mode == M_ONCE || m->held ? C_ACCENT : C_T1;
}

static void draw_key(cairo_t *cr, const Key *k)
{
    const KeyDef *d = k->d;
    double cx = k->x + k->w / 2, cy = k->y + k->h / 2;
    double fs = row_h * 0.40, fs_small = MIN(row_h * 0.28, 18.0);
    gboolean special = d->kind != K_TEXT && d->kind != K_SPACE;
    uint32_t bg = special ? C_SPECIAL : C_KEY;
    const Mod *m = d->kind == K_SHIFT || d->kind == K_CAPS ? &m_shift :
                   d->kind == K_CTRL ? &m_ctrl : d->kind == K_ALT ? &m_alt : NULL;
    if (m && m->mode == M_LOCK)
        bg = C_ACCENT;
    rrect(cr, k->x, k->y, k->w, k->h, 8);
    rgb(cr, bg, 1);
    cairo_fill(cr);

    char buf[32];
    switch (d->kind) {
    case K_TEXT: {
        key_text(d, shifted(), m_shift.mode == M_LOCK, buf, sizeof buf);
        gboolean big = g_utf8_strlen(buf, -1) <= 1;
        text_at(cr, buf, NULL, cx, cy, big ? fs : fs * 0.7, FALSE, C_T1);
        /* The shifted character, small in the corner, on keys where it is
         * not simply the capital: that is how a laptop keycap reads. */
        if (!is_letter(d) && d->st && strcmp(d->st, d->t) && !shifted())
            text_at(cr, d->st, NULL, k->x + k->w - 12, k->y + 13,
                    fs_small * 0.85, FALSE, C_T3);
        if (is_letter(d) && korean() && !shifted()) {
            /* Korean keys also carry their Latin letter, as the physical
             * keycaps here do, so the two layouts can be learnt together. */
            char up[2] = { (char)(d->t[0] - 'a' + 'A'), 0 };
            text_at(cr, up, NULL, k->x + k->w - 12, k->y + 13,
                    fs_small * 0.85, FALSE, C_T3);
        }
        if (has_alts(d)) {
            /* a dot says "hold me for more" */
            cairo_arc(cr, k->x + k->w / 2, k->y + k->h - 7, 1.8, 0, 2 * G_PI);
            rgb(cr, C_T3, 1);
            cairo_fill(cr);
        }
        break;
    }
    case K_SPACE: {
        const char *name = korean() ? T("Korean", "한국어 두벌식")
                                    : T("English (US)", "영어(미국)");
        text_at(cr, name, NULL, cx, cy, fs_small, FALSE, C_T2);
        break;
    }
    case K_BKSP:
        icon_backspace(cr, cx, cy, row_h * 0.42, C_T1);
        break;
    case K_ENTER:
        icon_enter(cr, cx, cy, row_h * 0.42, C_T1);
        break;
    case K_SHIFT:
        icon_shift(cr, cx, cy, row_h * 0.42, m_shift.mode != M_OFF || m_shift.held,
                   mod_color(&m_shift));
        if (m_shift.mode == M_LOCK) {
            cairo_rectangle(cr, cx - row_h * 0.12, cy + row_h * 0.27,
                            row_h * 0.24, 2.5);
            rgb(cr, 0xffffff, 1);
            cairo_fill(cr);
        }
        break;
    case K_CAPS:
        text_at(cr, d->t, NULL, cx, cy, fs_small, FALSE,
                m_shift.mode == M_LOCK ? 0xffffff : C_T1);
        break;
    case K_CTRL:
    case K_ALT:
        text_at(cr, d->t, NULL, cx, cy, fs_small, TRUE, mod_color(m));
        break;
    case K_LANG: {
        /* 한/영 with the language in use lit. */
        char mk[160];
        g_snprintf(mk, sizeof mk,
                   "<span foreground='#%06x'>한</span><span foreground='#%06x'>/</span>"
                   "<span foreground='#%06x'>영</span>",
                   korean() ? C_T1 : C_T3, C_T3, korean() ? C_T3 : C_T1);
        text_at(cr, NULL, mk, cx, cy, fs_small * 1.1, TRUE, C_T1);
        break;
    }
    case K_CODE:
        switch (d->code) {
        case KEY_UP:    icon_chevron(cr, cx, cy, row_h * 0.34, 0, 2.4, C_T1); break;
        case KEY_RIGHT: icon_chevron(cr, cx, cy, row_h * 0.34, 1, 2.4, C_T1); break;
        case KEY_DOWN:  icon_chevron(cr, cx, cy, row_h * 0.34, 2, 2.4, C_T1); break;
        case KEY_LEFT:  icon_chevron(cr, cx, cy, row_h * 0.34, 3, 2.4, C_T1); break;
        default:        text_at(cr, d->t, NULL, cx, cy, fs_small, FALSE, C_T1);
        }
        break;
    default:
        text_at(cr, d->t, NULL, cx, cy, fs_small, FALSE, C_T1);
    }
}

static void draw_strip(cairo_t *cr)
{
    /* the grip: the handle that says "this edge moves" */
    Key *g = &strip[S_GRIP];
    rrect(cr, g->x + g->w / 2 - 24, 9, 48, 5, 2.5);
    rgb(cr, C_T3, 1);
    cairo_fill(cr);

    Key *chip = &strip[S_CHIP];
    rrect(cr, chip->x, chip->y, chip->w, chip->h, chip->h / 2);
    rgb(cr, C_SPECIAL, 1);
    cairo_fill(cr);
    text_at(cr, korean() ? "한" : "EN", NULL, chip->x + chip->w / 2,
            chip->y + chip->h / 2, MIN(strip_h * 0.34, 20.0), TRUE, C_T1);

    /* Close. Accent-filled, icon and word, the one thing on the keyboard
     * that is not grey. */
    Key *c = &strip[S_CLOSE];
    rrect(cr, c->x, c->y, c->w, c->h, c->h / 2);
    rgb(cr, C_ACCENT, 1);
    cairo_fill(cr);
    double is = MIN(c->h * 0.55, 30.0);
    const char *word = T("Close", "닫기");
    PangoLayout *pl = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(TEXT_FONT);
    pango_font_description_set_absolute_size(fd, MIN(strip_h * 0.32, 19.0) * PANGO_SCALE);
    pango_font_description_set_weight(fd, PANGO_WEIGHT_SEMIBOLD);
    pango_layout_set_font_description(pl, fd);
    pango_layout_set_text(pl, word, -1);
    int tw, th;
    pango_layout_get_pixel_size(pl, &tw, &th);
    double total = is + 10 + tw;
    double x0 = c->x + (c->w - total) / 2;
    icon_hide(cr, x0 + is / 2, c->y + c->h / 2, is, 0xffffff);
    rgb(cr, 0xffffff, 1);
    cairo_move_to(cr, round(x0 + is + 10), round(c->y + (c->h - th) / 2.0));
    pango_cairo_show_layout(cr, pl);
    pango_font_description_free(fd);
    g_object_unref(pl);
}

static void render_cache(void)
{
    GdkWindow *gw = gtk_widget_get_window(area);
    if (!gw || alloc_w <= 0)
        return;
    int scale = gtk_widget_get_scale_factor(area);
    if (cache && (cache_w != alloc_w || cache_h != (int)kb_h ||
                  cache_scale != scale)) {
        cairo_surface_destroy(cache);
        cache = NULL;
    }
    if (!cache) {
        /* GTK 3 takes this size in device pixels, not logical ones (the
         * scale only sets the device scale): at scale 2 a logical-sized
         * image covers a quarter of the keyboard. */
        cache = gdk_window_create_similar_image_surface(gw, CAIRO_FORMAT_ARGB32,
                                                        alloc_w * scale,
                                                        (int)kb_h * scale, scale);
        cache_w = alloc_w;
        cache_h = (int)kb_h;
        cache_scale = scale;
    }
    cairo_t *cr = cairo_create(cache);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    rgb(cr, C_BG, 1);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_rectangle(cr, 0, 0, alloc_w, 1);
    rgb(cr, C_LINE, 1);
    cairo_fill(cr);
    draw_strip(cr);
    for (int i = 0; i < nkeys[page]; i++)
        draw_key(cr, &keys[page][i]);
    cairo_destroy(cr);
}

/* ── drawing a frame ──────────────────────────────────────────────── */

static void lit_rect(const Key *k, GdkRectangle *r)
{
    double top = body_top();
    r->x = (int)floor(k->x) - 1;
    r->y = (int)floor(top + k->y * body_h / kb_h) - 1;
    r->width = (int)ceil(k->w) + 2;
    r->height = (int)ceil(k->h * body_h / kb_h) + 2;
}

static void preview_rect(Preview *p)
{
    Key *k = p->key;
    double top = body_top();
    double bw = MAX(k->w * 1.15, row_h * 1.1), bh = row_h * 1.25;
    double bx = clampd(k->x + k->w / 2 - bw / 2, 2, alloc_w - bw - 2);
    double by = MAX(2.0, top + k->y - bh - 8);
    p->bx = bx; p->by = by; p->bw = bw; p->bh = bh;
}

static void draw_preview(cairo_t *cr, Preview *p)
{
    double a = clampd(p->s.x, 0, 1);
    if (a <= 0.001)
        return;
    preview_rect(p);
    double sc = lp_motion_reduced() ? 1.0 : 0.96 + 0.04 * a;
    cairo_save(cr);
    cairo_translate(cr, p->bx + p->bw / 2, p->by + p->bh);
    cairo_scale(cr, sc, sc);
    cairo_translate(cr, -p->bw / 2, -p->bh);
    cairo_push_group(cr);
    rrect(cr, 0, 0, p->bw, p->bh, 10);
    rgb(cr, C_BUBBLE, 1);
    cairo_fill_preserve(cr);
    rgb(cr, 0x5e5e5e, 1);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    text_at(cr, p->text, NULL, p->bw / 2, p->bh / 2, row_h * 0.62, FALSE, 0xffffff);
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, a);
    cairo_restore(cr);
}

static void menu_rect(GdkRectangle *r)
{
    double top = body_top();
    r->x = (int)floor(menu.x) - 4;
    r->y = (int)floor(top + menu.y) - 4;
    r->width = (int)ceil(menu.cw * menu.n) + 8;
    r->height = (int)ceil(menu.ch) + 8;
}

static void draw_menu(cairo_t *cr)
{
    double a = clampd(menu.s.x, 0, 1);
    if (a <= 0.001 || !menu.key)
        return;
    double top = body_top();
    double mx = menu.x, my = top + menu.y, mw = menu.cw * menu.n, mh = menu.ch;
    /* grows out of the key it belongs to */
    double ox = menu.key->x + menu.key->w / 2, oy = top + menu.key->y;
    double sc = lp_motion_reduced() ? 1.0 : 0.96 + 0.04 * a;
    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, sc, sc);
    cairo_translate(cr, -ox, -oy);
    cairo_push_group(cr);
    rrect(cr, mx, my, mw, mh, 10);
    rgb(cr, 0x2c2c2c, 1);
    cairo_fill_preserve(cr);
    rgb(cr, 0x4a4a4a, 1);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    for (int i = 0; i < menu.n; i++) {
        double cx = mx + i * menu.cw;
        if (i == menu.sel) {
            rrect(cr, cx + 3, my + 3, menu.cw - 6, mh - 6, 8);
            rgb(cr, C_ACCENT, 1);
            cairo_fill(cr);
        }
        text_at(cr, menu.items[i], NULL, cx + menu.cw / 2, my + mh / 2,
                row_h * 0.45, FALSE, i == menu.sel ? 0xffffff : C_T1);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, a);
    cairo_restore(cr);
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)w; (void)data;
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    if (!cache)
        render_cache();
    if (!cache)
        return TRUE;

    double top = body_top();
    double alpha = body_alpha();
    double sy = body_h / kb_h;

    cairo_save(cr);
    cairo_translate(cr, 0, top);
    if (sy != 1.0)
        cairo_scale(cr, 1, sy);   /* only while the grip is dragged */
    if (old_cache && xfade.x < 1.0) {
        double t = clampd(xfade.x, 0, 1);
        cairo_set_source_surface(cr, old_cache, 0, 0);
        cairo_paint_with_alpha(cr, alpha);
        cairo_set_source_surface(cr, cache, 0, 0);
        cairo_paint_with_alpha(cr, alpha * t);
    } else {
        cairo_set_source_surface(cr, cache, 0, 0);
        cairo_paint_with_alpha(cr, alpha);
    }
    /* Lit keys: a white wash over the key, so its label stays readable. */
    for (int s = 0; s < 2; s++) {
        Key *arr = s ? strip : keys[page];
        int n = s ? N_STRIP - 1 : nkeys[page];   /* the drag strip is not lit */
        for (int i = 0; i < n; i++) {
            Key *k = &arr[i];
            k->drawn_lit = k->hl.x > 0.001;
            if (!k->drawn_lit)
                continue;
            double r = (k->d->kind == K_CLOSE || k->d->kind == K_CHIP) ? k->h / 2 : 8;
            rrect(cr, k->x, k->y, k->w, k->h, r);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.20 * clampd(k->hl.x, 0, 1) * alpha);
            cairo_fill(cr);
        }
    }
    cairo_restore(cr);

    for (int i = 0; i < MAXTOUCH; i++) {
        Preview *p = &previews[i];
        p->drawn = p->used && p->s.x > 0.001;
        if (p->drawn)
            draw_preview(cr, p);
    }
    menu.drawn = menu.key && menu.s.x > 0.001;
    if (menu.drawn)
        draw_menu(cr);
    return TRUE;
}

/* Frames that only light or fade keys redraw just those keys. */
static void damage_animating(void)
{
    if (!area)
        return;
    for (int s = 0; s < 2; s++) {
        Key *arr = s ? strip : keys[page];
        int n = s ? N_STRIP : nkeys[page];
        for (int i = 0; i < n; i++) {
            Key *k = &arr[i];
            if (k->hl.moving || k->drawn_lit != (k->hl.x > 0.001)) {
                GdkRectangle r;
                lit_rect(k, &r);
                gtk_widget_queue_draw_area(area, r.x, r.y, r.width, r.height);
            }
        }
    }
    for (int i = 0; i < MAXTOUCH; i++) {
        Preview *p = &previews[i];
        if (!p->used)
            continue;
        if (p->s.moving || p->drawn != (p->s.x > 0.001)) {
            preview_rect(p);
            gtk_widget_queue_draw_area(area, (int)p->bx - 4, (int)p->by - 4,
                                       (int)p->bw + 8, (int)p->bh + 8);
        }
        if (!p->s.moving && p->s.x <= 0.001 && !p->drawn)
            p->used = FALSE;
    }
    if (menu.key && (menu.s.moving || menu.drawn != (menu.s.x > 0.001))) {
        GdkRectangle r;
        menu_rect(&r);
        gtk_widget_queue_draw_area(area, r.x, r.y, r.width, r.height);
    }
    if (xfade.moving && area)
        gtk_widget_queue_draw(area);
}

/* The input region is what can be seen of the keyboard, and nothing
 * while it is leaving. */
static void update_input_region(void)
{
    if (!win || !mapped)
        return;
    int y;
    if (!want_shown && !drag_on)
        y = -1;                       /* empty */
    else if (lp_motion_reduced() && !drag_on)
        y = (int)(alloc_h - body_h);
    else
        y = (int)ceil(body_top());
    if (y >= alloc_h)
        y = -1;
    if (y == last_ir_y)
        return;
    g_debug("input region from y %d", y);
    last_ir_y = y;
    cairo_region_t *r = cairo_region_create();
    if (y >= 0) {
        cairo_rectangle_int_t rect = { 0, y, alloc_w, alloc_h - y };
        cairo_region_union_rectangle(r, &rect);
    }
    gtk_widget_input_shape_combine_region(win, r);
    cairo_region_destroy(r);
}

/* At rest and fully shown, the keyboard tells the compositor it is
 * opaque, so nothing under it is drawn. */
static void update_opaque_region(void)
{
    GdkWindow *gw = win ? gtk_widget_get_window(win) : NULL;
    if (!gw)
        return;
    if (want_shown && !slide.moving && slide.x >= 1.0 && !resize_on &&
        !drag_on && body_alpha() >= 1.0) {
        cairo_rectangle_int_t rect = { 0, (int)(alloc_h - kb_h) + 1, alloc_w,
                                       (int)kb_h - 1 };
        cairo_region_t *r = cairo_region_create_rectangle(&rect);
        gdk_window_set_opaque_region(gw, r);
        cairo_region_destroy(r);
    } else {
        gdk_window_set_opaque_region(gw, NULL);
    }
}

static double last_top = -1;

static void on_frame(GtkWidget *w, gpointer data)
{
    (void)w; (void)data;
    double top = body_top();
    if (slide.moving || drag_on)
        g_debug("slide %.4f v %+.2f top %.1f%s", slide.x, slide.v, top,
                want_shown ? "" : " (hiding)");
    if (top != last_top || slide.moving) {
        last_top = top;
        update_input_region();
        if (area)
            gtk_widget_queue_draw(area);
    } else {
        damage_animating();
    }
    if (!slide.moving)
        update_opaque_region();
    if (old_cache && !xfade.moving) {
        cairo_surface_destroy(old_cache);
        old_cache = NULL;
    }
    if (!want_shown && !slide.moving && slide.x <= 0.0 && !drag_on)
        finish_hide();
}

static void kick(void)
{
    lp_motion_kick(motion);
    /* A spring that was jumped has nothing to animate but the state still
     * changed, and lp_motion_kick draws that only when nothing runs. */
    if (lp_motion_running(motion))
        damage_animating();
}

static void redraw_all(void)
{
    if (area)
        gtk_widget_queue_draw(area);
}

/* ── language, pages and labels ───────────────────────────────────── */

static void relabel(gboolean crossfade)
{
    if (!area || !gtk_widget_get_realized(area)) {
        if (cache) {
            cairo_surface_destroy(cache);
            cache = NULL;
        }
        return;
    }
    if (crossfade && cache && mapped && slide.x > 0) {
        if (old_cache)
            cairo_surface_destroy(old_cache);
        old_cache = cache;
        cache = NULL;
        lp_spring_init(&xfade, LP_SPRING_PRESS, 0);
        /* A page switch is ≤120ms: the press spring's exit is 91ms. */
        lp_spring_set_target_out(&xfade, 1.0);
    }
    render_cache();
    redraw_all();
    kick();
}

void ui_relabel(void)
{
    relabel(TRUE);
}

void ui_field_purpose(uint32_t p)
{
    purpose = p;
    relabel(FALSE);
}

static void set_page(int p)
{
    if (p == page)
        return;
    page = p;
    relabel(TRUE);
    osk_state_changed();
}

/* ── showing and hiding ───────────────────────────────────────────── */

static GdkMonitor *monitor(void)
{
    GdkDisplay *d = gdk_display_get_default();
    GdkWindow *gw = win ? gtk_widget_get_window(win) : NULL;
    GdkMonitor *m = gw ? gdk_display_get_monitor_at_window(d, gw) : NULL;
    if (!m)
        m = gdk_display_get_primary_monitor(d);
    if (!m && gdk_display_get_n_monitors(d) > 0)
        m = gdk_display_get_monitor(d, 0);
    return m;
}

static void compute_height(void)
{
    GdkMonitor *m = monitor();
    if (m) {
        GdkRectangle g;
        gdk_monitor_get_geometry(m, &g);
        mon_h = g.height;
    }
    double f = clampd(cfg.height_frac, 0.22, 0.55);
    kb_h = round(clampd(f * mon_h, 240, mon_h * 0.6));
    body_h = kb_h;
}

static void apply_size(void)
{
    if (!win)
        return;
    int want = (int)(kb_h + headroom());
    if (resize_on)
        want = (int)(round(mon_h * 0.6) + headroom());
    gtk_widget_set_size_request(area, -1, want);
    gtk_window_resize(GTK_WINDOW(win), 1, want);
    if (!resize_on && (want_shown || slide.x > 0))
        gtk_layer_set_exclusive_zone(GTK_WINDOW(win), (int)kb_h);
}

static void on_size_allocate(GtkWidget *w, GdkRectangle *a, gpointer data)
{
    (void)w; (void)data;
    if (a->width == alloc_w && a->height == alloc_h)
        return;
    gboolean width_changed = a->width != alloc_w;
    alloc_w = a->width;
    alloc_h = a->height;
    last_ir_y = -2;
    if (width_changed)
        layout();
    if (width_changed && cache) {
        cairo_surface_destroy(cache);
        cache = NULL;
    }
    update_input_region();
}

static void on_scale(GObject *o, GParamSpec *ps, gpointer data)
{
    (void)o; (void)ps; (void)data;
    if (cache) {
        cairo_surface_destroy(cache);
        cache = NULL;
    }
    redraw_all();
}

static void cancel_all_touches(void);

/* Touch handlers, declared for the GTK signal wiring below. */
static gboolean on_touch(GtkWidget *w, GdkEvent *ev, gpointer data);
static gboolean on_button(GtkWidget *w, GdkEventButton *ev, gpointer data);
static gboolean on_motion_ev(GtkWidget *w, GdkEventMotion *ev, gpointer data);

static gboolean dbg_event(GtkWidget *w, GdkEvent *ev, gpointer d) /*DBGTMP*/
{
    (void)w; (void)d;
    if (ev->type != GDK_MOTION_NOTIFY)
        g_debug("win event type %d", ev->type);
    return FALSE;
}

static void create_window(void)
{
    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "lp-osk");
    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_namespace(GTK_WINDOW(win), "lp-osk");
    /* Overlay, not top: a full-screen application covers the top layer,
     * and a keyboard that vanishes under the browser it is typing into
     * is exactly the keyboard that cannot be closed or used. */
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    /* Never take keyboard focus: a tap on a key must leave the focus on
     * the text field the key is typing into. */
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_widget_set_app_paintable(win, TRUE);
    GdkScreen *scr = gtk_widget_get_screen(win);
    GdkVisual *vis = gdk_screen_get_rgba_visual(scr);
    if (vis)
        gtk_widget_set_visual(win, vis);

    area = gtk_drawing_area_new();
    gtk_widget_set_app_paintable(area, TRUE);
    gtk_widget_add_events(area, GDK_TOUCH_MASK | GDK_BUTTON_PRESS_MASK |
                                GDK_BUTTON_RELEASE_MASK | GDK_BUTTON_MOTION_MASK);
    g_signal_connect(area, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(area, "touch-event", G_CALLBACK(on_touch), NULL);
    g_signal_connect(area, "button-press-event", G_CALLBACK(on_button), NULL);
    g_signal_connect(area, "button-release-event", G_CALLBACK(on_button), NULL);
    g_signal_connect(area, "motion-notify-event", G_CALLBACK(on_motion_ev), NULL);
    g_signal_connect(area, "size-allocate", G_CALLBACK(on_size_allocate), NULL);
    g_signal_connect(area, "notify::scale-factor", G_CALLBACK(on_scale), NULL);
    gtk_container_add(GTK_CONTAINER(win), area);
    g_signal_connect(win, "event", G_CALLBACK(dbg_event), NULL); /*DBGTMP*/

    motion = lp_motion_new(area, on_frame, NULL);
    lp_spring_init(&slide, LP_SPRING_SHEET, 0);
    lp_spring_init(&xfade, LP_SPRING_PRESS, 1);
    lp_spring_init(&menu.s, LP_SPRING_MENU, 0);
    lp_motion_add(motion, &slide);
    lp_motion_add(motion, &xfade);
    lp_motion_add(motion, &menu.s);
    for (int i = 0; i < MAXTOUCH; i++) {
        lp_spring_init(&previews[i].s, LP_SPRING_MENU, 0);
        lp_motion_add(motion, &previews[i].s);
    }
    for (int p = 0; p < N_PAGES; p++)
        for (int i = 0; i < nkeys[p]; i++)
            lp_motion_add(motion, &keys[p][i].hl);
    for (int i = 0; i < N_STRIP; i++)
        if (strip[i].d)
            lp_motion_add(motion, &strip[i].hl);
    gtk_widget_show(area);
}

void ui_show(void)
{
    if (!win)
        create_window();
    gboolean was = want_shown;
    want_shown = TRUE;
    if (!mapped) {
        compute_height();
        body_h = kb_h;
        /* The zone goes on now, at the start: the window above resizes
         * once, while the keyboard slides into the room it left. */
        apply_size();
        gtk_layer_set_exclusive_zone(GTK_WINDOW(win), (int)kb_h);
        last_ir_y = -2;
        gtk_widget_show(win);
        mapped = TRUE;
        lp_spring_init(&slide, LP_SPRING_SHEET, slide.x);
        type_refresh_keymap();
    }
    if (!was) {
        /* From wherever it is, carrying whatever speed it has: a show
         * during a hide turns round instead of starting again. */
        lp_spring_set_target(&slide, 1.0);
        update_input_region();
        kick();
        osk_state_changed();
    }
}

static void begin_hide(double velocity, gboolean fling)
{
    if (!win || !mapped)
        return;
    g_debug("hide from slide %.3f v %+.2f%s", slide.x, fling ? velocity : slide.v,
            slide.moving ? " (was moving)" : "");
    gboolean was = want_shown;
    want_shown = FALSE;
    cancel_all_touches();
    if (fling)
        slide.v = velocity;
    lp_spring_set_target_out(&slide, 0.0);
    update_input_region();     /* empty, now */
    kick();
    if (was)
        osk_state_changed();
}

void ui_hide(void)
{
    begin_hide(0, FALSE);
}

gboolean ui_visible(void)
{
    return want_shown;
}

static void finish_hide(void)
{
    if (!mapped)
        return;
    mapped = FALSE;
    gtk_layer_set_exclusive_zone(GTK_WINDOW(win), 0);
    gtk_widget_hide(win);
    /* Nothing is kept for a keyboard nobody sees: the images are the
     * largest thing this process owns (a 4K-wide keyboard is ~11MB). */
    if (cache) {
        cairo_surface_destroy(cache);
        cache = NULL;
    }
    if (old_cache) {
        cairo_surface_destroy(old_cache);
        old_cache = NULL;
    }
    page = PAGE_LETTERS;
    osk_state_changed();
}

/* ── modifiers ────────────────────────────────────────────────────── */

static Mod *mod_of(const KeyDef *d)
{
    switch (d->kind) {
    case K_SHIFT: return &m_shift;
    case K_CTRL:  return &m_ctrl;
    case K_ALT:   return &m_alt;
    default:      return NULL;
    }
}

/* A key was typed: one-shot modifiers are spent, held ones remember
 * they were used so letting go of them does not latch them. */
static void spend_mods(void)
{
    gboolean changed = FALSE;
    Mod *all[3] = { &m_shift, &m_ctrl, &m_alt };
    for (int i = 0; i < 3; i++) {
        Mod *m = all[i];
        if (m->held)
            m->used = TRUE;
        if (m->mode == M_ONCE) {
            m->mode = M_OFF;
            changed = TRUE;
        }
    }
    if (changed)
        relabel(FALSE);
}

static void mod_down(Mod *m)
{
    m->held++;
    m->used = FALSE;
    relabel(FALSE);
}

static void mod_up(Mod *m)
{
    if (m->held > 0)
        m->held--;
    if (!m->used) {
        /* a tap: off -> once -> (tapped again quickly) locked -> off */
        gint64 now = g_get_monotonic_time();
        if (m->mode == M_ONCE && now - m->tap_us < 400000)
            m->mode = M_LOCK;
        else
            m->mode = m->mode == M_OFF ? M_ONCE : M_OFF;
        m->tap_us = now;
    } else if (m->mode == M_ONCE) {
        m->mode = M_OFF;
    }
    relabel(FALSE);
    osk_state_changed();
}

/* ── typing ───────────────────────────────────────────────────────── */

static uint32_t touch_mods(const Touch *t)
{
    return (t->shift ? MOD_SHIFT : 0) | (t->ctrl ? MOD_CTRL : 0) |
           (t->alt ? MOD_ALT : 0);
}

static void type_string(const char *s, uint32_t mods)
{
    if (s[0] && !s[1] && (unsigned char)s[0] < 128 && type_ascii(s[0], mods))
        return;
    type_text(s);
}

static void toggle_lang(void)
{
    int next = cfg.lang == LANG_KO ? LANG_EN : LANG_KO;
    if (next == LANG_KO && !(cfg.layouts & LAYOUT_KO))
        next = LANG_EN;
    type_set_lang(next);
}

/* The one place a key does what it is for. */
static void emit(Touch *t, Key *k, const char *alt_text)
{
    const KeyDef *d = k->d;
    uint32_t mods = touch_mods(t);
    switch (d->kind) {
    case K_TEXT:
        if (alt_text) {
            /* a jamo from the menu joins the syllable being built */
            gunichar c = g_utf8_get_char(alt_text);
            if (hangul_is_jamo(c) && !*g_utf8_next_char(alt_text))
                type_jamo(c);
            else
                type_text(alt_text);
        } else if (is_letter(d) && korean() && !t->ctrl && !t->alt) {
            type_jamo(type_ko_jamo(d->t[0], t->shift));
        } else if (t->ctrl || t->alt) {
            /* Ctrl+C is the key C with Control, in any language. */
            char c = d->t[0];
            gboolean sh = t->shift || (is_letter(d) && t->caps);
            if (!d->t[1] && (unsigned char)c < 128)
                type_ascii(c, (mods & ~MOD_SHIFT) | (sh ? MOD_SHIFT : 0));
        } else {
            char buf[32];
            key_text(d, t->shift, t->caps, buf, sizeof buf);
            type_string(buf, 0);
        }
        spend_mods();
        break;
    case K_SPACE:
        type_ascii(' ', mods & (MOD_CTRL | MOD_ALT));
        spend_mods();
        break;
    case K_ENTER: type_key(KEY_ENTER, mods); spend_mods(); break;
    case K_TAB:   type_key(KEY_TAB, mods); spend_mods(); break;
    case K_ESC:   type_key(KEY_ESC, mods); spend_mods(); break;
    case K_BKSP:  type_backspace(mods); spend_mods(); break;
    case K_CODE:  type_key(d->code, mods); spend_mods(); break;
    case K_LANG:
    case K_CHIP:
        toggle_lang();
        break;
    case K_PAGE:
        set_page(page == PAGE_LETTERS ? PAGE_SYMBOLS : PAGE_LETTERS);
        break;
    case K_CAPS:
        m_shift.mode = m_shift.mode == M_LOCK ? M_OFF : M_LOCK;
        relabel(FALSE);
        osk_state_changed();
        break;
    case K_CLOSE:
        begin_hide(0, FALSE);
        osk_dismissed();
        break;
    default:
        break;
    }
}

/* ── touches ──────────────────────────────────────────────────────── */

static Key *hit(double sx, double sy)
{
    double top = body_top();
    double ky = sy - top;
    if (ky < 0 || ky > kb_h)
        return NULL;
    if (ky < strip_h) {
        /* The close key's target runs to the corner of the screen. */
        Key *c = &strip[S_CLOSE];
        if (sx >= c->x - 6)
            return c;
        Key *chip = &strip[S_CHIP];
        if (sx <= chip->x + chip->w + 6)
            return chip;
        Key *g = &strip[S_GRIP];
        if (sx >= g->x && sx <= g->x + g->w && ky <= strip_h * 0.55)
            return g;
        return &strip[S_STRIP];
    }
    /* Gaps between keys belong to the nearest key: a finger that lands
     * between two keys meant one of them. */
    Key *best = NULL;
    double bd = 1e18;
    for (int i = 0; i < nkeys[page]; i++) {
        Key *k = &keys[page][i];
        double dx = sx < k->x ? k->x - sx : sx > k->x + k->w ? sx - k->x - k->w : 0;
        double dy = ky < k->y ? k->y - ky : ky > k->y + k->h ? ky - k->y - k->h : 0;
        double dd = dx * dx + dy * dy;
        if (dd < bd) {
            bd = dd;
            best = k;
        }
    }
    return best;
}

static void light(Key *k)
{
    if (!k)
        return;
    lp_spring_jump(&k->hl, 1.0);     /* instantly, on touch-down */
    GdkRectangle r;
    lit_rect(k, &r);
    if (area)
        gtk_widget_queue_draw_area(area, r.x, r.y, r.width, r.height);
}

static gboolean key_still_held(Key *k, Touch *except)
{
    for (int i = 0; i < MAXTOUCH; i++)
        if (touches[i].used && &touches[i] != except && touches[i].key == k)
            return TRUE;
    return FALSE;
}

static void unlight(Key *k, Touch *t)
{
    if (!k || key_still_held(k, t))
        return;
    lp_spring_set_target_out(&k->hl, 0.0);
    kick();
}

static int preview_open(Key *k, const char *text)
{
    for (int i = 0; i < MAXTOUCH; i++) {
        Preview *p = &previews[i];
        if (p->used)
            continue;
        p->used = TRUE;
        p->key = k;
        g_strlcpy(p->text, text, sizeof p->text);
        if (lp_motion_reduced())
            lp_spring_jump(&p->s, 1.0);
        else
            lp_spring_set_target(&p->s, 1.0);
        preview_rect(p);
        if (area)
            gtk_widget_queue_draw_area(area, (int)p->bx - 4, (int)p->by - 4,
                                       (int)p->bw + 8, (int)p->bh + 8);
        kick();
        return i;
    }
    return -1;
}

static void preview_close(Touch *t)
{
    if (t->pv < 0)
        return;
    lp_spring_set_target_out(&previews[t->pv].s, 0.0);
    t->pv = -1;
    kick();
}

static void preview_show_for(Touch *t)
{
    Key *k = t->key;
    if (!k || k->d->kind != K_TEXT || secret_field())
        return;
    char buf[32];
    key_text(k->d, t->shift, t->caps, buf, sizeof buf);
    t->pv = preview_open(k, buf);
}

static void timer_stop(Touch *t)
{
    if (t->timer)
        g_source_remove(t->timer);
    t->timer = 0;
}

static gboolean on_repeat(gpointer p)
{
    Touch *t = p;
    if (!t->used || !t->key) {
        t->timer = 0;
        return G_SOURCE_REMOVE;
    }
    emit(t, t->key, NULL);
    if (!t->repeating) {
        t->repeating = TRUE;
        t->timer = g_timeout_add(50, on_repeat, t);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void menu_close(void)
{
    if (!menu.open)
        return;
    menu.open = FALSE;
    lp_spring_set_target_out(&menu.s, 0.0);
    kick();
}

static void menu_select(Touch *t)
{
    double ky = t->y - body_top();
    if (ky > menu.key->y + menu.key->h + row_h) {
        menu.sel = -1;        /* dragged well away: cancel */
    } else {
        int s = (int)floor((t->x - menu.x) / menu.cw);
        menu.sel = CLAMP(s, 0, menu.n - 1);
    }
    GdkRectangle r;
    menu_rect(&r);
    if (area)
        gtk_widget_queue_draw_area(area, r.x, r.y, r.width, r.height);
}

static gboolean on_long_press(gpointer p)
{
    Touch *t = p;
    t->timer = 0;
    if (!t->used || !t->key || t->committed || t->drag || menu.open)
        return G_SOURCE_REMOVE;
    const KeyDef *d = t->key->d;
    if (!has_alts(d))
        return G_SOURCE_REMOVE;
    menu.n = 0;
    uint32_t ka = ko_alt(d, t->shift);
    if (ka) {
        menu.items[0][g_unichar_to_utf8(ka, menu.items[0])] = 0;
        menu.n = 1;
    }
    /* The alternates, capitalised when the key would type a capital. */
    char **parts = g_strsplit(ka ? "" : d->alts, " ", 12);
    gboolean up = is_letter(d) ? (t->shift != t->caps) : FALSE;
    for (int i = 0; parts[i] && menu.n < 12; i++) {
        if (!*parts[i])
            continue;
        char *s = up ? g_utf8_strup(parts[i], -1) : g_strdup(parts[i]);
        g_strlcpy(menu.items[menu.n++], s, sizeof menu.items[0]);
        g_free(s);
    }
    g_strfreev(parts);
    if (!menu.n)
        return G_SOURCE_REMOVE;
    Key *k = t->key;
    menu.key = k;
    menu.id = t->id;
    menu.cw = MAX(k->w, row_h * 0.95);
    menu.ch = row_h * 1.05;
    menu.x = clampd(k->x + k->w / 2 - menu.cw / 2, pad, alloc_w - pad - menu.cw * menu.n);
    menu.y = k->y - menu.ch - 6;
    menu.sel = 0;
    menu.open = TRUE;
    preview_close(t);
    if (lp_motion_reduced()) {
        lp_spring_jump(&menu.s, 1.0);
    } else {
        lp_spring_init(&menu.s, LP_SPRING_MENU, 0);
        lp_spring_set_target(&menu.s, 1.0);
    }
    menu_select(t);
    kick();
    redraw_all();
    return G_SOURCE_REMOVE;
}

static Touch *touch_find(gintptr id)
{
    for (int i = 0; i < MAXTOUCH; i++)
        if (touches[i].used && touches[i].id == id)
            return &touches[i];
    return NULL;
}

/* A character still waiting for its finger to lift is typed now: another
 * finger has gone down, and fast typing keeps its order only this way. */
static void commit_pending(Touch *except)
{
    for (;;) {
        Touch *first = NULL;
        for (int i = 0; i < MAXTOUCH; i++) {
            Touch *t = &touches[i];
            if (!t->used || t == except || t->committed || !t->key || t->drag ||
                t->resize || (menu.open && menu.id == t->id))
                continue;
            Kind kd = t->key->d->kind;
            if (kd != K_TEXT && kd != K_SPACE && kd != K_ENTER && kd != K_TAB &&
                kd != K_ESC && kd != K_CODE)
                continue;
            if (!first || t->t0 < first->t0)
                first = t;
        }
        if (!first)
            return;
        first->committed = TRUE;
        timer_stop(first);
        emit(first, first->key, NULL);
    }
}

static void drag_begin(Touch *t)
{
    t->drag = TRUE;
    drag_on = TRUE;
    drag_x0 = slide.x;
    lp_velocity_reset(&t->vt);
    lp_velocity_add(&t->vt, g_get_monotonic_time(), t->y);
}

static void drag_move(Touch *t)
{
    double dy = t->y - t->y0;
    double x = clampd(drag_x0 - dy / (kb_h + 2), 0.0, 1.0);
    lp_spring_jump(&slide, x);
    on_frame(area, NULL);
}

static void drag_end(Touch *t, gint64 now)
{
    lp_velocity_add(&t->vt, now, t->y);
    double v = -lp_velocity_get(&t->vt) / (kb_h + 2);   /* units/s, up +ve */
    drag_on = FALSE;
    t->drag = FALSE;
    if (lp_spring_project(slide.x, v) < 0.5) {
        begin_hide(v, TRUE);
        osk_dismissed();
    } else {
        lp_spring_fling(&slide, 1.0, v);
        last_ir_y = -2;
        update_input_region();
        kick();
    }
}

static void resize_move(Touch *t)
{
    double h = resize_h0 - (t->y - t->y0);
    body_h = round(clampd(h, 240, mon_h * 0.6));
    redraw_all();
}

static void resize_end(Touch *t)
{
    (void)t;
    resize_on = FALSE;
    t->resize = FALSE;
    kb_h = body_h;
    cfg.height_frac = kb_h / mon_h;
    osk_config_save();
    layout();
    if (cache) {
        cairo_surface_destroy(cache);
        cache = NULL;
    }
    apply_size();
    last_ir_y = -2;
    update_input_region();
    redraw_all();
}

static void touch_begin(gintptr id, double x, double y)
{
    if (!want_shown || touch_find(id))
        return;
    Touch *t = NULL;
    for (int i = 0; i < MAXTOUCH && !t; i++)
        if (!touches[i].used)
            t = &touches[i];
    if (!t)
        return;
    Key *k = hit(x, y);
    g_debug("touch down %#lx at %.0f,%.0f on %s, slide %.3f%s", (unsigned long)id,
            x, y, k ? k->d->name : "nothing", slide.x, slide.moving ? " (moving)" : "");
    if (!k)
        return;
    gint64 now = g_get_monotonic_time();
    /* Chatter: a touchscreen can report one tap as two a few ms apart.
     * No finger lifts and lands again on the same key in 35ms. */
    if (k->d->kind == K_TEXT && now - k->last_up < 35000)
        return;

    commit_pending(NULL);
    memset(t, 0, sizeof *t);
    t->used = TRUE;
    t->id = id;
    t->x0 = t->x = x;
    t->y0 = t->y = y;
    t->t0 = now;
    t->pv = -1;
    t->key = k;
    /* Caps Lock capitalises letters only, as on a laptop; Shift, held or
     * tapped once, shifts everything. */
    t->shift = m_shift.held > 0 || m_shift.mode == M_ONCE;
    t->caps = m_shift.mode == M_LOCK;
    t->ctrl = m_ctrl.held > 0 || m_ctrl.mode != M_OFF;
    t->alt = m_alt.held > 0 || m_alt.mode != M_OFF;
    lp_velocity_reset(&t->vt);
    lp_velocity_add(&t->vt, now, y);

    switch (k->d->kind) {
    case K_STRIP:
        drag_begin(t);
        return;
    case K_GRIP:
        t->resize = TRUE;
        resize_on = TRUE;
        resize_h0 = kb_h;
        body_h = kb_h;
        light(k);
        apply_size();   /* room to grow: one configure, not one a frame */
        return;
    case K_SHIFT: case K_CTRL: case K_ALT:
        light(k);
        mod_down(mod_of(k->d));
        return;
    default:
        break;
    }
    light(k);
    if (acts_on_down(k->d)) {
        t->committed = TRUE;
        emit(t, k, NULL);
        t->timer = g_timeout_add(400, on_repeat, t);
        return;
    }
    preview_show_for(t);
    if (k->d->kind == K_TEXT && has_alts(k->d))
        t->timer = g_timeout_add(400, on_long_press, t);
}

static void touch_update(gintptr id, double x, double y)
{
    Touch *t = touch_find(id);
    if (!t)
        return;
    t->x = x;
    t->y = y;
    gint64 now = g_get_monotonic_time();
    lp_velocity_add(&t->vt, now, y);
    if (t->drag) {
        drag_move(t);
        return;
    }
    if (t->resize) {
        resize_move(t);
        return;
    }
    if (menu.open && menu.id == id) {
        menu_select(t);
        return;
    }
    Key *k = t->key;
    if (!k || t->committed || t->repeating)
        return;
    Kind kd = k->d->kind;
    if (kd == K_SHIFT || kd == K_CTRL || kd == K_ALT || kd == K_CLOSE)
        return;
    /* A quick downward flick that starts on a key is a swipe to close:
     * from there the keyboard follows the finger like the top strip. */
    double dy = y - t->y0, dx = x - t->x0;
    if (dy > row_h * 1.2 && dy > 2 * fabs(dx) && now - t->t0 < 250000) {
        timer_stop(t);
        preview_close(t);
        unlight(k, t);
        t->key = NULL;
        t->y0 = y - dy;
        drag_begin(t);
        drag_move(t);
        return;
    }
    /* Otherwise the finger may slide to the key it meant. */
    Key *n = hit(x, y);
    if (n && n != k && n->d->kind == K_TEXT && kd == K_TEXT) {
        timer_stop(t);
        preview_close(t);
        unlight(k, t);
        t->key = n;
        light(n);
        preview_show_for(t);
        if (has_alts(n->d))
            t->timer = g_timeout_add(400, on_long_press, t);
    }
}

static void touch_end(gintptr id, double x, double y, gboolean cancel)
{
    Touch *t = touch_find(id);
    if (!t)
        return;
    t->x = x;
    t->y = y;
    gint64 now = g_get_monotonic_time();
    timer_stop(t);
    Key *k = t->key;
    /* Off the list before acting: what this touch does may hide the
     * keyboard, and hiding cancels every touch still on the list. */
    t->used = FALSE;

    if (t->drag) {
        if (cancel) {
            drag_on = FALSE;
            lp_spring_set_target(&slide, want_shown ? 1.0 : 0.0);
            kick();
        } else {
            drag_end(t, now);
        }
    } else if (t->resize) {
        resize_end(t);
        unlight(k, t);
    } else if (menu.open && menu.id == id) {
        if (!cancel && menu.sel >= 0)
            emit(t, menu.key, menu.items[menu.sel]);
        menu_close();
        unlight(k, t);
    } else if (k) {
        Mod *m = mod_of(k->d);
        if (m) {
            if (!cancel)
                mod_up(m);
            else if (m->held > 0)
                m->held--;
        } else if (!cancel && !t->committed) {
            emit(t, k, NULL);
        }
        preview_close(t);
        if (k->d->kind == K_TEXT)
            k->last_up = now;
        unlight(k, t);
    }
}

static void cancel_all_touches(void)
{
    for (int i = 0; i < MAXTOUCH; i++)
        if (touches[i].used)
            touch_end(touches[i].id, touches[i].x, touches[i].y, TRUE);
    menu_close();
    drag_on = FALSE;
}

static gboolean on_touch(GtkWidget *w, GdkEvent *ev, gpointer data)
{
    (void)w; (void)data;
    gintptr id = (gintptr)ev->touch.sequence;
    double x = ev->touch.x, y = ev->touch.y;
    switch (ev->type) {
    case GDK_TOUCH_BEGIN:
        type_osk_press(ev->touch.time);   /* ours: type.c, head comment 1. */
        touch_begin(id, x, y);
        break;
    case GDK_TOUCH_UPDATE: touch_update(id, x, y); break;
    case GDK_TOUCH_END:    touch_end(id, x, y, FALSE); break;
    case GDK_TOUCH_CANCEL: touch_end(id, x, y, TRUE); break;
    default: break;
    }
    return TRUE;
}

#define MOUSE_ID ((gintptr)1)

/* The mouse and the touchpad work too, as one more finger. Pointer events
 * GDK makes up from a real finger are dropped: that finger has already
 * been handled as a touch, and handling it twice is a double letter. */
static gboolean from_touch(GdkEvent *ev)
{
    if (gdk_event_get_pointer_emulated(ev))
        return TRUE;
    GdkDevice *src = gdk_event_get_source_device(ev);
    return src && gdk_device_get_source(src) == GDK_SOURCE_TOUCHSCREEN;
}

static gboolean on_button(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w; (void)data;
    g_debug("button %s %u at %.0f,%.0f%s", ev->type == GDK_BUTTON_PRESS ? "press" :
            ev->type == GDK_BUTTON_RELEASE ? "release" : "other", ev->button,
            ev->x, ev->y, from_touch((GdkEvent *)ev) ? " (from touch)" : "");
    /* Any press here is ours, not one that may have moved a text
     * cursor (type.c, head comment 1.). */
    if (ev->type == GDK_BUTTON_PRESS)
        type_osk_press(ev->time);
    if (from_touch((GdkEvent *)ev) || ev->button != GDK_BUTTON_PRIMARY)
        return TRUE;
    if (ev->type == GDK_BUTTON_PRESS)
        touch_begin(MOUSE_ID, ev->x, ev->y);
    else if (ev->type == GDK_BUTTON_RELEASE)
        touch_end(MOUSE_ID, ev->x, ev->y, FALSE);
    return TRUE;
}

static gboolean on_motion_ev(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
    (void)w; (void)data;
    if (from_touch((GdkEvent *)ev))
        return TRUE;
    touch_update(MOUSE_ID, ev->x, ev->y);
    return TRUE;
}

/* ── tests ────────────────────────────────────────────────────────── */

static Key *key_named(const char *name)
{
    for (int i = 0; i < N_STRIP; i++)
        if (strip[i].d && !strcmp(strip[i].d->name, name))
            return &strip[i];
    for (int i = 0; i < nkeys[page]; i++)
        if (!strcmp(keys[page][i].d->name, name))
            return &keys[page][i];
    return NULL;
}

gboolean ui_test_touch(int id, int phase, const char *name, double dx,
                       double dy, char *err, size_t errlen)
{
    gintptr tid = 0x10000 + id;
    Touch *t = touch_find(tid);
    double x, y;
    if (name && *name && strcmp(name, "-")) {
        Key *k = key_named(name);
        if (!k) {
            g_snprintf(err, errlen, "no key \"%s\" on this page", name);
            return FALSE;
        }
        x = k->x + k->w / 2 + dx;
        y = body_top() + k->y + k->h / 2 + dy;
    } else if (t) {
        x = t->x0 + dx;
        y = t->y0 + dy;
    } else {
        g_snprintf(err, errlen, "touch %d is not down", id);
        return FALSE;
    }
    if (!want_shown && phase == 0) {
        g_snprintf(err, errlen, "the keyboard is not shown");
        return FALSE;
    }
    switch (phase) {
    case 0: touch_begin(tid, x, y); break;
    case 1: touch_update(tid, x, y); break;
    case 2: touch_end(tid, x, y, FALSE); break;
    default: touch_end(tid, x, y, TRUE); break;
    }
    return TRUE;
}

void ui_describe(GString *out)
{
    double top = body_top();
    g_string_append_printf(out, "surface %d x %d, keyboard %.0f high, top at %.1f, "
                           "page %s, lang %s, slide %.3f\n",
                           alloc_w, alloc_h, kb_h, top,
                           page == PAGE_LETTERS ? "letters" : "symbols",
                           korean() ? "ko" : "en", slide.x);
    for (int s = 0; s < 2; s++) {
        Key *arr = s ? keys[page] : strip;
        int n = s ? nkeys[page] : N_STRIP;
        for (int i = 0; i < n; i++) {
            Key *k = &arr[i];
            if (!k->d)
                continue;
            char buf[32] = "";
            key_text(k->d, shifted(), m_shift.mode == M_LOCK, buf, sizeof buf);
            g_string_append_printf(out, "%-12s %7.1f %7.1f %6.1f x %5.1f  %s\n",
                                   k->d->name, k->x, top + k->y, k->w, k->h, buf);
        }
    }
}

const char *ui_page_name(void)
{
    return page == PAGE_LETTERS ? "letters" : "symbols";
}

void ui_init(void)
{
    /* Geometry needs a width; until the surface exists, a guess keeps
     * `describe` meaningful. Nothing is created here. */
    layout();
}
