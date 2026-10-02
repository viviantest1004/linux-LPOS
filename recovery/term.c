/* term.c - the terminal the recovery shell runs in.
 *
 * The recovery shell used to be the kernel's text console on tty1. That
 * cannot be used by touch alone - the console has no on-screen keyboard -
 * and its font is 16x32 pixels, a line of text 2.5 mm tall on the 4K
 * panel. So the shell runs on a pseudo-terminal inside lp-recovery, and
 * this file is the terminal: it starts /bin/lpsh on the pty's slave side,
 * reads what the shell writes, keeps a screen of character cells, and
 * draws them with the same font machinery as the menus (D2Coding from
 * lp-glyphs-mono.h, 44 px on the 4K panel). Keys come from the physical
 * keyboard (evdev, turned into the bytes a terminal sends) and from the
 * on-screen keyboard; both are written into the pty.
 *
 * ── How much of a terminal ──
 *
 * Enough of xterm for a shell and the tools a repair uses: the shell's
 * line editor, ls --color, less, nano, vi, e2fsck's progress, top.
 *   - UTF-8 in, 16/256/24-bit colour, bold (as bright), underline,
 *     reverse; East Asian wide characters take two cells.
 *   - Cursor movement, erase, insert/delete of lines and characters,
 *     scroll regions, save/restore, the alternate screen (1049) that
 *     full-screen programs use, application cursor keys (DECCKM), tab
 *     stops, the DEC line-drawing character set, auto-wrap with the
 *     deferred wrap xterm has.
 *   - Replies to the queries programs make (cursor position, device
 *     attributes), because a program that asks and gets no answer can
 *     hang waiting for one.
 * Everything else - mouse reporting, titles, bracketed paste, blinking -
 * is parsed and ignored. TERM is xterm-256color, and mkrecovery.sh copies
 * that terminfo entry into the recovery root.
 *
 * ── Scrollback ──
 *
 * 2000 lines. The screen is the last `rows` lines of one ring of lines,
 * and a line scrolled off the top is simply no longer part of the screen,
 * which makes scrollback free. A drag with a finger over the terminal
 * scrolls it (the lines follow the finger), Shift+PgUp/PgDn does it from
 * a keyboard, and any key typed brings the view back to the bottom.
 *
 * When the on-screen keyboard opens or closes the terminal loses or gains
 * rows: lines move between the screen and the scrollback the way xterm
 * does it (empty lines below the cursor go first), the pty is told the
 * new size (TIOCSWINSZ, which sends the shell SIGWINCH), and nothing the
 * person was looking at is lost.
 *
 * ── Drawing ──
 *
 * Glyphs are shrunk from the 4K master to this screen's cell size once,
 * on first use, and kept as alpha maps; a cell is then a fill and one
 * blend. Only rows that changed are redrawn and put on the screen.
 */
#include "recovery.h"
#include "ui/lp-glyphs-mono.h"

#define TIOCGPTN   0x80045430
#define TIOCSPTLCK 0x40045431
#define TIOCSWINSZ 0x5414
#define TIOCSCTTY  0x540E

#define HIST     2000           /* lines in the ring, screen included */
#define MAXROWS  256
#define A_BOLD   0x01000000u
#define A_UNDER  0x02000000u
#define A_REV    0x04000000u
#define A_WIDE   0x08000000u    /* left half of a two-cell character */
#define A_CONT   0x10000000u    /* right half: draws nothing itself */
#define CP_MASK  0x001fffffu
#define DEFCOL   0x01000000u    /* "the default colour" in fg/bg */

typedef struct { u32 cp; u32 fg; u32 bg; } cell_t;

/* ── The palette ──────────────────────────────────────────────────── */
#define T_BG 0x160d15u          /* the panel: the desktop's aubergine, nearly black */
#define T_FG 0xe8e3e6u
static const u32 PAL16[16] = {
    0x2e2a33, 0xe0404a, 0x3cc36b, 0xd9a93a, 0x4a8ce8, 0xc062cc, 0x3ab8c8, 0xd0cfd4,
    0x6c6873, 0xff6b6b, 0x5ce38c, 0xf5c84c, 0x6aa8ff, 0xe07ee8, 0x5ad8e6, 0xffffff,
};

static u32 pal256(int n)
{
    if (n < 16)
        return PAL16[n < 0 ? 0 : n];
    if (n < 232) {
        n -= 16;
        static const u8 lv[6] = { 0, 95, 135, 175, 215, 255 };
        return (u32)lv[n / 36] << 16 | (u32)lv[(n / 6) % 6] << 8 | lv[n % 6];
    }
    if (n > 255)
        n = 255;
    u32 g = (u32)(8 + (n - 232) * 10);
    return g << 16 | g << 8 | g;
}

/* ── State ────────────────────────────────────────────────────────── */
static int mfd = -1;                    /* pty master */
static pid_t child = -1;
static rect_t area;                     /* the panel on the screen */
static int ox0, oy0;                    /* the first cell's top-left */
static int cw, ch, gpx, gasc;           /* cell size, glyph size, baseline */
static int cols, rows, maxcols;

static cell_t *ring;                    /* HIST lines of maxcols */
static int first, nlines;               /* ring window */
static cell_t *alt;                     /* the alternate screen */
static bool on_alt;
static int view;                        /* lines scrolled back, 0 = live */

static int cx, cy;                      /* cursor, on the screen */
static bool wrapnext, autowrap = true, cursor_on = true, appcursor, origin;
static int top, bot;                    /* scroll region, inclusive */
static u32 cur_fg = DEFCOL, cur_bg = DEFCOL, cur_attr;
static bool g0_line, g1_line, shift_out;
static u8 tabs[512];
typedef struct {
    int x, y; u32 fg, bg, attr; bool g0, g1, so, origin, wrapnext;
} saved_t;
static saved_t saved, saved_alt;
static bool insert_mode;
static int drawn_cx = -1, drawn_cy = -1;

static bool dirty_row[MAXROWS];
static bool all_dirty;
static int drag_acc;

static cell_t *L(int y)
{
    if (on_alt)
        return alt + (u64)y * maxcols;
    int i = nlines - rows + y;
    return ring + (u64)((first + i) % HIST) * maxcols;
}

static cell_t blank_cell(void)
{
    /* Erasing fills with the current background (xterm's bce). */
    return (cell_t){ ' ', DEFCOL, cur_bg };
}

static void clear_cells(cell_t *c, int n)
{
    cell_t b = blank_cell();
    for (int i = 0; i < n; i++)
        c[i] = b;
}

static void touch(int y)
{
    if (y >= 0 && y < rows && y < MAXROWS)
        dirty_row[y] = true;
}

static void touch_all(void) { all_dirty = true; }

