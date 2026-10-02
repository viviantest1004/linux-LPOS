/* osk.c - lp-recovery's on-screen keyboard.
 *
 * The owner uses the XPS as a tablet as often as a laptop, and asked for
 * recovery to work with a finger alone: the administrator password, the
 * typed REINSTALL, and the recovery shell itself. There is no compositor
 * and no desktop keyboard (desktop/osk) down here - only the framebuffer
 * - so this is a keyboard of its own, drawn with lp-ui.h and made to look
 * like the desktop's: the same dark keys and orange accent, the same
 * letters and symbols pages, and the Close key where the desktop keeps
 * it, in the top-right corner (the owner's one complaint about on-screen
 * keyboards is that closing them is always forgotten). Whoever closes it
 * gets it back from the "Keyboard" button every screen that takes text
 * shows.
 *
 * ── Keys ──
 *
 * A full terminal keyboard, because the shell is a real shell: every
 * printable ASCII character, Tab, Esc, Ctrl and Alt (one-shot: tap Ctrl,
 * then C), Shift (tap once for one capital, again to lock, again to
 * release - the key shows which), the four arrows, Home/End/PgUp/PgDn,
 * Del, F1-F12 and Enter. Letters on one page, symbols and the navigation
 * keys on the other ("?123" / "ABC"). Five rows of 15 units, each at
 * least 118 pixels tall on the 4K panel - 59 logical pixels at scale 2,
 * over the 48 a fingertip needs.
 *
 * ── Touch ──
 *
 * A key lights the moment a finger lands on it (COMMON.md Motion: press
 * feedback on touch-down, no delay; only the fade back is animated). A
 * character is typed when the finger lifts, so a finger that landed on
 * the wrong key can slide onto the right one - unless another finger
 * lands first, which types the first key at once: fast typists lift the
 * first finger after the second is down, and the order must survive
 * that. Backspace, Del and the arrows act on touch-down instead and
 * repeat while held (400 ms, then 16 a second), because holding them is
 * how people use them.
 *
 * ── Drawing ──
 *
 * The keys are drawn once into a canvas of their own whenever the page
 * or a modifier changes; a frame copies the part of it that is on the
 * screen and draws the lit key over it. Showing and hiding slide the
 * panel on the sheet spring (260 ms in, 0.7x out, interruptible: a tap on
 * "Keyboard" mid-slide turns it round from where it is and how fast it is
 * going); with reduce motion it simply appears. While it slides away it
 * takes no touches - the person has decided, and the screen under it
 * gets the next one.
 */
#include "recovery.h"

void (*osk_emit)(const osk_key_t *k);
void (*osk_closed)(void);
bool osk_reduce_motion;

/* The desktop keyboard's colours (desktop/osk/ui.c). */
#define C_BG      0x1a1a1au
#define C_LINE    0x333333u
#define C_KEY     0x363636u
#define C_SPECIAL 0x262626u
#define C_LIT     0x5e5e5eu
#define C_T1      0xe8e8e8u
#define C_T2      0xa8a8a8u
#define C_ACCENT  0xe95420u

/* Internal actions beyond the OSK_ ones callers see. */
enum { KA_SHIFT = 100, KA_CTRL, KA_ALT, KA_PAGE, KA_CLOSE };

typedef struct {
    int         act;            /* OSK_ or KA_ */
    int         w4;             /* width in quarter units; 0 ends a row */
    const char *lab;            /* label, and the text of a text key */
    const char *slab;           /* shifted text key, or NULL */
    int         fn;
} kdef_t;

#define T(c, s)      { OSK_TEXT, 4, c, s, 0 }
#define K(l, a, w)   { a, w, l, 0, 0 }
#define F(n)         { OSK_FN, 4, "F" #n, 0, n }
#define END          { 0, 0, 0, 0, 0 }

