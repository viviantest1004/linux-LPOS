/* lpboot.c - LP's boot menu: a UEFI application that asks "LP or LP
 * Recovery?", counts down five seconds, and starts the kernel.
 *
 * ── Why our own and not GRUB or systemd-boot ──
 *
 * The owner asked for a choice at boot between the system and a recovery
 * mode, on a 3840x2160 touch panel. A text menu in the firmware's 8x19
 * font is a line of text a few millimetres high on that panel, and
 * neither stock loader draws anything a finger can hit. The kernel is its
 * own EFI program (the EFI stub), so a loader has only one job left - pick
 * a command line and call LoadImage/StartImage - and that is small enough
 * to write with the rest of the product's look: the desktop's gradient,
 * the LP mark, Pretendard in English or Korean, big cards.
 *
 * ── What it does ──
 *
 *   1. Reads three variables under LP's vendor GUID (LP_GUID below):
 *        LPBootCount  u32  normal boots started and not yet confirmed.
 *                          The menu adds one before starting LP; the OS
 *                          sets it back to 0 once it has come up
 *                          (`lp-reboot-recovery --boot-ok`). Two or more
 *                          means the last two boots died on the way up,
 *                          so Recovery is preselected and the menu says
 *                          why. 100 or more is the recovery system's mark
 *                          for "a reinstall started and did not finish".
 *        LPBootNext   "recovery", consumed once: `lp-reboot-recovery`
 *                          (Settings, the power menu) sets it and reboots.
 *        LPLang       "ko" for Korean; anything else, or nothing, is
 *                          English (`lp-reboot-recovery --lang ko|en`).
 *      A variable rather than a file on the EFI partition because the
 *      installed system mounts that partition read-only (fstab), and a
 *      variable is one write through efivarfs from anywhere.
 *   2. Draws the menu with the Graphics Output Protocol at the panel's
 *      native mode: the EDID's preferred timing when the firmware exposes
 *      it, otherwise the mode the firmware already set (which on the XPS
 *      is native). Drawing is off-screen, then one Blt, so nothing
 *      flickers; the menu fades in over 200 ms and the highlight moves
 *      between the cards on the menu spring (180 ms, COMMON.md Motion).
 *   3. Keys (arrows, Tab, Enter, Esc to stop the countdown), a mouse
 *      (EFI_SIMPLE_POINTER) and a touch screen or tablet
 *      (EFI_ABSOLUTE_POINTER) when the firmware provides them.
 *   4. Starts \EFI\LP\vmlinuz.efi from its own partition with the command
 *      line in \EFI\LP\cmdline.txt (the installer writes the root's
 *      PARTUUID there). Recovery replaces any root= with
 *      root=PARTLABEL=LP-RECOVERY and adds lp.mode=recovery, plus
 *      lp.lang=ko when the menu is in Korean. \EFI\LP\initrd.img is passed
 *      if it exists. When anything fails the error is on the screen and
 *      the person picks again - a boot menu that dies leaves a black
 *      screen, which is the one outcome worse than a wrong choice.
 *
 * It writes a line per decision to COM1 (port 0x3f8) for the test logs.
 * On a PC without the port the writes go nowhere, which is what they
 * should do; it never writes text on the screen through ConOut.
 *
 * Built by boot/efi/Makefile with clang and lld-link alone (efi.h). */
#include "efi.h"
#include "../../recovery/ui/lp-ui.h"

/* memset/memcpy: the compiler emits calls to these for struct copies and
 * zeroing even in freestanding code, and there is no C library here. */
void *memset(void *d, int c, UINTN n)
{
    u8 *p = d;
    while (n--)
        *p++ = (u8)c;
    return d;
}
void *memcpy(void *d, const void *s, UINTN n)
{
    u8 *p = d;
    const u8 *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_RUNTIME_SERVICES *RT;
static EFI_HANDLE self;

/* LP's vendor GUID: 8accd20f-82a3-4c56-b45f-f3cd7ab7a4b0. The same one in
 * userland/lp-reboot-recovery and the recovery system. */
static EFI_GUID LP_GUID = { 0x8accd20f, 0x82a3, 0x4c56,
                            { 0xb4, 0x5f, 0xf3, 0xcd, 0x7a, 0xb7, 0xa4, 0xb0 } };
#define VAR_ATTRS (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | \
                   EFI_VARIABLE_RUNTIME_ACCESS)
#define REINSTALL_MARK 100          /* LPBootCount >= this: unfinished reinstall */

/* ── COM1 log ─────────────────────────────────────────────────────── */
static inline u8 inb(u16 port)
{
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outb(u16 port, u8 v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static void serial_puts(const char *s)
{
    for (; *s; s++) {
        /* Wait for the transmitter, but not forever: with no UART at
         * 0x3f8 the status reads 0xff and says "ready" at once, and a
         * UART that never drains must not hang the boot. */
        for (int i = 0; i < 100000 && !(inb(0x3fd) & 0x20); i++)
            ;
        outb(0x3f8, (u8)*s);
    }
}
static void logf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    lpui_vfmt(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    serial_puts("lpboot: ");
    serial_puts(buf);
    serial_puts("\r\n");
}

/* ── Variables ────────────────────────────────────────────────────── */
static u32 var_u32(CHAR16 *name, u32 dflt)
{
    u32 v = 0, attrs;
    UINTN n = sizeof(v);
    if (RT->GetVariable(name, &LP_GUID, &attrs, &n, &v) != EFI_SUCCESS || n != sizeof(v))
        return dflt;
    return v;
}
static void set_u32(CHAR16 *name, u32 v)
{
    RT->SetVariable(name, &LP_GUID, VAR_ATTRS, sizeof(v), &v);
}
static int var_str(CHAR16 *name, char *out, int cap)
{
    u32 attrs;
    UINTN n = (UINTN)cap - 1;
    if (RT->GetVariable(name, &LP_GUID, &attrs, &n, out) != EFI_SUCCESS)
        n = 0;
    out[n] = 0;
    /* A trailing newline from `echo` through efivarfs is not part of it. */
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == 0))
        out[--n] = 0;
    return (int)n;
}

