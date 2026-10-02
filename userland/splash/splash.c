/* splash - the boot screen, drawn on the framebuffer, until the desktop
 * is ready to take it over.
 *
 * The bare-metal firmware used to draw this: it asked the GPU for a
 * framebuffer over the mailbox and wrote pixels into it. That firmware
 * and Linux cannot both be the kernel the GPU loads, so when the board
 * started booting Linux the splash went with it.
 *
 * This is the same idea one layer higher up. Linux hands us the
 * framebuffer as /dev/fb0 and tells us its shape through two ioctls;
 * everything after that is arithmetic. It works on the board, where the
 * GPU set the mode up, and in a virtual machine or on a laptop, where
 * the emulated card or EFI did - none of them is treated differently.
 *
 * ── What it draws ──
 *
 * The LP mark and the system's name, on the desktop's own aubergine
 * gradient (scene.h has the picture; this file has the screen and the
 * clock). The logo fades in over 400ms. Then, on a machine that has a
 * desktop to wait for, a small spinner comes up under it after a second
 * and turns until the desktop says it is ready.
 *
 *     splash                  fade the logo in and exit - or hold, with
 *                             the spinner, when the kernel command line
 *                             has the word `splash` (as Ubuntu's does)
 *     splash --hold           hold whatever the command line says
 *     splash stop             tell a running splash the desktop is here:
 *                             it fades the spinner out, leaves the logo on
 *                             the screen, and exits; returns once it has
 *     splash stop --console   the same, but hand the screen back to the
 *                             text console (a text login follows, not a
 *                             desktop)
 *
 * Holding is opt-in because a machine that boots to a text console (the
 * Pi images, recovery) has nobody to say "stop", and a splash that sat
 * on the console for its whole timeout would hide the login prompt.
 *
 * ── The hand-off ──
 *
 * `splash stop` is sent by the desktop's start script just before the
 * compositor starts. The splash does not clear the screen on the way
 * out: the frame it leaves - gradient and logo, no spinner - is exactly
 * what desktop/branding/lp-splash-fade draws as the session's first
 * frame (both are drawn by scene.h), and that one then fades into the
 * desktop. So the screen goes firmware logo -> splash -> desktop without
 * a black frame or a line of console text in between. It also leaves the
 * VT in graphics mode (KD_GRAPHICS, set when holding starts), so the
 * console cannot draw its text over the logo while the compositor is
 * starting; the compositor's seat takes the VT from there. With
 * --console, or when nothing has said stop after 90 seconds, the console
 * is given back (KD_TEXT) and whatever it holds appears.
 *
 * ── Why it redraws so little ──
 *
 * The XPS panel is 3840x2160. Redrawing that at 60 frames a second is a
 * full core, and during boot that core belongs to everything else. So
 * after the first frame only what changes is drawn: the logo's box while
 * it fades in, and then only the spinner's box - about 60x60 pixels on
 * the 4K panel - with one write() per row of it.
 *
 * write() rather than mmap(), deliberately. On simpledrm and the other
 * DRM drivers' fbdev emulation, a write() is flushed to the screen at
 * once, and only the rows (and, for a one-row write, the columns) it
 * covered; a store through mmap() is caught by page faults and flushed
 * by deferred I/O at most 20 times a second. On efifb both are direct.
 *
 * ── Why a spring and not a curve ──
 *
 * The fades are the design system's springs (design/feel.md §2), done in
 * fixed point because this libc has no floating point. A spring can be
 * turned round half way - the spinner told to go while it is still
 * arriving leaves from where it is, at the speed it has - where a timed
 * curve would jump.
 *
 * ── Reduced motion ──
 *
 * When /etc/lp/reduce-motion exists, or the first account has asked for
 * it (~/.config/lp/reduce-motion, what Settings writes), the fades take
 * 100ms and the spinner does not turn: its ring breathes, slowly
 * brightening and dimming, so the screen still says "working".
 *
 * All arithmetic is integer: positions in 1/32 pixel, colours in 8.8
 * fixed point, alpha in 0..256, springs in 1/2^24.
 */
#include "types.h"
#include "osname.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "syscall.h"
#include "scene.h"