static const kdef_t LETTERS[] = {
    T("`", "~"), T("1", "!"), T("2", "@"), T("3", "#"), T("4", "$"), T("5", "%"),
    T("6", "^"), T("7", "&"), T("8", "*"), T("9", "("), T("0", ")"), T("-", "_"),
    T("=", "+"), K("⌫", OSK_BKSP, 8), END,
    K("Tab", OSK_TAB, 6), T("q", "Q"), T("w", "W"), T("e", "E"), T("r", "R"),
    T("t", "T"), T("y", "Y"), T("u", "U"), T("i", "I"), T("o", "O"), T("p", "P"),
    T("[", "{"), T("]", "}"), { OSK_TEXT, 6, "\\", "|", 0 }, END,
    K("Ctrl", KA_CTRL, 7), T("a", "A"), T("s", "S"), T("d", "D"), T("f", "F"),
    T("g", "G"), T("h", "H"), T("j", "J"), T("k", "K"), T("l", "L"), T(";", ":"),
    T("'", "\""), K("⏎", OSK_ENTER, 9), END,
    K("⇧", KA_SHIFT, 12), T("z", "Z"), T("x", "X"), T("c", "C"), T("v", "V"),
    T("b", "B"), T("n", "N"), T("m", "M"), T(",", "<"), T(".", ">"), T("/", "?"),
    K("↑", OSK_UP, 4), K("Del", OSK_DEL, 4), END,
    K("?123", KA_PAGE, 7), K("Esc", OSK_ESC, 6), K("Alt", KA_ALT, 5),
    K(" ", OSK_SPACE, 30), K("←", OSK_LEFT, 4), K("↓", OSK_DOWN, 4),
    K("→", OSK_RIGHT, 4), END,
};

static const kdef_t SYMBOLS[] = {
    T("~", 0), T("!", 0), T("@", 0), T("#", 0), T("$", 0), T("%", 0), T("^", 0),
    T("&", 0), T("*", 0), T("(", 0), T(")", 0), T("_", 0), T("+", 0),
    K("⌫", OSK_BKSP, 8), END,
    K("Tab", OSK_TAB, 6), F(1), F(2), F(3), F(4), F(5), F(6), F(7), F(8), F(9),
    F(10), F(11), F(12), { OSK_TEXT, 6, "|", 0, 0 }, END,
    K("Ctrl", KA_CTRL, 7), K("Home", OSK_HOME, 4), K("End", OSK_END, 4),
    K("PgUp", OSK_PGUP, 4), K("PgDn", OSK_PGDN, 4), T("{", 0), T("}", 0), T("[", 0),
    T("]", 0), T(":", 0), T("\"", 0), T("'", 0), K("⏎", OSK_ENTER, 9), END,
    K("⇧", KA_SHIFT, 12), T("<", 0), T(">", 0), T("?", 0), T("/", 0), T("\\", 0),
    T("|", 0), T(";", 0), T(",", 0), T(".", 0), T("`", 0),
    K("↑", OSK_UP, 4), K("Del", OSK_DEL, 4), END,
    K("ABC", KA_PAGE, 7), K("Esc", OSK_ESC, 6), K("Alt", KA_ALT, 5),
    K(" ", OSK_SPACE, 30), K("←", OSK_LEFT, 4), K("↓", OSK_DOWN, 4),
    K("→", OSK_RIGHT, 4), END,
};

#define NROWS 5
#define MAXK  80
typedef struct { const kdef_t *d; rect_t r; } key_t_;     /* r: keyboard coordinates */
static key_t_ keys[2][MAXK];
static int nkeys[2];
static key_t_ close_key;                /* in the strip */
static rect_t close_hit;                /* its hit area, to the corner */

static int page;                        /* 0 letters, 1 symbols */
enum { M_OFF, M_ONCE, M_LOCK };
static int m_shift, m_ctrl, m_alt;

static int kb_h, strip_h, pad;
static lpui_canvas_t cv_kb;
static bool kb_stale = true;

