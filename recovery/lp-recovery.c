/* lp-recovery - LP's recovery menu, on the recovery partition.
 *
 * The boot menu's "LP Recovery" starts the same kernel with
 * root=PARTLABEL=LP-RECOVERY lp.mode=recovery. That root is a small
 * disk-rooted system (tools/mkrecovery.sh builds it: our static userland
 * plus the Debian tools a repair needs), and our init, seeing
 * lp.mode=recovery, runs this program on tty1 as root instead of a normal
 * boot. Nothing else runs: no shell on the serial port, no services.
 *
 * It is the macOS Recovery idea for this machine - one fullscreen menu
 * with big targets for the 4K touch panel, and keys:
 *
 *   Recovery shell (administrator)  a root shell with the installed
 *        system at /mnt/lp, after an administrator's password (auth.c).
 *        The shell runs on a pty inside this program (term.c), not on
 *        the kernel console, so it can be used by touch.
 *   Reinstall LP  keep /home and the accounts, or erase everything, from
 *        the payload in /reinstall, with a typed confirmation
 *        (reinstall.c).
 *   Check and repair disks  fsck of the EFI partition and LP-ROOT, and a
 *        read-only check of LP-RECOVERY itself, with the output on
 *        screen.
 *   Exit recovery and restart  the boot menu then starts LP.
 *
 * and a status line: disk state, battery, clock.
 *
 * Everything works by touch alone (the owner's request): the screens
 * that take text - the password, the typed REINSTALL, the shell - open
 * with an on-screen keyboard (osk.c) under them, which has a Close key
 * and comes back from the "Keyboard" button. A physical keyboard works
 * everywhere as well.
 *
 * English by default, Korean when the boot menu was in Korean (it passes
 * lp.lang=ko), or when the installed system's locale is Korean. The
 * shell's terminal is English: its font (lp-glyphs-mono.h) carries ASCII
 * and the line-drawing characters, not Hangul.
 *
 * Motion follows COMMON.md: the focus highlight moves on the menu spring
 * (interruptible - retargeting keeps its velocity), screens crossfade in
 * the menu spring's time, a touch shows its press on touch-down with no
 * delay, and with "reduce motion" set on the installed system the
 * highlight jumps and the crossfade is 100 ms.
 *
 * The screens are drawn with recovery/ui/lp-ui.h, the same code and font
 * as the boot menu, so the two look like one product.
 */
#include "recovery.h"
#include "lp-efivar.h"

#define SIGPIPE_  13

/* lp-ui.h's screen scale and language, shared by every file of the
 * program (see LPUI_SHARED_STATE there). */
int  lpui_permille = 1000;
bool lpui_korean;

static lpui_canvas_t cv_old;            /* the last frame, for crossfades */
static bool reduced;

/* ── Layout ───────────────────────────────────────────────────────── */
static int ox, oy;
static int X(int v) { return ox + lpui_px(v); }
static int Y(int v) { return oy + lpui_px(v); }
static int P(int v) { int p = lpui_px(v); return p < 1 ? 1 : p; }
static int TXT(int v) { return lpui_text_px(v); }

enum { SC_MENU, SC_AUTH, SC_RE_CHOOSE, SC_RE_CONFIRM, SC_RE_PROGRESS, SC_RE_DONE,
       SC_CHECK, SC_RESTART, SC_SHELL };
static int screen = -1;

enum { T_ROW, T_BUTTON, T_PRIMARY, T_DANGER, T_CHIP, T_FIELD };
typedef struct { rect_t r; int id; int style; bool disabled; } target_t;
#define MAXT 80
static target_t tg[MAXT];
static int ntg, focus, pressed = -1;

/* target ids */
enum {
    ID_SHELL, ID_REINSTALL, ID_CHECK, ID_EXIT,
    ID_KEEP = 10, ID_ERASE,
    ID_CHIP = 100,
    ID_FIELD = 200, ID_BACK, ID_KBD, ID_OPEN, ID_GO, ID_RESTART, ID_DONE,
};

/* ── State of the screens ─────────────────────────────────────────── */
static auth_info_t ai;
static int who;
static char pw[128];
static int pwlen;
static char confirm[16];
static int conflen;
static char msg[240];
static u32 msg_color;
static char shell_admin[40];            /* who opened the shell, for the log */
static int re_mode;
static re_progress_t re_prog;
static char re_result[240];
static bool re_ok;
static char version[96];
static bool interrupted;
static bool check_done;
#define OUTN 64
static char out_line[OUTN][120];
static u32 out_col[OUTN];
static int nout;
static int disk_status;                 /* LPS_ id */
static s64 last_status;

/* ── Drawing primitives in the house style ───────────────────────── */
static void card(lpui_canvas_t *c, rect_t r, int rad)
{
    lpui_rrect(c, r.x, r.y, r.w, r.h, rad, 0xffffff, LPUI_CARD_A);
    lpui_rrect_stroke(c, r.x, r.y, r.w, r.h, rad, P(3), 0xffffff, LPUI_LINE_A);
}

static void button(lpui_canvas_t *c, const target_t *t, const char *label)
{
    rect_t r = t->r;
    int rad = r.h / 2;
    u32 a = t->disabled ? 110 : 256;
    if (t->style == T_PRIMARY || t->style == T_DANGER) {
        lpui_rrect(c, r.x, r.y, r.w, r.h, rad, t->style == T_DANGER ? LPUI_DANGER : LPUI_ACCENT,
                   t->disabled ? 90 : 256);
    } else {
        lpui_rrect(c, r.x, r.y, r.w, r.h, rad, 0xffffff, 36);
        lpui_rrect_stroke(c, r.x, r.y, r.w, r.h, rad, P(3), 0xffffff, LPUI_LINE_A);
    }
    int tp = TXT(60);
    int w = lpui_text_width(LPG_M, tp, label);
    lpui_text(c, LPG_M, tp, r.x + (r.w - w) / 2, r.y + r.h / 2 + lpui_ascent(LPG_M, tp) * 36 / 100,
              LPUI_INK, a, label);
}

static void chrome(lpui_canvas_t *c, int title, const char *sub)
{
    int tp = TXT(104), sp = TXT(40);
    lpui_text_center(c, LPG_T, tp, SW / 2, Y(500), LPUI_INK, 256, lpui_s(title));
    if (sub)
        lpui_text_wrap(c, LPG_S, sp, X(620), Y(590), P(2600), LPUI_INK2, 220, true, sub);
}

static void status_line(lpui_canvas_t *c)
{
    if (osk_shown() || screen == SC_SHELL)
        return;
    int sp = TXT(40);
    int y = Y(2085);
    lpui_text(c, LPG_S, sp, X(160), y, disk_status == LPS_R_DISK_OK ? LPUI_INK2 : LPUI_ACCENT,
              200, lpui_s(disk_status));
    bool chg = false;
    int bat = battery_percent(&chg);
    if (bat >= 0) {
        char b[64];
        lpui_fmt(b, sizeof b, lpui_s(chg ? LPS_R_CHARGING : LPS_R_BATTERY), bat);
        lpui_text_center(c, LPG_S, sp, SW / 2, y, LPUI_INK2, 200, b);
    }
    char t[48];
    clock_text(t, sizeof t);
    lpui_text(c, LPG_S, sp, X(3680) - lpui_text_width(LPG_S, sp, t), y, LPUI_INK2, 200, t);
}

