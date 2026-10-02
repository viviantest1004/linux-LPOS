/* screen.c - lp-recovery's framebuffer, VT and input.
 *
 * ── The picture ──
 *
 * Drawing happens in three canvases the size of the screen (lp-ui.h):
 * the gradient and mark (cv_bg, made once), the screen's content on top
 * of it (cv_base, remade when the content changes) and the frame that
 * goes out (cv_frame: base plus the moving focus highlight). Only the
 * rectangle that changed is sent to the screen, a row at a time with
 * write(): on simpledrm and the DRM drivers' fbdev emulation - which is
 * what i915 gives us on the XPS - a write() reaches the panel at once,
 * while stores through mmap() wait for deferred I/O (the boot splash
 * found this first; userland/splash/splash.c).
 *
 * The VT is put in KD_GRAPHICS for as long as the program runs, so the
 * kernel's console never draws text or a cursor over it - the recovery
 * shell included, which is drawn by term.c, not by the console (whose
 * font is a few millimetres high at 4K and has no on-screen keyboard).
 *
 * ── Input ──
 *
 * evdev, not the tty: the menu needs touch, and a key's press and
 * release, neither of which a tty carries. Every /dev/input/event* is
 * classified by what it reports - keys, absolute position (a touch screen
 * or a tablet), relative motion (a mouse). Keyboards are GRABBED while
 * the menu owns the screen. Without that the kernel's keyboard handler
 * also feeds every key to tty1, and a password typed into the menu would
 * sit in tty1's input queue for whatever reads tty1 next.
 *
 * Devices are looked for again every two seconds, so a USB keyboard
 * plugged in after the menu came up works.
 *
 * A touch screen is read finger by finger (the multitouch slots of
 * ABS_MT_*, protocol B), and each finger's press, move and lift is its
 * own event: two thumbs typing fast on the on-screen keyboard overlap,
 * and the second key must not be lost because the first finger was still
 * down. Single-touch screens (ABS_X/Y with BTN_TOUCH) and tablets are
 * finger 0. The keyboard grab stays on for the recovery shell too: the
 * shell runs on a pty inside this program (term.c), so every key it gets
 * comes from here, and tty1 never sees one.
 */
#include "recovery.h"

int SW, SH;
lpui_canvas_t cv_bg, cv_base, cv_frame;
u8 *logo_scratch;

/* ── The framebuffer ──────────────────────────────────────────────── */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPUT_VSCREENINFO 0x4601
#define FB_ACTIVATE_FORCE   128
#define KDSETMODE           0x4B3A
#define KD_TEXT             0
#define KD_GRAPHICS         1
#define TCFLSH              0x540B

static int fb_fd = -1;
static u32 fb_bpp, fb_line, fb_xoff, fb_yoff;
static u32 fb_off[3], fb_len[3];
static u8 *rowbuf;