/* ── Scrolling ────────────────────────────────────────────────────── */
static void ring_append(void)
{
    if (nlines < HIST)
        nlines++;
    else {
        first = (first + 1) % HIST;
        if (view > 0 && view < HIST - rows)
            view++;                     /* keep the scrolled-back view still */
    }
    cell_t *l = ring + (u64)((first + nlines - 1) % HIST) * maxcols;
    cell_t b = { ' ', DEFCOL, DEFCOL };
    for (int i = 0; i < maxcols; i++)
        l[i] = b;
}

static void scroll_up(int n)
{
    if (n <= 0)
        return;
    if (n > bot - top + 1)
        n = bot - top + 1;
    if (!on_alt && top == 0 && bot == rows - 1) {
        /* The whole screen: lines go into the scrollback. */
        for (int i = 0; i < n; i++) {
            ring_append();
            clear_cells(L(rows - 1), maxcols);
        }
    } else {
        for (int y = top; y <= bot - n; y++)
            memcpy(L(y), L(y + n), sizeof(cell_t) * (size_t)maxcols);
        for (int y = bot - n + 1; y <= bot; y++)
            clear_cells(L(y), maxcols);
    }
    for (int y = top; y <= bot; y++)
        touch(y);
}

static void scroll_down(int n)
{
    if (n <= 0)
        return;
    if (n > bot - top + 1)
        n = bot - top + 1;
    for (int y = bot; y >= top + n; y--)
        memcpy(L(y), L(y - n), sizeof(cell_t) * (size_t)maxcols);
    for (int y = top; y < top + n; y++)
        clear_cells(L(y), maxcols);
    for (int y = top; y <= bot; y++)
        touch(y);
}

/* ── The pty ──────────────────────────────────────────────────────── */
static void send(const char *s, size_t n)
{
    while (n > 0 && mfd >= 0) {
        long w = lp_write(mfd, s, n);
        if (w == -11) {                 /* EAGAIN: the shell is not reading */
            lp_sleep_ms(5);
            continue;
        }
        if (w <= 0)
            return;
        s += w;
        n -= (size_t)w;
    }
}

static void sends(const char *s) { send(s, strlen(s)); }

static void set_winsize(void)
{
    if (mfd < 0)
        return;
    u16 ws[4] = { (u16)rows, (u16)cols, (u16)(cols * cw), (u16)(rows * ch) };
    lp_ioctl(mfd, TIOCSWINSZ, ws);
}

/* ── Geometry ─────────────────────────────────────────────────────── */
static void metrics(void)
{
    /* The 4K master shrunk by the screen's scale, but never below a
     * 16 px face: a VM window at a third of 4K still gets readable
     * text, and more columns than 80. */
    int pm = lpui_px(1000);
    int floor = 16 * 1000 / LPG_MONO_PX;
    if (pm < floor)
        pm = floor;
    gpx = (LPG_MONO_PX * pm + 500) / 1000;
    cw = (LPG_MONO_CELL_W * pm + 500) / 1000;
    ch = (LPG_MONO_LINE * pm + 500) / 1000;
    gasc = (LPG_MONO_ASCENT * pm + 500) / 1000;
    if (cw < 6) cw = 6;
    if (ch < 12) ch = 12;
}

static void resize_rows(int nr)
{
    if (nr < 2)
        nr = 2;
    if (nr > MAXROWS)
        nr = MAXROWS;
    if (nr == rows)
        return;
    if (on_alt) {
        cell_t *n = malloc(sizeof(cell_t) * (size_t)maxcols * (size_t)nr);
        if (!n)
            return;
        for (int y = 0; y < nr; y++) {
            if (y < rows)
                memcpy(n + (u64)y * maxcols, alt + (u64)y * maxcols, sizeof(cell_t) * (size_t)maxcols);
            else {
                cell_t b = { ' ', DEFCOL, DEFCOL };
                for (int i = 0; i < maxcols; i++)
                    n[(u64)y * maxcols + i] = b;
            }
        }
        free(alt);
        alt = n;
    }
    if (nr > rows) {
        /* Taller: lines come back from the scrollback at the top, and
         * when there are none, empty ones are added at the bottom. */
        int d = nr - rows;
        int hist = nlines - rows;
        int pull = d < hist ? d : hist;
        for (int i = pull; i < d; i++)
            ring_append();
        if (on_alt)
            saved_alt.y += pull;        /* the main screen's cursor */
        else
            cy += pull;
    } else {
        /* Shorter: empty lines below the cursor go first, then lines off
         * the top into the scrollback. */
        int d = rows - nr, drop = 0;
        if (!on_alt) {
            for (int y = rows - 1; y > cy && drop < d; y--) {
                cell_t *l = L(y);
                bool empty = true;
                for (int i = 0; i < cols && empty; i++)
                    empty = (l[i].cp & CP_MASK) == ' ' && l[i].bg == DEFCOL;
                if (!empty)
                    break;
                drop++;
            }
            nlines -= drop;
            cy -= d - drop;
        } else {
            saved_alt.y -= d;
            if (saved_alt.y < 0)
                saved_alt.y = 0;
        }
        if (cy < 0)
            cy = 0;
    }
    rows = nr;
    if (cy >= rows)
        cy = rows - 1;
    top = 0;
    bot = rows - 1;
    view = 0;
    touch_all();
    set_winsize();
}

void term_set_area(rect_t a)
{
    area = a;
    int pad = lpui_px(28);
    ox0 = a.x + pad;
    oy0 = a.y + pad;
    int nr = (a.h - 2 * pad) / ch;
    if (!ring) {
        rows = nr < 2 ? 2 : nr > MAXROWS ? MAXROWS : nr;
        return;
    }
    resize_rows(nr);
}

rect_t term_area(void) { return area; }

/* ── Characters ───────────────────────────────────────────────────── */
static int wcw(u32 c)
{
    if (c < 0x300)
        return 1;
    if ((c >= 0x300 && c <= 0x36f) || (c >= 0x200b && c <= 0x200f) ||
        (c >= 0xfe00 && c <= 0xfe0f) || (c >= 0x1ab0 && c <= 0x1aff) ||
        (c >= 0x20d0 && c <= 0x20ff))
        return 0;
    if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0x303e) ||
        (c >= 0x3041 && c <= 0x33ff) || (c >= 0x3400 && c <= 0x4dbf) ||
        (c >= 0x4e00 && c <= 0x9fff) || (c >= 0xa000 && c <= 0xa4cf) ||
        (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) ||
        (c >= 0xfe30 && c <= 0xfe4f) || (c >= 0xff00 && c <= 0xff60) ||
        (c >= 0xffe0 && c <= 0xffe6) || (c >= 0x1f300 && c <= 0x1f64f) ||
        (c >= 0x1f900 && c <= 0x1f9ff) || (c >= 0x20000 && c <= 0x3fffd))
        return 2;
    return 1;
}