/* ── Geometry ─────────────────────────────────────────────────────── */
static void layout_page(int p, const kdef_t *d)
{
    int unit_w = SW - 2 * pad;          /* 15 units = 60 quarters */
    int row_h = (kb_h - strip_h - 2 * pad) / NROWS;
    int gap = lpui_px(14) < 2 ? 2 : lpui_px(14);
    int y = strip_h + pad, n = 0;
    for (int row = 0; row < NROWS; row++) {
        int q = 0;
        for (; d->w4; d++) {
            int x0 = pad + unit_w * q / 60, x1 = pad + unit_w * (q + d->w4) / 60;
            if (n < MAXK)
                keys[p][n++] = (key_t_){ d, { x0 + gap / 2, y + gap / 2, x1 - x0 - gap, row_h - gap } };
            q += d->w4;
        }
        d++;                            /* the row's END */
        y += row_h;
    }
    nkeys[p] = n;
}

static void layout(void)
{
    kb_h = lpui_px(820);
    strip_h = lpui_px(112);
    pad = lpui_px(24);
    if (kb_h < 200) {                   /* a tiny screen still gets usable keys */
        kb_h = 200;
        strip_h = 30;
        pad = 4;
    }
    layout_page(0, LETTERS);
    layout_page(1, SYMBOLS);
    int unit = (SW - 2 * pad) / 15;
    int cw = unit * 3;
    static const kdef_t CLOSE = { KA_CLOSE, 12, 0, 0, 0 };
    int m = lpui_px(10);
    close_key = (key_t_){ &CLOSE, { SW - pad - cw, m, cw, strip_h - 2 * m } };
    close_hit = (rect_t){ SW - pad - cw - m, 0, cw + pad + m, strip_h };
}

/* ── Drawing ──────────────────────────────────────────────────────── */
static bool is_special(const kdef_t *d)
{
    return d->act != OSK_TEXT && d->act != OSK_SPACE && d->act != OSK_FN;
}

static const char *key_label(const kdef_t *d)
{
    if (d->act == KA_CLOSE)
        return lpui_s(LPS_R_OSK_CLOSE);
    if (d->act == OSK_TEXT && d->slab && m_shift != M_OFF)
        return d->slab;
    return d->lab;
}

/* Draw one key into `c` with its box at (x, y). */
static void key_draw(lpui_canvas_t *c, const key_t_ *k, int x, int y, bool lit, u32 lit_a)
{
    const kdef_t *d = k->d;
    int w = k->r.w, h = k->r.h, rad = lpui_px(18);
    u32 fill = d->act == KA_CLOSE ? C_ACCENT : is_special(d) ? C_SPECIAL : C_KEY;
    u32 ink = C_T1;
    int mode = d->act == KA_SHIFT ? m_shift : d->act == KA_CTRL ? m_ctrl :
               d->act == KA_ALT ? m_alt : M_OFF;
    if (mode == M_LOCK) {
        fill = 0xffffffu;
        ink = 0x1a1a1au;
    } else if (mode == M_ONCE)
        ink = C_ACCENT;
    lpui_rrect(c, x, y, w, h, rad, fill, 256);
    if (lit)
        lpui_rrect(c, x, y, w, h, rad, d->act == KA_CLOSE ? 0xffffffu : C_LIT, lit_a);
    const char *lab = key_label(d);
    if (!lab || !lab[0] || !strcmp(lab, " "))
        return;
    bool word = lab[1] && (u8)lab[0] < 0x80;        /* "Tab", "PgUp", "F10" */
    int tp = lpui_text_px(word ? 44 : 60);
    if (d->act == KA_CLOSE) {
        /* "× Close": the cross, then the word */
        int tw = lpui_text_width(LPG_M, tp, "×") + lpui_px(18) + lpui_text_width(LPG_M, lpui_text_px(48), lab);
        int tx = x + (w - tw) / 2, by = y + h / 2 + lpui_ascent(LPG_M, tp) * 36 / 100;
        tx += lpui_text(c, LPG_M, tp, tx, by, 0xffffffu, 256, "×") + lpui_px(18);
        lpui_text(c, LPG_M, lpui_text_px(48), tx, by, 0xffffffu, 256, lab);
        return;
    }
    int tw = lpui_text_width(LPG_M, tp, lab);
    lpui_text(c, LPG_M, tp, x + (w - tw) / 2, y + h / 2 + lpui_ascent(LPG_M, tp) * 36 / 100,
              d->act == OSK_TEXT || d->act == OSK_FN ? ink : (mode ? ink : C_T2), 256, lab);
}