bool scr_open(void)
{
    long fd = lp_open("/dev/fb0", O_RDWR | O_CLOEXEC, 0);
    if (fd < 0)
        return false;
    u8 var[160], fix[80];
    memset(var, 0, sizeof var);
    memset(fix, 0, sizeof fix);
    if (lp_ioctl((int)fd, FBIOGET_VSCREENINFO, var) < 0 ||
        lp_ioctl((int)fd, FBIOGET_FSCREENINFO, fix) < 0) {
        lp_close((int)fd);
        return false;
    }
#define U32AT(b, o) (*(u32 *)((b) + (o)))
    SW = (int)U32AT(var, 0);
    SH = (int)U32AT(var, 4);
    fb_xoff = U32AT(var, 16);
    fb_yoff = U32AT(var, 20);
    fb_bpp = U32AT(var, 24);
    for (int c = 0; c < 3; c++) {
        fb_off[c] = U32AT(var, 32 + 12 * c);
        fb_len[c] = U32AT(var, 36 + 12 * c);
    }
    fb_line = U32AT(fix, 48);
    /* Make the display show this framebuffer. When a DRM driver (i915 on
     * the XPS, virtio-gpu in a VM) takes over from the firmware's
     * framebuffer, its fbdev emulation only sets a mode when fbcon draws
     * on it - and with a quiet boot and deferred takeover nothing ever
     * has. The panel then keeps scanning out the boot menu's last
     * picture while every row written here goes to a buffer nobody
     * shows: the menu ran, and the screen stayed on the logo. Putting the
     * same mode back with FB_ACTIVATE_FORCE makes the driver commit it
     * (drm_fb_helper_set_par). On a plain simpledrm/efifb it is a
     * no-op. */
    U32AT(var, 84) = FB_ACTIVATE_FORCE;          /* activate: NOW | FORCE */
    if (lp_ioctl((int)fd, FBIOPUT_VSCREENINFO, var) < 0)
        rlog("screen: could not set the mode (the old picture may stay)");
    if ((fb_bpp != 32 && fb_bpp != 16 && fb_bpp != 24) || SW < 320 || SH < 200) {
        lp_close((int)fd);
        return false;
    }
    fb_fd = (int)fd;
    size_t px = (size_t)SW * (size_t)SH;
    cv_bg = (lpui_canvas_t){ malloc(px * 4), SW, SH, SW };
    cv_base = (lpui_canvas_t){ malloc(px * 4), SW, SH, SW };
    cv_frame = (lpui_canvas_t){ malloc(px * 4), SW, SH, SW };
    logo_scratch = malloc(LPUI_LOGO_SCRATCH);
    rowbuf = malloc((size_t)fb_line * 2 > (size_t)SW * 4 ? (size_t)fb_line * 2 : (size_t)SW * 4);
    if (!cv_bg.px || !cv_base.px || !cv_frame.px || !logo_scratch || !rowbuf)
        return false;
    rlog("screen %dx%d, %u bpp", SW, SH, fb_bpp);
    return true;
}

static u32 chan(u32 v8, u32 len)
{
    return len >= 8 ? v8 << (len - 8) : v8 >> (8 - len);
}

static void fb_put(const lpui_canvas_t *c, int x, int y, int w, int h);

/* ── Settling ──
 *
 * On a DRM driver's fbdev emulation a write() is shown by a flush that
 * runs a moment later, and in a VM (virtio-gpu) that flush showed the
 * picture as it was one write EARLIER: the last frame of anything - the
 * keyboard closing, a screen appearing - stayed invisible until the next
 * thing was drawn, so the keyboard seemed not to close. The rectangle
 * drawn since the screen last settled is therefore written once more
 * when nothing has been drawn for SETTLE_MS; on a driver that shows
 * every write at once that costs one redundant copy of a few rows. */
#define SETTLE_MS 90
static const lpui_canvas_t *settle_c;
static int sx0, sy0, sx1, sy1;          /* union of rows written, or sx1 == 0 */
static s64 settle_at;

void scr_present(const lpui_canvas_t *c, int x, int y, int w, int h)
{
    if (fb_fd < 0 || w <= 0 || h <= 0)
        return;
    fb_put(c, x, y, w, h);
    if (settle_c != c)
        sx1 = 0;                        /* a different picture: start over */
    settle_c = c;
    if (!sx1) {
        sx0 = x; sy0 = y; sx1 = x + w; sy1 = y + h;
    } else {
        if (x < sx0) sx0 = x;
        if (y < sy0) sy0 = y;
        if (x + w > sx1) sx1 = x + w;
        if (y + h > sy1) sy1 = y + h;
    }
    settle_at = lp_monotonic_ms() + SETTLE_MS;
}

int scr_settle(void)
{
    if (!sx1 || !settle_c)
        return -1;
    s64 now = lp_monotonic_ms();
    if (now < settle_at)
        return (int)(settle_at - now);
    fb_put(settle_c, sx0, sy0, sx1 - sx0, sy1 - sy0);
    sx1 = 0;
    return -1;
}