/* DEC special graphics: what `lqqk` means after ESC ( 0. */
static u32 dec_line(u32 c)
{
    static const u16 M[32] = {
        0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1,   /* ` a b c d e f g */
        0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba,   /* h i j k l m n o */
        0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c,   /* p q r s t u v w */
        0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7, ' ',      /* x y z { | } ~   */
    };
    if (c >= 0x60 && c <= 0x7e)
        return M[c - 0x60];
    return c;
}

static void put(u32 c)
{
    if ((shift_out ? g1_line : g0_line) && c >= 0x60 && c <= 0x7e)
        c = dec_line(c);
    int w = wcw(c);
    if (w == 0)
        return;                         /* combining marks: not drawn */
    if (wrapnext) {
        wrapnext = false;
        if (autowrap) {
            cx = 0;
            if (cy == bot)
                scroll_up(1);
            else if (cy < rows - 1)
                cy++;
        }
    }
    if (w == 2 && cx == cols - 1) {
        /* A wide character does not fit in the last column: wrap first. */
        L(cy)[cx] = blank_cell();
        if (autowrap) {
            cx = 0;
            if (cy == bot)
                scroll_up(1);
            else if (cy < rows - 1)
                cy++;
        }
    }
    cell_t *l = L(cy);
    /* Overwriting half of a wide character clears the other half. */
    if (l[cx].cp & A_CONT && cx > 0)
        l[cx - 1] = blank_cell();
    if (l[cx].cp & A_WIDE && cx + 1 < cols)
        l[cx + 1] = blank_cell();
    if (insert_mode && cx + w < cols)
        memmove(l + cx + w, l + cx, sizeof(cell_t) * (size_t)(cols - cx - w));
    l[cx] = (cell_t){ c | cur_attr | (w == 2 ? A_WIDE : 0), cur_fg, cur_bg };
    if (w == 2 && cx + 1 < cols)
        l[cx + 1] = (cell_t){ ' ' | A_CONT, cur_fg, cur_bg };
    touch(cy);
    if (cx + w >= cols) {
        cx = cols - 1;
        wrapnext = true;
    } else
        cx += w;
}

static void newline(void)
{
    if (cy == bot)
        scroll_up(1);
    else if (cy < rows - 1)
        cy++;
    touch(cy);
}

/* ── Escape sequences ─────────────────────────────────────────────── */
enum { S_GROUND, S_ESC, S_CSI, S_OSC, S_OSC_ESC, S_CHARSET, S_STR, S_STR_ESC, S_HASH };
static int st, cs_target;
static int par[16], npar;
static bool par_started;
static char priv, inter;
static u32 utf_cp;
static int utf_left;

static int P0(int i, int dflt) { return i < npar && par[i] > 0 ? par[i] : dflt; }

static void clamp_cursor(void)
{
    if (cx < 0) cx = 0;
    if (cx >= cols) cx = cols - 1;
    int lo = origin ? top : 0, hi = origin ? bot : rows - 1;
    if (cy < lo) cy = lo;
    if (cy > hi) cy = hi;
}

static void move_to(int x, int y)
{
    touch(cy);
    cx = x;
    cy = y + (origin ? top : 0);
    wrapnext = false;
    clamp_cursor();
    touch(cy);
}

static void erase_line(int y, int from, int to)
{
    if (from < 0) from = 0;
    if (to > cols) to = cols;
    if (from < to)
        clear_cells(L(y) + from, to - from);
    touch(y);
}

static void reset_all(void)
{
    cur_fg = cur_bg = DEFCOL;
    cur_attr = 0;
    top = 0;
    bot = rows - 1;
    autowrap = true;
    cursor_on = true;
    appcursor = origin = false;
    g0_line = g1_line = shift_out = false;
    wrapnext = false;
    insert_mode = false;
    for (int i = 0; i < (int)sizeof tabs; i++)
        tabs[i] = i % 8 == 0;
}

static void save_cursor(void)
{
    saved = (saved_t){ cx, cy, cur_fg, cur_bg, cur_attr, g0_line, g1_line,
                       shift_out, origin, wrapnext };
}

static void restore_cursor(void)
{
    touch(cy);
    cx = saved.x;
    cy = saved.y;
    cur_fg = saved.fg;
    cur_bg = saved.bg;
    cur_attr = saved.attr;
    g0_line = saved.g0;
    g1_line = saved.g1;
    shift_out = saved.so;
    origin = saved.origin;
    wrapnext = saved.wrapnext;
    if (cx >= cols) cx = cols - 1;
    if (cy >= rows) cy = rows - 1;
    touch(cy);
}

static void alt_screen(bool on, bool with_cursor)
{
    if (on == on_alt)
        return;
    if (on) {
        if (with_cursor)
            save_cursor();
        saved_alt = saved;
        free(alt);
        alt = malloc(sizeof(cell_t) * (size_t)maxcols * (size_t)rows);
        if (!alt)
            return;
        on_alt = true;
        for (int y = 0; y < rows; y++)
            clear_cells(L(y), maxcols);
    } else {
        on_alt = false;
        free(alt);
        alt = 0;
        saved = saved_alt;
        if (with_cursor)
            restore_cursor();
    }
    top = 0;
    bot = rows - 1;
    view = 0;
    touch_all();
}

static void sgr(void)
{
    if (npar == 0) {
        cur_fg = cur_bg = DEFCOL;
        cur_attr = 0;
        return;
    }
    for (int i = 0; i < npar; i++) {
        int p = par[i];
        if (p == 0) { cur_fg = cur_bg = DEFCOL; cur_attr = 0; }
        else if (p == 1) cur_attr |= A_BOLD;
        else if (p == 4) cur_attr |= A_UNDER;
        else if (p == 7) cur_attr |= A_REV;
        else if (p == 22) cur_attr &= ~A_BOLD;
        else if (p == 24) cur_attr &= ~A_UNDER;
        else if (p == 27) cur_attr &= ~A_REV;
        else if (p >= 30 && p <= 37) cur_fg = PAL16[p - 30];
        else if (p == 39) cur_fg = DEFCOL;
        else if (p >= 40 && p <= 47) cur_bg = PAL16[p - 40];
        else if (p == 49) cur_bg = DEFCOL;
        else if (p >= 90 && p <= 97) cur_fg = PAL16[p - 90 + 8];
        else if (p >= 100 && p <= 107) cur_bg = PAL16[p - 100 + 8];
        else if (p == 38 || p == 48) {
            u32 col;
            if (i + 2 < npar && par[i + 1] == 5) {
                col = pal256(par[i + 2]);
                i += 2;
            } else if (i + 4 < npar && par[i + 1] == 2) {
                col = (u32)(par[i + 2] & 255) << 16 | (u32)(par[i + 3] & 255) << 8 | (u32)(par[i + 4] & 255);
                i += 4;
            } else
                break;
            if (p == 38) cur_fg = col;
            else cur_bg = col;
        }
    }
}