/* ── Files on our own partition ───────────────────────────────────── */
static EFI_HANDLE dev;          /* the partition lpboot.efi was loaded from */

static EFI_FILE_PROTOCOL *open_file_on(EFI_HANDLE h, CHAR16 *path)
{
    EFI_GUID fsg = EFI_SIMPLE_FS_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root, *f;
    if (BS->HandleProtocol(h, &fsg, (void **)&fs) != EFI_SUCCESS)
        return 0;
    if (fs->OpenVolume(fs, &root) != EFI_SUCCESS)
        return 0;
    EFI_STATUS st = root->Open(root, &f, path, EFI_FILE_MODE_READ, 0);
    root->Close(root);
    return st == EFI_SUCCESS ? f : 0;
}

static EFI_FILE_PROTOCOL *open_file(CHAR16 *path)
{
    return open_file_on(dev, path);
}

static bool exists_on(EFI_HANDLE h, CHAR16 *path)
{
    EFI_FILE_PROTOCOL *f = open_file_on(h, path);
    if (f)
        f->Close(f);
    return f != 0;
}

static int read_small(CHAR16 *path, char *out, int cap)
{
    EFI_FILE_PROTOCOL *f = open_file(path);
    UINTN n = (UINTN)cap - 1;
    if (!f)
        return -1;
    if (f->Read(f, &n, out) != EFI_SUCCESS)
        n = 0;
    f->Close(f);
    out[n] = 0;
    return (int)n;
}

/* ── The screen ───────────────────────────────────────────────────── */
static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
static EFI_HANDLE gop_handle;
static lpui_canvas_t bg, base, frame;   /* gradient+mark, + cards, + highlight */
static u8 *logo_scratch;
static int W, H;

static void *alloc(UINTN n)
{
    void *p = 0;
    if (BS->AllocatePool(EfiLoaderData, n, &p) != EFI_SUCCESS)
        return 0;
    return p;
}

/* The panel's own resolution. The EDID's first detailed timing is the
 * preferred mode by definition (EDID 1.3+); if the firmware does not
 * expose the EDID, the mode it already set is kept, which is what every
 * laptop firmware does on its own panel anyway. */
static void pick_mode(void)
{
    EFI_GUID eg = EFI_EDID_ACTIVE_GUID;
    EFI_EDID_ACTIVE_PROTOCOL *edid = 0;
    u32 want_w = 0, want_h = 0;
    if (gop_handle && BS->HandleProtocol(gop_handle, &eg, (void **)&edid) == EFI_SUCCESS &&
        edid && edid->SizeOfEdid >= 128) {
        const u8 *e = edid->Edid + 54;
        want_w = (u32)e[2] | ((u32)(e[4] >> 4) << 8);
        want_h = (u32)e[5] | ((u32)(e[7] >> 4) << 8);
    }
    if (want_w && want_h) {
        for (u32 m = 0; m < gop->Mode->MaxMode; m++) {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
            UINTN sz;
            if (gop->QueryMode(gop, m, &sz, &info) != EFI_SUCCESS)
                continue;
            if (info->HorizontalResolution == want_w && info->VerticalResolution == want_h) {
                if (m != gop->Mode->Mode)
                    gop->SetMode(gop, m);
                break;
            }
        }
    }
    W = (int)gop->Mode->Info->HorizontalResolution;
    H = (int)gop->Mode->Info->VerticalResolution;
    logf("screen %dx%d (EDID %dx%d)", W, H, (int)want_w, (int)want_h);
}

static void blt(lpui_canvas_t *c, int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w > 0 && h > 0)
        gop->Blt(gop, c->px, EfiBltBufferToVideo, (UINTN)x, (UINTN)y, (UINTN)x, (UINTN)y,
                 (UINTN)w, (UINTN)h, (UINTN)c->stride * 4);
}

/* ── Layout (in 4K pixels, see lp-ui.h) ───────────────────────────── */
enum { CH_LP, CH_REC };
typedef struct { int x, y, w, h; } rect_t;
static rect_t card[2];
static int ox, oy;                      /* where the 3840x2160 design sits */

static int X(int v) { return ox + lpui_px(v); }
static int Y(int v) { return oy + lpui_px(v); }

static void layout(void)
{
    lpui_set_screen(W, H);
    ox = (W - lpui_px(3840)) / 2;
    oy = (H - lpui_px(2160)) / 2;
    int cw = 1080, gap = 140, top = 860, ch = 600;
    int x0 = (3840 - 2 * cw - gap) / 2;
    for (int i = 0; i < 2; i++) {
        card[i].x = X(x0 + i * (cw + gap));
        card[i].y = Y(top);
        card[i].w = lpui_px(cw);
        card[i].h = lpui_px(ch);
    }
}