/* ── Rows that reach the right edge ──
 *
 * On the DRM drivers' fbdev emulation (virtio-gpu in every VM - QEMU,
 * KVM, UTM - and any driver that draws through a shadow buffer) a
 * write() is shown by a flush of the rectangle it covered, and the
 * kernel works that rectangle out from the byte range
 * (drm_fb_helper_memory_range_to_clip). For a write of ONE line that
 * ends exactly at the end of the line it gets the right edge wrong -
 * end % line_length is 0, so x2 is 0 - and the rectangle is empty:
 * nothing is flushed. Every row of the whole picture is such a write,
 * so the menu came up black, and only what stopped short of the right
 * edge (the focus box moving) ever showed, as an island in the black.
 * Writes that span two lines or more are read as whole lines and are
 * flushed whole. So rows that reach the right edge are sent as whole
 * lines, two at a time, in one write(). */
static void put_rows(const lpui_canvas_t *c, int y, int n, bool native, u32 bytes)
{
    const u8 *out;
    if (native && c->stride == SW)
        out = (const u8 *)(c->px + (u64)y * c->stride);
    else {
        for (int j = 0; j < n; j++) {
            const u32 *src = c->px + (u64)(y + j) * c->stride;
            u8 *d = rowbuf + (size_t)j * fb_line;
            for (int i = 0; i < SW; i++) {
                u32 p = src[i];
                u32 v = chan((p >> 16) & 255, fb_len[0]) << fb_off[0] |
                        chan((p >> 8) & 255, fb_len[1]) << fb_off[1] |
                        chan(p & 255, fb_len[2]) << fb_off[2];
                memcpy(d + (size_t)i * bytes, &v, bytes);
            }
        }
        out = rowbuf;
    }
    lp_lseek(fb_fd, (s64)(y + (int)fb_yoff) * fb_line, 0);
    lp_write(fb_fd, out, (size_t)n * fb_line);
}

static void fb_put(const lpui_canvas_t *c, int x, int y, int w, int h)
{
    if (fb_fd < 0)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    if (w <= 0 || h <= 0)
        return;
    u32 bytes = fb_bpp / 8;
    bool native = fb_bpp == 32 && fb_off[0] == 16 && fb_off[1] == 8 && fb_off[2] == 0;
    if (fb_xoff == 0 && (u32)(x + w) * bytes == fb_line && (u32)SW * bytes == fb_line) {
        /* Whole lines, two to a write (see above); one row alone takes
         * its neighbour along, which is drawn already and costs nothing
         * to send again. */
        if (h == 1) {
            if (y + 1 < SH) h = 2;
            else { y--; h = 2; }
        }
        for (int j = 0; j < h; j += 2)
            put_rows(c, j + 2 <= h ? y + j : y + h - 2, 2, native, bytes);
        return;
    }
    for (int j = 0; j < h; j++) {
        const u32 *src = c->px + (u64)(y + j) * c->stride + x;
        const u8 *out;
        if (native)
            out = (const u8 *)src;
        else {
            for (int i = 0; i < w; i++) {
                u32 p = src[i];
                u32 v = chan((p >> 16) & 255, fb_len[0]) << fb_off[0] |
                        chan((p >> 8) & 255, fb_len[1]) << fb_off[1] |
                        chan(p & 255, fb_len[2]) << fb_off[2];
                memcpy(rowbuf + (size_t)i * bytes, &v, bytes);
            }
            out = rowbuf;
        }
        s64 off = (s64)(y + j + (int)fb_yoff) * fb_line + (s64)(x + (int)fb_xoff) * bytes;
        lp_lseek(fb_fd, off, 0);
        lp_write(fb_fd, out, (size_t)w * bytes);
    }
}

/* ── Input devices ────────────────────────────────────────────────── */
#define MAXDEV   24
#define MAXSLOT  10             /* fingers tracked per touch screen */
enum { D_KBD = 1, D_ABS = 2, D_MT = 4, D_REL = 8 };
typedef struct {
    int  x, y;                  /* device units */
    bool down, was_down, changed;
} slot_t;
typedef struct {
    int  fd, num, kind;
    int  minx, maxx, miny, maxy;
    slot_t s[MAXSLOT];          /* single-touch devices use s[0] */
    int  slot;                  /* current multitouch slot */
    bool moved;                 /* relative mouse moved, or its button */
    bool btn;                   /* mouse / tablet button, BTN_TOUCH */
} dev_t_;
static dev_t_ devs[MAXDEV];
static int ndev;
static bool grabbed;
static s64 last_scan;
static bool shift_l, shift_r, ctrl_l, ctrl_r, alt_l, alt_r, caps;
static int mouse_x = -1, mouse_y = -1;
static int extra_fd = -1;