/* ── The framebuffer ──────────────────────────────────────────────────
 * Two ioctls describe it. Rather than declare the kernel's structures -
 * they are large, and most of what is in them is for setting a mode, not
 * reading one - we read the fields we need out of a byte buffer at the
 * offsets the kernel puts them at. Those offsets are part of the ABI and
 * cannot move.
 */
#define FBIOGET_VSCREENINFO  0x4600
#define FBIOGET_FSCREENINFO  0x4602
#define FBIOPAN_DISPLAY      0x4606

#define VAR_SIZE        160
#define VAR_XRES          0
#define VAR_YRES          4
#define VAR_XOFFSET      16
#define VAR_YOFFSET      20
#define VAR_BPP          24
#define VAR_RED_OFF      32     /* struct fb_bitfield: offset, length, msb */
#define VAR_GREEN_OFF    44
#define VAR_BLUE_OFF     56

#define FIX_SIZE         80
#define FIX_LINE_LENGTH  48

/* 8192 pixels across at 32 bits is the widest row we take. */
#define MAX_ROW_BYTES    32768

/* The VT's mode. In KD_GRAPHICS the console stops drawing text and its
 * cursor on the framebuffer, which is what keeps boot messages off the
 * logo. */
#define KDSETMODE        0x4B3A
#define KD_TEXT          0
#define KD_GRAPHICS      1
#define SPL_O_NOCTTY     0400

#define SPL_CLOCK_MONOTONIC 1

typedef struct {
    u32 xres, yres, xoff, yoff, bpp, line_length;
    u32 off[3], len[3];             /* red, green, blue */
} fb_t;

/* The variable screen info as the kernel last described it: FBIOPAN_DISPLAY
 * takes it back (see show_fb). */
static u8 fb_var[VAR_SIZE];

static bool fb_query(int fd, fb_t *fb)
{
    u8 *var = fb_var, fix[FIX_SIZE];
    memset(var, 0, VAR_SIZE);
    memset(fix, 0, sizeof(fix));

    if (lp_ioctl(fd, FBIOGET_VSCREENINFO, var) < 0) return false;
    if (lp_ioctl(fd, FBIOGET_FSCREENINFO, fix) < 0) return false;

    fb->xres        = *(u32 *)(var + VAR_XRES);
    fb->yres        = *(u32 *)(var + VAR_YRES);
    fb->xoff        = *(u32 *)(var + VAR_XOFFSET);
    fb->yoff        = *(u32 *)(var + VAR_YOFFSET);
    fb->bpp         = *(u32 *)(var + VAR_BPP);
    fb->off[0]      = *(u32 *)(var + VAR_RED_OFF);
    fb->len[0]      = *(u32 *)(var + VAR_RED_OFF + 4);
    fb->off[1]      = *(u32 *)(var + VAR_GREEN_OFF);
    fb->len[1]      = *(u32 *)(var + VAR_GREEN_OFF + 4);
    fb->off[2]      = *(u32 *)(var + VAR_BLUE_OFF);
    fb->len[2]      = *(u32 *)(var + VAR_BLUE_OFF + 4);
    fb->line_length = *(u32 *)(fix + FIX_LINE_LENGTH);

    if (fb->xres == 0 || fb->yres == 0 || fb->line_length == 0)
        return false;
    if (fb->xres > SCENE_MAX_W || fb->line_length > MAX_ROW_BYTES)
        return false;               /* wider than we are prepared for */
    if (fb->bpp != 16 && fb->bpp != 24 && fb->bpp != 32)
        return false;               /* 8-bit palettes need a colour map */
    for (int c = 0; c < 3; c++)
        if (fb->len[c] == 0 || fb->len[c] > 16)
            return false;
    return true;
}

/* ── State ────────────────────────────────────────────────────────── */

static scene_t  scene;
static fb_t     fb;
static long     fbfd = -1;
static bool     fb_lost;            /* a write failed: the device went away */
static const char *fbdev = "/dev/fb0";
static u8       row[MAX_ROW_BYTES] __attribute__((aligned(8)));