static rect_t status_band(void) { return (rect_t){ 0, Y(2010), SW, SH - Y(2010) }; }

static void field(lpui_canvas_t *c, const target_t *t, const char *text, bool secret)
{
    rect_t r = t->r;
    lpui_rrect(c, r.x, r.y, r.w, r.h, P(28), 0x000000, 70);
    lpui_rrect_stroke(c, r.x, r.y, r.w, r.h, P(28), P(3), 0xffffff, 70);
    int tp = TXT(60);
    int base = r.y + r.h / 2 + lpui_ascent(LPG_M, tp) * 36 / 100;
    int x = r.x + P(40);
    char shown[140];
    int n = 0;
    if (secret) {
        /* bullets, as many as fit */
        int bw = lpui_text_width(LPG_M, tp, "•");
        int max = bw ? (r.w - P(100)) / bw : 20;
        int k = (int)strlen(text);
        if (k > max) k = max;
        for (int i = 0; i < k && n + 3 < (int)sizeof shown; i++) {
            memcpy(shown + n, "•", 3);
            n += 3;
        }
        shown[n] = 0;
    } else
        strlcpy(shown, text, sizeof shown);
    int w = lpui_text(c, LPG_M, tp, x, base, LPUI_INK, 256, shown);
    /* the caret */
    lpui_rrect(c, x + w + P(8), r.y + r.h / 5, P(6), r.h * 3 / 5, 0, LPUI_ACCENT, 256);
}

/* ── Screens: layout ──────────────────────────────────────────────── */
static void add(int x, int y, int w, int h, int id, int style)
{
    if (ntg < MAXT)
        tg[ntg++] = (target_t){ { X(x), Y(y), P(w), P(h) }, id, style, false };
}

/* Buttons centred in a row. */
static void add_buttons_h(int y, int h, const int *ids, const int *styles, int n)
{
    int w = 560, gap = 60, total = n * w + (n - 1) * gap, x = 1920 - total / 2;
    for (int i = 0; i < n; i++)
        add(x + i * (w + gap), y, w, h, ids[i], styles[i]);
}

static void add_buttons(int y, const int *ids, const int *styles, int n)
{
    add_buttons_h(y, 160, ids, styles, n);
}

static int rows_top(void) { return interrupted ? 790 : 720; }

static void layout(void)
{
    ntg = 0;
    switch (screen) {
    case SC_MENU:
        for (int i = 0; i < 4; i++)
            add(770, rows_top() + i * 264, 2300, 230, ID_SHELL + i, T_ROW);
        break;
    case SC_AUTH: {
        /* With the on-screen keyboard up, everything moves up to sit
         * above it (compact); closed, the screen has room to breathe. */
        bool k = osk_shown();
        int chips = ai.mode == AUTH_ADMINS ? ai.nusers : ai.mode == AUTH_RECOVERY ? 1 : 0;
        int cw[AUTH_MAX_USERS], total = 0;
        for (int i = 0; i < chips; i++) {
            const char *name = ai.mode == AUTH_RECOVERY ? lpui_s(LPS_R_AUTH_RECNAME) : ai.users[i];
            cw[i] = lpui_text_width(LPG_M, 60, name) + 160;
            if (cw[i] < 420) cw[i] = 420;
            total += cw[i] + (i ? 40 : 0);
        }
        int x = 1920 - total / 2;
        for (int i = 0; i < chips; i++) {
            add(x, k ? 640 : 760, cw[i], k ? 130 : 150, ID_CHIP + i, T_CHIP);
            x += cw[i] + 40;
        }
        if (chips)
            add(1120, k ? 840 : 1030, 1600, k ? 140 : 170, ID_FIELD, T_FIELD);
        if (chips) {
            int ids[] = { ID_BACK, ID_KBD, ID_OPEN };
            int st[] = { T_BUTTON, T_BUTTON, T_PRIMARY };
            add_buttons_h(k ? 1020 : 1360, k ? 130 : 160, ids, st, 3);
        } else {
            int ids[] = { ID_BACK };
            int st[] = { T_BUTTON };
            add_buttons_h(k ? 1020 : 1360, k ? 130 : 160, ids, st, 1);
        }
        break;
    }
    case SC_RE_CHOOSE:
        if (version[0]) {
            add(770, 720, 2300, 260, ID_KEEP, T_ROW);
            add(770, 1014, 2300, 260, ID_ERASE, T_ROW);
        }
        {
            int ids[] = { ID_BACK };
            int st[] = { T_BUTTON };
            add_buttons(1400, ids, st, 1);
        }
        break;
    case SC_RE_CONFIRM: {
        bool k = osk_shown();
        add(1320, k ? 960 : 1170, 1200, k ? 130 : 170, ID_FIELD, T_FIELD);
        int ids[] = { ID_BACK, ID_KBD, ID_GO };
        int st[] = { T_BUTTON, T_BUTTON, re_mode == RE_ERASE ? T_DANGER : T_PRIMARY };
        add_buttons_h(k ? 1120 : 1380, k ? 120 : 160, ids, st, 3);
        tg[ntg - 1].disabled = strcmp(confirm, "REINSTALL") != 0;
        break;
    }
    case SC_SHELL: {
        /* The keyboard toggle, top right, in screen coordinates: this
         * screen fills the whole panel whatever its shape. */
        int bw = lpui_px(460), bh = lpui_px(116);
        tg[ntg++] = (target_t){ { SW - lpui_px(50) - bw, lpui_px(17), bw, bh }, ID_KBD, T_BUTTON, false };
        break;
    }
    case SC_RE_DONE: {
        int ids[] = { re_ok ? ID_RESTART : ID_BACK };
        int st[] = { re_ok ? T_PRIMARY : T_BUTTON };
        add_buttons(1250, ids, st, 1);
        break;
    }
    case SC_CHECK:
        if (check_done) {
            int ids[] = { ID_DONE };
            int st[] = { T_PRIMARY };
            add_buttons(1840, ids, st, 1);
        }
        break;
    }
    if (focus >= ntg)
        focus = ntg ? ntg - 1 : 0;
}

static const target_t *find(int id)
{
    for (int i = 0; i < ntg; i++)
        if (tg[i].id == id)
            return &tg[i];
    return 0;
}

/* ── Screens: drawing ─────────────────────────────────────────────── */
static void draw_menu(lpui_canvas_t *c)
{
    chrome(c, LPS_R_TITLE, lpui_s(LPS_R_CHOOSE));
    if (interrupted)
        lpui_text_wrap(c, LPG_S, TXT(40), X(620), Y(700), P(2600), LPUI_ACCENT, 256, true,
                       lpui_s(LPS_R_INTERRUPTED));
    static const int T[4][2] = {
        { LPS_R_SHELL, LPS_R_SHELL_SUB }, { LPS_R_REINSTALL, LPS_R_REINSTALL_SUB },
        { LPS_R_CHECK, LPS_R_CHECK_SUB }, { LPS_R_EXIT, LPS_R_EXIT_SUB },
    };
    for (int i = 0; i < ntg; i++) {
        rect_t r = tg[i].r;
        card(c, r, P(44));
        int k = tg[i].id - ID_SHELL;
        lpui_text(c, LPG_M, TXT(60), r.x + P(90), r.y + P(108), LPUI_INK, 256, lpui_s(T[k][0]));
        lpui_text(c, LPG_S, TXT(40), r.x + P(90), r.y + P(180), LPUI_INK2, 210, lpui_s(T[k][1]));
    }
    lpui_text_center(c, LPG_S, TXT(40), SW / 2, Y(1960), LPUI_INK2, 170, lpui_s(LPS_R_HINT));
}

