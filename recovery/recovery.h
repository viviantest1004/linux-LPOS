/* recovery.h - what the parts of lp-recovery share.
 *
 *   screen.c     the framebuffer, the VT, and evdev input (keys, touch,
 *                tablet, mouse)
 *   osk.c        the on-screen keyboard: everything in recovery works
 *                with a finger alone
 *   term.c       the terminal the recovery shell runs in: a pty and a
 *                small VT100/xterm emulator drawn with our own font
 *   system.c     partitions by GPT name, mounts, child processes, the
 *                log, battery and time for the status line
 *   auth.c       the administrator check that stands in front of the
 *                recovery shell
 *   reinstall.c  putting the payload in /reinstall back on LP-ROOT
 *   lp-recovery.c  the screens, and main
 *
 * See lp-recovery.c for what the program is and why it is shaped this
 * way.
 */
#ifndef LP_RECOVERY_H
#define LP_RECOVERY_H

#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"
#define LPUI_SHARED_STATE            /* see lp-ui.h */
#include "ui/lp-ui.h"

/* ── Paths on the recovery partition ─────────────────────────────── */
#define MNT_ROOT      "/mnt/lp"             /* LP-ROOT, the installed system */
#define MNT_ESP       "/mnt/esp"            /* LP-ESP, while boot files are written */
#define PAYLOAD_DIR   "/reinstall"
#define STATE_DIR     "/var/lib/lp-recovery"
#define LOG_FILE      "/var/log/lp-recovery.log"
#define RECOVERY_PW   "/etc/lp/recovery-password"

#define O_NOCTTY_     0400

typedef struct { int x, y, w, h; } rect_t;

/* ── screen.c ─────────────────────────────────────────────────────── */
extern int SW, SH;                          /* screen size in pixels */
extern lpui_canvas_t cv_bg, cv_base, cv_frame;
extern u8 *logo_scratch;

bool scr_open(void);
void scr_present(const lpui_canvas_t *c, int x, int y, int w, int h);
/* Write the last drawn rectangle once more when drawing has paused;
 * ms until that is due, or -1 when there is nothing to settle. */
int  scr_settle(void);
void scr_graphics(bool on);                 /* KD_GRAPHICS + keyboard grab */

/* UEV_FD: the extra descriptor (in_watch_fd) is readable. */
enum { UEV_NONE, UEV_KEY, UEV_PTR, UEV_FD };
typedef struct {
    int  kind;
    int  code;          /* Linux key code (UEV_KEY) */
    u32  ch;            /* the character it types, US layout, or 0 */
    bool press;         /* key down or auto-repeat */
    bool shift, ctrl, alt;  /* UEV_KEY: modifiers held */
    int  x, y;          /* UEV_PTR: screen pixels */
    bool down;          /* UEV_PTR: finger / button is down */
    bool mouse;         /* UEV_PTR from a relative mouse: draw a cursor */
    int  slot;          /* UEV_PTR: which finger (0 for mice and tablets) */
} uev_t;

/* Wait up to timeout_ms for one event. 1 = got one, 0 = timed out. */
int  in_wait(uev_t *ev, int timeout_ms);
/* One more descriptor for in_wait to watch (the terminal's pty), or -1. */
void in_watch_fd(int fd);
void in_rescan(void);
void in_drain(void);

/* Linux key codes we act on */
#define K_ESC 1
#define K_BACKSPACE 14
#define K_TAB 15
#define K_ENTER 28
#define K_SPACE 57
#define K_KPENTER 96
#define K_HOME 102
#define K_UP 103
#define K_PGUP 104
#define K_LEFT 105
#define K_RIGHT 106
#define K_END 107
#define K_DOWN 108
#define K_PGDN 109
#define K_INSERT 110
#define K_DELETE 111
#define K_F1 59
#define K_F11 87
#define K_F12 88

/* ── system.c ─────────────────────────────────────────────────────── */
typedef struct {
    bool found;
    int  num;                   /* GPT partition number */
    char dev[48];               /* /dev/nvme0n1p3 */
    char partuuid[40];
} part_t;
extern char disk_dev[48];       /* the disk LP-RECOVERY is on */
extern part_t p_esp, p_rec, p_root;

void sys_init(void);
void sys_find_partitions(void);
/* Mount LP-ROOT at MNT_ROOT (read-only unless rw). true on success. */
bool sys_root_mount(bool rw);
bool sys_root_remount(bool rw);
void sys_root_umount(void);
bool sys_root_mounted(void);
/* ext4 superblock state of LP-ROOT: 0 clean, 1 needs a check, -1 unknown */
int  sys_root_state(void);

/* One line to the log (and the serial port). printf-style; our libc has
 * snprintf but no vsnprintf, hence a macro. */
void rlog_line(const char *msg);
#define rlog(...) do { char _rl[400]; snprintf(_rl, sizeof _rl, __VA_ARGS__); \
                       rlog_line(_rl); } while (0)

/* Run argv, stdin from /dev/null. Every line of its output (stdout and
 * stderr) goes to on_line when that is given, and to the log. Returns the
 * exit status, or -1 when it could not be started. */
int  run(char *const argv[], void (*on_line)(const char *, void *), void *ctx);
/* The same, polled: tick() is called about every 100 ms while it runs so
 * the screen can keep moving. */