/* The logo's colour at full strength, per pixel of its box, made once so
 * each frame of the fade is a blend rather than a distance field. 1M
 * pixels is a 4K logo box four times over; the pages are only touched
 * as far as the box needs. */
#define LOGO_MAX (1u << 20)
static u16      logo_full[LOGO_MAX][3];
static bool     logo_cached;

/* Set from signal handlers, read by the frame loop. */
static volatile int stop_req;       /* 0, STOP_DESKTOP or STOP_CONSOLE */
#define STOP_DESKTOP 1
#define STOP_CONSOLE 2

static void on_term(int sig) { (void)sig; if (!stop_req) stop_req = STOP_DESKTOP; }
static void on_usr1(int sig) { (void)sig; stop_req = STOP_CONSOLE; }

static bool trace;

/* ── The PC maker's logo (ACPI BGRT) ─────────────────────────────────
 *
 * A PC's firmware shows its maker's logo from the moment it is switched
 * on - Dell's, on the XPS - and tells the system where, in the BGRT
 * table: /sys/firmware/acpi/bgrt has the picture (a BMP), the place it
 * was drawn at, and whether it was drawn at all. When it was, the boot
 * screen keeps it, as Ubuntu's does: black, the maker's logo where the
 * firmware put it, our own logo small near the bottom and the spinner
 * between the two (scene_init_oem). The logo is drawn again by us rather
 * than left alone, because the screen we draw on may be a new one - the
 * graphics driver's - which starts out black.
 *
 * Its offsets are for the screen mode the firmware drew in. When the
 * screen now has another size (a virtual machine's card replacing the
 * firmware's), the logo goes where the firmware convention puts it:
 * centred across, its middle 38.2% of the way down. */
#define BGRT_DIR "/sys/firmware/acpi/bgrt/"
#define OEM_MAX_BYTES (8u << 20)
static u8 oem_bmp[OEM_MAX_BYTES];
static scene_oem_t oem;
static s32 oem_fx, oem_fy;          /* where the firmware drew it */
static bool oem_on;                 /* the maker's logo is part of this screen */

static long read_num(const char *path)
{
    char b[32];
    long n = proc_read(path, b, sizeof b - 1);
    if (n <= 0)
        return -1;
    b[n] = 0;
    return strtol(b, NULL, 10);
}

static void oem_load(void)
{
    oem.ok = false;
    long st = read_num(BGRT_DIR "status");
    if (st < 0 || !(st & 1))
        return;                     /* no table, or the firmware did not show it */
    long fd = lp_open(BGRT_DIR "image", O_RDONLY, 0);
    if (fd < 0)
        return;
    u32 n = 0;
    for (;;) {
        long got = lp_read((int)fd, oem_bmp + n, OEM_MAX_BYTES - n);
        if (got <= 0)
            break;
        n += (u32)got;
        if (n == OEM_MAX_BYTES)
            break;
    }
    lp_close((int)fd);
    if (!scene_oem_parse(&oem, oem_bmp, n))
        return;
    oem_fx = (s32)read_num(BGRT_DIR "xoffset");
    oem_fy = (s32)read_num(BGRT_DIR "yoffset");
}

/* Lay the scene out for this screen: under the maker's logo when there is
 * one that fits, else the full-screen splash. */
static void scene_for_screen(void)
{
    oem_on = scene_oem_place(&oem, fb.xres, fb.yres, oem_fx, oem_fy);
    if (oem_on)
        scene_init_oem(&scene, fb.xres, fb.yres, LP_OS_NAME, oem.y + (s32)oem.h);
    else
        scene_init(&scene, fb.xres, fb.yres, LP_OS_NAME);
}

/* ── Time ─────────────────────────────────────────────────────────── */

static s64 now_us(void)
{
    s64 ts[2] = { 0, 0 };           /* { tv_sec, tv_nsec }, 64-bit on all three */
    sys_call2(SYS_clock_gettime, SPL_CLOCK_MONOTONIC, (long)ts);
    return ts[0] * 1000000 + ts[1] / 1000;
}