/* ── State ────────────────────────────────────────────────────────── */
static int sel;                         /* the highlighted card */
static int countdown_ms = 5000;         /* < 0: stopped */
static int reason;                      /* LPS_ id of the explanation line, or -1 */
static int failed_boots;
static bool have_recovery;             /* \EFI\LP\recovery.ok: LP-RECOVERY holds a system */
static char errtext[160];
static lpui_spring_t hl;                /* highlight position, 0 = LP, 1.0 = Recovery */
static int cursor_x = -1, cursor_y = -1;/* the mouse pointer, when there is one */

static void draw_card(lpui_canvas_t *c, int i)
{
    rect_t r = card[i];
    int rad = lpui_px(56);
    lpui_rrect(c, r.x, r.y, r.w, r.h, rad, 0xffffff, LPUI_CARD_A);
    lpui_rrect_stroke(c, r.x, r.y, r.w, r.h, rad, lpui_px(3) ? lpui_px(3) : 1, 0xffffff, LPUI_LINE_A);
    int tp = lpui_text_px(104), sp = lpui_text_px(40);
    int cx = r.x + r.w / 2;
    lpui_text_center(c, LPG_T, tp, cx, r.y + r.h / 2 + lpui_ascent(LPG_T, tp) / 3,
                     LPUI_INK, 256, lpui_s(i == CH_LP ? LPS_B_LP : LPS_B_RECOVERY));
    lpui_text_wrap(c, LPG_S, sp, r.x + lpui_px(60), r.y + r.h / 2 + lpui_px(150),
                   r.w - lpui_px(120), LPUI_INK2, 220, true,
                   lpui_s(i == CH_LP ? LPS_B_LP_SUB : LPS_B_RECOVERY_SUB));
}

/* Lines under the cards: why Recovery is preselected, the countdown, and
 * the last error. Redrawn on their own every second of the countdown, so
 * the band is restored from the background first. */
static int lines_top(void) { return Y(1560); }

static void draw_lines(lpui_canvas_t *c)
{
    int sp = lpui_text_px(40), lh = lpui_line(LPG_S, sp);
    int y = lines_top() + lpui_ascent(LPG_S, sp);
    char buf[200];
    if (reason >= 0) {
        lpui_fmt(buf, sizeof(buf), lpui_s(reason), failed_boots);
        int n = lpui_text_wrap(c, LPG_S, sp, X(520), y, lpui_px(2800), LPUI_ACCENT, 256, true, buf);
        y += n * lh;
    }
    if (errtext[0]) {
        lpui_fmt(buf, sizeof(buf), lpui_s(LPS_B_ERROR), errtext);
        int n = lpui_text_wrap(c, LPG_S, sp, X(520), y, lpui_px(2800), LPUI_DANGER, 256, true, buf);
        y += n * lh;
    }
    if (countdown_ms >= 0) {
        int s = (countdown_ms + 999) / 1000;
        lpui_fmt(buf, sizeof(buf), lpui_s(sel == CH_LP ? LPS_B_COUNT_LP : LPS_B_COUNT_REC), s);
        lpui_text_center(c, LPG_S, sp, W / 2, y, LPUI_INK, 256, buf);
    } else if (countdown_ms == -2) {
        lpui_text_center(c, LPG_S, sp, W / 2, y, LPUI_INK, 256, lpui_s(LPS_B_STARTING));
    } else {
        lpui_text_center(c, LPG_S, sp, W / 2, y, LPUI_INK2, 200, lpui_s(LPS_B_STOPPED));
    }
}

static void draw_base(void)
{
    lpui_copy_rect(&base, &bg, 0, 0, W, H);
    for (int i = 0; i < 2; i++)
        draw_card(&base, i);
    draw_lines(&base);
    int sp = lpui_text_px(40);
    lpui_text_center(&base, LPG_S, sp, W / 2, Y(2040), LPUI_INK2, 170, lpui_s(LPS_B_HINT));
}

/* Only the band of lines changes with the countdown. */
static void redraw_lines(void)
{
    int top = lines_top(), h = Y(2000) - top;
    lpui_copy_rect(&base, &bg, 0, top, W, h);
    draw_lines(&base);
}

static rect_t hl_rect(void)
{
    /* Interpolate between the two cards by the spring's position. */
    s64 p = hl.x;                       /* 16.16 */
    rect_t a = card[0], b = card[1], r;
    r.x = a.x + (int)(((s64)(b.x - a.x) * p) >> 16);
    r.y = a.y + (int)(((s64)(b.y - a.y) * p) >> 16);
    r.w = a.w;
    r.h = a.h;
    return r;
}

static rect_t last_hl;
static rect_t last_cursor;

static void compose(rect_t *dirty)
{
    rect_t r = hl_rect();
    int pad = lpui_px(24);
    rect_t d = { r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad };
    /* The union of where the highlight was and where it is. */
    int x0 = d.x < last_hl.x ? d.x : last_hl.x;
    int y0 = d.y < last_hl.y ? d.y : last_hl.y;
    int x1 = d.x + d.w > last_hl.x + last_hl.w ? d.x + d.w : last_hl.x + last_hl.w;
    int y1 = d.y + d.h > last_hl.y + last_hl.h ? d.y + d.h : last_hl.y + last_hl.h;
    if (last_hl.w == 0) { x0 = d.x; y0 = d.y; x1 = d.x + d.w; y1 = d.y + d.h; }
    last_hl = d;
    *dirty = (rect_t){ x0, y0, x1 - x0, y1 - y0 };
    lpui_copy_rect(&frame, &base, x0, y0, x1 - x0, y1 - y0);
    int rad = lpui_px(56);
    lpui_rrect(&frame, r.x, r.y, r.w, r.h, rad, 0xffffff, LPUI_SEL_A - LPUI_CARD_A);
    int t = lpui_px(10) ? lpui_px(10) : 2;
    lpui_rrect_stroke(&frame, r.x - t, r.y - t, r.w + 2 * t, r.h + 2 * t, rad + t, t,
                      LPUI_ACCENT, 256);
}