static void draw_auth(lpui_canvas_t *c)
{
    chrome(c, LPS_R_AUTH_TITLE,
           lpui_s(ai.mode == AUTH_RECOVERY ? LPS_R_AUTH_RECPW : LPS_R_AUTH_WHO));
    for (int i = 0; i < ntg; i++) {
        const target_t *t = &tg[i];
        if (t->style == T_CHIP) {
            int k = t->id - ID_CHIP;
            const char *name = ai.mode == AUTH_RECOVERY ? lpui_s(LPS_R_AUTH_RECNAME) : ai.users[k];
            target_t ch = *t;
            ch.style = k == who ? T_PRIMARY : T_BUTTON;
            button(c, &ch, name);
        } else if (t->style == T_FIELD) {
            lpui_text(c, LPG_S, TXT(40), t->r.x, t->r.y - P(24), LPUI_INK2, 220,
                      lpui_s(LPS_R_AUTH_PASSWORD));
            field(c, t, pw, true);
        } else {
            int id = t->id;
            button(c, t, lpui_s(id == ID_BACK ? LPS_R_BACK : id == ID_KBD ? LPS_R_KEYBOARD :
                                LPS_R_AUTH_OPEN));
        }
    }
    const char *m = ai.mode == AUTH_NOBODY ? lpui_s(LPS_R_AUTH_NOBODY) : msg;
    if (m[0])
        lpui_text_wrap(c, LPG_S, TXT(40), X(620), Y(osk_shown() ? 1225 : 1290), P(2600),
                       ai.mode == AUTH_NOBODY ? LPUI_DANGER : msg_color, 256, true, m);
}

static void draw_re_choose(lpui_canvas_t *c)
{
    char sub[160];
    if (version[0])
        lpui_fmt(sub, sizeof sub, lpui_s(LPS_R_RE_PAYLOAD), version);
    else
        strlcpy(sub, lpui_s(LPS_R_NO_PAYLOAD), sizeof sub);
    chrome(c, LPS_R_RE_TITLE, sub);
    for (int i = 0; i < ntg; i++) {
        const target_t *t = &tg[i];
        if (t->style == T_ROW) {
            bool erase = t->id == ID_ERASE;
            card(c, t->r, P(44));
            lpui_text(c, LPG_M, TXT(60), t->r.x + P(90), t->r.y + P(112),
                      erase ? LPUI_DANGER : LPUI_INK, 256,
                      lpui_s(erase ? LPS_R_RE_ERASE : LPS_R_RE_KEEP));
            lpui_text_wrap(c, LPG_S, TXT(40), t->r.x + P(90), t->r.y + P(188), t->r.w - P(180),
                           LPUI_INK2, 210, false,
                           lpui_s(erase ? LPS_R_RE_ERASE_SUB : LPS_R_RE_KEEP_SUB));
        } else
            button(c, t, lpui_s(LPS_R_BACK));
    }
}

static void draw_re_confirm(lpui_canvas_t *c)
{
    /* With the keyboard up (compact), what will happen is still all on
     * the screen; the version and the power-loss note are not - the
     * previous screen showed the one, the progress screen shows the
     * other. */
    bool k = osk_shown();
    chrome(c, LPS_R_RE_TITLE, 0);
    int sp = TXT(40), lh = lpui_line(LPG_S, sp);
    lpui_text_center(c, LPG_M, TXT(60), SW / 2, Y(k ? 600 : 640),
                     re_mode == RE_ERASE ? LPUI_DANGER : LPUI_INK, 256,
                     lpui_s(re_mode == RE_ERASE ? LPS_R_RE_ERASE : LPS_R_RE_KEEP));
    int y = Y(k ? 690 : 760);
    char b[200];
    if (re_mode == RE_KEEP) {
        y += lh * lpui_text_wrap(c, LPG_S, sp, X(620), y, P(2600), LPUI_INK, 256, true,
                                 lpui_s(LPS_R_RE_KEPT));
        y += lh * lpui_text_wrap(c, LPG_S, sp, X(620), y, P(2600), LPUI_INK, 256, true,
                                 lpui_s(LPS_R_RE_REPLACED));
    } else
        y += lh * lpui_text_wrap(c, LPG_S, sp, X(620), y, P(2600), LPUI_DANGER, 256, true,
                                 lpui_s(LPS_R_RE_ERASED));
    if (!k) {
        lpui_fmt(b, sizeof b, lpui_s(LPS_R_RE_PAYLOAD), version);
        y += lh * lpui_text_wrap(c, LPG_S, sp, X(620), y, P(2600), LPUI_INK2, 220, true, b);
        lpui_text_wrap(c, LPG_S, sp, X(620), y, P(2600), LPUI_INK2, 190, true,
                       lpui_s(LPS_R_RE_AGAIN));
    }
    lpui_text_center(c, LPG_S, sp, SW / 2, Y(k ? 935 : 1130), LPUI_INK, 256, lpui_s(LPS_R_RE_TYPE));
    for (int i = 0; i < ntg; i++) {
        const target_t *t = &tg[i];
        if (t->style == T_FIELD)
            field(c, t, confirm, false);
        else
            button(c, t, lpui_s(t->id == ID_BACK ? LPS_R_CANCEL : t->id == ID_KBD ?
                                LPS_R_KEYBOARD : LPS_R_RE_GO));
    }
}

/* ── The recovery shell's screen ──────────────────────────────────────
 * A header band with what this is and the keyboard toggle, the terminal
 * under it, and the on-screen keyboard (drawn by osk.c over the bottom)
 * when it is up. Laid out in screen pixels, not the 4K frame: the
 * terminal should use every row the panel has. */
static int shell_head(void) { return lpui_px(150); }

static rect_t shell_area(void)
{
    int m = lpui_px(40), y = shell_head();
    int bottom = osk_shown() ? osk_rest_top() - lpui_px(24) : SH - m;
    return (rect_t){ m, y, SW - 2 * m, bottom - y };
}

static void draw_shell(lpui_canvas_t *c)
{
    int sp = TXT(40);
    lpui_text(c, LPG_S, sp, lpui_px(70), shell_head() / 2 + lpui_ascent(LPG_S, sp) * 36 / 100,
              LPUI_INK2, 230, lpui_s(LPS_R_SHELL_BAR));
    for (int i = 0; i < ntg; i++)
        button(c, &tg[i], lpui_s(LPS_R_KEYBOARD));
    term_draw(c, true);
}

static rect_t bar_rect(void) { return (rect_t){ X(720), Y(980), P(2400), P(44) }; }

static void draw_re_progress(lpui_canvas_t *c)
{
    chrome(c, LPS_R_RE_TITLE, 0);
    int sp = TXT(40);
    lpui_text_center(c, LPG_M, TXT(60), SW / 2, Y(880), LPUI_INK, 256,
                     lpui_s(re_prog.step ? re_prog.step : LPS_R_RE_VERIFY));
    rect_t b = bar_rect();
    lpui_rrect(c, b.x, b.y, b.w, b.h, b.h / 2, 0xffffff, 40);
    int fw = (int)((s64)b.w * re_prog.permille / 1000);
    if (fw > b.h)
        lpui_rrect(c, b.x, b.y, fw, b.h, b.h / 2, LPUI_ACCENT, 256);
    char pct[16];
    lpui_fmt(pct, sizeof pct, "%d%%", re_prog.permille / 10);
    lpui_text_center(c, LPG_S, sp, SW / 2, Y(1120), LPUI_INK2, 220, pct);
    lpui_text_wrap(c, LPG_S, sp, X(620), Y(1260), P(2600), LPUI_INK2, 190, true,
                   lpui_s(LPS_R_RE_AGAIN));
}