static void set_mode(bool on)
{
    for (int i = 0; i < (npar ? npar : 1); i++) {
        int p = npar ? par[i] : 0;
        if (!priv && p == 4)
            insert_mode = on;           /* IRM */
        if (priv == '?') {
            switch (p) {
            case 1:    appcursor = on; break;
            case 6:    origin = on; move_to(0, 0); break;
            case 7:    autowrap = on; break;
            case 25:   cursor_on = on; touch(cy); break;
            case 47: case 1047: alt_screen(on, false); break;
            case 1049: alt_screen(on, true); break;
            case 1048: if (on) save_cursor(); else restore_cursor(); break;
            default:   break;           /* mouse, paste, focus: ignored */
            }
        }
    }
}

static void csi(u32 f)
{
    int n = P0(0, 1);
    if (priv == '>' || priv == '=') {
        if (f == 'c')
            sends("\033[>0;10;0c");     /* secondary attributes */
        return;
    }
    if (inter == ' ' || inter == '!' || inter == '"' || inter == '$') {
        if (inter == '!' && f == 'p') { /* DECSTR, soft reset */
            reset_all();
            touch_all();
        }
        return;
    }
    switch (f) {
    case '@': {                         /* ICH */
        cell_t *l = L(cy);
        if (n > cols - cx) n = cols - cx;
        memmove(l + cx + n, l + cx, sizeof(cell_t) * (size_t)(cols - cx - n));
        clear_cells(l + cx, n);
        touch(cy);
        break;
    }
    case 'A': move_to(cx, cy - (origin ? top : 0) - n); break;
    case 'B': case 'e': move_to(cx, cy - (origin ? top : 0) + n); break;
    case 'C': case 'a': move_to(cx + n, cy - (origin ? top : 0)); break;
    case 'D': move_to(cx - n, cy - (origin ? top : 0)); break;
    case 'E': move_to(0, cy - (origin ? top : 0) + n); break;
    case 'F': move_to(0, cy - (origin ? top : 0) - n); break;
    case 'G': case '`': move_to(n - 1, cy - (origin ? top : 0)); break;
    case 'H': case 'f': move_to(P0(1, 1) - 1, P0(0, 1) - 1); break;
    case 'd': move_to(cx, n - 1); break;
    case 'I':
        for (int i = 0; i < n; i++) {
            do cx++; while (cx < cols - 1 && !tabs[cx]);
        }
        if (cx >= cols) cx = cols - 1;
        break;
    case 'Z':
        for (int i = 0; i < n && cx > 0; i++) {
            do cx--; while (cx > 0 && !tabs[cx]);
        }
        break;
    case 'J': {
        int m = npar ? par[0] : 0;
        if (m == 0) {
            erase_line(cy, cx, cols);
            for (int y = cy + 1; y < rows; y++) erase_line(y, 0, cols);
        } else if (m == 1) {
            erase_line(cy, 0, cx + 1);
            for (int y = 0; y < cy; y++) erase_line(y, 0, cols);
        } else if (m == 2 || m == 3) {
            for (int y = 0; y < rows; y++) erase_line(y, 0, cols);
        }
        wrapnext = false;
        break;
    }
    case 'K': {
        int m = npar ? par[0] : 0;
        if (m == 0) erase_line(cy, cx, cols);
        else if (m == 1) erase_line(cy, 0, cx + 1);
        else erase_line(cy, 0, cols);
        wrapnext = false;
        break;
    }
    case 'L':
        if (cy >= top && cy <= bot) {
            int t = top;
            top = cy;
            scroll_down(n);
            top = t;
        }
        break;
    case 'M':
        if (cy >= top && cy <= bot) {
            /* Moved inside the region by hand: deleting lines never
             * feeds the scrollback, which scroll_up would. */
            if (n > bot - cy + 1) n = bot - cy + 1;
            for (int y = cy; y <= bot - n; y++)
                memcpy(L(y), L(y + n), sizeof(cell_t) * (size_t)maxcols);
            for (int y = bot - n + 1; y <= bot; y++)
                clear_cells(L(y), maxcols);
            for (int y = cy; y <= bot; y++)
                touch(y);
        }
        break;
    case 'P': {                         /* DCH */
        cell_t *l = L(cy);
        if (n > cols - cx) n = cols - cx;
        memmove(l + cx, l + cx + n, sizeof(cell_t) * (size_t)(cols - cx - n));
        clear_cells(l + cols - n, n);
        touch(cy);
        break;
    }
    case 'X': erase_line(cy, cx, cx + n); break;
    case 'S': scroll_up(n); break;
    case 'T': if (!priv) scroll_down(n); break;
    case 'b':                           /* REP: repeat the last character */
        if (cx > 0) {
            u32 c = L(cy)[cx - 1].cp & CP_MASK;
            for (int i = 0; i < n && i < cols; i++)
                put(c);
        }
        break;
    case 'c': if (!priv) sends("\033[?62;22c"); break;
    case 'g':
        if (!npar || par[0] == 0) { if (cx < (int)sizeof tabs) tabs[cx] = 0; }
        else if (par[0] == 3) memset(tabs, 0, sizeof tabs);
        break;
    case 'h': set_mode(true); break;
    case 'l': set_mode(false); break;
    case 'm': sgr(); break;
    case 'n':
        if (npar && par[0] == 6) {
            char b[32];
            int k = snprintf(b, sizeof b, "\033[%d;%dR", cy - (origin ? top : 0) + 1, cx + 1);
            send(b, (size_t)k);
        } else if (npar && par[0] == 5)
            sends("\033[0n");
        break;
    case 'r':
        if (!priv) {
            int t = P0(0, 1) - 1, b = P0(1, rows) - 1;
            if (b >= rows) b = rows - 1;
            if (t < b) {
                top = t;
                bot = b;
                move_to(0, 0);
            }
        }
        break;
    case 's': if (!priv) save_cursor(); break;
    case 'u': if (!priv) restore_cursor(); break;
    default: break;                     /* t (window ops), q, ... */
    }
}