/* A small ring for the mouse: there is no hardware cursor in GOP. */
static void draw_cursor(void)
{
    if (cursor_x < 0)
        return;
    int s = lpui_px(36) < 10 ? 10 : lpui_px(36);
    rect_t old = last_cursor;
    if (old.w) {
        lpui_copy_rect(&frame, &base, old.x, old.y, old.w, old.h);
        rect_t d;
        compose(&d);                    /* the highlight may overlap it */
        blt(&frame, d.x, d.y, d.w, d.h);
        blt(&frame, old.x, old.y, old.w, old.h);
    }
    rect_t r = { cursor_x - s / 2, cursor_y - s / 2, s, s };
    lpui_rrect(&frame, r.x, r.y, s, s, s / 2, 0xffffff, 230);
    lpui_rrect_stroke(&frame, r.x, r.y, s, s, s / 2, s / 6 ? s / 6 : 1, 0x0d2740, 200);
    last_cursor = r;
    blt(&frame, r.x, r.y, r.w, r.h);
}

static void present_all(void)
{
    lpui_copy_rect(&frame, &base, 0, 0, W, H);
    last_hl.w = 0;
    rect_t d;
    compose(&d);
    last_cursor.w = 0;
    blt(&frame, 0, 0, W, H);
}

static void present_dirty(void)
{
    rect_t d;
    compose(&d);
    blt(&frame, d.x, d.y, d.w, d.h);
    if (cursor_x >= 0) {
        last_cursor.w = 0;
        draw_cursor();
    }
}

/* ── Time ─────────────────────────────────────────────────────────────
 * One periodic 10 ms timer drives everything: the fade, the spring and
 * the countdown. Firmware timers are not exact, but a countdown a few
 * percent long is invisible and nothing here needs wall time. */
#define TICK_MS 10
static EFI_EVENT tick;

/* Crossfade the screen between `base` and `other` (black when NULL),
 * easing out. to_other=false goes from other to base (a fade in),
 * true from base to other (a fade out). */
static void fade_from(lpui_canvas_t *other, int ms, bool to_other)
{
    int steps = ms / TICK_MS;
    for (int i = 1; i <= steps; i++) {
        UINTN idx;
        BS->WaitForEvent(1, &tick, &idx);
        u32 t = (u32)(i * 256 / steps);
        u32 e = 256 - ((256 - t) * (256 - t) >> 8);   /* 1 - (1-t)^2 */
        u32 a = to_other ? 256 - e : e;                /* weight of base */
        if (other == 0)
            lpui_dim(&frame, &base, a);
        else
            lpui_crossfade(&frame, &base, other, a);
        blt(&frame, 0, 0, W, H);
    }
}

/* ── Pointers ─────────────────────────────────────────────────────── */
#define MAXP 8
static EFI_SIMPLE_POINTER_PROTOCOL *mice[MAXP];
static EFI_ABSOLUTE_POINTER_PROTOCOL *touch[MAXP];
static int nmice, ntouch;
static bool touch_down[MAXP];

static void find_pointers(void)
{
    EFI_GUID sg = EFI_SIMPLE_POINTER_GUID, ag = EFI_ABSOLUTE_POINTER_GUID;
    EFI_HANDLE *hs;
    UINTN n;
    if (BS->LocateHandleBuffer(ByProtocol, &sg, 0, &n, &hs) == EFI_SUCCESS) {
        for (UINTN i = 0; i < n && nmice < MAXP; i++)
            if (BS->HandleProtocol(hs[i], &sg, (void **)&mice[nmice]) == EFI_SUCCESS)
                mice[nmice]->Reset(mice[nmice], false), nmice++;
        BS->FreePool(hs);
    }
    if (BS->LocateHandleBuffer(ByProtocol, &ag, 0, &n, &hs) == EFI_SUCCESS) {
        for (UINTN i = 0; i < n && ntouch < MAXP; i++)
            if (BS->HandleProtocol(hs[i], &ag, (void **)&touch[ntouch]) == EFI_SUCCESS)
                touch[ntouch]->Reset(touch[ntouch], false), ntouch++;
        BS->FreePool(hs);
    }
    logf("pointers: %d mouse, %d touch/tablet", nmice, ntouch);
}

static int card_at(int x, int y)
{
    for (int i = 0; i < 2; i++)
        if (x >= card[i].x && x < card[i].x + card[i].w &&
            y >= card[i].y && y < card[i].y + card[i].h)
            return i;
    return -1;
}

/* ── Selection ────────────────────────────────────────────────────── */
static bool animating;

static void select_card(int i)
{
    if (i == sel)
        return;
    bool was_counting = countdown_ms >= 0;
    sel = i;
    hl.target = (s64)i << 16;
    animating = true;
    if (was_counting)
        redraw_lines();
}

static void stop_countdown(void)
{
    if (countdown_ms < 0)
        return;
    countdown_ms = -1;
    redraw_lines();
    present_all();
}

/* ── Starting the kernel ──────────────────────────────────────────── */
static CHAR16 opts[1024];

static bool file_exists(CHAR16 *path)
{
    EFI_FILE_PROTOCOL *f = open_file(path);
    if (f)
        f->Close(f);
    return f != 0;
}