static void render(void)
{
    if (!cv_kb.px || cv_kb.w != SW || cv_kb.h != kb_h) {
        free(cv_kb.px);
        cv_kb = (lpui_canvas_t){ malloc((size_t)SW * (size_t)kb_h * 4), SW, kb_h, SW };
        if (!cv_kb.px)
            return;
    }
    for (int i = 0; i < SW * kb_h; i++)
        cv_kb.px[i] = C_BG;
    for (int x = 0; x < SW; x++)
        cv_kb.px[x] = C_LINE;
    /* the strip: what this is, and Close */
    int sp = lpui_text_px(40);
    lpui_text(&cv_kb, LPG_S, sp, pad + lpui_px(20),
              strip_h / 2 + lpui_ascent(LPG_S, sp) * 36 / 100, C_T2, 256,
              lpui_s(LPS_R_KEYBOARD));
    key_draw(&cv_kb, &close_key, close_key.r.x, close_key.r.y, false, 0);
    for (int i = 0; i < nkeys[page]; i++)
        key_draw(&cv_kb, &keys[page][i], keys[page][i].r.x, keys[page][i].r.y, false, 0);
    kb_stale = false;
}

/* ── State: shown, sliding, pressed ───────────────────────────────── */
static lpui_spring_t slide;             /* 0 hidden .. 65536 shown */
static s64 last_tick;
static bool dirty;
static int dirty_y0 = -1;               /* top of what changed, screen y */

static int top_for(s64 x)
{
    if (x < 0) x = 0;
    if (x > 65536) x = 65536;
    return SH - (int)((s64)kb_h * x >> 16);
}

int  osk_height(void)   { return kb_h; }
bool osk_shown(void)    { return slide.target > 0; }
int  osk_top(void)      { return top_for(slide.x); }
int  osk_rest_top(void) { return top_for(slide.target); }

static void mark(int y0)
{
    if (y0 < 0)
        y0 = 0;
    if (dirty_y0 < 0 || y0 < dirty_y0)
        dirty_y0 = y0;
    dirty = true;
}

bool osk_dirty(rect_t *r)
{
    if (!dirty)
        return false;
    dirty = false;
    *r = (rect_t){ 0, dirty_y0, SW, SH - dirty_y0 };
    dirty_y0 = -1;
    return true;
}

void osk_init(void)
{
    layout();
    lpui_spring_init(&slide, LPUI_SPRING_SHEET, 0);
    last_tick = lp_monotonic_ms();
    kb_stale = true;
}

void osk_show(bool on, bool animate)
{
    s64 target = on ? 65536 : 0;
    if (on && kb_stale)
        render();
    mark(top_for(slide.x < target ? slide.x : target));
    if (!animate || osk_reduce_motion) {
        lpui_spring_init(&slide, LPUI_SPRING_SHEET, target);
        mark(top_for(0) - 1);
        mark(top_for(65536));
        return;
    }
    /* Retarget: the spring keeps where it is and how fast it moves. In
     * the sheet's time going up, 0.7x of it going down. */
    slide.target = target;
    slide.k = on ? 856 : 1747;
    slide.c10 = on ? 556 : 794;
    last_tick = lp_monotonic_ms();
}

/* Pressed key: which, by which finger, and the fade after release. */
static int pk = -1, pslot = -1;         /* index into keys[page], or -2 = Close */
static bool pk_repeat;
static s64 next_repeat;
static int fade_k = -1;
static s64 fade_t0;
#define FADE_MS 130
static bool slot_ours[16];