static void sleep_us(s64 us)
{
    if (us <= 0)
        return;
    long ts[2] = { (long)(us / 1000000), (long)(us % 1000000) * 1000L };
    sys_call2(SYS_nanosleep, (long)ts, 0);   /* a signal ends it early: fine */
}

/* ── Springs, in fixed point ──────────────────────────────────────────
 * x'' = -w^2 (x - target) - 2 w x', critically damped, stepped once a
 * millisecond (w dt is at most 0.1, where this is indistinguishable from
 * the closed form). "At rest" is within 0.5% of the distance and slower
 * than 5% of it a second - lp-motion.c's rule - and logo.h's LP_W16_*
 * are the w that reach it in the published time, so a 260ms spring here
 * is a 260ms spring on the desktop. */
#define ONE (1 << 24)

typedef struct {
    s64 x, v, target;               /* 1/2^24; v per second */
    s64 w;                          /* rad/s in 1/16 */
} spring_t;

/* Head for target, keeping the current velocity: a spring turned round
 * half way leaves from where it is, at the speed it has. */
static void spring_go(spring_t *s, s64 target, s64 w16)
{
    s->target = target;
    s->w = w16;
}

static bool spring_rest(const spring_t *s)
{
    s64 d = s->x - s->target, v = s->v;
    return (d < 0 ? -d : d) < ONE / 200 && (v < 0 ? -v : v) < ONE / 20;
}

/* Advance by ms; true while it is still moving. */
static bool spring_step(spring_t *s, s64 ms)
{
    if (spring_rest(s)) {
        s->x = s->target;
        s->v = 0;
        return false;
    }
    for (s64 i = 0; i < ms; i++) {
        s64 a = -((s->w * s->w * (s->x - s->target)) >> 8) - ((2 * s->w * s->v) >> 4);
        s->v += a / 1000;
        s->x += s->v / 1000;
        if (spring_rest(s)) {
            s->x = s->target;
            s->v = 0;
            return false;
        }
    }
    return true;
}

static u32 alpha_of(const spring_t *s)
{
    s64 a = s->x >> 16;             /* 0..256 */
    return (u32)(a < 0 ? 0 : a > 256 ? 256 : a);
}

/* ── Drawing ──────────────────────────────────────────────────────── */

static void put_pixel(u32 x, const u32 c[3], u32 px, u32 py)
{
    u32 v = 0;
    for (int k = 0; k < 3; k++)
        v |= scene_quantise(c[k], fb.len[k], px, py, k) << fb.off[k];
    if (fb.bpp == 32)
        *(u32 *)(row + x * 4) = v;
    else if (fb.bpp == 24) {
        row[x * 3]     = (u8)v;
        row[x * 3 + 1] = (u8)(v >> 8);
        row[x * 3 + 2] = (u8)(v >> 16);
    } else
        *(u16 *)(row + x * 2) = (u16)v;
}

/* Put n pixels of `row` on the screen at (x0, y). */
static void write_span(u32 y, u32 x0, u32 n)
{
    if (fb_lost)
        return;
    u32 bpp = fb.bpp / 8;
    s64 off = (s64)(fb.yoff + y) * fb.line_length + (s64)(fb.xoff + x0) * bpp;
    if (lp_lseek((int)fbfd, off, SEEK_SET) != off ||
        lp_write((int)fbfd, row, n * bpp) != (long)(n * bpp))
        fb_lost = true;             /* the device went away (see reopen) */
}

/* The whole screen: the gradient, and the logo at `logo` (0..256). */
static void draw_full(u32 logo)
{
    for (u32 y = 0; y < fb.yres && !fb_lost; y++) {
        bool in_box = (s32)y >= scene.by0 && (s32)y <= scene.by1;
        bool in_oem = oem_on && (s32)y >= oem.y && (s32)y < oem.y + (s32)oem.h;
        u32 rsq = scene_rowsq(&scene, y);
        for (u32 x = 0; x < fb.xres; x++) {
            u32 c[3];
            if (!(in_oem && scene_oem_pixel(&oem, x, y, c)))
                scene_bg_row(&scene, x, rsq, c);
            if (logo && in_box && (s32)x >= scene.bx0 && (s32)x <= scene.bx1) {
                u32 bg[3] = { c[0], c[1], c[2] }, fg[3] = { c[0], c[1], c[2] };
                scene_logo(&scene, x, y, fg);
                scene_mix(c, bg, fg, logo);
            }
            put_pixel(x, c, x, y);
        }
        write_span(y, 0, fb.xres);
    }
}