static void draw_re_done(lpui_canvas_t *c)
{
    chrome(c, LPS_R_RE_TITLE, 0);
    if (re_ok)
        lpui_text_center(c, LPG_M, TXT(60), SW / 2, Y(950), LPUI_INK, 256, lpui_s(LPS_R_RE_DONE));
    else
        lpui_text_wrap(c, LPG_S, TXT(40), X(620), Y(900), P(2600), LPUI_DANGER, 256, true, re_result);
    for (int i = 0; i < ntg; i++)
        button(c, &tg[i], lpui_s(tg[i].id == ID_RESTART ? LPS_R_RESTART : LPS_R_BACK));
}

static rect_t out_rect(void) { return (rect_t){ X(420), Y(640), P(3000), P(1140) }; }

static void draw_check(lpui_canvas_t *c)
{
    chrome(c, LPS_R_CK_TITLE, 0);
    rect_t r = out_rect();
    lpui_rrect(c, r.x, r.y, r.w, r.h, P(36), 0x000000, 80);
    int sp = TXT(36), lh = lpui_line(LPG_S, sp);
    int fit = (r.h - P(60)) / (lh ? lh : 1);
    int first = nout > fit ? nout - fit : 0;
    for (int i = first; i < nout; i++)
        lpui_text(c, LPG_S, sp, r.x + P(50), r.y + P(40) + lpui_ascent(LPG_S, sp) + (i - first) * lh,
                  out_col[i], 256, out_line[i]);
    for (int i = 0; i < ntg; i++)
        button(c, &tg[i], lpui_s(LPS_R_CK_DONE));
}

static void draw_restart(lpui_canvas_t *c)
{
    chrome(c, LPS_R_TITLE, 0);
    lpui_text_center(c, LPG_M, TXT(60), SW / 2, Y(1000), LPUI_INK, 256, lpui_s(LPS_R_RESTARTING));
}

static void draw_base(void)
{
    lpui_copy_rect(&cv_base, &cv_bg, 0, 0, SW, SH);
    lpui_canvas_t *c = &cv_base;
    switch (screen) {
    case SC_MENU:        draw_menu(c); break;
    case SC_AUTH:        draw_auth(c); break;
    case SC_RE_CHOOSE:   draw_re_choose(c); break;
    case SC_RE_CONFIRM:  draw_re_confirm(c); break;
    case SC_RE_PROGRESS: draw_re_progress(c); break;
    case SC_RE_DONE:     draw_re_done(c); break;
    case SC_CHECK:       draw_check(c); break;
    case SC_RESTART:     draw_restart(c); break;
    case SC_SHELL:       draw_shell(c); break;
    }
    status_line(c);
}

/* ── Overlays: focus highlight, press, mouse pointer ──────────────── */
static lpui_spring_t hx, hy, hw, hh;
static bool hl_on;
static rect_t ov_last;                  /* what the overlays covered last time */
static int cur_x = -1, cur_y = -1;
static bool anim;

static rect_t hl_now(void)
{
    return (rect_t){ (int)(hx.x >> 16), (int)(hy.x >> 16), (int)(hw.x >> 16), (int)(hh.x >> 16) };
}

static rect_t grow(rect_t r, int d) { return (rect_t){ r.x - d, r.y - d, r.w + 2 * d, r.h + 2 * d }; }

static rect_t unite(rect_t a, rect_t b)
{
    if (a.w <= 0 || a.h <= 0) return b;
    if (b.w <= 0 || b.h <= 0) return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (rect_t){ x0, y0, x1 - x0, y1 - y0 };
}

static rect_t overlays_bound(void)
{
    rect_t b = { 0, 0, 0, 0 };
    if (hl_on)
        b = grow(hl_now(), P(24));
    if (pressed >= 0 && pressed < ntg)
        b = unite(b, tg[pressed].r);
    if (cur_x >= 0) {
        int s = P(40) < 12 ? 12 : P(40);
        b = unite(b, (rect_t){ cur_x - s, cur_y - s, 2 * s, 2 * s });
    }
    return b;
}

/* Draw the overlays into `c`, a window onto the frame whose top-left is
 * screen position (dx, dy) - clipping to the window keeps a translucent
 * overlay from being blended twice where two updates overlap. */
static void draw_overlays(lpui_canvas_t *c, int dx, int dy)
{
    if (hl_on && focus < ntg) {
        rect_t r = hl_now();
        const target_t *t = &tg[focus];
        int rad = t->style == T_ROW ? P(44) : t->style == T_FIELD ? P(28) : r.h / 2;
        if (t->style == T_ROW)
            lpui_rrect(c, r.x - dx, r.y - dy, r.w, r.h, rad, 0xffffff, LPUI_SEL_A - LPUI_CARD_A);
        int th = P(8) < 2 ? 2 : P(8);
        lpui_rrect_stroke(c, r.x - dx - th, r.y - dy - th, r.w + 2 * th, r.h + 2 * th, rad + th,
                          th, LPUI_ACCENT, 256);
    }
    if (pressed >= 0 && pressed < ntg) {
        rect_t r = tg[pressed].r;
        int rad = tg[pressed].style == T_ROW ? P(44) : tg[pressed].style == T_FIELD ? P(28) : r.h / 2;
        lpui_rrect(c, r.x - dx, r.y - dy, r.w, r.h, rad, 0xffffff, 60);
    }
    if (cur_x >= 0) {
        int s = P(36) < 10 ? 10 : P(36);
        lpui_rrect(c, cur_x - s / 2 - dx, cur_y - s / 2 - dy, s, s, s / 2, 0xffffff, 230);
        lpui_rrect_stroke(c, cur_x - s / 2 - dx, cur_y - s / 2 - dy, s, s, s / 2,
                          s / 6 ? s / 6 : 1, 0x2a0f27, 200);
    }
}

static void compose_rect(rect_t d)
{
    if (d.x < 0) { d.w += d.x; d.x = 0; }
    if (d.y < 0) { d.h += d.y; d.y = 0; }
    if (d.x + d.w > SW) d.w = SW - d.x;
    if (d.y + d.h > SH) d.h = SH - d.y;
    if (d.w <= 0 || d.h <= 0)
        return;
    lpui_copy_rect(&cv_frame, &cv_base, d.x, d.y, d.w, d.h);
    lpui_canvas_t win = { cv_frame.px + (u64)d.y * cv_frame.stride + d.x, d.w, d.h, cv_frame.stride };
    osk_compose(&win, d.x, d.y);
    draw_overlays(&win, d.x, d.y);
    scr_present(&cv_frame, d.x, d.y, d.w, d.h);
}

/* After the overlays moved: redraw where they were and where they are. */
static void present_overlays(void)
{
    rect_t now = overlays_bound();
    compose_rect(unite(ov_last, now));
    ov_last = now;
}

/* After part of the base changed. */
static void present_rect(rect_t r)
{
    rect_t now = overlays_bound();
    compose_rect(unite(r, unite(ov_last, now)));
    ov_last = now;
}