static const key_t_ *kref(int k)
{
    if (k == -2)
        return &close_key;
    return k >= 0 && k < nkeys[page] ? &keys[page][k] : 0;
}

static void mark_key(int k)
{
    const key_t_ *kk = kref(k);
    if (kk)
        mark(osk_top() + kk->r.y);
}

static bool repeats(const kdef_t *d)
{
    return d->act == OSK_BKSP || d->act == OSK_DEL || d->act == OSK_UP ||
           d->act == OSK_DOWN || d->act == OSK_LEFT || d->act == OSK_RIGHT;
}

/* Where (x, y) lands: a key index, -2 for Close, -1 for the panel's
 * empty space, -3 outside the keyboard. */
static int key_at(int x, int y)
{
    if (!osk_shown())
        return -3;
    int top = osk_top();
    if (y < top)
        return -3;
    y -= top;
    if (x >= close_hit.x && y >= close_hit.y && y < close_hit.y + close_hit.h)
        return -2;
    for (int i = 0; i < nkeys[page]; i++) {
        rect_t r = keys[page][i].r;
        /* the gap between keys belongs to the nearer key */
        int g = lpui_px(7);
        if (x >= r.x - g && x < r.x + r.w + g && y >= r.y - g && y < r.y + r.h + g)
            return i;
    }
    return -1;
}

static void release_mods(void)
{
    bool changed = false;
    if (m_shift == M_ONCE) { m_shift = M_OFF; changed = true; }
    if (m_ctrl == M_ONCE)  { m_ctrl = M_OFF; changed = true; }
    if (m_alt == M_ONCE)   { m_alt = M_OFF; changed = true; }
    if (changed) {
        render();
        mark(osk_top());
    }
}

static void emit_key(const kdef_t *d)
{
    osk_key_t o;
    memset(&o, 0, sizeof o);
    o.shift = m_shift != M_OFF;
    o.ctrl = m_ctrl != M_OFF;
    o.alt = m_alt != M_OFF;
    o.what = d->act;
    if (d->act == OSK_TEXT) {
        const char *t = (o.shift && d->slab) ? d->slab : d->lab;
        strlcpy(o.text, t, sizeof o.text);
        o.shift = false;                /* already applied */
    } else if (d->act == OSK_SPACE) {
        o.what = OSK_TEXT;
        strlcpy(o.text, " ", sizeof o.text);
    } else if (d->act == OSK_FN)
        o.fn = d->fn;
    if (osk_emit)
        osk_emit(&o);
    release_mods();
}

static void activate(int k)
{
    if (k == -2) {
        osk_show(false, true);
        if (osk_closed)
            osk_closed();
        return;
    }
    const key_t_ *kk = kref(k);
    if (!kk)
        return;
    const kdef_t *d = kk->d;
    switch (d->act) {
    case KA_SHIFT:
        m_shift = m_shift == M_OFF ? M_ONCE : m_shift == M_ONCE ? M_LOCK : M_OFF;
        break;
    case KA_CTRL:
        m_ctrl = m_ctrl == M_OFF ? M_ONCE : M_OFF;
        break;
    case KA_ALT:
        m_alt = m_alt == M_OFF ? M_ONCE : M_OFF;
        break;
    case KA_PAGE:
        page = !page;
        break;
    default:
        emit_key(d);
        return;
    }
    render();
    mark(osk_top());
}

static void unlight(void)
{
    if (pk != -1 && !osk_reduce_motion) {
        fade_k = pk;
        fade_t0 = lp_monotonic_ms();
    }
    mark_key(pk);
    pk = -1;
    pslot = -1;
    pk_repeat = false;
}