static void cache_logo(void)
{
    u32 w = (u32)(scene.bx1 - scene.bx0 + 1), h = (u32)(scene.by1 - scene.by0 + 1);
    logo_cached = scene.bx1 >= scene.bx0 && scene.by1 >= scene.by0 && w * h <= LOGO_MAX;
    if (!logo_cached)
        return;
    u32 i = 0;
    for (u32 y = (u32)scene.by0; y <= (u32)scene.by1; y++)
        for (u32 x = (u32)scene.bx0; x <= (u32)scene.bx1; x++, i++) {
            u32 c[3];
            scene_bg(&scene, x, y, c);
            scene_logo(&scene, x, y, c);
            logo_full[i][0] = (u16)c[0];
            logo_full[i][1] = (u16)c[1];
            logo_full[i][2] = (u16)c[2];
        }
}

/* Only the logo's box, at `alpha`. */
static void draw_logo(u32 alpha)
{
    if (!logo_cached) {             /* a box too big to keep: skip the fade */
        if (alpha == 256)
            draw_full(256);
        return;
    }
    u32 w = (u32)(scene.bx1 - scene.bx0 + 1), i = 0;
    for (u32 y = (u32)scene.by0; y <= (u32)scene.by1; y++) {
        u32 rsq = scene_rowsq(&scene, y);
        for (u32 x = 0; x < w; x++, i++) {
            u32 bg[3], fg[3] = { logo_full[i][0], logo_full[i][1], logo_full[i][2] }, c[3];
            scene_bg_row(&scene, x + (u32)scene.bx0, rsq, bg);
            scene_mix(c, bg, fg, alpha);
            put_pixel(x, c, x + (u32)scene.bx0, y);
        }
        write_span(y, (u32)scene.bx0, w);
    }
}

/* Only the spinner's box: the stroke pointing at `head`, at `alpha`
 * overall (0 draws the background back). */
static void draw_spinner(u32 alpha, u32 head, bool ring)
{
    static const u8 rgb[3] = LP_RGB_WORD;
    u32 ink[3] = { (u32)rgb[0] << 8, (u32)rgb[1] << 8, (u32)rgb[2] << 8 };
    s32 hx, hy;
    scene_spin_head(&scene, head, &hx, &hy);
    u32 w = (u32)(scene.sx1 - scene.sx0 + 1);
    for (u32 y = (u32)scene.sy0; y <= (u32)scene.sy1; y++) {
        u32 rsq = scene_rowsq(&scene, y);
        for (u32 x = 0; x < w; x++) {
            u32 px = x + (u32)scene.sx0, bg[3], c[3];
            scene_bg_row(&scene, px, rsq, bg);
            scene_logo(&scene, px, y, bg);  /* nothing, unless a tiny screen overlaps them */
            u32 a = alpha ? scene_spin(&scene, px, y, head, hx, hy, ring) * alpha / 256 : 0;
            scene_mix(c, bg, ink, a);
            put_pixel(x, c, px, y);
        }
        write_span(y, (u32)scene.sx0, w);
    }
}

/* ── The screen ───────────────────────────────────────────────────── */

/* Wait for the framebuffer, and describe it.
 *
 * The graphics hardware is found by probing a bus, and that finishes
 * some time after init starts - so at the moment we are run there is
 * often no /dev/fb0 yet, and being early is not a reason to give up.
 * On a PC it can also be replaced under us: the firmware's framebuffer
 * (simpledrm or efifb) is removed when the real graphics driver loads,
 * and a new /dev/fb0 appears a moment later. Either way we wait, up to
 * `wait_ms`, and stop waiting if we are told to stop. */