static void build_cmdline(int choice, char *out, int cap)
{
    char file[1024];
    int n = read_small(L"\\EFI\\LP\\cmdline.txt", file, sizeof(file));
    if (n < 0)
        file[0] = 0;
    int o = 0;
#define PUT(s) do { for (const char *_p = (s); *_p && o < cap - 1; ) out[o++] = *_p++; } while (0)
    if (file_exists(L"\\EFI\\LP\\initrd.img"))
        PUT("initrd=\\EFI\\LP\\initrd.img ");
    /* Word by word, so root= can be dropped for recovery and newlines and
     * carriage returns from an editor on another OS turn into spaces. */
    const char *p = file;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (!*p)
            break;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
            p++;
        bool is_root = (p - s) >= 5 && s[0] == 'r' && s[1] == 'o' && s[2] == 'o' &&
                       s[3] == 't' && s[4] == '=';
        if (choice == CH_REC && is_root)
            continue;
        while (s < p && o < cap - 2)
            out[o++] = *s++;
        out[o++] = ' ';
    }
    if (choice == CH_REC) {
        /* The installer writes this disk's own recovery root into
         * recovery.ok ("root=PARTUUID=..."). The label is only the
         * fallback: every LP disk has an LP-RECOVERY, the USB stick
         * this was installed from among them, and with the stick still
         * plugged in the kernel may take the first it finds. */
        char rec[128];
        int rn = read_small(L"\\EFI\\LP\\recovery.ok", rec, sizeof(rec));
        bool own = rn > 5 && rec[0] == 'r' && rec[1] == 'o' && rec[2] == 'o' &&
                   rec[3] == 't' && rec[4] == '=';
        if (own) {
            int k = 0;
            while (rec[k] && rec[k] != ' ' && rec[k] != '\t' && rec[k] != '\r' &&
                   rec[k] != '\n')
                k++;
            rec[k] = 0;
            PUT(rec);
        } else {
            PUT("root=PARTLABEL=LP-RECOVERY");
        }
        PUT(" lp.mode=recovery");
        if (lpui_korean)
            PUT(" lp.lang=ko");
    }
    while (o > 0 && out[o - 1] == ' ')
        o--;
    out[o] = 0;
#undef PUT
}

static const char *status_name(EFI_STATUS st)
{
    switch (st) {
    case EFI_NOT_FOUND:          return "vmlinuz.efi not found";
    case EFI_LOAD_ERROR:         return "not a valid kernel";
    case EFI_SECURITY_VIOLATION: return "Secure Boot refused it";
    case EFI_ACCESS_DENIED:      return "access denied";
    case EFI_OUT_OF_RESOURCES:   return "out of memory";
    case EFI_UNSUPPORTED:        return "unsupported";
    case EFI_DEVICE_ERROR:       return "device error";
    default:                     return "the kernel returned";
    }
}

static EFI_STATUS start_kernel(int choice)
{
    EFI_GUID dpg = EFI_DEVICE_PATH_GUID, lig = EFI_LOADED_IMAGE_GUID;
    EFI_DEVICE_PATH_PROTOCOL *dp;
    if (BS->HandleProtocol(dev, &dpg, (void **)&dp) != EFI_SUCCESS)
        return EFI_NOT_FOUND;

    /* The partition's device path, then a file path node for the kernel,
     * then the end node. */
    static u8 path[512];
    UINTN len = 0;
    const u8 *q = (const u8 *)dp;
    for (;;) {
        u16 nl = (u16)(q[2] | (q[3] << 8));
        if (q[0] == 0x7f && q[1] == 0xff)
            break;
        if (len + nl > sizeof(path) - 128 || nl < 4)
            return EFI_OUT_OF_RESOURCES;
        memcpy(path + len, q, nl);
        len += nl;
        q += nl;
    }
    static const CHAR16 kpath[] = L"\\EFI\\LP\\vmlinuz.efi";
    UINTN klen = sizeof(kpath);
    u8 *fp = path + len;
    fp[0] = 4;                  /* media */
    fp[1] = 4;                  /* file path */
    fp[2] = (u8)(4 + klen);
    fp[3] = (u8)((4 + klen) >> 8);
    memcpy(fp + 4, kpath, klen);
    len += 4 + klen;
    path[len] = 0x7f; path[len + 1] = 0xff; path[len + 2] = 4; path[len + 3] = 0;

    char cmd[1024];
    build_cmdline(choice, cmd, sizeof(cmd));
    int i = 0;
    for (; cmd[i] && i < 1023; i++)
        opts[i] = (CHAR16)(u8)cmd[i];
    opts[i] = 0;
    logf("starting %s: %s", choice == CH_LP ? "LP" : "LP Recovery", cmd);

    EFI_HANDLE img;
    EFI_STATUS st = BS->LoadImage(false, self, (EFI_DEVICE_PATH_PROTOCOL *)path, 0, 0, &img);
    if (st != EFI_SUCCESS)
        return st;
    EFI_LOADED_IMAGE_PROTOCOL *li;
    if (BS->HandleProtocol(img, &lig, (void **)&li) == EFI_SUCCESS) {
        li->LoadOptions = opts;
        li->LoadOptionsSize = (u32)((i + 1) * sizeof(CHAR16));
    }
    st = BS->StartImage(img, 0, 0);
    /* Only reached if the kernel gave up before ExitBootServices. */
    BS->UnloadImage(img);
    return st == EFI_SUCCESS ? EFI_LOAD_ERROR : st;
}