static void esc(u32 c)
{
    switch (c) {
    case '[': st = S_CSI; npar = 0; par_started = false; priv = inter = 0;
              memset(par, 0, sizeof par); return;
    case ']': st = S_OSC; return;
    case 'P': case 'X': case '^': case '_': st = S_STR; return;
    case '(': case ')': case '*': case '+': st = S_CHARSET; cs_target = (int)c; return;
    case '#': st = S_HASH; return;
    case '7': save_cursor(); break;
    case '8': restore_cursor(); break;
    case 'D': newline(); break;
    case 'E': cx = 0; newline(); break;
    case 'M':
        if (cy == top) scroll_down(1);
        else if (cy > 0) { touch(cy); cy--; touch(cy); }
        break;
    case 'H': if (cx < (int)sizeof tabs) tabs[cx] = 1; break;
    case 'c':
        reset_all();
        alt_screen(false, false);
        for (int y = 0; y < rows; y++) erase_line(y, 0, cols);
        move_to(0, 0);
        break;
    default: break;                     /* = > (keypad modes) and the rest */
    }
    st = S_GROUND;
}

static void feed(u8 b)
{
    /* Strings (OSC, DCS...) end at BEL or ESC \ and are thrown away. */
    if (st == S_OSC || st == S_STR) {
        if (b == 7 && st == S_OSC) st = S_GROUND;
        else if (b == 0x1b) st = st == S_OSC ? S_OSC_ESC : S_STR_ESC;
        return;
    }
    if (st == S_OSC_ESC || st == S_STR_ESC) {
        st = b == '\\' ? S_GROUND : (st == S_OSC_ESC ? S_OSC : S_STR);
        return;
    }
    /* C0 controls act in every state (as in a VT100). */
    if (b < 0x20 || b == 0x7f) {
        switch (b) {
        case 7: break;
        case 8: if (cx > 0) { cx--; wrapnext = false; } break;
        case 9:
            do cx++; while (cx < cols - 1 && !tabs[cx]);
            if (cx >= cols) cx = cols - 1;
            break;
        case 10: case 11: case 12: newline(); wrapnext = false; break;
        case 13: cx = 0; wrapnext = false; break;
        case 14: shift_out = true; break;
        case 15: shift_out = false; break;
        case 0x18: case 0x1a: st = S_GROUND; break;
        case 0x1b: st = S_ESC; utf_left = 0; break;
        default: break;
        }
        return;
    }
    switch (st) {
    case S_ESC:
        esc(b);
        return;
    case S_CHARSET:
        if (cs_target == '(') g0_line = b == '0';
        else if (cs_target == ')') g1_line = b == '0';
        st = S_GROUND;
        return;
    case S_HASH:
        st = S_GROUND;
        return;
    case S_CSI:
        if (b >= '0' && b <= '9') {
            if (npar == 0) npar = 1;
            par[npar - 1] = par[npar - 1] * 10 + (b - '0');
            if (par[npar - 1] > 65535) par[npar - 1] = 65535;
        } else if (b == ';' || b == ':') {
            if (npar == 0) npar = 1;
            if (npar < 16) par[npar++] = 0;
        } else if (b >= 0x3c && b <= 0x3f) {
            priv = (char)b;
        } else if (b >= 0x20 && b <= 0x2f) {
            inter = (char)b;
        } else if (b >= 0x40 && b <= 0x7e) {
            st = S_GROUND;
            csi(b);
        }
        return;
    default:
        break;
    }
    /* Ground: UTF-8. */
    if (b < 0x80) {
        utf_left = 0;
        put(b);
    } else if ((b & 0xc0) == 0x80) {
        if (utf_left > 0) {
            utf_cp = utf_cp << 6 | (b & 0x3f);
            if (--utf_left == 0)
                put(utf_cp);
        }
    } else if ((b & 0xe0) == 0xc0) { utf_cp = b & 0x1f; utf_left = 1; }
    else if ((b & 0xf0) == 0xe0) { utf_cp = b & 0x0f; utf_left = 2; }
    else if ((b & 0xf8) == 0xf0) { utf_cp = b & 0x07; utf_left = 3; }
    else put(0xfffd);
}

static void feed_str(const char *s)
{
    while (*s)
        feed((u8)*s++);
}

/* ── Starting and stopping the shell ──────────────────────────────── */
static bool start_pty(char *slave, size_t n)
{
    long fd = lp_open("/dev/ptmx", O_RDWR | O_NOCTTY_ | O_CLOEXEC | O_NONBLOCK, 0);
    if (fd < 0) {
        /* devpts not mounted yet (init mounts it; a hand-started test
         * may not have): do it here. */
        lp_mkdir("/dev/pts", 0755);
        lp_mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "ptmxmode=0666");
        fd = lp_open("/dev/ptmx", O_RDWR | O_NOCTTY_ | O_CLOEXEC | O_NONBLOCK, 0);
        if (fd < 0)
            return false;
    }
    int unlock = 0;
    u32 num = 0;
    if (lp_ioctl((int)fd, TIOCSPTLCK, &unlock) < 0 || lp_ioctl((int)fd, TIOCGPTN, &num) < 0) {
        lp_close((int)fd);
        return false;
    }
    snprintf(slave, n, "/dev/pts/%u", num);
    mfd = (int)fd;
    return true;
}

const char *term_start(rect_t a, const char *hello)
{
    metrics();
    area = a;
    int pad = lpui_px(28);
    maxcols = (a.w - 2 * pad) / cw;
    if (maxcols < 20)
        maxcols = 20;
    if (maxcols > 500)
        maxcols = 500;
    cols = maxcols;
    if (!ring) {
        ring = malloc(sizeof(cell_t) * (size_t)maxcols * HIST);
        if (!ring)
            return "out of memory";
    }
    first = 0;
    nlines = 0;
    rows = 0;
    on_alt = false;
    view = 0;
    cx = cy = 0;
    drawn_cx = drawn_cy = -1;
    term_set_area(a);                   /* rows empty lines */
    reset_all();
    touch_all();

    char slave[32];
    if (!start_pty(slave, sizeof slave))
        return "no pseudo-terminal (/dev/ptmx)";
    set_winsize();
    if (hello)
        feed_str(hello);

    pid_t pid = lp_fork();
    if (pid < 0) {
        lp_close(mfd);
        mfd = -1;
        return "fork failed";
    }
    if (pid == 0) {
        lp_setsid();
        long s = lp_open(slave, O_RDWR, 0);
        if (s < 0)
            lp_exit(126);
        lp_ioctl((int)s, TIOCSCTTY, (void *)0);
        lp_dup2((int)s, 0);
        lp_dup2((int)s, 1);
        lp_dup2((int)s, 2);
        if (s > 2)
            lp_close((int)s);
        lp_term_sane(0);
        lp_term_set_utf8(0);
        for (int sig = 1; sig < 32; sig++)
            if (sig != 9 && sig != 19)
                lp_signal_default(sig);
        mkdirs("/root", 0700);
        lp_chdir("/root");
        const char *sh = lp_exists("/bin/lpsh") ? "/bin/lpsh" : "/bin/sh";
        char *argv[] = { (char *)"-lpsh", 0 };
        char *envp[] = { "HOME=/root", "PATH=/usr/sbin:/usr/bin:/sbin:/bin",
                         "TERM=xterm-256color", "COLORTERM=truecolor", "USER=root",
                         "LOGNAME=root", "SHELL=/bin/lpsh", "LANG=C.UTF-8",
                         "PS1=recovery# ", 0 };
        lp_execve(sh, argv, envp);
        lp_exit(127);
    }
    child = pid;
    in_watch_fd(mfd);
    return 0;
}