static void present_all(void)
{
    ov_last = overlays_bound();
    compose_rect((rect_t){ 0, 0, SW, SH });
}

static void set_focus(int i, bool jump)
{
    if (i < 0 || i >= ntg) {
        hl_on = false;
        return;
    }
    focus = i;
    rect_t r = tg[i].r;
    if (!hl_on || jump || reduced) {
        lpui_spring_init(&hx, LPUI_SPRING_MENU, (s64)r.x << 16);
        lpui_spring_init(&hy, LPUI_SPRING_MENU, (s64)r.y << 16);
        lpui_spring_init(&hw, LPUI_SPRING_MENU, (s64)r.w << 16);
        lpui_spring_init(&hh, LPUI_SPRING_MENU, (s64)r.h << 16);
        hl_on = true;
        return;
    }
    /* Retarget: position and velocity carry on from where they are. */
    hx.target = (s64)r.x << 16;
    hy.target = (s64)r.y << 16;
    hw.target = (s64)r.w << 16;
    hh.target = (s64)r.h << 16;
    anim = true;
}

static bool springs_step(int ms)
{
    bool a = lpui_spring_step(&hx, ms);
    a |= lpui_spring_step(&hy, ms);
    a |= lpui_spring_step(&hw, ms);
    a |= lpui_spring_step(&hh, ms);
    return a;
}

/* ── Screen changes ───────────────────────────────────────────────────
 * The new screen fades in over the old one in the menu spring's time
 * (180 ms), eased out; 100 ms when motion is reduced. */
static void scr_present_mix(const lpui_canvas_t *a, const lpui_canvas_t *b, u32 wa)
{
    /* cv_old is overwritten row by row with the mix, then presented. */
    for (int y = 0; y < SH; y++) {
        u32 *d = cv_old.px + (u64)y * cv_old.stride;
        const u32 *s = a->px + (u64)y * a->stride;
        const u32 *t = b->px + (u64)y * b->stride;
        for (int x = 0; x < SW; x++)
            d[x] = lpui_mix(t[x], s[x], wa);
    }
    scr_present(&cv_old, 0, 0, SW, SH);
}

static void go(int sc, bool from_black)
{
    /* What is on the screen now. */
    if (from_black)
        memset(cv_old.px, 0, (size_t)SW * SH * 4);
    else
        lpui_copy_rect(&cv_old, &cv_frame, 0, 0, SW, SH);
    screen = sc;
    pressed = -1;
    focus = 0;
    hl_on = false;
    /* The screens that take text open with the keyboard up - recovery
     * must work with a finger alone - and it arrives with the screen,
     * inside the crossfade. */
    osk_show((sc == SC_AUTH && ai.mode != AUTH_NOBODY) || sc == SC_RE_CONFIRM ||
             sc == SC_SHELL, false);
    layout();
    draw_base();
    set_focus(ntg && sc != SC_SHELL ? 0 : -1, true);
    /* the finished frame */
    lpui_copy_rect(&cv_frame, &cv_base, 0, 0, SW, SH);
    ov_last = overlays_bound();
    lpui_canvas_t full = cv_frame;
    osk_compose(&full, 0, 0);
    draw_overlays(&full, 0, 0);
    rect_t ignore;
    osk_dirty(&ignore);
    /* keep a copy of the old picture for the mix (cv_old is reused) */
    int dur = reduced ? 100 : 180;
    s64 t0 = lp_monotonic_ms();
    static lpui_canvas_t oldpic;
    if (!oldpic.px) {
        oldpic = cv_old;
        oldpic.px = malloc((size_t)SW * SH * 4);
    }
    if (oldpic.px) {
        lpui_copy_rect(&oldpic, &cv_old, 0, 0, SW, SH);
        for (;;) {
            s64 t = lp_monotonic_ms() - t0;
            if (t >= dur)
                break;
            u32 f = (u32)(t * 256 / dur);
            u32 e = 256 - ((256 - f) * (256 - f) >> 8);
            scr_present_mix(&cv_frame, &oldpic, e);
        }
    }
    scr_present(&cv_frame, 0, 0, SW, SH);
    last_status = lp_monotonic_ms();
}

static void redraw(void)
{
    draw_base();
    present_all();
}

static void set_msg(u32 col, const char *fmt, int v, const char *s);
static void move_focus(int d);
static void back(void);

/* ── The recovery shell ───────────────────────────────────────────────
 * Only reached from auth_submit, after an administrator's password. The
 * shell runs on a pty inside this program (term.c) so it can be used by
 * touch: the on-screen keyboard types into it, and a physical keyboard
 * still works (its keys come through evdev, never through tty1). LP-ROOT
 * is read-write while the shell is open and read-only again after. */
static void open_shell(const char *who_name)
{
    strlcpy(shell_admin, who_name, sizeof shell_admin);
    sys_root_remount(true);
    rlog("recovery shell opened (administrator: %s)", who_name);
    static const char hello[] =
        "\033[1mYou are root on the recovery system. The installed system is at /mnt/lp.\r\n"
        "Type exit to return.\033[0m\r\n\r\n";
    /* The terminal needs its size before the screen is drawn: the
     * keyboard is up on this screen, so the area is the one above it. */
    osk_show(true, false);
    const char *err = term_start(shell_area(), hello);
    if (err) {
        rlog("recovery shell: %s", err);
        sys_root_remount(false);
        set_msg(LPUI_DANGER, lpui_s(LPS_R_SHELL_FAILED), 0, err);
        redraw();
        return;
    }
    msg[0] = 0;
    go(SC_SHELL, false);
}

static void close_shell(void)
{
    term_stop();
    rlog("recovery shell closed (administrator: %s)", shell_admin);
    lp_sync();
    sys_root_remount(false);
    in_drain();
    go(SC_MENU, false);
}

/* ── Exit ─────────────────────────────────────────────────────────── */
static void restart(void)
{
    go(SC_RESTART, false);
    rlog("exit recovery: restarting");
    if (lp_efivars_ready()) {
        long c = lp_bootcount_get();
        /* A fresh start for LP - unless a reinstall is unfinished, which
         * keeps the menu pointing here until it is done. */
        if (c >= 0 && c < (long)LP_BOOT_REINSTALL_MARK)
            lp_bootcount_set(0);
        lp_efivar_delete("LPBootNext");
    }
    lp_sync();
    sys_root_umount();
    lp_umount(MNT_ESP, 0);
    lp_sync();
    /* init stops everything, unmounts and reboots (SIGUSR2). If it has
     * not after 15 s, do it here. */
    lp_kill(1, SIGUSR2);
    lp_sleep_ms(15000);
    lp_sync();
    lp_reboot(LINUX_REBOOT_CMD_RESTART);
    for (;;)
        lp_sleep_ms(1000);
}

/* ── Check and repair disks ───────────────────────────────────────── */
static void out_add(const char *s, u32 col)
{
    if (nout == OUTN) {
        memmove(out_line[0], out_line[1], sizeof out_line[0] * (OUTN - 1));
        memmove(&out_col[0], &out_col[1], sizeof out_col[0] * (OUTN - 1));
        nout--;
    }
    strlcpy(out_line[nout], s, sizeof out_line[0]);
    out_col[nout++] = col;
    draw_base();
    present_rect(out_rect());
}