/* A frame (EV_SYN) can change several fingers at once, and in_wait hands
 * out one event per call, so events wait here. */
#define QN 32
static uev_t queue[QN];
static int qhead, qlen;

static void push(const uev_t *e)
{
    if (qlen == QN)
        return;
    queue[(qhead + qlen++) % QN] = *e;
}

static bool pop(uev_t *e)
{
    if (!qlen)
        return false;
    *e = queue[qhead];
    qhead = (qhead + 1) % QN;
    qlen--;
    return true;
}

void in_watch_fd(int fd) { extra_fd = fd; }

#define EVIOCGBIT(ev, len) _LP_IOC(2u, 'E', 0x20 + (ev), (len))
#define EVIOCGABS(abs)     _LP_IOC(2u, 'E', 0x40 + (abs), 24)
#define EVIOCGRAB          _LP_IOC(1u, 'E', 0x90, sizeof(int))
#define TEST(bits, n)      ((bits)[(n) / 8] & (1u << ((n) % 8)))

static bool dev_open(int num)
{
    for (int i = 0; i < ndev; i++)
        if (devs[i].num == num)
            return true;
    if (ndev >= MAXDEV)
        return false;
    char path[40];
    snprintf(path, sizeof path, "/dev/input/event%d", num);
    long fd = lp_open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC, 0);
    if (fd < 0)
        return false;
    u8 ev[4] = { 0 }, keys[96] = { 0 }, abs[8] = { 0 }, rel[4] = { 0 };
    lp_ioctl((int)fd, EVIOCGBIT(0, sizeof ev), ev);
    lp_ioctl((int)fd, EVIOCGBIT(1, sizeof keys), keys);
    lp_ioctl((int)fd, EVIOCGBIT(3, sizeof abs), abs);
    lp_ioctl((int)fd, EVIOCGBIT(2, sizeof rel), rel);
    dev_t_ d;
    memset(&d, 0, sizeof d);
    d.fd = (int)fd;
    d.num = num;
    if (TEST(ev, 1) && TEST(keys, 30) && TEST(keys, 28))       /* KEY_A, KEY_ENTER */
        d.kind |= D_KBD;
    if (TEST(ev, 3) && TEST(abs, 0x35) && TEST(abs, 0x36))     /* ABS_MT_POSITION_X/Y */
        d.kind |= D_ABS | D_MT;
    else if (TEST(ev, 3) && TEST(abs, 0) && TEST(abs, 1))      /* ABS_X/Y */
        d.kind |= D_ABS;
    else if (TEST(ev, 2) && TEST(rel, 0) && TEST(rel, 1))      /* REL_X/Y */
        d.kind |= D_REL;
    if (!d.kind) {
        lp_close((int)fd);
        return false;
    }
    if (d.kind & D_ABS) {
        s32 info[6];
        int ax = (d.kind & D_MT) ? 0x35 : 0, ay = (d.kind & D_MT) ? 0x36 : 1;
        if (lp_ioctl((int)fd, EVIOCGABS(ax), info) == 0) { d.minx = info[1]; d.maxx = info[2]; }
        if (lp_ioctl((int)fd, EVIOCGABS(ay), info) == 0) { d.miny = info[1]; d.maxy = info[2]; }
        if (d.maxx <= d.minx) d.maxx = d.minx + 1;
        if (d.maxy <= d.miny) d.maxy = d.miny + 1;
    }
    if ((d.kind & D_KBD) && grabbed)
        lp_ioctl((int)fd, EVIOCGRAB, (void *)1);
    devs[ndev++] = d;
    rlog("input event%d:%s%s%s", num, (d.kind & D_KBD) ? " keyboard" : "",
         (d.kind & D_MT) ? " touch" : (d.kind & D_ABS) ? " tablet" : "",
         (d.kind & D_REL) ? " mouse" : "");
    return true;
}

void in_rescan(void)
{
    for (int n = 0; n < 32; n++)
        dev_open(n);
    last_scan = lp_monotonic_ms();
}