static bool open_fb(int wait_ms)
{
    for (int waited = 0; waited <= wait_ms && !stop_req; waited += 50) {
        fbfd = lp_open(fbdev, O_WRONLY, 0);
        if (fbfd >= 0) {
            if (fb_query((int)fbfd, &fb)) {
                fb_lost = false;
                scene_for_screen();
                return true;
            }
            lp_close((int)fbfd);
            fbfd = -1;
        }
        lp_sleep_ms(50);
    }
    return false;
}

/* Put what we drew on the screen.
 *
 * On a DRM driver's framebuffer emulation - simpledrm early on, i915 or
 * amdgpu or virtio-gpu after - /dev/fb0 is a buffer of its own, and the
 * screen goes on showing whatever the firmware left until something
 * makes that buffer the one on the screen. The console does that when
 * it takes over; with the console deferred and quiet, nothing did, and
 * the whole boot showed the firmware's logo and nothing of ours. A pan
 * to where the buffer already is commits it: the next frame on the
 * screen is ours. On efifb the buffer is the screen, and the pan is
 * refused, harmlessly. */
static void show_fb(void)
{
    *(u32 *)(fb_var + VAR_XOFFSET) = fb.xoff;
    *(u32 *)(fb_var + VAR_YOFFSET) = fb.yoff;
    long r = lp_ioctl((int)fbfd, FBIOPAN_DISPLAY, fb_var);
    if (trace)
        dprintf(2, "splash: pan %ld\n", r);
}

static long vtfd = -1;

static void vt_mode(int mode)
{
    if (vtfd < 0)
        vtfd = lp_open("/dev/tty0", O_RDWR | SPL_O_NOCTTY, 0);
    if (vtfd >= 0)
        lp_ioctl((int)vtfd, KDSETMODE, (void *)(long)mode);
}

/* `splash` as a word of the kernel command line. */
static bool cmdline_says_hold(void)
{
    char buf[1024];
    long n = proc_read("/proc/cmdline", buf, sizeof buf - 1);
    if (n <= 0)
        return false;
    buf[n] = 0;
    for (char *p = buf; *p; ) {
        while (*p == ' ' || *p == '\n')
            p++;
        char *w = p;
        while (*p && *p != ' ' && *p != '\n')
            p++;
        if (p - w == 6 && memcmp(w, "splash", 6) == 0)
            return true;
    }
    return false;
}

static bool motion_reduced(void)
{
    if (lp_exists("/etc/lp/reduce-motion"))
        return true;
    lp_user_t u;
    char path[128];
    if (lp_user_by_uid(1000, &u)) {
        snprintf(path, sizeof path, "%s/.config/lp/reduce-motion", u.home);
        if (lp_exists(path))
            return true;
    }
    return false;
}

/* ── splash stop ──────────────────────────────────────────────────────
 * Find the running splash by its name in /proc - no pid file, so there
 * is nothing to go stale if the machine lost power mid-boot - signal
 * it, and wait until it has gone, so the caller knows nothing will be
 * written to the screen after this returns. "Gone" includes a zombie:
 * init reaps its children on its own schedule, and the splash has
 * finished drawing the moment it exits. */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

static bool comm_of(long pid, char *out, size_t cap)
{
    char path[48];
    snprintf(path, sizeof path, "/proc/%ld/comm", pid);
    long n = proc_read(path, out, cap - 1);
    if (n <= 0)
        return false;
    out[n] = 0;
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
    return true;
}

static bool still_running(long pid)
{
    char path[48], buf[256];
    snprintf(path, sizeof path, "/proc/%ld/stat", pid);
    long n = proc_read(path, buf, sizeof buf - 1);
    if (n <= 0)
        return false;
    buf[n] = 0;
    char *p = strrchr(buf, ')');
    return p && p[1] == ' ' && p[2] != 'Z' && p[2] != 'X';
}