/* ── An LP already installed on this machine ──────────────────────── */
/*
 * The installer's own partition carries \EFI\LP\installer (mkdisk).
 * Started from there - the stick first in the firmware's order, or a
 * VM whose drive list puts the image above the disk LP went onto - this
 * menu ran the installer again over a machine that already had LP:
 * installed, restarted, "Install LP?". So first it looks at the other
 * FAT partitions for an installed LP (a boot menu and a command line,
 * no installer mark) and hands over to that disk's own menu, as if the
 * firmware had started it. Not when the stick was picked by hand from
 * the firmware's boot menu (picked_by_hand) - that is how LP is
 * installed again, and a person who pressed F12 and chose the stick was
 * given the old system instead - and not when a key is down as it
 * starts.
 */
/* Was this start picked by hand - the firmware's one-time boot menu (F12
 * on a Dell), or BootNext - rather than being the firmware's own first
 * choice? BootCurrent is the entry that started us, BootOrder[0] the one
 * it starts by itself. An install puts its own "LP" entry first, so the
 * stick left in at the first reboot is started only when the order has
 * lost that entry (a BIOS update, a reset) - and then it is the first
 * choice and hands over. Picked from the menu, the stick is what the
 * person asked for: the installer. Without the variables nothing can be
 * told, and the old rule stands. */
static bool picked_by_hand(void)
{
    static EFI_GUID gv = { 0x8be4df61, 0x93ca, 0x11d2,
                           { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c } };
    u16 cur = 0, order[64];
    u32 attrs;
    UINTN n = sizeof(cur);
    if (RT->GetVariable(L"BootCurrent", &gv, &attrs, &n, &cur) != EFI_SUCCESS || n != sizeof(cur))
        return false;
    n = sizeof(order);
    if (RT->GetVariable(L"BootOrder", &gv, &attrs, &n, order) != EFI_SUCCESS || n < sizeof(u16))
        return false;
    logf("BootCurrent %d, BootOrder[0] %d", (int)cur, (int)order[0]);
    return order[0] != cur;
}

static void chain_installed(void)
{
    if (!file_exists(L"\\EFI\\LP\\installer"))
        return;
    EFI_INPUT_KEY key;
    if (ST->ConIn && ST->ConIn->ReadKeyStroke(ST->ConIn, &key) == EFI_SUCCESS) {
        logf("a key is down: the installer, not the installed LP");
        return;
    }
    if (picked_by_hand()) {
        logf("started from the firmware's boot menu: the installer, not the installed LP");
        return;
    }
    EFI_GUID fsg = EFI_SIMPLE_FS_GUID, dpg = EFI_DEVICE_PATH_GUID;
    EFI_HANDLE *hs;
    UINTN n;
    if (BS->LocateHandleBuffer(ByProtocol, &fsg, 0, &n, &hs) != EFI_SUCCESS)
        return;
    for (UINTN i = 0; i < n; i++) {
        if (hs[i] == dev || !exists_on(hs[i], L"\\EFI\\LP\\lpboot.efi")
            || !exists_on(hs[i], L"\\EFI\\LP\\cmdline.txt")
            || exists_on(hs[i], L"\\EFI\\LP\\installer"))
            continue;
        EFI_DEVICE_PATH_PROTOCOL *dp;
        if (BS->HandleProtocol(hs[i], &dpg, (void **)&dp) != EFI_SUCCESS)
            continue;
        /* That partition's device path, a file node for its menu, the end. */
        static u8 path[512];
        UINTN len = 0;
        const u8 *q = (const u8 *)dp;
        bool ok = true;
        for (;;) {
            u16 nl = (u16)(q[2] | (q[3] << 8));
            if (q[0] == 0x7f && q[1] == 0xff)
                break;
            if (len + nl > sizeof(path) - 128 || nl < 4) {
                ok = false;
                break;
            }
            memcpy(path + len, q, nl);
            len += nl;
            q += nl;
        }
        if (!ok)
            continue;
        static const CHAR16 mpath[] = L"\\EFI\\LP\\lpboot.efi";
        u8 *fp = path + len;
        fp[0] = 4;
        fp[1] = 4;
        fp[2] = (u8)(4 + sizeof(mpath));
        fp[3] = (u8)((4 + sizeof(mpath)) >> 8);
        memcpy(fp + 4, mpath, sizeof(mpath));
        len += 4 + sizeof(mpath);
        path[len] = 0x7f; path[len + 1] = 0xff; path[len + 2] = 4; path[len + 3] = 0;
        EFI_HANDLE img;
        if (BS->LoadImage(false, self, (EFI_DEVICE_PATH_PROTOCOL *)path, 0, 0, &img)
                != EFI_SUCCESS)
            continue;
        logf("LP is installed on another disk: starting its menu");
        BS->FreePool(hs);
        BS->StartImage(img, 0, 0);
        /* It came back (it could not start LP either): the installer, then. */
        BS->UnloadImage(img);
        logf("the installed LP's menu returned; the installer after all");
        return;
    }
    BS->FreePool(hs);
}