static void on_fsck_line(const char *s, void *ctx)
{
    (void)ctx;
    /* fsck.fat looks for iconv's codepage modules, which recovery does
     * not carry, says so in two lines, and uses its own CP850 table -
     * the same answer. Those two lines read like a failure; they are not
     * shown. */
    if (!strncmp(s, "Cannot initialize conversion from ", 34) ||
        !strncmp(s, "Using internal CP850 conversion table", 37))
        return;
    out_add(s, LPUI_INK2);
}

static void check_one(const char *name, const part_t *p, char *const argv[], bool readonly)
{
    char b[160];
    if (!p->found) {
        lpui_fmt(b, sizeof b, lpui_s(LPS_R_CK_MISSING), name);
        out_add(b, LPUI_DANGER);
        return;
    }
    lpui_fmt(b, sizeof b, lpui_s(LPS_R_CK_RUNNING), name);
    out_add(b, LPUI_INK);
    int r = run(argv, on_fsck_line, 0);
    rlog("check %s: exit %d", name, r);
    if (r == 0)
        lpui_fmt(b, sizeof b, lpui_s(LPS_R_CK_CLEAN), name);
    else if (!readonly && (r == 1 || r == 2 || r == 3))
        lpui_fmt(b, sizeof b, lpui_s(LPS_R_CK_FIXED), name);
    else
        lpui_fmt(b, sizeof b, lpui_s(LPS_R_CK_LEFT), name, r);
    out_add(b, r == 0 ? LPUI_INK : (!readonly && r <= 3) ? LPUI_ACCENT : LPUI_DANGER);
    out_add("", LPUI_INK2);
}

extern void sys_log_pause(bool pause);

static void check_disks(void)
{
    nout = 0;
    check_done = false;
    go(SC_CHECK, false);
    rlog("check and repair disks");
    sys_root_umount();
    lp_umount(MNT_ESP, 0);
    char *esp[] = { "/usr/sbin/fsck.vfat", "-a", "-w", p_esp.dev, 0 };
    check_one("LP-ESP", &p_esp, esp, false);
    char *root[] = { "/usr/sbin/e2fsck", "-f", "-y", p_root.dev, 0 };
    check_one("LP-ROOT", &p_root, root, false);
    /* LP-RECOVERY is the running system: checked, never repaired, and
     * made read-only for the check so the answer is not about a
     * filesystem changing under it. */
    sys_log_pause(true);
    bool ro = lp_mount(p_rec.dev, "/", "ext4", MS_REMOUNT | MS_RDONLY, NULL) == 0;
    char *rec[] = { "/usr/sbin/e2fsck", "-f", "-n", p_rec.dev, 0 };
    check_one("LP-RECOVERY", &p_rec, rec, true);
    if (ro)
        lp_mount(p_rec.dev, "/", "ext4", MS_REMOUNT, NULL);
    sys_log_pause(false);
    sys_root_mount(false);
    int st = sys_root_state();
    disk_status = !p_root.found ? LPS_R_DISK_NOROOT : st == 1 ? LPS_R_DISK_CHECK : LPS_R_DISK_OK;
    check_done = true;
    layout();
    set_focus(0, true);
    redraw();
}

/* ── Reinstall ────────────────────────────────────────────────────── */
static void on_progress(const re_progress_t *p, void *ctx)
{
    (void)ctx;
    re_prog = *p;
    draw_base();
    present_rect((rect_t){ 0, Y(780), SW, Y(1160) - Y(780) });
}

static void reinstall_go(void)
{
    re_prog = (re_progress_t){ LPS_R_RE_VERIFY, 0 };
    go(SC_RE_PROGRESS, false);
    bool damaged = false;
    const char *err = re_run(re_mode, on_progress, 0, &damaged);
    re_ok = err == 0;
    if (err) {
        lpui_fmt(re_result, sizeof re_result,
                 lpui_s(damaged ? LPS_R_RE_DAMAGED : LPS_R_RE_FAILED), err);
        rlog("reinstall failed: %s", err);
    }
    interrupted = re_interrupted();
    int st = sys_root_state();
    disk_status = !p_root.found ? LPS_R_DISK_NOROOT : st == 1 ? LPS_R_DISK_CHECK : LPS_R_DISK_OK;
    go(SC_RE_DONE, false);
}

/* ── Actions ──────────────────────────────────────────────────────── */
static bool msg_wait;                   /* msg is the lockout countdown */

static void set_msg(u32 col, const char *fmt, int v, const char *s)
{
    msg_wait = fmt == lpui_s(LPS_R_AUTH_WAIT);
    if (s)
        lpui_fmt(msg, sizeof msg, fmt, s);
    else
        lpui_fmt(msg, sizeof msg, fmt, v);
    msg_color = col;
}

static void auth_submit(void)
{
    if (ai.mode == AUTH_NOBODY)
        return;
    int wait = auth_lockout_left();
    if (wait > 0) {
        set_msg(LPUI_DANGER, lpui_s(LPS_R_AUTH_WAIT), wait, 0);
        redraw();
        return;
    }
    set_msg(LPUI_INK2, lpui_s(LPS_R_AUTH_CHECKING), 0, 0);
    redraw();
    const char *why = "";
    int r = auth_check(&ai, who, pw, &wait, &why);
    memset(pw, 0, sizeof pw);
    pwlen = 0;
    switch (r) {
    case 0: {
        char name[40];
        strlcpy(name, ai.mode == AUTH_RECOVERY ? "recovery password" : ai.users[who], sizeof name);
        msg[0] = 0;
        open_shell(name);
        return;
    }
    case 1:
        if (wait > 0)
            set_msg(LPUI_DANGER, lpui_s(LPS_R_AUTH_WAIT), wait, 0);
        else
            set_msg(LPUI_DANGER, lpui_s(LPS_R_AUTH_WRONG), auth_tries_left(), 0);
        break;
    case 2:
        set_msg(LPUI_DANGER, lpui_s(LPS_R_AUTH_WAIT), wait, 0);
        break;
    case 3:
        set_msg(LPUI_DANGER, lpui_s(LPS_R_AUTH_FORMAT), 0, why);
        break;
    default:
        set_msg(LPUI_DANGER, lpui_s(LPS_R_AUTH_LOCKED), 0, 0);
        break;
    }
    redraw();
}

static void type_char(u32 ch)
{
    if (screen == SC_AUTH && ai.mode != AUTH_NOBODY) {
        if (auth_lockout_left() > 0)
            return;
        if (pwlen < (int)sizeof pw - 1) {
            pw[pwlen++] = (char)ch;
            pw[pwlen] = 0;
        }
        if (msg_color == LPUI_DANGER)
            msg[0] = 0;
    } else if (screen == SC_RE_CONFIRM) {
        if (ch >= 'a' && ch <= 'z')
            ch -= 32;
        if (conflen < (int)sizeof confirm - 1 && ch > ' ') {
            confirm[conflen++] = (char)ch;
            confirm[conflen] = 0;
        }
        layout();
    } else
        return;
    redraw();
}

static void backspace(void)
{
    if (screen == SC_AUTH && pwlen > 0)
        pw[--pwlen] = 0;
    else if (screen == SC_RE_CONFIRM && conflen > 0) {
        confirm[--conflen] = 0;
        layout();
    } else
        return;
    redraw();
}

/* The keyboard is on its way up or down: the screen under it takes its
 * other layout at once (compact above the keyboard, or roomy without
 * it), the shell's terminal gains or loses rows, and the keyboard slides
 * over the result. */