bool term_running(void) { return child > 0; }
int  term_fd(void) { return mfd; }

void term_stop(void)
{
    in_watch_fd(-1);
    if (child > 0) {
        /* Hang up the whole session, as closing a terminal does. */
        lp_kill(-child, SIGHUP);
        lp_kill(child, SIGHUP);
        for (int i = 0; i < 20; i++) {
            int stt;
            if (lp_waitpid(child, &stt, WNOHANG) == child) {
                child = -1;
                break;
            }
            lp_sleep_ms(50);
        }
        if (child > 0) {
            lp_kill(-child, SIGKILL);
            lp_kill(child, SIGKILL);
            int stt;
            lp_waitpid(child, &stt, 0);
            child = -1;
        }
    }
    if (mfd >= 0)
        lp_close(mfd);
    mfd = -1;
    /* Anything the shell left running in the background: its session is
     * gone, reap what exits. */
    int stt;
    while (lp_waitpid(-1, &stt, WNOHANG) > 0)
        ;
    free(alt);
    alt = 0;
    on_alt = false;
}

bool term_pump(void)
{
    if (mfd < 0)
        return false;
    static u8 buf[16384];
    for (int round = 0; round < 8; round++) {
        long n = lp_read(mfd, buf, sizeof buf);
        if (n > 0) {
            for (long i = 0; i < n; i++)
                feed(buf[i]);
            continue;
        }
        if (n == -11)                   /* EAGAIN: all read */
            break;
        /* EIO: every slave descriptor is closed - the shell is gone. */
        break;
    }
    if (child > 0) {
        int stt;
        if (lp_waitpid(child, &stt, WNOHANG) == child) {
            rlog("recovery shell exited (status %d)",
                 LP_WIFEXITED(stt) ? LP_WEXITSTATUS(stt) : -1);
            child = -1;
            /* whatever it printed last */
            long n;
            while ((n = lp_read(mfd, buf, sizeof buf)) > 0)
                for (long i = 0; i < n; i++)
                    feed(buf[i]);
        }
    }
    return child > 0;
}

/* ── Keys ─────────────────────────────────────────────────────────── */
static void to_bottom(void)
{
    if (view) {
        view = 0;
        touch_all();
    }
}

/* Cursor and editing keys, with xterm's modifier encoding. */
static void send_special(char final, int tilde, bool shift, bool ctrl, bool alt_)
{
    int mod = 1 + (shift ? 1 : 0) + (alt_ ? 2 : 0) + (ctrl ? 4 : 0);
    char b[24];
    int k;
    if (tilde)
        k = mod > 1 ? snprintf(b, sizeof b, "\033[%d;%d~", tilde, mod)
                    : snprintf(b, sizeof b, "\033[%d~", tilde);
    else if (mod > 1)
        k = snprintf(b, sizeof b, "\033[1;%d%c", mod, final);
    else
        k = snprintf(b, sizeof b, appcursor ? "\033O%c" : "\033[%c", final);
    send(b, (size_t)k);
}

static void send_fn(int n, bool shift, bool ctrl, bool alt_)
{
    static const int T[13] = { 0, 0, 0, 0, 0, 15, 17, 18, 19, 20, 21, 23, 24 };
    if (n >= 1 && n <= 4) {
        char b[16];
        int mod = 1 + (shift ? 1 : 0) + (alt_ ? 2 : 0) + (ctrl ? 4 : 0);
        int k = mod > 1 ? snprintf(b, sizeof b, "\033[1;%d%c", mod, 'P' + n - 1)
                        : snprintf(b, sizeof b, "\033O%c", 'P' + n - 1);
        send(b, (size_t)k);
    } else if (n >= 5 && n <= 12)
        send_special(0, T[n], shift, ctrl, alt_);
}

static void send_text(const char *t, bool ctrl, bool alt_)
{
    char b[16];
    int k = 0;
    if (alt_)
        b[k++] = 0x1b;
    if (ctrl && t[0] && !t[1]) {
        char c = t[0];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c >= '@' && c <= '_') b[k++] = (char)(c - '@');
        else if (c == ' ' || c == '2') b[k++] = 0;
        else if (c == '?' || c == '/') b[k++] = 0x7f;
        else b[k++] = c;
        send(b, (size_t)k);
        return;
    }
    size_t n = strlen(t);
    if (n + (size_t)k > sizeof b)
        n = sizeof b - (size_t)k;
    memcpy(b + k, t, n);
    send(b, n + (size_t)k);
}

static void scroll_view(int lines)
{
    int maxv = (on_alt ? 0 : nlines - rows);
    int v = view + lines;
    if (v < 0) v = 0;
    if (v > maxv) v = maxv;
    if (v != view) {
        view = v;
        touch_all();
    }
}

void term_key(const uev_t *e)
{
    if (e->shift && (e->code == K_PGUP || e->code == K_PGDN)) {
        scroll_view(e->code == K_PGUP ? rows / 2 : -rows / 2);
        return;
    }
    to_bottom();
    bool s = e->shift, c = e->ctrl, a = e->alt;
    switch (e->code) {
    case K_ENTER: case K_KPENTER: send(a ? "\033\r" : "\r", a ? 2 : 1); return;
    case K_BACKSPACE: send(a ? "\033\177" : c ? "\010" : "\177", a ? 2 : 1); return;
    case K_TAB: if (s) sends("\033[Z"); else send("\t", 1); return;
    case K_ESC: send("\033", 1); return;
    case K_UP: send_special('A', 0, s, c, a); return;
    case K_DOWN: send_special('B', 0, s, c, a); return;
    case K_RIGHT: send_special('C', 0, s, c, a); return;
    case K_LEFT: send_special('D', 0, s, c, a); return;
    case K_HOME: send_special('H', 0, s, c, a); return;
    case K_END: send_special('F', 0, s, c, a); return;
    case K_INSERT: send_special(0, 2, s, c, a); return;
    case K_DELETE: send_special(0, 3, s, c, a); return;
    case K_PGUP: send_special(0, 5, s, c, a); return;
    case K_PGDN: send_special(0, 6, s, c, a); return;
    case K_F11: send_fn(11, s, c, a); return;
    case K_F12: send_fn(12, s, c, a); return;
    }
    if (e->code >= K_F1 && e->code < K_F1 + 10) {
        send_fn(e->code - K_F1 + 1, s, c, a);
        return;
    }
    if (e->ch) {
        char t[2] = { (char)e->ch, 0 };
        send_text(t, c, a);
    }
}