static void boot(int choice)
{
    if (choice == CH_REC && !have_recovery) {
        const char *m = lpui_korean ? "No recovery system on this disk"
                                    : "No recovery system on this disk";
        int k = 0;
        for (; m[k] && k < (int)sizeof(errtext) - 1; k++)
            errtext[k] = m[k];
        errtext[k] = 0;
        logf("recovery chosen, but there is no recovery system");
        countdown_ms = -1;
        draw_base();
        lpui_copy_rect(&frame, &base, 0, 0, W, H);
        last_hl.w = 0;
        rect_t d;
        compose(&d);
        blt(&frame, 0, 0, W, H);
        return;
    }
    u32 count = var_u32(L"LPBootCount", 0);
    /* Count the attempt before making it: a boot that hangs never gets to
     * say it failed, so the number has to already be there. Recovery is
     * not counted - it is where a failing machine is sent. */
    if (choice == CH_LP && count < REINSTALL_MARK)
        set_u32(L"LPBootCount", count + 1 > 99 ? 99 : count + 1);
    countdown_ms = -2;
    errtext[0] = 0;
    redraw_lines();
    present_all();
    /* Fade to black, in the menu spring's exit time (0.7 x 180 ms): the
     * kernel's splash starts from black, with the PC maker's logo back
     * where the firmware showed it and ours small under it, as Ubuntu's
     * boot does (userland/splash, scene_init_oem). It used to start from
     * this menu's gradient; from that, the maker's logo coming back on
     * black was a jump. */
    lpui_copy_rect(&base, &frame, 0, 0, W, H);
    fade_from(0, 120, true);

    EFI_STATUS st = start_kernel(choice);

    /* Back here means it did not start. Undo the count - this attempt
     * never reached the kernel - and say what happened. */
    if (choice == CH_LP && count < REINSTALL_MARK)
        set_u32(L"LPBootCount", count);
    logf("start failed: %s", status_name(st));
    int k = 0;
    for (const char *s = status_name(st); *s && k < (int)sizeof(errtext) - 1; )
        errtext[k++] = *s++;
    errtext[k] = 0;
    countdown_ms = -1;
    draw_base();
    lpui_copy_rect(&frame, &base, 0, 0, W, H);
    last_hl.w = 0;
    rect_t d;
    compose(&d);
    blt(&frame, 0, 0, W, H);
}

/* ── Input ────────────────────────────────────────────────────────── */
static void on_key(EFI_INPUT_KEY k)
{
    stop_countdown();
    switch (k.ScanCode) {
    case SCAN_LEFT: case SCAN_UP: case SCAN_HOME:   select_card(CH_LP); return;
    case SCAN_RIGHT: case SCAN_DOWN: case SCAN_END: select_card(CH_REC); return;
    case SCAN_ESC: return;              /* stopping the countdown was all */
    }
    switch (k.UnicodeChar) {
    case '\t': select_card(!sel); break;
    case '\r': case '\n': case ' ': boot(sel); break;
    case '1': select_card(CH_LP); break;
    case '2': select_card(CH_REC); break;
    }
}

static void poll_pointers(void)
{
    for (int i = 0; i < ntouch; i++) {
        EFI_ABSOLUTE_POINTER_STATE s;
        if (touch[i]->GetState(touch[i], &s) != EFI_SUCCESS)
            continue;
        EFI_ABSOLUTE_POINTER_MODE *m = touch[i]->Mode;
        u64 rx = m->AbsoluteMaxX > m->AbsoluteMinX ? m->AbsoluteMaxX - m->AbsoluteMinX : 1;
        u64 ry = m->AbsoluteMaxY > m->AbsoluteMinY ? m->AbsoluteMaxY - m->AbsoluteMinY : 1;
        int x = (int)((s.CurrentX - m->AbsoluteMinX) * (u64)W / rx);
        int y = (int)((s.CurrentY - m->AbsoluteMinY) * (u64)H / ry);
        bool down = (s.ActiveButtons & EFI_ABSP_TouchActive) != 0;
        int c = card_at(x, y);
        if (down) {
            /* Feedback on touch-down, with no delay (COMMON.md Motion). */
            stop_countdown();
            if (c >= 0)
                select_card(c);
        } else if (touch_down[i] && c >= 0 && c == sel) {
            touch_down[i] = false;
            boot(c);
            return;
        }
        touch_down[i] = down;
    }
    for (int i = 0; i < nmice; i++) {
        EFI_SIMPLE_POINTER_STATE s;
        if (mice[i]->GetState(mice[i], &s) != EFI_SUCCESS)
            continue;
        EFI_SIMPLE_POINTER_MODE *m = mice[i]->Mode;
        s64 rx = m->ResolutionX ? (s64)m->ResolutionX : 1;
        s64 ry = m->ResolutionY ? (s64)m->ResolutionY : 1;
        if (cursor_x < 0) { cursor_x = W / 2; cursor_y = H / 2; }
        /* Resolution is counts per millimetre; a millimetre of mouse is a
         * few pixels, more on a bigger screen. */
        cursor_x += (int)(s.RelativeMovementX * lpui_px(12) / rx);
        cursor_y += (int)(s.RelativeMovementY * lpui_px(12) / ry);
        if (cursor_x < 0) cursor_x = 0;
        if (cursor_y < 0) cursor_y = 0;
        if (cursor_x >= W) cursor_x = W - 1;
        if (cursor_y >= H) cursor_y = H - 1;
        int c = card_at(cursor_x, cursor_y);
        if (c >= 0)
            select_card(c);
        stop_countdown();
        draw_cursor();
        if (s.LeftButton && c >= 0) {
            boot(c);
            return;
        }
    }
}