static int stop_running(bool console)
{
    char me[64];
    if (!comm_of(lp_getpid(), me, sizeof me))
        strlcpy(me, "splash", sizeof me);

    long pids[16];
    int  n = 0;
    long fd = lp_open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 0;
    char buf[4096];
    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0)
            break;
        for (long off = 0; off < got && n < 16; ) {
            char *rec = buf + off;
            u16 len = *(u16 *)(rec + DIRENT_RECLEN);
            char *name = rec + DIRENT_NAME;
            if (len == 0)
                break;
            off += len;
            if (name[0] < '1' || name[0] > '9')
                continue;
            long pid = strtol(name, NULL, 10);
            char comm[64];
            if (pid != lp_getpid() && comm_of(pid, comm, sizeof comm) &&
                strcmp(comm, me) == 0 && still_running(pid))
                pids[n++] = pid;
        }
    }
    lp_close((int)fd);

    for (int i = 0; i < n; i++)
        lp_kill((pid_t)pids[i], console ? SIGUSR1 : SIGTERM);
    /* The spinner's exit is 182ms; the logo, if it was still arriving,
     * up to 400. Two seconds is a ceiling for a machine under load. */
    for (int waited = 0; waited < 2000; waited += 10) {
        bool any = false;
        for (int i = 0; i < n; i++)
            any |= still_running(pids[i]);
        if (!any)
            return 0;
        lp_sleep_ms(10);
    }
    return 1;
}

static void usage(void)
{
    printf("usage: splash [--hold] [device]   draw the boot screen (default /dev/fb0)\n");
    printf("       splash stop [--console]   end it: the desktop, or the console, takes over\n");
    printf("  --reduced  as if motion were reduced   --trace  a line per frame on stderr\n\n");
    printf("It holds the screen, with a spinner, until `splash stop` when the kernel\n");
    printf("command line has the word `splash` or --hold is given; otherwise it fades\n");
    printf("the logo in and exits.\n");
}