void term_osk(const osk_key_t *k)
{
    to_bottom();
    bool s = k->shift, c = k->ctrl, a = k->alt;
    switch (k->what) {
    case OSK_TEXT: send_text(k->text, c, a); return;
    case OSK_BKSP: send(a ? "\033\177" : "\177", a ? 2 : 1); return;
    case OSK_ENTER: send("\r", 1); return;
    case OSK_TAB: if (s) sends("\033[Z"); else send("\t", 1); return;
    case OSK_ESC: send("\033", 1); return;
    case OSK_UP: send_special('A', 0, s, c, a); return;
    case OSK_DOWN: send_special('B', 0, s, c, a); return;
    case OSK_RIGHT: send_special('C', 0, s, c, a); return;
    case OSK_LEFT: send_special('D', 0, s, c, a); return;
    case OSK_HOME: send_special('H', 0, s, c, a); return;
    case OSK_END: send_special('F', 0, s, c, a); return;
    case OSK_PGUP: send_special(0, 5, s, c, a); return;
    case OSK_PGDN: send_special(0, 6, s, c, a); return;
    case OSK_DEL: send_special(0, 3, s, c, a); return;
    case OSK_FN: send_fn(k->fn, s, c, a); return;
    }
}

void term_drag(int dy_px, bool start)
{
    if (start)
        drag_acc = 0;
    drag_acc += dy_px;
    int lines = drag_acc / ch;
    if (lines) {
        drag_acc -= lines * ch;
        scroll_view(lines);             /* finger down: older lines come into view */
    }
}

/* ── Drawing ──────────────────────────────────────────────────────── */
typedef struct { u32 cp; u8 *a; } gcache_t;
#define GC_N 512
static gcache_t gc[GC_N];
static u8 *gc_ascii[128];

static u8 *render_glyph(u32 cp)
{
    u8 *a = malloc((size_t)cw * (size_t)ch);
    if (!a)
        return 0;
    memset(a, 0, (size_t)cw * (size_t)ch);
    const lpg_glyph_t *g = lpui_glyph(&LPG_MONO, cp);
    if (!g)
        return a;
    static u32 *tmp;
    static int tmpn;
    if (tmpn < cw * ch) {
        free(tmp);
        tmp = malloc(sizeof(u32) * (size_t)cw * (size_t)ch);
        tmpn = tmp ? cw * ch : 0;
        if (!tmp)
            return a;
    }
    memset(tmp, 0, sizeof(u32) * (size_t)cw * (size_t)ch);
    lpui_canvas_t c = { tmp, cw, ch, cw };
    lpui_glyph_draw(&c, &LPG_MONO, g, gpx, 0, gasc, 0xffffff, 256);
    for (int i = 0; i < cw * ch; i++)
        a[i] = (u8)((tmp[i] >> 8) & 255);
    return a;
}

static u8 *glyph_alpha(u32 cp)
{
    if (cp < 128)
        return gc_ascii[cp] ? gc_ascii[cp] : (gc_ascii[cp] = render_glyph(cp));
    u32 h = (cp * 2654435761u) % GC_N;
    for (int i = 0; i < GC_N; i++) {
        gcache_t *e = &gc[(h + (u32)i) % GC_N];
        if (e->a && e->cp == cp)
            return e->a;
        if (!e->a) {
            e->cp = cp;
            e->a = render_glyph(cp);
            return e->a;
        }
    }
    return 0;
}

/* Box-drawing lines are drawn, not taken from the font, so that they
 * join across cells at any cell size. up/down/left/right: 0 none,
 * 1 light, 2 heavy, 3 double. */
static bool box_parts(u32 c, int *u, int *d, int *l, int *r)
{
    static const struct { u16 cp; u8 u, d, l, r; } B[] = {
        { 0x2500, 0, 0, 1, 1 }, { 0x2501, 0, 0, 2, 2 }, { 0x2502, 1, 1, 0, 0 }, { 0x2503, 2, 2, 0, 0 },
        { 0x250c, 0, 1, 0, 1 }, { 0x250f, 0, 2, 0, 2 }, { 0x2510, 0, 1, 1, 0 }, { 0x2513, 0, 2, 2, 0 },
        { 0x2514, 1, 0, 0, 1 }, { 0x2517, 2, 0, 0, 2 }, { 0x2518, 1, 0, 1, 0 }, { 0x251b, 2, 0, 2, 0 },
        { 0x251c, 1, 1, 0, 1 }, { 0x2523, 2, 2, 0, 2 }, { 0x2524, 1, 1, 1, 0 }, { 0x252b, 2, 2, 2, 0 },
        { 0x252c, 0, 1, 1, 1 }, { 0x2533, 0, 2, 2, 2 }, { 0x2534, 1, 0, 1, 1 }, { 0x253b, 2, 0, 2, 2 },
        { 0x253c, 1, 1, 1, 1 }, { 0x254b, 2, 2, 2, 2 },
        { 0x2550, 0, 0, 3, 3 }, { 0x2551, 3, 3, 0, 0 }, { 0x2554, 0, 3, 0, 3 }, { 0x2557, 0, 3, 3, 0 },
        { 0x255a, 3, 0, 0, 3 }, { 0x255d, 3, 0, 3, 0 }, { 0x2560, 3, 3, 0, 3 }, { 0x2563, 3, 3, 3, 0 },
        { 0x2566, 0, 3, 3, 3 }, { 0x2569, 3, 0, 3, 3 }, { 0x256c, 3, 3, 3, 3 },
        { 0x256d, 0, 1, 0, 1 }, { 0x256e, 0, 1, 1, 0 }, { 0x256f, 1, 0, 1, 0 }, { 0x2570, 1, 0, 0, 1 },
        { 0x2574, 0, 0, 1, 0 }, { 0x2575, 1, 0, 0, 0 }, { 0x2576, 0, 0, 0, 1 }, { 0x2577, 0, 1, 0, 0 },
    };
    for (unsigned i = 0; i < sizeof B / sizeof B[0]; i++)
        if (B[i].cp == c) {
            *u = B[i].u; *d = B[i].d; *l = B[i].l; *r = B[i].r;
            return true;
        }
    return false;
}

static void fill(lpui_canvas_t *c, int x, int y, int w, int h, u32 col)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    for (int j = 0; j < h; j++) {
        u32 *p = c->px + (u64)(y + j) * c->stride + x;
        for (int i = 0; i < w; i++)
            p[i] = col;
    }
}