/* ── Entry ────────────────────────────────────────────────────────── */
EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
    ST = systab;
    BS = systab->BootServices;
    RT = systab->RuntimeServices;
    self = image;

    /* The firmware's watchdog resets the machine after five minutes in a
     * boot application; a menu left waiting (countdown stopped) must not
     * reboot under the person reading it. */
    BS->SetWatchdogTimer(0, 0, 0, 0);

    EFI_GUID lig = EFI_LOADED_IMAGE_GUID, gg = EFI_GOP_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li;
    if (BS->HandleProtocol(image, &lig, (void **)&li) != EFI_SUCCESS)
        return EFI_LOAD_ERROR;
    dev = li->DeviceHandle;

    chain_installed();

    char lang[16];
    var_str(L"LPLang", lang, sizeof(lang));
    lpui_korean = lang[0] == 'k' && lang[1] == 'o';

    u32 count = var_u32(L"LPBootCount", 0);
    char next[32];
    var_str(L"LPBootNext", next, sizeof(next));
    bool asked = next[0] == 'r';        /* "recovery" */
    if (next[0])
        RT->SetVariable(L"LPBootNext", &LP_GUID, VAR_ATTRS, 0, 0);   /* once */

    /* The installer (and the image build) put \EFI\LP\recovery.ok on
     * this partition only once LP-RECOVERY holds a recovery system. With
     * no system there, Recovery is a kernel with no root to run - a
     * machine that preselected it after two bad starts would hang on a
     * black screen instead of trying LP again. So without the mark the
     * menu never chooses Recovery by itself, and choosing it says why
     * it cannot start. */
    have_recovery = file_exists(L"\\EFI\\LP\\recovery.ok");

    reason = -1;
    sel = CH_LP;
    if (!have_recovery) {
        if (count >= 2 && count < REINSTALL_MARK)
            set_u32(L"LPBootCount", 0);
        count = 0;
        asked = false;
    }
    if (count >= REINSTALL_MARK) {
        sel = CH_REC;
        reason = LPS_B_REINSTALL_OPEN;
    } else if (count >= 2) {
        sel = CH_REC;
        reason = LPS_B_FAILED_BOOTS;
        failed_boots = (int)count;
    }
    if (asked) {
        sel = CH_REC;
        reason = LPS_B_ASKED;
    }
    logf("count=%d next=%s lang=%s recovery=%s -> %s", (int)count, next[0] ? next : "-",
         lpui_korean ? "ko" : "en", have_recovery ? "yes" : "no",
         sel == CH_LP ? "LP" : "LP Recovery");

    /* The GOP on the console's handle is the one the screen shows; any
     * GOP is second best. */
    EFI_HANDLE *hs;
    UINTN n;
    if (BS->HandleProtocol(ST->ConsoleOutHandle, &gg, (void **)&gop) == EFI_SUCCESS)
        gop_handle = ST->ConsoleOutHandle;
    else if (BS->LocateHandleBuffer(ByProtocol, &gg, 0, &n, &hs) == EFI_SUCCESS && n) {
        gop_handle = hs[0];
        BS->HandleProtocol(hs[0], &gg, (void **)&gop);
        BS->FreePool(hs);
    }
    if (!gop) {
        /* No screen at all: there is nobody to ask. Boot what would have
         * been chosen. */
        logf("no graphics output; booting %s", sel == CH_LP ? "LP" : "LP Recovery");
        return start_kernel(sel);
    }
    ST->ConOut->EnableCursor(ST->ConOut, false);
    pick_mode();

    UINTN px = (UINTN)W * (UINTN)H;
    bg = (lpui_canvas_t){ alloc(px * 4), W, H, W };
    base = (lpui_canvas_t){ alloc(px * 4), W, H, W };
    frame = (lpui_canvas_t){ alloc(px * 4), W, H, W };
    logo_scratch = alloc(LPUI_LOGO_SCRATCH);
    if (!bg.px || !base.px || !frame.px || !logo_scratch)
        return start_kernel(sel);

    layout();
    lpui_gradient(&bg);
    lpui_logo(&bg, W / 2, Y(470), lpui_px(380), logo_scratch);
    lpui_spring_init(&hl, LPUI_SPRING_MENU, (s64)sel << 16);
    draw_base();

    find_pointers();
    BS->CreateEvent(EVT_TIMER, 0, 0, 0, &tick);
    BS->SetTimer(tick, TimerPeriodic, TICK_MS * 10000);

    /* Fade in from black over 200 ms: the finished picture (cards and
     * highlight) is built in `base` for the fade, then base is rebuilt
     * without the highlight, which is its normal content. */
    lpui_copy_rect(&frame, &base, 0, 0, W, H);
    last_hl.w = 0;
    rect_t d;
    compose(&d);
    lpui_copy_rect(&base, &frame, 0, 0, W, H);
    fade_from(0, 200, false);
    draw_base();
    present_all();

    EFI_EVENT evs[2 + 2 * MAXP];
    int nev = 0;
    evs[nev++] = tick;
    evs[nev++] = ST->ConIn->WaitForKey;
    int remaining_ms = countdown_ms;
    int last_shown = (remaining_ms + 999) / 1000;

    for (;;) {
        UINTN idx = 0;
        BS->WaitForEvent((UINTN)nev, evs, &idx);
        if (idx == 1) {
            EFI_INPUT_KEY k;
            while (ST->ConIn->ReadKeyStroke(ST->ConIn, &k) == EFI_SUCCESS)
                on_key(k);
        }
        /* Pointers are polled on the tick rather than waited on: some
         * firmware signals WaitForInput only on movement, and a touch
         * that lifts without moving must still be seen. */
        if (idx == 0) {
            poll_pointers();
            if (countdown_ms >= 0) {
                countdown_ms -= TICK_MS;
                if (countdown_ms <= 0) {
                    countdown_ms = 0;
                    boot(sel);
                    continue;
                }
                int s = (countdown_ms + 999) / 1000;
                if (s != last_shown) {
                    last_shown = s;
                    redraw_lines();
                    present_all();
                }
            }
            if (animating) {
                animating = lpui_spring_step(&hl, TICK_MS);
                present_dirty();
            }
        }
    }
}