int main(int argc, char **argv)
{
    bool hold = false, hold_flag = false, force_reduced = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(argv[i], "stop") == 0) {
            bool console = i + 1 < argc && strcmp(argv[i + 1], "--console") == 0;
            return stop_running(console);
        } else if (strcmp(argv[i], "--hold") == 0)
            hold_flag = true;
        else if (strcmp(argv[i], "--trace") == 0)
            trace = true;
        else if (strcmp(argv[i], "--reduced") == 0)
            force_reduced = true;
        else
            fbdev = argv[i];
    }
    /* The desktop image holds without being told: its start script
     * says `splash stop` just before the compositor takes the screen
     * (desktop/session/start-desktop, desktop/installer/lp-setup-gate). */
    hold = hold_flag || cmdline_says_hold() ||
           lp_exists("/usr/lib/lp/start-desktop.session");
    oem_load();

    lp_signal_handler(SIGTERM, on_term);
    lp_signal_handler(SIGUSR1, on_usr1);

    /* Eight seconds covers a slow probe on a cold board without holding
     * anything up: init starts this and carries straight on, and a
     * machine with no screen simply has a process asleep for a moment. */
    if (!open_fb(8000))
        return 1;                   /* no screen attached - not an error */

    if (hold)
        vt_mode(KD_GRAPHICS);

    bool reduced = force_reduced || motion_reduced();
    s64 logo_w = reduced ? LP_W16_REDUCED : LP_W16_LOGO_IN;
    s64 in_w   = reduced ? LP_W16_REDUCED : LP_W16_SPIN_IN;
    s64 out_w  = reduced ? LP_W16_REDUCED : LP_W16_SPIN_OUT;

    s64 t_draw = now_us();
    draw_full(0);
    show_fb();
    cache_logo();
    if (trace)
        dprintf(2, "splash: %ux%u@%u, first frame %ld us, logo box %dx%d, spinner box %dx%d\n",
                fb.xres, fb.yres, fb.bpp, (long)(now_us() - t_draw),
                scene.bx1 - scene.bx0 + 1, scene.by1 - scene.by0 + 1,
                scene.sx1 - scene.sx0 + 1, scene.sy1 - scene.sy0 + 1);

    spring_t logo = { 0, 0, 0, 0 }, spin = { 0, 0, 0, 0 };
    spring_go(&logo, ONE, logo_w);

    enum { LOGO_IN, HOLDING, LEAVING } phase = LOGO_IN;
    s64 start = now_us(), last = start, logo_done = 0;
    s64 next = start, carry = 0;
    const s64 period = 16667;       /* 60 frames a second */
    bool spin_shown = false;
    int  frames = 0;
    s64  max_gap = 0, max_draw = 0, prev_frame = 0;

    for (;;) {
        s64 now = now_us();
        s64 ms = (now - last + carry) / 1000;
        carry = (now - last + carry) % 1000;
        last = now;

        if (fb_lost) {
            /* The graphics driver replaced the firmware's framebuffer.
             * Draw the whole picture again on the new one, logo and all -
             * there is nothing to fade in on a screen that was just lit. */
            lp_close((int)fbfd);
            if (!open_fb(3000))
                break;
            draw_full(256);
            show_fb();
            cache_logo();
            logo.x = logo.target = ONE;
            logo.v = 0;
            if (hold)
                vt_mode(KD_GRAPHICS);
        }

        bool moving = false;
        if (phase == LOGO_IN) {
            moving = spring_step(&logo, ms);
            draw_logo(moving ? alpha_of(&logo) : 256);
            if (!moving) {
                logo_done = now;
                phase = hold ? HOLDING : LEAVING;
            }
        } else {
            if (phase == HOLDING && (stop_req || now - start > (s64)LP_MOTION_HOLD_MS * 1000)) {
                if (!stop_req)
                    stop_req = STOP_CONSOLE;    /* nobody came: give the console back */
                phase = LEAVING;
                spring_go(&spin, 0, out_w);
            }
            if (phase == HOLDING && !spin_shown &&
                now - logo_done >= (s64)LP_MOTION_SPIN_DELAY_MS * 1000) {
                spin_shown = true;
                spring_go(&spin, ONE, in_w);
            }
            moving = spring_step(&spin, ms);
            if (spin_shown) {
                s64 t = now - logo_done;
                u32 head = (u32)((t / 1000) % LP_MOTION_SPIN_TURN_MS * 65536 / LP_MOTION_SPIN_TURN_MS);
                u32 a = alpha_of(&spin);
                if (reduced) {
                    /* breathe: 35% to 100% and back over PULSE_MS */
                    u32 p = (u32)((t / 1000) % LP_MOTION_PULSE_MS);
                    u32 half = LP_MOTION_PULSE_MS / 2;
                    u32 u = p < half ? p * 64 / half : (LP_MOTION_PULSE_MS - p) * 64 / half;
                    u32 wave = LP_WAVE[u > 64 ? 64 : u];
                    a = a * (90 + (u32)((166 * (u64)wave) >> 16)) / 256;
                }
                draw_spinner(a, head, reduced);
            }
            if (phase == LEAVING && !moving)
                break;
        }

        if (trace) {
            /* one line a frame: when it started (ms since the first
             * frame), the gap since the one before, and how long the
             * drawing took - the numbers the frame-gap reports quote */
            s64 end = now_us();
            dprintf(2, "splash: frame t_ms=%ld dt_us=%ld draw_us=%ld\n", (long)((now - start) / 1000),
                    prev_frame ? (long)(now - prev_frame) : 0L, (long)(end - now));
            if (prev_frame && now - prev_frame > max_gap)
                max_gap = now - prev_frame;
            if (end - now > max_draw)
                max_draw = end - now;
            prev_frame = now;
            frames++;
        }

        /* Nothing to draw until the spinner is due: sleep until then
         * (a signal cuts the sleep short). Otherwise, the next frame. */
        if (phase == HOLDING && !spin_shown) {
            s64 due = logo_done + (s64)LP_MOTION_SPIN_DELAY_MS * 1000 - now_us();
            sleep_us(due > 100000 ? 100000 : due);
            next = now_us();
            prev_frame = 0;
            continue;
        }
        next += period;
        s64 slack = next - now_us();
        if (slack < -period)
            next = now_us();        /* fell behind: do not try to catch up */
        else
            sleep_us(slack);
    }

    if (trace)
        dprintf(2, "splash: %d frames, max frame gap %ld us, max draw %ld us, stop=%d\n",
                frames, (long)max_gap, (long)max_draw, stop_req);

    if (fbfd >= 0)
        lp_close((int)fbfd);
    if (hold && stop_req == STOP_CONSOLE)
        vt_mode(KD_TEXT);
    return 0;
}