static void dev_drop(int i)
{
    lp_close(devs[i].fd);
    devs[i] = devs[--ndev];
}

void scr_graphics(bool on)
{
    lp_ioctl(STDIN_FILENO, KDSETMODE, (void *)(long)(on ? KD_GRAPHICS : KD_TEXT));
    grabbed = on;
    for (int i = 0; i < ndev; i++)
        if (devs[i].kind & D_KBD)
            lp_ioctl(devs[i].fd, EVIOCGRAB, (void *)(long)(on ? 1 : 0));
    /* Whatever reached the tty's queue before the grab must not be read
     * by anybody later. */
    lp_ioctl(STDIN_FILENO, TCFLSH, (void *)0);
}

void in_drain(void)
{
    u8 buf[24 * 16];
    for (int i = 0; i < ndev; i++)
        while (lp_read(devs[i].fd, buf, sizeof buf) > 0)
            ;
    qlen = 0;
}

/* US layout. Passwords and the one confirmation word are ASCII, and the
 * recovery shell is an English console; Korean input is a desktop
 * feature (the OSK track's input method). */
static const char KEYMAP[2][58] = {
    { 0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9,
      'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13, 0,
      'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
      'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ' },
    { 0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 9,
      'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 13, 0,
      'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
      'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ' },
};
/* The keypad, as with Num Lock on: KEY_KP7 (71) .. KEY_KPDOT (83). */
static const char KEYPAD[] = "789-456+1230.";

static u32 key_char(int code)
{
    if (code >= 71 && code <= 83)
        return (u8)KEYPAD[code - 71];
    if (code == 98)
        return '/';                     /* KEY_KPSLASH */
    if (code <= 0 || code >= 58)
        return 0;
    bool shift = shift_l || shift_r;
    char c = KEYMAP[0][code];
    bool letter = c >= 'a' && c <= 'z';
    int layer = (letter ? (shift != caps) : shift) ? 1 : 0;
    u8 ch = (u8)KEYMAP[layer][code];
    return ch >= 32 && ch < 127 ? ch : 0;
}

typedef struct { u16 type, code; s32 value; } ie_t;

static void ptr_event(dev_t_ *d, int slot, int x, int y, bool down)
{
    uev_t e;
    memset(&e, 0, sizeof e);
    e.kind = UEV_PTR;
    e.slot = slot;
    if (d->kind & D_REL) {
        e.x = mouse_x;
        e.y = mouse_y;
        e.mouse = true;
    } else {
        e.x = (int)((s64)(x - d->minx) * SW / (d->maxx - d->minx));
        e.y = (int)((s64)(y - d->miny) * SH / (d->maxy - d->miny));
    }
    if (e.x < 0) e.x = 0;
    if (e.y < 0) e.y = 0;
    if (e.x >= SW) e.x = SW - 1;
    if (e.y >= SH) e.y = SH - 1;
    e.down = down;
    push(&e);
}