static void osk_changed(void)
{
    int keep = focus < ntg ? tg[focus].id : -1;
    layout();
    if (screen == SC_SHELL)
        term_set_area(shell_area());
    focus = 0;
    for (int i = 0; i < ntg; i++)
        if (tg[i].id == keep)
            focus = i;
    if (screen != SC_SHELL)
        set_focus(focus, true);
    redraw();
}

static void toggle_osk(bool on)
{
    osk_show(on, true);
    osk_changed();
}

/* A key of the on-screen keyboard. */
static void on_osk_key(const osk_key_t *k)
{
    if (screen == SC_SHELL) {
        term_osk(k);
        return;
    }
    switch (k->what) {
    case OSK_TEXT:
        for (const char *p = k->text; *p; p++)
            if ((u8)*p >= 32 && (u8)*p < 127)
                type_char((u8)*p);
        return;
    case OSK_BKSP:
        backspace();
        return;
    case OSK_ENTER:
        if (screen == SC_AUTH && pwlen > 0)
            auth_submit();
        else if (screen == SC_RE_CONFIRM && !strcmp(confirm, "REINSTALL"))
            reinstall_go();
        return;
    case OSK_ESC:
        back();
        return;
    case OSK_TAB: case OSK_DOWN: case OSK_RIGHT:
        move_focus(1);
        return;
    case OSK_UP: case OSK_LEFT:
        move_focus(-1);
        return;
    }
}

static void back(void)
{
    switch (screen) {
    case SC_AUTH:
    case SC_RE_CHOOSE:
    case SC_CHECK:
        memset(pw, 0, sizeof pw);
        pwlen = 0;
        go(SC_MENU, false);
        break;
    case SC_RE_CONFIRM:
        go(SC_RE_CHOOSE, false);
        break;
    case SC_RE_DONE:
        if (!re_ok)
            go(SC_MENU, false);
        break;
    }
}

static void activate(int id)
{
    switch (id) {
    case ID_SHELL:
        msg[0] = 0;
        memset(pw, 0, sizeof pw);
        pwlen = 0;
        who = 0;
        auth_prepare(&ai);
        go(SC_AUTH, false);
        if (ai.mode != AUTH_NOBODY && find(ID_FIELD)) {
            for (int i = 0; i < ntg; i++)
                if (tg[i].id == ID_FIELD)
                    set_focus(i, true);
            int wait = auth_lockout_left();
            if (wait > 0)
                set_msg(LPUI_DANGER, lpui_s(LPS_R_AUTH_WAIT), wait, 0);
            redraw();
        }
        return;
    case ID_REINSTALL:
        if (!re_payload_present(version, sizeof version))
            version[0] = 0;
        go(SC_RE_CHOOSE, false);
        return;
    case ID_CHECK:
        check_disks();
        return;
    case ID_EXIT:
    case ID_RESTART:
        restart();
        return;
    case ID_KEEP:
    case ID_ERASE:
        re_mode = id == ID_KEEP ? RE_KEEP : RE_ERASE;
        confirm[0] = 0;
        conflen = 0;
        go(SC_RE_CONFIRM, false);
        for (int i = 0; i < ntg; i++)
            if (tg[i].id == ID_FIELD)
                set_focus(i, true);
        present_all();
        return;
    case ID_BACK:
        back();
        return;
    case ID_DONE:
        go(SC_MENU, false);
        return;
    case ID_KBD:
        toggle_osk(!osk_shown());
        return;
    case ID_FIELD:
    case ID_OPEN:
    case ID_GO:
        if (screen == SC_AUTH && id != ID_FIELD)
            auth_submit();
        else if (screen == SC_AUTH && id == ID_FIELD && pwlen > 0)
            auth_submit();
        else if (screen == SC_RE_CONFIRM && !strcmp(confirm, "REINSTALL") && id != ID_FIELD)
            reinstall_go();
        else if (screen == SC_RE_CONFIRM && id == ID_FIELD && !strcmp(confirm, "REINSTALL"))
            reinstall_go();
        return;
    }
    if (id >= ID_CHIP && id < ID_CHIP + AUTH_MAX_USERS) {
        who = id - ID_CHIP;
        redraw();
    }
}

/* ── Input ────────────────────────────────────────────────────────── */
static int hit(int x, int y)
{
    for (int i = ntg - 1; i >= 0; i--) {
        rect_t r = tg[i].r;
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
            return i;
    }
    return -1;
}

static bool ptr_down;
static int ptr_slot;                    /* the finger pressing a target */
static int drag_slot = -1, drag_y;      /* the finger scrolling the terminal */

static void on_ptr(const uev_t *e)
{
    if (e->mouse) {
        cur_x = e->x;
        cur_y = e->y;
    } else if (cur_x >= 0) {
        cur_x = cur_y = -1;
        present_overlays();
    }
    /* The keyboard first: it is on top. */
    if (osk_pointer(e)) {
        if (e->mouse)
            present_overlays();
        return;
    }
    /* A finger on the terminal scrolls it. */
    if (screen == SC_SHELL) {
        rect_t a = term_area();
        bool in = e->x >= a.x && e->x < a.x + a.w && e->y >= a.y && e->y < a.y + a.h;
        if (e->down && drag_slot < 0 && !ptr_down && in) {
            drag_slot = e->slot;
            drag_y = e->y;
            term_drag(0, true);
            return;
        }
        if (drag_slot >= 0 && e->slot == drag_slot) {
            if (e->down) {
                term_drag(e->y - drag_y, false);
                drag_y = e->y;
            } else
                drag_slot = -1;
            return;
        }
    }
    if (ptr_down && e->slot != ptr_slot)
        return;                         /* one finger at a time on buttons */
    int t = hit(e->x, e->y);
    if (e->down && !ptr_down) {
        ptr_down = true;
        ptr_slot = e->slot;
        pressed = t;                    /* feedback on touch-down, no delay */
        if (t >= 0 && !tg[t].disabled) {
            if (screen != SC_SHELL)
                set_focus(t, false);
        } else
            pressed = -1;
        present_overlays();
    } else if (!e->down && ptr_down) {
        ptr_down = false;
        int p = pressed;
        pressed = -1;
        present_overlays();
        if (p >= 0 && p == t) {
            /* A tap on a text field asks for the keyboard; Enter (or the
             * button beside it) is what submits. */
            if (tg[p].id == ID_FIELD) {
                if (!osk_shown())
                    toggle_osk(true);
            } else
                activate(tg[p].id);
        }
    } else if (e->down) {
        if (t != pressed && pressed >= 0) {
            pressed = -1;               /* slid off: cancel */
            present_overlays();
        }
    } else {
        if (e->mouse && t >= 0 && t != focus && !tg[t].disabled && screen != SC_SHELL)
            set_focus(t, false);
        present_overlays();
    }
}

static void move_focus(int d)
{
    if (!ntg)
        return;
    int i = focus;
    for (int k = 0; k < ntg; k++) {
        i = (i + d + ntg) % ntg;
        if (!tg[i].disabled)
            break;
    }
    set_focus(i, false);
    if (screen == SC_AUTH && tg[i].style == T_CHIP) {
        who = tg[i].id - ID_CHIP;
        redraw();
    }
}