bool osk_pointer(const uev_t *e)
{
    int slot = e->slot >= 0 && e->slot < 16 ? e->slot : 0;
    int k = key_at(e->x, e->y);
    bool slide_away = slide.target == 0;

    if (e->down && !slot_ours[slot] && pslot != slot) {
        /* A finger lands. */
        if (k == -3 || slide_away)
            return false;
        slot_ours[slot] = true;
        if (pk != -1 && pslot != slot) {
            /* Another finger is still on a key: that key is typed now. */
            int prev = pk;
            bool was_repeat = pk_repeat;
            unlight();
            if (!was_repeat)
                activate(prev);
        }
        if (k == -1)
            return true;                /* the panel between keys */
        pk = k;
        pslot = slot;
        fade_k = -1;
        mark_key(k);
        const key_t_ *kk = kref(k);
        pk_repeat = kk && kk->d != 0 && k >= 0 && repeats(kk->d);
        if (pk_repeat) {
            emit_key(kk->d);
            next_repeat = lp_monotonic_ms() + 400;
        }
        return true;
    }
    if (e->down) {
        /* A finger moves. */
        if (!slot_ours[slot])
            return false;
        if (slot == pslot && k != pk) {
            if (pk_repeat || (k >= 0 && repeats(kref(k)->d)) || k < -1) {
                unlight();              /* slid off a repeating key: stop */
            } else if (k >= 0 || k == -2) {
                mark_key(pk);
                pk = k;
                mark_key(pk);
            }
        }
        return true;
    }
    /* A finger lifts. */
    if (!slot_ours[slot]) {
        /* hover of a mouse or tablet: the keyboard hides what is under it */
        return k != -3 && !slide_away;
    }
    slot_ours[slot] = false;
    if (slot == pslot) {
        int p = pk;
        bool was_repeat = pk_repeat;
        unlight();
        if (!was_repeat && p != -1 && (k == p || (p == -2 && k == -2)))
            activate(p);
    }
    return true;
}

int osk_tick(void)
{
    s64 now = lp_monotonic_ms();
    int dt = (int)(now - last_tick);
    last_tick = now;
    int wait = -1;
    if (slide.x != slide.target || slide.v) {
        int before = osk_top();
        lpui_spring_step(&slide, dt > 0 ? dt : 1);
        int after = osk_top();
        mark(before < after ? before : after);
        if (slide.x != slide.target || slide.v)
            wait = 8;
    }
    if (pk_repeat && pk >= 0) {
        if (now >= next_repeat) {
            emit_key(kref(pk)->d);
            next_repeat = now + 60;
        }
        int w = (int)(next_repeat - now);
        if (wait < 0 || w < wait)
            wait = w < 1 ? 1 : w;
    }
    if (fade_k != -1) {
        mark_key(fade_k);
        if (now - fade_t0 >= FADE_MS)
            fade_k = -1;
        else if (wait < 0 || wait > 16)
            wait = 16;
    }
    return wait;
}

void osk_compose(lpui_canvas_t *c, int dx, int dy)
{
    int top = osk_top();
    if (top >= SH || !cv_kb.px)
        return;
    if (kb_stale)
        render();
    for (int j = 0; j < c->h; j++) {
        int sy = dy + j, ky = sy - top;
        if (ky < 0 || ky >= kb_h)
            continue;
        u32 *d = c->px + (u64)j * c->stride;
        const u32 *s = cv_kb.px + (u64)ky * cv_kb.stride + dx;
        int w = c->w;
        if (dx + w > SW)
            w = SW - dx;
        for (int i = 0; i < w; i++)
            d[i] = s[i];
    }
    /* the lit key, and the one fading back */
    if (pk != -1) {
        const key_t_ *k = kref(pk);
        if (k)
            key_draw(c, k, k->r.x - dx, top + k->r.y - dy, true, 256);
    }
    if (fade_k != -1 && fade_k != pk) {
        const key_t_ *k = kref(fade_k);
        s64 t = lp_monotonic_ms() - fade_t0;
        if (k && t < FADE_MS)
            key_draw(c, k, k->r.x - dx, top + k->r.y - dy, true,
                     (u32)(256 - t * 256 / FADE_MS));
    }
}