/* Read what one device has into the queue. -1 when it is gone. */
static int dev_read(dev_t_ *d)
{
    u8 raw[sizeof(long) * 2 + 8];
    for (;;) {
        long n = lp_read(d->fd, raw, sizeof raw);
        if (n < (long)sizeof raw)
            return n == -11 /* EAGAIN */ ? 0 : -1;
        ie_t e;
        memcpy(&e, raw + sizeof(long) * 2, sizeof e);
        if (e.type == 1 && (d->kind & D_KBD) && e.code < 0x100) {       /* EV_KEY */
            bool on = e.value != 0;
            switch (e.code) {
            case 42:  shift_l = on; continue;
            case 54:  shift_r = on; continue;
            case 29:  ctrl_l = on; continue;
            case 97:  ctrl_r = on; continue;
            case 56:  alt_l = on; continue;
            case 100: alt_r = on; continue;
            case 58:  if (e.value == 1) caps = !caps; continue;
            }
            if (e.value == 0)
                continue;
            uev_t k;
            memset(&k, 0, sizeof k);
            k.kind = UEV_KEY;
            k.code = e.code;
            k.ch = key_char(e.code);
            k.press = true;
            k.shift = shift_l || shift_r;
            k.ctrl = ctrl_l || ctrl_r;
            k.alt = alt_l || alt_r;
            push(&k);
            continue;
        }
        if (e.type == 1 && (e.code == 0x14a || e.code == 0x110)) {       /* BTN_TOUCH/LEFT */
            d->btn = e.value != 0;
            if (!(d->kind & D_MT)) {
                d->s[0].down = d->btn;
                d->s[0].changed = true;
            }
            d->moved = true;
            continue;
        }
        if (e.type == 3 && (d->kind & D_ABS)) {
            if (d->kind & D_MT) {
                if (e.code == 0x2f) {                                   /* ABS_MT_SLOT */
                    d->slot = e.value >= 0 && e.value < MAXSLOT ? e.value : -1;
                    continue;
                }
                slot_t *s = d->slot >= 0 ? &d->s[d->slot] : 0;
                if (!s)
                    continue;
                if (e.code == 0x35) { s->x = e.value; s->changed = true; }
                else if (e.code == 0x36) { s->y = e.value; s->changed = true; }
                else if (e.code == 0x39) { s->down = e.value >= 0; s->changed = true; }  /* TRACKING_ID */
            } else {
                if (e.code == 0) { d->s[0].x = e.value; d->s[0].changed = true; }
                else if (e.code == 1) { d->s[0].y = e.value; d->s[0].changed = true; }
            }
            continue;
        }
        if (e.type == 2 && (d->kind & D_REL)) {
            if (mouse_x < 0) { mouse_x = SW / 2; mouse_y = SH / 2; }
            int k = lpui_px(4) > 1 ? lpui_px(4) : 1;
            if (e.code == 0) mouse_x += e.value * k;
            if (e.code == 1) mouse_y += e.value * k;
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_x >= SW) mouse_x = SW - 1;
            if (mouse_y >= SH) mouse_y = SH - 1;
            d->moved = true;
            continue;
        }
        if (e.type == 0) {                                              /* EV_SYN */
            if (d->kind & D_REL) {
                if (d->moved)
                    ptr_event(d, 0, 0, 0, d->btn);
                d->moved = false;
                continue;
            }
            for (int i = 0; i < MAXSLOT; i++) {
                slot_t *s = &d->s[i];
                if (!s->changed)
                    continue;
                s->changed = false;
                /* A tablet (usb-tablet in a VM) moves with nothing down:
                 * that is hover, which the menu shows like a mouse. */
                if (s->down || s->was_down || !(d->kind & D_MT))
                    ptr_event(d, i, s->x, s->y, s->down);
                s->was_down = s->down;
            }
            d->moved = false;
        }
    }
}

typedef struct { int fd; short events, revents; } pollfd_t;

int in_wait(uev_t *ev, int timeout_ms)
{
    memset(ev, 0, sizeof *ev);
    if (pop(ev))
        return 1;
    s64 now = lp_monotonic_ms();
    if (now - last_scan > 2000)
        in_rescan();
    pollfd_t pf[MAXDEV + 1];
    int n = 0;
    for (int i = 0; i < ndev; i++)
        pf[n++] = (pollfd_t){ devs[i].fd, 1 /* POLLIN */, 0 };
    int xi = -1;
    if (extra_fd >= 0) {
        xi = n;
        pf[n++] = (pollfd_t){ extra_fd, 1, 0 };
    }
    if (timeout_ms < 0)
        timeout_ms = 0;
    struct { long s, ns; } ts = { timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L };
    long r = sys_call5(SYS_ppoll, (long)pf, n, (long)&ts, 0, 8);
    if (r <= 0)
        return 0;
    for (int i = 0; i < ndev; i++) {
        if (!pf[i].revents)
            continue;
        if (dev_read(&devs[i]) < 0 || (pf[i].revents & (8 | 16))) {  /* ERR, HUP */
            dev_drop(i);
            /* keep pf in step with devs */
            pf[i] = pf[ndev];
            i--;
        }
    }
    if (pop(ev))
        return 1;
    if (xi >= 0 && pf[xi].revents) {
        ev->kind = UEV_FD;
        return 1;
    }
    return 0;
}