int  run_ticking(char *const argv[], void (*on_line)(const char *, void *),
                 void *ctx, void (*tick)(void *), void *tctx);

bool file_read(const char *path, char *buf, size_t cap);
bool file_write_atomic(const char *path, const char *data, size_t n, mode_t mode);
bool file_copy(const char *from, const char *to, mode_t mode);
/* rm -r that never leaves the filesystem it started on. rm_hook, when
 * set, is called every 256 entries removed, for a progress bar. */
long rm_tree(const char *path, u64 *count);
extern void (*rm_hook)(u64, void *);
void mkdirs(const char *path, mode_t mode);

int  battery_percent(bool *charging);   /* -1 when there is no battery */
void clock_text(char *out, size_t n);   /* "Sat 23:47" */
bool motion_reduced(void);

/* LP-RECOVERY is remounted read-only to check it; the log's open file
 * would stop that, so it is closed around the check. */
void sys_log_pause(bool pause);

/* ── auth.c ───────────────────────────────────────────────────────── */
#define AUTH_MAX_USERS 6
typedef struct {
    int  mode;                          /* AUTH_ADMINS, AUTH_RECOVERY, AUTH_NOBODY */
    int  nusers;
    char users[AUTH_MAX_USERS][32];
} auth_info_t;
enum { AUTH_ADMINS, AUTH_RECOVERY, AUTH_NOBODY };

void auth_prepare(auth_info_t *ai);
/* 0 ok; 1 wrong; 2 locked out (wait *wait_s); 3 unsupported hash (*why);
 * 4 no usable password on that account. */
int  auth_check(const auth_info_t *ai, int user, const char *pw, int *wait_s,
                const char **why);
int  auth_lockout_left(void);           /* seconds still to wait, or 0 */
int  auth_tries_left(void);

/* ── reinstall.c ──────────────────────────────────────────────────── */
enum { RE_KEEP, RE_ERASE };
typedef struct {
    int  step;                          /* LPS_ id of what is happening */
    int  permille;                      /* overall progress */
} re_progress_t;
bool re_payload_present(char *version, size_t n);
/* Returns NULL on success, or a short English reason. `show` is called
 * whenever step or progress changes. */
const char *re_run(int mode, void (*show)(const re_progress_t *, void *), void *ctx,
                   bool *damaged);
/* Is there an unfinished reinstall on this partition? */
bool re_interrupted(void);

/* ── osk.c ────────────────────────────────────────────────────────── */
enum {
    OSK_TEXT, OSK_BKSP, OSK_ENTER, OSK_TAB, OSK_ESC, OSK_SPACE,
    OSK_UP, OSK_DOWN, OSK_LEFT, OSK_RIGHT, OSK_HOME, OSK_END,
    OSK_PGUP, OSK_PGDN, OSK_DEL, OSK_FN,
};
typedef struct {
    int  what;          /* OSK_ */
    char text[8];       /* OSK_TEXT: the character, UTF-8 */
    int  fn;            /* OSK_FN: 1..12 */
    bool shift, ctrl, alt;
} osk_key_t;

void osk_init(void);                    /* after scr_open and lpui_set_screen */
extern bool osk_reduce_motion;
/* Show or hide. `animate` false jumps (a new screen, reduced motion). */
void osk_show(bool on, bool animate);
bool osk_shown(void);                   /* where it is going */
int  osk_height(void);                  /* the whole panel, pixels */
int  osk_top(void);                     /* its top edge on the screen now */
int  osk_rest_top(void);                /* its top edge once it has arrived */
/* Pointer events. true when the keyboard took it; typed keys come out
 * through the callback. */
bool osk_pointer(const uev_t *e);
extern void (*osk_emit)(const osk_key_t *k);
extern void (*osk_closed)(void);        /* the Close key was used */
/* Timers: auto-repeat and the slide. Returns how long the caller may
 * sleep, in ms (-1: nothing pending). */
int  osk_tick(void);
/* The keyboard changed on the screen since the last call (rect set). */
bool osk_dirty(rect_t *r);
/* Draw the keyboard as it is now into `c`, a window whose top-left is
 * screen position (dx, dy). */
void osk_compose(lpui_canvas_t *c, int dx, int dy);

/* ── term.c ───────────────────────────────────────────────────────── */
/* Start /bin/lpsh as root on a new pty, the terminal filling `area`.
 * NULL, or a short English reason. */
const char *term_start(rect_t area, const char *hello);
void term_stop(void);                   /* hang up and reap */
bool term_running(void);
int  term_fd(void);
void term_set_area(rect_t area);        /* the keyboard came or went */
rect_t term_area(void);
/* Read what the shell wrote. false once the shell has exited. */
bool term_pump(void);
void term_key(const uev_t *e);          /* a physical key */
void term_osk(const osk_key_t *k);      /* a key of the on-screen keyboard */
/* Touch drag: scroll the scrollback. */
void term_drag(int dy_px, bool start);
/* Rows that changed since the last draw, in screen pixels. */
bool term_dirty(rect_t *r);
/* Draw into `c` (the base canvas): all of it, or only what changed. */
void term_draw(lpui_canvas_t *c, bool all);

#endif