static void draw_box(lpui_canvas_t *c, int x, int y, u32 cp, u32 fg)
{
    int u, d, l, r;
    if (!box_parts(cp, &u, &d, &l, &r))
        return;
    int t1 = cw / 10 < 1 ? 1 : cw / 10, t2 = t1 * 2;
    int mx = x + cw / 2, my = y + ch / 2;
    /* horizontal half-strokes */
    for (int side = 0; side < 2; side++) {
        int k = side ? r : l;
        if (!k) continue;
        int x0 = side ? mx : x, x1 = side ? x + cw : mx + 1;
        if (k == 3) {
            fill(c, x0, my - t1 - t1 / 2 - 1, x1 - x0, t1, fg);
            fill(c, x0, my + t1 / 2 + 1, x1 - x0, t1, fg);
        } else {
            int t = k == 2 ? t2 : t1;
            fill(c, x0, my - t / 2, x1 - x0, t, fg);
        }
    }
    for (int side = 0; side < 2; side++) {
        int k = side ? d : u;
        if (!k) continue;
        int y0 = side ? my : y, y1 = side ? y + ch : my + 1;
        if (k == 3) {
            fill(c, mx - t1 - t1 / 2 - 1, y0, t1, y1 - y0, fg);
            fill(c, mx + t1 / 2 + 1, y0, t1, y1 - y0, fg);
        } else {
            int t = k == 2 ? t2 : t1;
            fill(c, mx - t / 2, y0, t, y1 - y0, fg);
        }
    }
}

static void draw_cell(lpui_canvas_t *c, int x, int y, cell_t cl, bool cursor)
{
    u32 fg = cl.fg == DEFCOL ? T_FG : cl.fg;
    u32 bg = cl.bg == DEFCOL ? T_BG : cl.bg;
    if ((cl.cp & A_BOLD) && cl.fg != DEFCOL) {
        for (int i = 0; i < 8; i++)
            if (fg == PAL16[i]) { fg = PAL16[i + 8]; break; }
    } else if ((cl.cp & A_BOLD) && cl.fg == DEFCOL)
        fg = 0xffffff;
    if (cl.cp & A_REV) {
        u32 t = fg; fg = bg; bg = t;
    }
    if (cursor) {
        bg = LPUI_ACCENT;
        fg = 0x1a0a14;
    }
    int w = (cl.cp & A_WIDE) ? 2 * cw : cw;
    fill(c, x, y, w, ch, bg);
    u32 cp = cl.cp & CP_MASK;
    if (cp == ' ' || !cp || (cl.cp & A_CONT))
        goto under;
    if (cp >= 0x2500 && cp <= 0x257f) {
        int u, d, l, r;
        if (box_parts(cp, &u, &d, &l, &r)) {
            draw_box(c, x, y, cp, fg);
            goto under;
        }
    }
    if (cp == 0x2588) {                 /* full block */
        fill(c, x, y, w, ch, fg);
        goto under;
    }
    u8 *a = (cl.cp & A_WIDE) ? 0 : glyph_alpha(cp);
    if (a && lpui_glyph(&LPG_MONO, cp)) {
        for (int j = 0; j < ch; j++) {
            if (y + j < 0 || y + j >= c->h)
                continue;
            u32 *p = c->px + (u64)(y + j) * c->stride + x;
            const u8 *s = a + j * cw;
            for (int i = 0; i < cw && x + i < c->w; i++)
                if (s[i])
                    p[i] = lpui_mix(p[i], fg, (u32)s[i] + (s[i] >> 7));
        }
    } else {
        /* Not in the font (Hangul, CJK, emoji): an outlined box the
         * character's width, so columns still line up. */
        int m = cw / 6 + 1;
        lpui_rrect_stroke(c, x + m, y + ch / 5, w - 2 * m, ch * 3 / 5, 0, 1, fg, 160);
    }
under:
    if (cl.cp & A_UNDER)
        fill(c, x, y + gasc + (ch - gasc) / 3, w, ch / 16 < 1 ? 1 : ch / 16, fg);
}

/* The cursor is drawn as part of its row: when it has moved, the row it
 * left and the row it is on are redrawn. */
static void sync_cursor(void)
{
    int c = view ? -1 : cx, y = view ? -1 : cy;
    if (c != drawn_cx || y != drawn_cy) {
        touch(drawn_cy);
        touch(y);
    }
}

bool term_dirty(rect_t *r)
{
    int y0 = -1, y1 = -1;
    sync_cursor();
    if (all_dirty) {
        *r = area;
        return true;
    }
    for (int y = 0; y < rows && y < MAXROWS; y++)
        if (dirty_row[y]) {
            if (y0 < 0) y0 = y;
            y1 = y;
        }
    if (y0 < 0)
        return false;
    *r = (rect_t){ ox0, oy0 + y0 * ch, cols * cw, (y1 - y0 + 1) * ch };
    return true;
}

void term_draw(lpui_canvas_t *c, bool all)
{
    if (!ring)
        return;
    if (all || all_dirty) {
        lpui_rrect(c, area.x, area.y, area.w, area.h, lpui_px(36), T_BG, 256);
        for (int y = 0; y < rows && y < MAXROWS; y++)
            dirty_row[y] = true;
    }
    sync_cursor();
    int shown_cy = view ? -1 : cy;
    drawn_cx = view ? -1 : cx;
    drawn_cy = shown_cy;
    for (int y = 0; y < rows && y < MAXROWS; y++) {
        if (!dirty_row[y])
            continue;
        dirty_row[y] = false;
        const cell_t *l;
        if (view && !on_alt) {
            int i = nlines - rows + y - view;
            if (i < 0)
                continue;
            l = ring + (u64)((first + i) % HIST) * maxcols;
        } else
            l = L(y);
        int py = oy0 + y * ch;
        for (int x = 0; x < cols; x++) {
            if (l[x].cp & A_CONT)
                continue;
            bool cur = cursor_on && y == shown_cy && (x == cx || (x == cx - 1 && (l[x].cp & A_WIDE)));
            draw_cell(c, ox0 + x * cw, py, l[x], cur);
        }
        /* the strip right of the last column */
        fill(c, ox0 + cols * cw, py, area.x + area.w - lpui_px(20) - (ox0 + cols * cw), ch, T_BG);
    }
    if (view && !on_alt) {
        /* where in the scrollback this is: a thin bar on the right */
        int total = nlines, h = area.h - 2 * lpui_px(28);
        int bh = h * rows / (total ? total : 1);
        if (bh < lpui_px(40)) bh = lpui_px(40);
        int by = area.y + lpui_px(28) + (h - bh) * (total - rows - view) / (total - rows ? total - rows : 1);
        fill(c, area.x + area.w - lpui_px(16), area.y + lpui_px(28), lpui_px(8), h, T_BG);
        lpui_rrect(c, area.x + area.w - lpui_px(16), by, lpui_px(8), bh, lpui_px(4), 0x8a7f88, 256);
    }
    all_dirty = false;
}