static void on_key(const uev_t *e)
{
    if (screen == SC_SHELL) {
        term_key(e);                    /* every key is the shell's */
        return;
    }
    switch (e->code) {
    case K_TAB: case K_DOWN: case K_RIGHT:
        move_focus(1);
        return;
    case K_UP: case K_LEFT:
        move_focus(-1);
        return;
    case K_HOME:
        set_focus(0, false);
        return;
    case K_END:
        set_focus(ntg - 1, false);
        return;
    case K_ESC:
        back();
        return;
    case K_BACKSPACE:
        backspace();
        return;
    case K_ENTER: case K_KPENTER:
        if (ntg && focus < ntg && !tg[focus].disabled)
            activate(tg[focus].id);
        else if (screen == SC_AUTH)
            auth_submit();
        return;
    }
    if (e->ch >= 32 && e->ch < 127) {
        /* On screens without a text field, digits pick menu rows. */
        if (screen == SC_MENU && e->ch >= '1' && e->ch <= '4') {
            set_focus(e->ch - '1', false);
            activate(ID_SHELL + (int)(e->ch - '1'));
            return;
        }
        type_char(e->ch);
    }
}

/* ── Setup ────────────────────────────────────────────────────────── */
static void choose_language(void)
{
    char cmd[1024], loc[256];
    proc_read("/proc/cmdline", cmd, sizeof cmd);
    if (strstr(cmd, "lp.lang=ko"))
        lpui_korean = true;
    else if (strstr(cmd, "lp.lang=en"))
        lpui_korean = false;
    else if (sys_root_mounted() && file_read(MNT_ROOT "/etc/default/locale", loc, sizeof loc) &&
             strstr(loc, "LANG=ko"))
        lpui_korean = true;
    else if (file_read(PAYLOAD_DIR "/manifest", loc, sizeof loc) && strstr(loc, "\nlang ko"))
        lpui_korean = true;
    rlog("language %s", lpui_korean ? "Korean" : "English");
}

/* The status line's clock in the installed system's time zone. Its
 * /etc/localtime is usually an absolute link into its own /usr/share/
 * zoneinfo, which from here is under /mnt/lp. */
static void choose_timezone(void)
{
    if (!sys_root_mounted())
        return;
    char link[200], path[260];
    long n = lp_readlink(MNT_ROOT "/etc/localtime", link, sizeof link - 1);
    if (n > 0) {
        link[n] = 0;
        if (link[0] == '/')
            snprintf(path, sizeof path, MNT_ROOT "%s", link);
        else
            snprintf(path, sizeof path, MNT_ROOT "/etc/%s", link);
    } else
        strlcpy(path, MNT_ROOT "/etc/localtime", sizeof path);
    if (lp_exists(path)) {
        /* Copied, because LP-ROOT is unmounted for checks and reinstalls
         * and the zone file must not vanish with it. */
        if (file_copy(path, "/etc/localtime", 0644))
            unsetenv("TZ");
    }
}

int main(void)
{
    lp_signal_ignore(SIGPIPE_);
    lp_signal_ignore(SIGINT);
    lp_signal_ignore(SIGQUIT);
    lp_signal_ignore(SIGHUP);
    lp_signal_ignore(SIGTSTP);
    sys_init();
    rlog("LP Recovery starting");
    sys_find_partitions();
    sys_root_mount(false);
    choose_language();
    choose_timezone();
    int st = sys_root_state();
    disk_status = !p_root.found ? LPS_R_DISK_NOROOT : st == 1 ? LPS_R_DISK_CHECK : LPS_R_DISK_OK;
    interrupted = re_interrupted();
    reduced = motion_reduced();

    if (!scr_open()) {
        static const char no[] =
            "\r\nLP Recovery: this machine has no framebuffer (/dev/fb0), so the\r\n"
            "recovery menu cannot be shown. Nothing has been changed.\r\n";
        lp_write(STDOUT_FILENO, no, sizeof no - 1);
        rlog("no framebuffer; waiting");
        for (;;)
            lp_sleep_ms(60000);
    }
    cv_old = cv_frame;
    cv_old.px = malloc((size_t)SW * SH * 4);
    if (!cv_old.px)
        return 1;
    lpui_set_screen(SW, SH);
    ox = (SW - lpui_px(3840)) / 2;
    oy = (SH - lpui_px(2160)) / 2;
    lpui_gradient(&cv_bg);
    lpui_logo(&cv_bg, SW / 2, Y(250), P(210), logo_scratch);
    osk_init();
    osk_emit = on_osk_key;
    osk_closed = osk_changed;
    osk_reduce_motion = reduced;
    in_rescan();
    scr_graphics(true);
    in_drain();
    go(SC_MENU, true);
    if (interrupted)
        set_focus(1, true), present_all();   /* Reinstall LP, to finish it */

    s64 last = lp_monotonic_ms();
    /* The whole picture again a moment after the start, and once more a
     * little later. Right after the mode is set (screen.c) a DRM driver
     * can still be bringing the output up, and rows written during that
     * are flushed to a plane nobody scans out: in a VM the first menu
     * stayed black until the first key made something redraw. Two
     * repaints of an unchanged picture cost nothing anybody can see. */
    s64 repaint_at[2] = { last + 400, last + 1500 };
    for (;;) {
        for (int ri = 0; ri < 2; ri++)
            if (repaint_at[ri] && lp_monotonic_ms() >= repaint_at[ri]) {
                repaint_at[ri] = 0;
                present_all();
            }
        /* Sleep until input, the shell's output, or the next frame of
         * whatever is moving (highlight spring, keyboard slide, key
         * repeat); a still menu wakes once a second for the clock. */
        int wait = anim ? 12 : (repaint_at[1] ? 100 : 250);
        int sw = scr_settle();
        if (sw >= 0 && sw < wait)
            wait = sw;
        int ow = osk_tick();
        if (ow >= 0 && ow < wait)
            wait = ow;
        rect_t r;
        if (osk_dirty(&r))
            present_rect(r);
        uev_t e;
        int got = in_wait(&e, wait);
        s64 now = lp_monotonic_ms();
        int dt = (int)(now - last);
        last = now;
        if (got) {
            if (e.kind == UEV_KEY)
                on_key(&e);
            else if (e.kind == UEV_PTR)
                on_ptr(&e);
            else if (e.kind == UEV_FD && screen == SC_SHELL && !term_pump()) {
                close_shell();
                continue;
            }
        }
        if (screen == SC_SHELL && term_dirty(&r)) {
            term_draw(&cv_base, false);
            present_rect(r);
        }
        if (anim) {
            anim = springs_step(dt > 0 ? dt : 1);
            present_overlays();
        }
        if (now - last_status >= 1000) {
            last_status = now;
            if (screen == SC_SHELL && !term_pump()) {
                /* exited without a word on the pty (it was quiet, and
                 * something in the background still holds it open) */
                close_shell();
                continue;
            }
            rect_t band = { 0, Y(1150), SW, Y(1400) - Y(1150) };
            if (screen == SC_AUTH) {
                int w = auth_lockout_left();
                if (w > 0) {
                    set_msg(LPUI_DANGER, lpui_s(LPS_R_AUTH_WAIT), w, 0);
                    draw_base();
                    present_rect(band);
                } else if (msg_wait) {
                    msg[0] = 0;
                    msg_wait = false;
                    draw_base();
                    present_rect(band);
                }
            }
            if (!osk_shown() && screen != SC_SHELL) {
                draw_base();
                present_rect(status_band());
            }
        }
    }
}
