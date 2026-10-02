/* dlsym(RTLD_DEFAULT) and g_spawn need declarations -std=c11 hides. */
#define _GNU_SOURCE 1

/*
 * video.c - 동영상 (LP Video): a player with a few editing tools.
 *
 * ── Why libmpv, and why at run time ──
 *
 * GTK's own GtkVideo needs the gstreamer media backend, which the image
 * does not ship (and on a Pi Zero 2 W it would be the slower choice).
 * mpv is already there for the command line, and libmpv.so.2 comes with
 * it: the best decoder the machine has, hardware decoding where the
 * driver offers it, and a renderer that draws into our own widget.
 *
 * The headers are not in the image, so mpv-min.h declares what is used
 * and the library is opened with dlopen(). That also means a machine
 * without libmpv still runs this program: it says so plainly, and the
 * editing tools - which are ffmpeg, not mpv - keep working.
 *
 * ── Two ways to draw a frame ──
 *
 * The normal path is mpv's OpenGL renderer drawing straight into a
 * GtkGLArea's framebuffer: no copies, the GPU scales. Some sessions have
 * no GL at all (a compositor running on the CPU, a broken driver); there
 * GtkGLArea fails to realize, and mpv's software renderer draws into a
 * memory buffer that becomes a GdkMemoryTexture in a GtkPicture. It is
 * slower, so it renders no bigger than the video or the window, and no
 * more often than 60 times a second.
 *
 * ── Threads ──
 *
 * mpv calls back from its own threads. Both callbacks (new events, new
 * frame) do nothing but schedule an idle on the GTK main loop; every
 * mpv_* call that touches the UI happens there. A flag per callback
 * keeps a burst of wakeups from queueing a burst of idles.
 */

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <dlfcn.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lp-i18n.h"
#include "lp-fit.h"
#include "mpv-min.h"
#include "edit.h"

/* ── libmpv, loaded at run time ────────────────────────────────── */

#define MPV_SYMS(X) \
    X(client_api_version) X(error_string) X(free) X(create) \
    X(initialize) X(terminate_destroy) X(set_option_string) X(command) \
    X(command_async) X(set_property) X(set_property_string) \
    X(get_property) X(observe_property) X(wait_event) \
    X(set_wakeup_callback) X(render_context_create) \
    X(render_context_set_update_callback) X(render_context_update) \
    X(render_context_render) X(render_context_report_swap) \
    X(render_context_free)

static struct {
    void *lib;
#define X(n) mpv_##n##_fn n;
    MPV_SYMS(X)
#undef X
} M;

static gboolean mpv_load(char **why)
{
    M.lib = dlopen("libmpv.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!M.lib) {
        *why = g_strdup(dlerror());
        return FALSE;
    }
#define X(n) \
    M.n = (mpv_##n##_fn)dlsym(M.lib, "mpv_" #n); \
    if (!M.n) { *why = g_strdup("mpv_" #n " missing"); return FALSE; }
    MPV_SYMS(X)
#undef X
    /* The struct layouts in mpv-min.h are those of API 2.x. A future
     * 3.x may move them; refuse it rather than crash in it. */
    unsigned long v = M.client_api_version();
    if ((v >> 16) != 2) {
        *why = g_strdup_printf("client API %lu.%lu, expected 2.x",
                               v >> 16, v & 0xffff);
        return FALSE;
    }
    return TRUE;
}

/* ── state ─────────────────────────────────────────────────────── */

enum { P_TIME = 1, P_DURATION, P_PAUSE, P_VOLUME, P_MUTE, P_EOF,
       P_SPEED, P_DWIDTH, P_DHEIGHT };

typedef void (*gl_get_integerv_fn)(unsigned int, int *);
typedef void (*gl_clear_color_fn)(float, float, float, float);
typedef void (*gl_clear_fn)(unsigned int);
typedef void *(*egl_gpa_fn)(const char *);

typedef struct {
    GtkApplication *gapp;
    GtkWidget *win, *header, *title, *stack, *overlay;
    GtkWidget *video;          /* GtkGLArea, or GtkPicture when sw */
    gboolean   sw;
    GtkWidget *ph, *ph_icon, *ph_title, *ph_sub;   /* over the video */
    GtkWidget *controls_rev, *controls;
    GtkWidget *play_btn, *seek, *pos_lbl, *dur_lbl;
    GtkWidget *mute_btn, *vol, *speed_btn, *loop_btn, *fs_btn;
    GtkWidget *edit_btn, *edit_rev, *timeline;
    GtkWidget *start_lbl, *end_lbl, *len_lbl, *saveas;
    GtkWidget *ops_box;
    GtkWidget *op_widget[OP_COUNT];
    GPtrArray *op_menus;       /* menu buttons, sensitive if any item is */
    GtkWidget *status, *busy_lbl, *progress, *done_lbl, *err_lbl, *idle_lbl;
    GSimpleAction *speed_action;

    /* player */
    mpv_handle         *mpv;
    mpv_render_context *rctx;
    gboolean            rctx_gl;
    char               *mpv_error;
    gint                events_queued, render_queued;
    char               *pending;   /* file waiting for the renderer */
    gboolean            ready;     /* renderer exists, files can load */
    gboolean            load_failed;
    gboolean            render_failed;

    char        *file;
    media_info_t mi;
    gboolean     probed;
    double       pos, dur, volume, speed;
    gboolean     paused, muted, eof;
    gint64       dw, dh;
    gboolean     seeking;          /* a seek of ours is in flight */
    gint64       seek_hold;        /* ... but trust mpv again after this */
    gboolean     updating;         /* setting widgets from mpv state */

    double       sel_a, sel_b;
    gboolean     play_sel;
    int          drag_mode;        /* 0 seek, 1 start, 2 end */
    double       drag_x0;

    guint        hide_id;
    double       mx, my;
    gboolean     over_controls;

    /* the running ffmpeg */
    GSubprocess      *job;
    GDataInputStream *job_in;
    char             *job_out, *job_err;
    double            job_len;
    gboolean          job_cancelled;
    char             *result;

    /* software rendering */
    gint64 sw_last;
    guint  sw_timer;

    /* GL */
    void              *egl;
    egl_gpa_fn         egl_gpa;
    gl_get_integerv_fn gl_get_integerv;
    gl_clear_color_fn  gl_clear_color;
    gl_clear_fn        gl_clear;
} app_t;

static app_t A;

static void update_ops(void);
static void sync_time(void);
static void update_placeholder(void);
static void show_status(const char *page);
static void player_open(const char *path);
static void set_video_widget_sw(void);

/* ── small helpers ─────────────────────────────────────────────── */

static char *fmt_time(double t, gboolean frac)
{
    if (!(t >= 0) || isinf(t)) t = 0;
    long total = (long)t;
    int h = total / 3600, m = (total / 60) % 60, s = total % 60;
    int cs = (int)((t - total) * 100);
    if (h)
        return frac ? g_strdup_printf("%d:%02d:%02d.%02d", h, m, s, cs)
                    : g_strdup_printf("%d:%02d:%02d", h, m, s);
    return frac ? g_strdup_printf("%d:%02d.%02d", m, s, cs)
                : g_strdup_printf("%d:%02d", m, s);
}

static void cmd(const char *a0, const char *a1, const char *a2)
{
    if (!A.mpv) return;
    const char *args[] = { a0, a1, a2, NULL };
    /* Async: a seek into a large file must not freeze the window. */
    M.command_async(A.mpv, 0, args);
}

static void set_flag(const char *name, gboolean v)
{
    if (!A.mpv) return;
    int f = v ? 1 : 0;
    M.set_property(A.mpv, name, MPV_FORMAT_FLAG, &f);
}

static void set_double(const char *name, double v)
{
    if (!A.mpv) return;
    M.set_property(A.mpv, name, MPV_FORMAT_DOUBLE, &v);
}

static void seek_abs(double t)
{
    if (A.dur > 0) t = CLAMP(t, 0, A.dur);
    else if (t < 0) t = 0;
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buf, sizeof buf, "%.3f", t);
    /* Show the new place at once, before mpv confirms it. Until the
     * seek is done (PLAYBACK_RESTART) mpv keeps reporting the old
     * position, and the slider would jump back and forth under the
     * pointer that is dragging it. The deadline is a safety net for a
     * seek that never completes. */
    A.pos = t;
    if (A.mpv && A.file) {
        A.seeking = TRUE;
        A.seek_hold = g_get_monotonic_time() + 1500000;
        cmd("seek", buf, "absolute+exact");
    }
    if (A.timeline) sync_time();
}

static void seek_rel(double d)
{
    seek_abs(A.pos + d);
}

static GtkWidget *icon_button(const char *icon, const char *tip,
                              GCallback cb)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_add_css_class(b, "flat");
    if (cb) g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static GtkWidget *label_icon_box(const char *icon, const char *label)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(icon));
    gtk_box_append(GTK_BOX(box), gtk_label_new(label));
    return box;
}

/* ── controls bar ──────────────────────────────────────────────── */

static void sync_play_icon(void)
{
    if (!A.play_btn) return;
    gboolean playing = !A.paused && !A.eof;
    gtk_button_set_icon_name(GTK_BUTTON(A.play_btn),
        playing ? "media-playback-pause-symbolic"
                : "media-playback-start-symbolic");
    gtk_widget_set_tooltip_text(A.play_btn,
        playing ? T("Pause (Space)", "일시 정지 (Space)")
                : T("Play (Space)", "재생 (Space)"));
}

static void sync_time(void)
{
    char *p = fmt_time(A.pos, FALSE);
    gtk_label_set_text(GTK_LABEL(A.pos_lbl), p);
    g_free(p);
    A.updating = TRUE;
    gtk_range_set_value(GTK_RANGE(A.seek), A.pos);
    A.updating = FALSE;
    gtk_widget_queue_draw(A.timeline);
}

static void sync_duration(void)
{
    char *d = fmt_time(A.dur, FALSE);
    gtk_label_set_text(GTK_LABEL(A.dur_lbl), d);
    g_free(d);
    gtk_range_set_range(GTK_RANGE(A.seek), 0, A.dur > 0 ? A.dur : 1);
}

static void sync_volume(void)
{
    A.updating = TRUE;
    gtk_range_set_value(GTK_RANGE(A.vol), A.volume);
    A.updating = FALSE;
    const char *icon = A.muted || A.volume <= 0 ? "audio-volume-muted-symbolic"
                     : A.volume < 34 ? "audio-volume-low-symbolic"
                     : A.volume < 67 ? "audio-volume-medium-symbolic"
                     : "audio-volume-high-symbolic";
    gtk_button_set_icon_name(GTK_BUTTON(A.mute_btn), icon);
    gtk_widget_set_tooltip_text(A.mute_btn, A.muted
        ? T("Unmute (M)", "소리 켜기 (M)") : T("Mute (M)", "음소거 (M)"));
}

static void sync_speed(void)
{
    char *l;
    if (fabs(A.speed - 1.0) < 0.001) l = g_strdup("1×");
    else {
        char buf[G_ASCII_DTOSTR_BUF_SIZE];
        l = g_strdup_printf("%s×", g_ascii_formatd(buf, sizeof buf, "%g",
                                                   A.speed));
    }
    gtk_menu_button_set_label(GTK_MENU_BUTTON(A.speed_btn), l);
    g_free(l);
    if (A.speed_action)
        g_simple_action_set_state(A.speed_action,
                                  g_variant_new_double(A.speed));
}

static gboolean has_picture(void)
{
    return A.file && (!A.probed || A.mi.has_video);
}

static void controls_show(gboolean show)
{
    gtk_revealer_set_reveal_child(GTK_REVEALER(A.controls_rev), show);
    gtk_widget_set_cursor_from_name(A.overlay, show ? NULL : "none");
}

static gboolean hide_timeout(gpointer d)
{
    (void)d;
    A.hide_id = 0;
    /* Only hide over a playing picture. Paused, the controls are what
     * the person is looking for; for music there is nothing to uncover. */
    if (!A.paused && !A.eof && !A.over_controls && has_picture() && A.mpv)
        controls_show(FALSE);
    return G_SOURCE_REMOVE;
}

static void controls_poke(void)
{
    controls_show(TRUE);
    if (A.hide_id) g_source_remove(A.hide_id);
    A.hide_id = g_timeout_add(2000, hide_timeout, NULL);
}

static void on_motion(GtkEventControllerMotion *c, double x, double y,
                      gpointer d)
{
    (void)c; (void)d;
    /* Wayland repeats the last position when the surface changes under
     * a still pointer (e.g. the controls fading out). Only real
     * movement counts, or the bar would reappear by itself. */
    if (fabs(x - A.mx) < 2 && fabs(y - A.my) < 2) return;
    A.mx = x; A.my = y;
    controls_poke();
}

static void on_controls_enter(GtkEventControllerMotion *c, double x,
                              double y, gpointer d)
{
    (void)c; (void)x; (void)y; (void)d;
    A.over_controls = TRUE;
}

static void on_controls_leave(GtkEventControllerMotion *c, gpointer d)
{
    (void)c; (void)d;
    A.over_controls = FALSE;
}

static void toggle_pause(void)
{
    if (!A.mpv || !A.file) return;
    if (A.eof || (A.paused && A.dur > 0 && A.pos >= A.dur - 0.05)) {
        /* At the end with keep-open, unpausing alone does nothing. */
        seek_abs(0);
        set_flag("pause", FALSE);
    } else {
        set_flag("pause", !A.paused);
    }
    A.play_sel = FALSE;
    controls_poke();
}

static void on_play(GtkButton *b, gpointer d) { (void)b; (void)d; toggle_pause(); }
static void on_back10(GtkButton *b, gpointer d) { (void)b; (void)d; seek_rel(-10); }
static void on_fwd10(GtkButton *b, gpointer d) { (void)b; (void)d; seek_rel(10); }

static gboolean on_seek_change(GtkRange *r, GtkScrollType s, double v,
                               gpointer d)
{
    (void)r; (void)s; (void)d;
    A.play_sel = FALSE;
    seek_abs(v);
    return FALSE;
}

static void on_volume(GtkRange *r, gpointer d)
{
    (void)d;
    if (A.updating) return;
    set_double("volume", gtk_range_get_value(r));
    if (A.muted) set_flag("mute", FALSE);
}

static void change_volume(double delta)
{
    double v = CLAMP(A.volume + delta, 0, 100);
    set_double("volume", v);
    if (A.muted && delta > 0) set_flag("mute", FALSE);
    controls_poke();
}

static void toggle_mute(void) { set_flag("mute", !A.muted); controls_poke(); }
static void on_mute(GtkButton *b, gpointer d) { (void)b; (void)d; toggle_mute(); }

static void on_loop(GtkToggleButton *b, gpointer d)
{
    (void)d;
    if (A.mpv)
        M.set_property_string(A.mpv, "loop-file",
                              gtk_toggle_button_get_active(b) ? "inf" : "no");
}

static const double speeds[] = { 0.5, 0.75, 1.0, 1.25, 1.5, 2.0 };

static void on_speed_action(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)d;
    set_double("speed", g_variant_get_double(p));
}

static void speed_step(int dir)
{
    int cur = 2;
    for (int i = 0; i < (int)G_N_ELEMENTS(speeds); i++)
        if (fabs(speeds[i] - A.speed) < 0.01) cur = i;
    cur = CLAMP(cur + dir, 0, (int)G_N_ELEMENTS(speeds) - 1);
    set_double("speed", speeds[cur]);
    controls_poke();
}

static void toggle_fullscreen(void)
{
    GtkWindow *w = GTK_WINDOW(A.win);
    if (gtk_window_is_fullscreen(w)) gtk_window_unfullscreen(w);
    else gtk_window_fullscreen(w);
}

static void on_fs(GtkButton *b, gpointer d) { (void)b; (void)d; toggle_fullscreen(); }

static void on_fullscreen_changed(GObject *o, GParamSpec *ps, gpointer d)
{
    (void)ps; (void)d;
    gboolean fs = gtk_window_is_fullscreen(GTK_WINDOW(o));
    /* Full screen means the picture and nothing else: the title bar
     * and the edit panel step aside and come back afterwards. */
    gtk_widget_set_visible(A.header, !fs);
    gtk_widget_set_visible(A.edit_rev, !fs);
    gtk_button_set_icon_name(GTK_BUTTON(A.fs_btn),
        fs ? "view-restore-symbolic" : "view-fullscreen-symbolic");
    gtk_widget_set_tooltip_text(A.fs_btn, fs
        ? T("Leave full screen (Esc)", "전체 화면 끝내기 (Esc)")
        : T("Full screen (F)", "전체 화면 (F)"));
    controls_poke();
}

/* ── the edit panel ────────────────────────────────────────────── */

static void sync_selection(void)
{
    char *a = fmt_time(A.sel_a, TRUE), *b = fmt_time(A.sel_b, TRUE);
    char *l = fmt_time(A.sel_b - A.sel_a, TRUE);
    char *s1 = g_strdup_printf("%s  %s", T("Start", "시작"), a);
    char *s2 = g_strdup_printf("%s  %s", T("End", "끝"), b);
    char *s3 = g_strdup_printf("%s  %s", T("Selected", "선택 길이"), l);
    gtk_label_set_text(GTK_LABEL(A.start_lbl), s1);
    gtk_label_set_text(GTK_LABEL(A.end_lbl), s2);
    gtk_label_set_text(GTK_LABEL(A.len_lbl), s3);
    g_free(a); g_free(b); g_free(l); g_free(s1); g_free(s2); g_free(s3);
    gtk_widget_queue_draw(A.timeline);
}

static void reset_selection(void)
{
    A.sel_a = 0;
    A.sel_b = A.dur > 0 ? A.dur : 0;
    A.play_sel = FALSE;
    sync_selection();
}

static void set_start(void)
{
    if (!A.file || A.dur <= 0) return;
    A.sel_a = CLAMP(A.pos, 0, A.dur);
    if (A.sel_b <= A.sel_a + 0.05) A.sel_b = A.dur;
    sync_selection();
}

static void set_end(void)
{
    if (!A.file || A.dur <= 0) return;
    A.sel_b = CLAMP(A.pos, 0, A.dur);
    if (A.sel_b <= A.sel_a + 0.05) A.sel_a = 0;
    sync_selection();
}

static void play_selection(void)
{
    if (!A.mpv || !A.file || A.sel_b <= A.sel_a) return;
    seek_abs(A.sel_a);
    set_flag("pause", FALSE);
    A.play_sel = TRUE;
}

static void on_set_start(GtkButton *b, gpointer d) { (void)b; (void)d; set_start(); }
static void on_set_end(GtkButton *b, gpointer d) { (void)b; (void)d; set_end(); }
static void on_play_sel(GtkButton *b, gpointer d) { (void)b; (void)d; play_selection(); }
static void on_reset_sel(GtkButton *b, gpointer d) { (void)b; (void)d; reset_selection(); }

#define TL_PAD 12.0

static double tl_t2x(double t, int w)
{
    if (A.dur <= 0) return TL_PAD;
    return TL_PAD + (w - 2 * TL_PAD) * CLAMP(t / A.dur, 0, 1);
}

static double tl_x2t(double x, int w)
{
    if (A.dur <= 0 || w <= 2 * TL_PAD) return 0;
    return CLAMP((x - TL_PAD) / (w - 2 * TL_PAD), 0, 1) * A.dur;
}

static void rounded(cairo_t *cr, double x, double y, double w, double h,
                    double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
}

static void tl_draw(GtkDrawingArea *da, cairo_t *cr, int w, int h,
                    gpointer d)
{
    (void)d;
    GdkRGBA acc = { 0.21, 0.52, 0.89, 1 };
    GtkStyleContext *sc = gtk_widget_get_style_context(GTK_WIDGET(da));
    GdkRGBA c;
    if (gtk_style_context_lookup_color(sc, "accent_bg_color", &c) ||
        gtk_style_context_lookup_color(sc, "theme_selected_bg_color", &c))
        acc = c;

    double ty = 12, th = h - 24;
    /* the whole file */
    cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
    rounded(cr, TL_PAD, ty, w - 2 * TL_PAD, th, 6);
    cairo_fill(cr);
    if (A.dur <= 0) return;

    /* the selection */
    double xa = tl_t2x(A.sel_a, w), xb = tl_t2x(A.sel_b, w);
    cairo_set_source_rgba(cr, acc.red, acc.green, acc.blue, 0.40);
    cairo_rectangle(cr, xa, ty, xb - xa, th);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, acc.red, acc.green, acc.blue, 1);
    cairo_rectangle(cr, xa, ty, xb - xa, 2);
    cairo_rectangle(cr, xa, ty + th - 2, xb - xa, 2);
    cairo_fill(cr);

    /* handles: a bar with a grip, drawn wide enough to grab by finger */
    for (int i = 0; i < 2; i++) {
        double x = i == 0 ? xa : xb;
        cairo_set_source_rgba(cr, acc.red, acc.green, acc.blue, 1);
        rounded(cr, x - 5, 4, 10, h - 8, 4);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
        cairo_rectangle(cr, x - 1, h / 2.0 - 7, 2, 14);
        cairo_fill(cr);
    }

    /* where playback is */
    double xp = tl_t2x(A.pos, w);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_rectangle(cr, xp - 1, 2, 2, h - 4);
    cairo_fill(cr);
    cairo_move_to(cr, xp - 6, 0);
    cairo_line_to(cr, xp + 6, 0);
    cairo_line_to(cr, xp, 7);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void tl_apply(double x)
{
    int w = gtk_widget_get_width(A.timeline);
    double t = tl_x2t(x, w);
    if (A.drag_mode == 1) {
        A.sel_a = MIN(t, A.sel_b - 0.1);
        if (A.sel_a < 0) A.sel_a = 0;
        sync_selection();
    } else if (A.drag_mode == 2) {
        A.sel_b = MAX(t, A.sel_a + 0.1);
        if (A.sel_b > A.dur) A.sel_b = A.dur;
        sync_selection();
    }
    /* Seek in every mode: dragging a handle shows the frame it is on,
     * which is how you find the exact place to cut. */
    A.play_sel = FALSE;
    seek_abs(A.drag_mode == 1 ? A.sel_a : A.drag_mode == 2 ? A.sel_b : t);
    if (!A.mpv) sync_time();
}

static void tl_begin(GtkGestureDrag *g, double x, double y, gpointer d)
{
    (void)g; (void)y; (void)d;
    int w = gtk_widget_get_width(A.timeline);
    double da = fabs(x - tl_t2x(A.sel_a, w));
    double db = fabs(x - tl_t2x(A.sel_b, w));
    A.drag_mode = 0;
    if (da <= 12 || db <= 12) A.drag_mode = da <= db ? 1 : 2;
    /* Both handles on the same spot at the right end: grab the start,
     * or it could never be moved away from the end. */
    if (A.drag_mode == 2 && da == db && x < tl_t2x(A.sel_b, w))
        A.drag_mode = 1;
    A.drag_x0 = x;
    tl_apply(x);
}

static void tl_update(GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    (void)g; (void)dy; (void)d;
    tl_apply(A.drag_x0 + dx);
}

static void tl_motion(GtkEventControllerMotion *c, double x, double y,
                      gpointer d)
{
    (void)c; (void)y; (void)d;
    int w = gtk_widget_get_width(A.timeline);
    gboolean near = A.dur > 0 && (fabs(x - tl_t2x(A.sel_a, w)) <= 12 ||
                                  fabs(x - tl_t2x(A.sel_b, w)) <= 12);
    gtk_widget_set_cursor_from_name(A.timeline, near ? "ew-resize" : NULL);
}

static void toggle_edit(void)
{
    if (!A.file) return;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.edit_btn),
        !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.edit_btn)));
}

static void on_edit_toggled(GtkToggleButton *b, gpointer d)
{
    (void)d;
    gtk_revealer_set_reveal_child(GTK_REVEALER(A.edit_rev),
                                  gtk_toggle_button_get_active(b));
}

/* ── running ffmpeg ────────────────────────────────────────────── */

static const char *op_doing(edit_op_t op)
{
    switch (op) {
    case OP_TRIM_FAST: case OP_TRIM_PRECISE:
        return T("Trimming…", "자르는 중…");
    case OP_CUT:       return T("Removing the selected part…", "선택한 부분을 빼는 중…");
    case OP_AUDIO_MP3: case OP_AUDIO_M4A:
        return T("Extracting the sound…", "소리를 뽑아내는 중…");
    case OP_FRAME:     return T("Saving the frame…", "장면을 저장하는 중…");
    case OP_GIF_480: case OP_GIF_720:
        return T("Making a GIF…", "GIF 를 만드는 중…");
    case OP_ROT_LEFT: case OP_ROT_RIGHT:
        return T("Rotating…", "돌리는 중…");
    case OP_NO_AUDIO:  return T("Removing the sound…", "소리를 없애는 중…");
    default:           return T("Converting…", "변환하는 중…");
    }
}

static void show_status(const char *page)
{
    gtk_stack_set_visible_child_name(GTK_STACK(A.status), page);
}

static void show_error(const char *msg)
{
    gtk_label_set_text(GTK_LABEL(A.err_lbl), msg);
    show_status("error");
}

static void job_read_line(GObject *src, GAsyncResult *res, gpointer d);

static void job_next_line(void)
{
    g_data_input_stream_read_line_async(A.job_in, G_PRIORITY_DEFAULT, NULL,
                                        job_read_line, NULL);
}

static void job_read_line(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    gsize len = 0;
    char *line = g_data_input_stream_read_line_finish(
                     G_DATA_INPUT_STREAM(src), res, &len, NULL);
    if (!line) return;                         /* EOF or error */
    if (G_DATA_INPUT_STREAM(src) != A.job_in) { g_free(line); return; }

    /* -progress writes key=value blocks. out_time_us is the position in
     * the output; ffmpeg 5 also writes out_time_ms, which (despite the
     * name) is in microseconds as well. */
    gint64 us = -1;
    if (g_str_has_prefix(line, "out_time_us="))
        us = g_ascii_strtoll(line + 12, NULL, 10);
    else if (g_str_has_prefix(line, "out_time_ms="))
        us = g_ascii_strtoll(line + 12, NULL, 10);
    if (us >= 0 && A.job_len > 0)
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(A.progress),
                                      CLAMP(us / 1e6 / A.job_len, 0, 1));
    else if (g_str_has_prefix(line, "progress=end"))
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(A.progress), 1);
    else if (A.job_len <= 0)
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(A.progress));
    g_free(line);
    job_next_line();
}

/* The last few lines ffmpeg wrote to stderr: the error, usually. */
static char *err_tail(const char *path)
{
    char *text = NULL;
    if (!path || !g_file_get_contents(path, &text, NULL, NULL)) return NULL;
    char **lines = g_strsplit(g_strstrip(text), "\n", -1);
    guint n = g_strv_length(lines);
    guint from = n > 4 ? n - 4 : 0;
    GString *s = g_string_new(NULL);
    for (guint i = from; i < n; i++) {
        if (!*g_strstrip(lines[i])) continue;
        if (s->len) g_string_append_c(s, '\n');
        g_string_append(s, lines[i]);
    }
    g_strfreev(lines);
    g_free(text);
    return g_string_free(s, s->len == 0);
}

static void job_finish(void)
{
    g_clear_object(&A.job);
    g_clear_object(&A.job_in);
    if (A.job_err) { g_unlink(A.job_err); g_clear_pointer(&A.job_err, g_free); }
    g_clear_pointer(&A.job_out, g_free);
    gtk_widget_set_sensitive(A.ops_box, TRUE);
}

static void on_job_done(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    GSubprocess *p = G_SUBPROCESS(src);
    g_subprocess_wait_finish(p, res, NULL);
    if (p != A.job) return;

    GStatBuf st;
    gboolean ok = g_subprocess_get_if_exited(p) &&
                  g_subprocess_get_exit_status(p) == 0 &&
                  g_stat(A.job_out, &st) == 0 && st.st_size > 0;

    if (A.job_cancelled) {
        g_unlink(A.job_out);
        gtk_label_set_text(GTK_LABEL(A.idle_lbl),
                           T("Cancelled. Nothing was saved.",
                             "취소했습니다. 아무것도 저장하지 않았습니다."));
        show_status("idle");
    } else if (ok) {
        g_free(A.result);
        A.result = g_strdup(A.job_out);
        char *base = g_path_get_basename(A.job_out);
        char *msg = g_strdup_printf(T("Saved as “%s”", "“%s” (으)로 저장했습니다"),
                                    base);
        gtk_label_set_text(GTK_LABEL(A.done_lbl), msg);
        g_free(msg); g_free(base);
        show_status("done");
    } else {
        char *tail = err_tail(A.job_err);
        /* A failed run can leave a half-written file behind. */
        g_unlink(A.job_out);
        char *msg = g_strdup_printf("%s\n%s",
            T("It did not work. ffmpeg said:", "실패했습니다. ffmpeg 메시지:"),
            tail ? tail : T("(no message)", "(메시지 없음)"));
        show_error(msg);
        g_free(msg); g_free(tail);
    }
    job_finish();
}

static void start_job(edit_op_t op, const char *out)
{
    GError *err = NULL;
    GPtrArray *argv = edit_build_argv(op, A.file, out, &A.mi,
                                      A.sel_a, A.sel_b, A.pos);

    int fd = g_file_open_tmp("lp-video-XXXXXX.log", &A.job_err, NULL);
    if (fd >= 0) close(fd);

    GSubprocessLauncher *l =
        g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE);
    /* stderr to a file, not a pipe: nobody reads it until the end, and
     * an unread pipe that fills up would stall ffmpeg. */
    if (A.job_err) g_subprocess_launcher_set_stderr_file_path(l, A.job_err);
    A.job = g_subprocess_launcher_spawnv(l, (const char *const *)argv->pdata,
                                         &err);
    g_object_unref(l);
    g_ptr_array_unref(argv);

    if (!A.job) {
        char *msg = g_strdup_printf("%s\n%s",
            T("ffmpeg could not be started.", "ffmpeg 을 실행하지 못했습니다."),
            err ? err->message : "");
        show_error(msg);
        g_free(msg);
        g_clear_error(&err);
        job_finish();
        return;
    }

    A.job_out = g_strdup(out);
    A.job_cancelled = FALSE;
    A.job_len = edit_expected_length(op, &A.mi, A.sel_a, A.sel_b);
    A.job_in = g_data_input_stream_new(g_subprocess_get_stdout_pipe(A.job));
    gtk_label_set_text(GTK_LABEL(A.busy_lbl), op_doing(op));
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(A.progress), 0);
    gtk_widget_set_sensitive(A.ops_box, FALSE);
    show_status("busy");
    job_next_line();
    g_subprocess_wait_async(A.job, NULL, on_job_done, NULL);
}

static void on_cancel(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!A.job) return;
    A.job_cancelled = TRUE;
    g_subprocess_force_exit(A.job);
}

typedef struct { edit_op_t op; GtkFileChooserNative *dlg; } save_req_t;

static void on_save_response(GtkNativeDialog *dlg, int resp, gpointer d)
{
    save_req_t *r = d;
    if (resp == GTK_RESPONSE_ACCEPT) {
        GFile *f = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dlg));
        char *path = f ? g_file_get_path(f) : NULL;
        GFile *in = g_file_new_for_path(A.file);
        if (path && f && g_file_equal(f, in))
            /* ffmpeg would read and write the same file and destroy it. */
            show_error(T("That is the original file. Choose another name - "
                         "the original is never overwritten.",
                         "원본 파일입니다. 다른 이름을 고르세요 - 원본은 "
                         "덮어쓰지 않습니다."));
        else if (path)
            start_job(r->op, path);
        g_object_unref(in);
        g_free(path);
        if (f) g_object_unref(f);
    }
    g_object_unref(dlg);
    g_free(r);
}

static void run_op(edit_op_t op)
{
    if (!A.file || A.job) return;
    if (!A.probed) {
        show_error(T("This file could not be read, so it cannot be edited.",
                     "이 파일을 읽지 못해서 편집할 수 없습니다."));
        return;
    }
    if (!edit_op_available(op, &A.mi)) return;
    if (edit_op_ranged(op) && A.sel_b - A.sel_a < 0.1) {
        show_error(T("The selection is too short. Set a start and an end first.",
                     "선택한 구간이 너무 짧습니다. 먼저 시작과 끝을 정하세요."));
        return;
    }
    if (op == OP_CUT && A.sel_a <= 0.05 && A.sel_b >= A.dur - 0.05) {
        show_error(T("The selection is the whole file - nothing would be left. "
                     "Set a start and an end around the part to remove.",
                     "선택이 파일 전체라서 남는 것이 없습니다. 뺄 부분의 "
                     "시작과 끝을 정하세요."));
        return;
    }

    char *out = edit_default_output(op, A.file, &A.mi);
    char *odir = g_path_get_dirname(out);
    /* Next to the original is the default, but a CD, a camera card
     * mounted read-only or someone else's folder cannot take it: then
     * ask where to save, starting in Videos, instead of failing. */
    gboolean writable = access(odir, W_OK) == 0;
    g_free(odir);
    if (!writable ||
        gtk_check_button_get_active(GTK_CHECK_BUTTON(A.saveas))) {
        save_req_t *r = g_new0(save_req_t, 1);
        r->op = op;
        GtkFileChooserNative *dlg = gtk_file_chooser_native_new(
            T("Save As", "다른 이름으로 저장"), GTK_WINDOW(A.win),
            GTK_FILE_CHOOSER_ACTION_SAVE,
            T("_Save", "저장(_S)"), T("_Cancel", "취소(_C)"));
        const char *vids = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
        char *dir = writable ? g_path_get_dirname(out)
                  : g_strdup(vids && g_file_test(vids, G_FILE_TEST_IS_DIR)
                             ? vids : g_get_home_dir());
        char *base = g_path_get_basename(out);
        GFile *df = g_file_new_for_path(dir);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), df, NULL);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), base);
        g_object_unref(df);
        g_free(dir); g_free(base);
        g_signal_connect(dlg, "response", G_CALLBACK(on_save_response), r);
        gtk_native_dialog_show(GTK_NATIVE_DIALOG(dlg));
    } else {
        start_job(op, out);
    }
    g_free(out);
}

static void on_op_clicked(GtkButton *b, gpointer d)
{
    /* Items inside a popover: close it before the work starts. */
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    run_op((edit_op_t)GPOINTER_TO_INT(d));
}

static void on_open_folder(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!A.result) return;
    char *dir = g_path_get_dirname(A.result);
    GFile *f = g_file_new_for_path(dir);
    char *uri = g_file_get_uri(f);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_show_uri(GTK_WINDOW(A.win), uri, GDK_CURRENT_TIME);
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_free(uri); g_object_unref(f); g_free(dir);
}

static void on_play_result(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!A.result) return;
    char *p = g_strdup(A.result);
    if (g_str_has_suffix(p, ".png") || g_str_has_suffix(p, ".gif")) {
        /* A picture: the image viewer, not this player. */
        GFile *f = g_file_new_for_path(p);
        char *uri = g_file_get_uri(f);
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_show_uri(GTK_WINDOW(A.win), uri, GDK_CURRENT_TIME);
        G_GNUC_END_IGNORE_DEPRECATIONS
        g_free(uri); g_object_unref(f);
    } else {
        player_open(p);
    }
    g_free(p);
}

static void on_dismiss(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gtk_label_set_text(GTK_LABEL(A.idle_lbl),
        T("Mark the part with Set start (I) and Set end (O), then choose a tool.",
          "시작 지점(I)과 끝 지점(O)으로 구간을 정한 다음 도구를 고르세요."));
    show_status("idle");
}

static void update_ops(void)
{
    for (int i = 0; i < OP_COUNT; i++)
        if (A.op_widget[i])
            gtk_widget_set_sensitive(A.op_widget[i],
                                     A.probed && edit_op_available(i, &A.mi));
    /* A menu button is usable when at least one of its items is. */
    for (guint i = 0; i < A.op_menus->len; i++) {
        GtkWidget *mb = g_ptr_array_index(A.op_menus, i);
        int *ops = g_object_get_data(G_OBJECT(mb), "ops");
        gboolean any = FALSE;
        for (int k = 0; ops[k] >= 0; k++)
            any = any || (A.probed && edit_op_available(ops[k], &A.mi));
        gtk_widget_set_sensitive(mb, any);
    }
}

/* One item in a tool's pop-up: a bold name over a dim explanation, so
 * that "Fast" and "Precise" say what they trade without a tooltip. */
static GtkWidget *op_item(edit_op_t op, const char *title, const char *sub)
{
    GtkWidget *b = gtk_button_new();
    gtk_widget_add_css_class(b, "flat");
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "lp-op-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_box_append(GTK_BOX(v), t);
    if (sub) {
        GtkWidget *s = gtk_label_new(sub);
        gtk_widget_add_css_class(s, "dim-label");
        gtk_widget_add_css_class(s, "caption");
        gtk_label_set_xalign(GTK_LABEL(s), 0);
        gtk_label_set_wrap(GTK_LABEL(s), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(s), 36);
        gtk_box_append(GTK_BOX(v), s);
    }
    gtk_button_set_child(GTK_BUTTON(b), v);
    g_signal_connect(b, "clicked", G_CALLBACK(on_op_clicked),
                     GINT_TO_POINTER(op));
    A.op_widget[op] = b;
    return b;
}

typedef struct { edit_op_t op; const char *title, *sub; } item_t;

static GtkWidget *op_menu(const char *icon, const char *label,
                          const char *tip, const item_t *items, int n)
{
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_child(GTK_MENU_BUTTON(mb), label_icon_box(icon, label));
    gtk_widget_set_tooltip_text(mb, tip);
    GtkWidget *pop = gtk_popover_new();
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    int *ops = g_new(int, n + 1);
    for (int i = 0; i < n; i++) {
        gtk_box_append(GTK_BOX(v), op_item(items[i].op, items[i].title,
                                           items[i].sub));
        ops[i] = items[i].op;
    }
    ops[n] = -1;
    g_object_set_data_full(G_OBJECT(mb), "ops", ops, g_free);
    gtk_popover_set_child(GTK_POPOVER(pop), v);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), pop);
    g_ptr_array_add(A.op_menus, mb);
    return mb;
}

static GtkWidget *op_button(edit_op_t op, const char *icon, const char *label,
                            const char *tip)
{
    GtkWidget *b = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(b), label_icon_box(icon, label));
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", G_CALLBACK(on_op_clicked),
                     GINT_TO_POINTER(op));
    A.op_widget[op] = b;
    return b;
}

/* ── mpv: events ───────────────────────────────────────────────── */

static void on_property(mpv_event_property *p, uint64_t id)
{
    gboolean have = p->format != MPV_FORMAT_NONE && p->data;
    double dv = have && p->format == MPV_FORMAT_DOUBLE ? *(double *)p->data : 0;
    int fv = have && p->format == MPV_FORMAT_FLAG ? *(int *)p->data : 0;
    gint64 iv = have && p->format == MPV_FORMAT_INT64 ? *(gint64 *)p->data : 0;

    switch (id) {
    case P_TIME:
        if (!have) break;
        if (A.seeking && g_get_monotonic_time() < A.seek_hold) break;
        A.seeking = FALSE;
        A.pos = dv;
        if (A.play_sel && A.pos >= A.sel_b - 0.03) {
            /* End of "play selection": again from the start if Loop is
             * on, otherwise stop right at the end mark. */
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.loop_btn)))
                seek_abs(A.sel_a);
            else {
                A.play_sel = FALSE;
                set_flag("pause", TRUE);
            }
        }
        sync_time();
        break;
    case P_DURATION:
        if (have && dv > 0 && fabs(dv - A.dur) > 0.001) {
            gboolean full = A.sel_b <= 0 || A.sel_b >= A.dur - 0.01;
            A.dur = dv;
            if (full) A.sel_b = dv;
            A.sel_b = MIN(A.sel_b, dv);
            A.sel_a = MIN(A.sel_a, A.sel_b);
            sync_duration();
            sync_selection();
        }
        break;
    case P_PAUSE:
        A.paused = fv;
        sync_play_icon();
        if (A.paused) controls_poke();
        break;
    case P_EOF:
        A.eof = fv;
        sync_play_icon();
        if (A.eof) controls_poke();
        break;
    case P_VOLUME:
        if (have) { A.volume = dv; sync_volume(); }
        break;
    case P_MUTE:
        A.muted = fv;
        sync_volume();
        break;
    case P_SPEED:
        if (have) { A.speed = dv; sync_speed(); }
        break;
    case P_DWIDTH:  A.dw = iv; break;
    case P_DHEIGHT: A.dh = iv; break;
    }
}

static gboolean mpv_events_idle(gpointer d)
{
    (void)d;
    g_atomic_int_set(&A.events_queued, 0);
    while (A.mpv) {
        mpv_event *e = M.wait_event(A.mpv, 0);
        if (!e || e->event_id == MPV_EVENT_NONE) break;
        switch (e->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE:
            on_property(e->data, e->reply_userdata);
            break;
        case MPV_EVENT_PLAYBACK_RESTART: {
            /* Our seek has landed. Paused, no further time-pos change
             * may come, so read where it landed now. */
            A.seeking = FALSE;
            double t;
            if (M.get_property(A.mpv, "time-pos", MPV_FORMAT_DOUBLE, &t) >= 0) {
                A.pos = t;
                sync_time();
            }
            break;
        }
        case MPV_EVENT_FILE_LOADED:
            A.load_failed = FALSE;
            update_placeholder();
            break;
        case MPV_EVENT_END_FILE: {
            mpv_event_end_file *ef = e->data;
            if (ef && ef->reason == MPV_END_FILE_REASON_ERROR) {
                A.load_failed = TRUE;
                update_placeholder();
            }
            break;
        }
        default:
            break;
        }
    }
    return G_SOURCE_REMOVE;
}

static void mpv_wakeup(void *d)
{
    (void)d;
    /* mpv's thread. Schedule, never touch GTK here. */
    if (g_atomic_int_compare_and_exchange(&A.events_queued, 0, 1))
        g_idle_add(mpv_events_idle, NULL);
}

static gboolean player_init(void)
{
    char *why = NULL;
    /* mpv refuses to start under a locale whose numbers are not C
     * (mpv_create returns NULL). GTK has just set everything from the
     * environment; numbers go back to C, which changes nothing a person
     * sees - the labels here format numbers themselves. */
    setlocale(LC_NUMERIC, "C");
    if (!mpv_load(&why)) {
        A.mpv_error = why;
        return FALSE;
    }
    A.mpv = M.create();
    if (!A.mpv) {
        A.mpv_error = g_strdup("mpv_create failed");
        return FALSE;
    }
    const char *opts[][2] = {
        { "vo", "libmpv" },
        { "hwdec", "auto-safe" },
        { "keep-open", "yes" },
        { "idle", "yes" },
        /* Our own controls; mpv's must not react to keys or draw. */
        { "input-default-bindings", "no" },
        { "input-vo-keyboard", "no" },
        { "osc", "no" },
        { "config", "no" },
        { "ytdl", "no" },
        /* Music shows the note placeholder, not a still of cover art. */
        { "audio-display", "no" },
        /* No sound device (or it is busy): play on silently rather than
         * refuse the file - a video is still worth watching, and music
         * still shows its progress. */
        { "audio-fallback-to-null", "yes" },
        /* The frame is drawn from the GTK main loop; mpv must not hold
         * it waiting for the frame's display time (see render()). */
        { "video-timing-offset", "0" },
        { "audio-client-name", "lp-video" },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(opts); i++)
        M.set_option_string(A.mpv, opts[i][0], opts[i][1]);
    /* With no sound card at all (a VM given none) PipeWire still takes
     * the stream and never plays it, and audio-fallback-to-null never
     * fires: playback sat at 0:00 on its first frame. Then there is
     * nothing to hear, so the audio goes nowhere and the video plays. */
    gchar *cards = NULL;
    if (g_file_get_contents("/proc/asound/cards", &cards, NULL, NULL) &&
        strstr(cards, "no soundcards"))
        M.set_option_string(A.mpv, "ao", "null");
    g_free(cards);
    if (g_getenv("LP_VIDEO_DEBUG")) {
        M.set_option_string(A.mpv, "terminal", "yes");
        M.set_option_string(A.mpv, "msg-level", "all=v");
    }
    int r = M.initialize(A.mpv);
    if (r < 0) {
        A.mpv_error = g_strdup(M.error_string(r));
        M.terminate_destroy(A.mpv);
        A.mpv = NULL;
        return FALSE;
    }
    M.observe_property(A.mpv, P_TIME, "time-pos", MPV_FORMAT_DOUBLE);
    M.observe_property(A.mpv, P_DURATION, "duration", MPV_FORMAT_DOUBLE);
    M.observe_property(A.mpv, P_PAUSE, "pause", MPV_FORMAT_FLAG);
    M.observe_property(A.mpv, P_VOLUME, "volume", MPV_FORMAT_DOUBLE);
    M.observe_property(A.mpv, P_MUTE, "mute", MPV_FORMAT_FLAG);
    M.observe_property(A.mpv, P_EOF, "eof-reached", MPV_FORMAT_FLAG);
    M.observe_property(A.mpv, P_SPEED, "speed", MPV_FORMAT_DOUBLE);
    M.observe_property(A.mpv, P_DWIDTH, "dwidth", MPV_FORMAT_INT64);
    M.observe_property(A.mpv, P_DHEIGHT, "dheight", MPV_FORMAT_INT64);
    M.set_wakeup_callback(A.mpv, mpv_wakeup, NULL);
    return TRUE;
}

/* The renderer exists: whatever was waiting can now load. */
static void player_ready(void)
{
    A.ready = TRUE;
    update_placeholder();
    if (A.pending) {
        char *p = A.pending;
        A.pending = NULL;
        set_flag("pause", FALSE);
        cmd("loadfile", p, NULL);
        g_free(p);
    }
}

/* ── mpv: frames, software path ────────────────────────────────── */

static void sw_render(void)
{
    if (!A.rctx || !A.sw) return;
    A.sw_last = g_get_monotonic_time();

    int scale = gtk_widget_get_scale_factor(A.video);
    int ww = gtk_widget_get_width(A.video) * scale;
    int wh = gtk_widget_get_height(A.video) * scale;
    if (ww < 16 || wh < 16) { ww = 640; wh = 360; }

    /* No bigger than the video (GtkPicture scales it up on the GPU-less
     * path far cheaper than mpv would), no bigger than the window. */
    int tw = ww, th = wh;
    if (A.dw > 0 && A.dh > 0) {
        double s = MIN((double)ww / A.dw, (double)wh / A.dh);
        if (s > 1) s = 1;
        tw = MAX(2, (int)(A.dw * s));
        th = MAX(2, (int)(A.dh * s));
    }
    size_t stride = ((size_t)tw * 4 + 63) & ~(size_t)63;
    void *buf = NULL;
    if (posix_memalign(&buf, 64, stride * th) != 0) return;

    int size[2] = { tw, th }, block = 0;
    mpv_render_param p[] = {
        { MPV_RENDER_PARAM_SW_SIZE, size },
        { MPV_RENDER_PARAM_SW_FORMAT, (void *)"rgb0" },
        { MPV_RENDER_PARAM_SW_STRIDE, &stride },
        { MPV_RENDER_PARAM_SW_POINTER, buf },
        { MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block },
        { MPV_RENDER_PARAM_INVALID, NULL },
    };
    if (M.render_context_render(A.rctx, p) < 0) { free(buf); return; }

    /* "rgb0": the fourth byte is padding with no promised value. GTK
     * 4.8 has no RGBX memory format, so make it opaque alpha. */
    for (int y = 0; y < th; y++) {
        guint8 *row = (guint8 *)buf + (size_t)y * stride;
        for (int x = 0; x < tw; x++) row[x * 4 + 3] = 0xff;
    }
    GBytes *bytes = g_bytes_new_with_free_func(buf, stride * th, free, buf);
    GdkTexture *tex = gdk_memory_texture_new(tw, th, GDK_MEMORY_R8G8B8A8,
                                             bytes, stride);
    gtk_picture_set_paintable(GTK_PICTURE(A.video), GDK_PAINTABLE(tex));
    g_object_unref(tex);
    g_bytes_unref(bytes);
}

static gboolean sw_timer_cb(gpointer d)
{
    (void)d;
    A.sw_timer = 0;
    sw_render();
    return G_SOURCE_REMOVE;
}

static gboolean render_idle(gpointer d)
{
    (void)d;
    g_atomic_int_set(&A.render_queued, 0);
    if (!A.rctx) return G_SOURCE_REMOVE;
    uint64_t flags = M.render_context_update(A.rctx);
    if (!(flags & MPV_RENDER_UPDATE_FRAME)) return G_SOURCE_REMOVE;
    if (A.sw) {
        /* At most ~60 frames a second: past that the CPU spent here is
         * taken from the decoder. Not 30: a 30 fps video would then
         * miss every frame that arrives a millisecond early, and mpv
         * counts each of those as dropped. */
        gint64 since = g_get_monotonic_time() - A.sw_last;
        if (since >= 15000) sw_render();
        else if (!A.sw_timer)
            A.sw_timer = g_timeout_add(MAX(1, (15000 - since) / 1000),
                                       sw_timer_cb, NULL);
    } else {
        gtk_gl_area_queue_render(GTK_GL_AREA(A.video));
    }
    return G_SOURCE_REMOVE;
}

static void mpv_render_update(void *d)
{
    (void)d;
    if (g_atomic_int_compare_and_exchange(&A.render_queued, 0, 1))
        g_idle_add(render_idle, NULL);
}

static void sw_init(void)
{
    if (!A.mpv || A.rctx) return;
    mpv_render_param p[] = {
        { MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_SW },
        { MPV_RENDER_PARAM_INVALID, NULL },
    };
    int r = M.render_context_create(&A.rctx, A.mpv, p);
    if (r < 0) {
        A.rctx = NULL;
        g_free(A.mpv_error);
        A.mpv_error = g_strdup_printf("render: %s", M.error_string(r));
        A.render_failed = TRUE;
        update_placeholder();
        return;
    }
    A.rctx_gl = FALSE;
    /* Hardware decoders hand over GPU surfaces; this renderer wants
     * memory. Decode in software rather than copy back and forth. */
    M.set_property_string(A.mpv, "hwdec", "no");
    M.render_context_set_update_callback(A.rctx, mpv_render_update, NULL);
    player_ready();
}

/* ── mpv: frames, OpenGL path ──────────────────────────────────── */

static void *gl_proc(void *ctx, const char *name)
{
    (void)ctx;
    void *p = A.egl_gpa ? A.egl_gpa(name) : NULL;
    if (!p) p = dlsym(RTLD_DEFAULT, name);
    if (!p) {
        static void *gles, *gl;
        if (!gles) gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
        if (gles) p = dlsym(gles, name);
        if (!p) {
            if (!gl) gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
            if (gl) p = dlsym(gl, name);
        }
    }
    return p;
}

static gboolean switch_to_sw_idle(gpointer d)
{
    (void)d;
    set_video_widget_sw();
    return G_SOURCE_REMOVE;
}

static void on_gl_realize(GtkGLArea *area, gpointer d)
{
    (void)d;
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area)) {
        g_idle_add(switch_to_sw_idle, NULL);
        return;
    }
    if (!A.mpv || A.rctx) return;

    if (!A.egl) A.egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (A.egl && !A.egl_gpa)
        A.egl_gpa = (egl_gpa_fn)dlsym(A.egl, "eglGetProcAddress");
    A.gl_get_integerv = (gl_get_integerv_fn)gl_proc(NULL, "glGetIntegerv");
    A.gl_clear_color = (gl_clear_color_fn)gl_proc(NULL, "glClearColor");
    A.gl_clear = (gl_clear_fn)gl_proc(NULL, "glClear");

    mpv_opengl_init_params gi = { gl_proc, NULL };
    mpv_render_param p[] = {
        { MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gi },
        { MPV_RENDER_PARAM_INVALID, NULL },
    };
    if (!A.gl_get_integerv || M.render_context_create(&A.rctx, A.mpv, p) < 0) {
        A.rctx = NULL;
        g_idle_add(switch_to_sw_idle, NULL);
        return;
    }
    A.rctx_gl = TRUE;
    M.render_context_set_update_callback(A.rctx, mpv_render_update, NULL);
    player_ready();
}

static void on_gl_unrealize(GtkGLArea *area, gpointer d)
{
    (void)d;
    if (!A.rctx || !A.rctx_gl) return;
    /* mpv frees its GL objects here, so its context must be current. */
    gtk_gl_area_make_current(area);
    M.render_context_free(A.rctx);
    A.rctx = NULL;
}

static gboolean on_gl_render(GtkGLArea *area, GdkGLContext *ctx, gpointer d)
{
    (void)ctx; (void)d;
    if (!A.rctx) {
        if (A.gl_clear) {
            A.gl_clear_color(0, 0, 0, 1);
            A.gl_clear(0x00004000);   /* GL_COLOR_BUFFER_BIT */
        }
        return TRUE;
    }
    /* GtkGLArea renders into its own framebuffer object, not 0; ask GL
     * which one is bound (GL_DRAW_FRAMEBUFFER_BINDING). */
    int fbo = 0;
    A.gl_get_integerv(0x8CA6, &fbo);
    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(area));
    mpv_opengl_fbo f = {
        .fbo = fbo,
        .w = gtk_widget_get_width(GTK_WIDGET(area)) * scale,
        .h = gtk_widget_get_height(GTK_WIDGET(area)) * scale,
        .internal_format = 0,
    };
    int flip = 1, block = 0;
    mpv_render_param p[] = {
        { MPV_RENDER_PARAM_OPENGL_FBO, &f },
        { MPV_RENDER_PARAM_FLIP_Y, &flip },
        /* Never sleep in the GTK main loop until the frame is due. */
        { MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block },
        { MPV_RENDER_PARAM_INVALID, NULL },
    };
    M.render_context_render(A.rctx, p);
    return TRUE;
}

/* ── the video widget ──────────────────────────────────────────── */

static void on_video_click(GtkGestureClick *g, int n, double x, double y,
                           gpointer d)
{
    (void)g; (void)x; (void)y; (void)d;
    if (n == 2) toggle_fullscreen();
}

static void attach_video(GtkWidget *w)
{
    gtk_widget_set_hexpand(w, TRUE);
    gtk_widget_set_vexpand(w, TRUE);
    gtk_widget_add_css_class(w, "lp-video-bg");
    GtkGesture *g = gtk_gesture_click_new();
    g_signal_connect(g, "pressed", G_CALLBACK(on_video_click), NULL);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(g));
    gtk_overlay_set_child(GTK_OVERLAY(A.overlay), w);
    A.video = w;
}

static void set_video_widget_sw(void)
{
    if (A.video && A.sw) return;
    A.sw = TRUE;
    GtkWidget *pic = gtk_picture_new();
    gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
    /* The GL area goes away (and frees its renderer on unrealize). */
    attach_video(pic);
    sw_init();
}

static gboolean gl_usable(void)
{
    if (g_getenv("LP_VIDEO_SW")) return FALSE;
    /* A session that draws GTK with cairo (session-run sets it in every
     * virtual machine, and under sway): a GtkGLArea there realizes, but
     * what mpv draws into it never reached the window - the video area
     * stayed black in a VM with virgl. The software path shows it. */
    const char *gsk = g_getenv("GSK_RENDERER");
    if (gsk && !strcmp(gsk, "cairo")) {
        if (g_getenv("LP_VIDEO_DEBUG"))
            g_printerr("lp-video: GSK_RENDERER=cairo, software rendering\n");
        return FALSE;
    }
    GError *err = NULL;
    GdkGLContext *c = gdk_display_create_gl_context(gdk_display_get_default(),
                                                    &err);
    gboolean ok = c && gdk_gl_context_realize(c, &err);
    if (!ok && err && g_getenv("LP_VIDEO_DEBUG"))
        g_printerr("lp-video: no GL (%s), software rendering\n", err->message);
    g_clear_error(&err);
    if (c) g_object_unref(c);
    return ok;
}

static void make_video_widget(void)
{
    if (A.mpv && gl_usable()) {
        GtkWidget *gl = gtk_gl_area_new();
        gtk_gl_area_set_auto_render(GTK_GL_AREA(gl), FALSE);
        g_signal_connect(gl, "realize", G_CALLBACK(on_gl_realize), NULL);
        g_signal_connect(gl, "unrealize", G_CALLBACK(on_gl_unrealize), NULL);
        g_signal_connect(gl, "render", G_CALLBACK(on_gl_render), NULL);
        A.sw = FALSE;
        attach_video(gl);
    } else {
        A.sw = FALSE;
        set_video_widget_sw();
    }
}

static void update_placeholder(void)
{
    const char *icon = NULL, *title = NULL;
    char *sub = NULL;
    if (!A.ph) return;          /* the window is still being built */
    if (!A.file) {
        gtk_widget_set_visible(A.ph, FALSE);
        return;
    }
    if (!A.mpv || A.render_failed) {
        icon = "dialog-warning-symbolic";
        title = T("Playback is not available", "재생할 수 없습니다");
        sub = g_strdup_printf("%s\n(%s)",
            T("The mpv library (libmpv) could not be loaded. The editing tools still work.",
              "mpv 라이브러리(libmpv)를 불러오지 못했습니다. 편집 도구는 쓸 수 있습니다."),
            A.mpv_error ? A.mpv_error : "?");
    } else if (A.load_failed) {
        icon = "dialog-error-symbolic";
        title = T("This file cannot be played", "이 파일은 재생할 수 없습니다");
        sub = g_strdup(T("It may be damaged, or in a format this machine does not know.",
                         "파일이 손상되었거나 이 기기가 모르는 형식일 수 있습니다."));
    } else if (A.probed && !A.mi.has_video && A.mi.has_audio) {
        icon = "audio-x-generic-symbolic";
        title = NULL;   /* the file name, below */
        sub = g_strdup(T("Audio", "오디오"));
    }
    if (!icon) {
        gtk_widget_set_visible(A.ph, FALSE);
        return;
    }
    char *base = g_path_get_basename(A.file);
    gtk_image_set_from_icon_name(GTK_IMAGE(A.ph_icon), icon);
    gtk_label_set_text(GTK_LABEL(A.ph_title), title ? title : base);
    gtk_label_set_text(GTK_LABEL(A.ph_sub), sub ? sub : "");
    gtk_widget_set_visible(A.ph, TRUE);
    g_free(base);
    g_free(sub);
}

/* ── opening files ─────────────────────────────────────────────── */

static void on_probe_done(GObject *src, GAsyncResult *res, gpointer d)
{
    char *path = d;
    char *out = NULL;
    GSubprocess *p = G_SUBPROCESS(src);
    gboolean ok = g_subprocess_communicate_utf8_finish(p, res, &out, NULL,
                                                       NULL);
    g_object_unref(p);
    /* The person may have opened another file while this ran. */
    if (g_strcmp0(path, A.file) == 0) {
        media_info_t mi;
        A.probed = ok && media_probe_parse(out, &mi);
        if (A.probed) {
            A.mi = mi;
            if (A.dur <= 0 && mi.duration > 0) {
                A.dur = mi.duration;
                sync_duration();
            }
            if (A.sel_b <= 0) A.sel_b = A.dur;
            sync_selection();
        }
        update_ops();
        update_placeholder();
        if (!A.probed) {
            gtk_label_set_text(GTK_LABEL(A.idle_lbl),
                T("This file could not be read by ffprobe; the editing tools are off.",
                  "ffprobe 가 이 파일을 읽지 못해서 편집 도구를 쓸 수 없습니다."));
            show_status("idle");
        }
    }
    g_free(out);
    g_free(path);
}

static void player_open(const char *path)
{
    if (!path) return;
    char *abs = g_canonicalize_filename(path, NULL);
    g_free(A.file);
    A.file = abs;
    A.probed = FALSE;
    A.load_failed = FALSE;
    memset(&A.mi, 0, sizeof A.mi);
    A.pos = 0; A.dur = 0; A.eof = FALSE; A.dw = A.dh = 0;
    A.play_sel = FALSE;
    A.sel_a = A.sel_b = 0;

    char *base = g_path_get_basename(abs);
    gtk_label_set_text(GTK_LABEL(A.title), base);
    gtk_window_set_title(GTK_WINDOW(A.win), base);
    g_free(base);
    gtk_stack_set_visible_child_name(GTK_STACK(A.stack), "player");
    gtk_widget_set_sensitive(A.edit_btn, TRUE);
    sync_duration();
    sync_time();
    sync_selection();
    update_ops();
    on_dismiss(NULL, NULL);

    GPtrArray *argv = media_probe_argv(abs);
    GSubprocess *p = g_subprocess_newv((const char *const *)argv->pdata,
                                       G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                       G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                       NULL);
    g_ptr_array_unref(argv);
    if (p)
        g_subprocess_communicate_utf8_async(p, NULL, NULL, on_probe_done,
                                            g_strdup(abs));

    if (A.mpv) {
        if (A.ready) {
            set_flag("pause", FALSE);
            cmd("loadfile", abs, NULL);
        } else {
            g_free(A.pending);
            A.pending = g_strdup(abs);
        }
    } else {
        /* Nothing can play it; the tools are what is left, so show them. */
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.edit_btn), TRUE);
    }
    update_placeholder();
    controls_poke();
}

static const char *media_exts[] = {
    ".mp4", ".m4v", ".mkv", ".webm", ".mov", ".avi", ".mpg", ".mpeg",
    ".ogv", ".3gp", ".ts", ".mts", ".m2ts", ".wmv", ".flv",
    ".mp3", ".m4a", ".aac", ".flac", ".wav", ".ogg", ".oga", ".opus",
};

static gboolean is_media(const char *name)
{
    char *low = g_ascii_strdown(name, -1);
    gboolean yes = FALSE;
    for (size_t i = 0; i < G_N_ELEMENTS(media_exts) && !yes; i++)
        yes = g_str_has_suffix(low, media_exts[i]);
    g_free(low);
    return yes;
}

static int cmp_names(gconstpointer a, gconstpointer b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    char *kx = g_utf8_collate_key_for_filename(x, -1);
    char *ky = g_utf8_collate_key_for_filename(y, -1);
    int r = strcmp(kx, ky);
    g_free(kx); g_free(ky);
    return r;
}

/* N and P: the next or previous media file in the same folder, in the
 * order a file manager shows them (file2 before file10). */
static void open_sibling(int dir)
{
    if (!A.file) return;
    char *d = g_path_get_dirname(A.file);
    char *me = g_path_get_basename(A.file);
    GDir *gd = g_dir_open(d, 0, NULL);
    if (!gd) { g_free(d); g_free(me); return; }
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const char *n;
    while ((n = g_dir_read_name(gd)))
        if (is_media(n)) g_ptr_array_add(names, g_strdup(n));
    g_dir_close(gd);
    g_ptr_array_sort(names, cmp_names);
    int idx = -1;
    for (guint i = 0; i < names->len; i++)
        if (strcmp(g_ptr_array_index(names, i), me) == 0) idx = i;
    int next = idx + dir;
    if (idx >= 0 && next >= 0 && next < (int)names->len) {
        char *p = g_build_filename(d, g_ptr_array_index(names, next), NULL);
        player_open(p);
        g_free(p);
    }
    g_ptr_array_unref(names);
    g_free(d); g_free(me);
}

static void on_open_response(GtkNativeDialog *dlg, int resp, gpointer d)
{
    (void)d;
    if (resp == GTK_RESPONSE_ACCEPT) {
        GFile *f = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dlg));
        char *p = f ? g_file_get_path(f) : NULL;
        if (p) player_open(p);
        g_free(p);
        if (f) g_object_unref(f);
    }
    g_object_unref(dlg);
}

static void open_dialog(void)
{
    GtkFileChooserNative *dlg = gtk_file_chooser_native_new(
        T("Open a Video or Music File", "동영상이나 음악 파일 열기"),
        GTK_WINDOW(A.win), GTK_FILE_CHOOSER_ACTION_OPEN,
        T("_Open", "열기(_O)"), T("_Cancel", "취소(_C)"));
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, T("Videos and music", "동영상과 음악"));
    gtk_file_filter_add_mime_type(f, "video/*");
    gtk_file_filter_add_mime_type(f, "audio/*");
    for (size_t i = 0; i < G_N_ELEMENTS(media_exts); i++)
        gtk_file_filter_add_suffix(f, media_exts[i] + 1);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, T("All files", "모든 파일"));
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), all);
    g_object_unref(f);
    g_object_unref(all);

    GFile *dir = NULL;
    if (A.file) {
        char *dd = g_path_get_dirname(A.file);
        dir = g_file_new_for_path(dd);
        g_free(dd);
    } else {
        const char *v = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
        if (v && g_file_test(v, G_FILE_TEST_IS_DIR)) dir = g_file_new_for_path(v);
    }
    if (dir) {
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir, NULL);
        g_object_unref(dir);
    }
    g_signal_connect(dlg, "response", G_CALLBACK(on_open_response), NULL);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dlg));
}

static void on_open(GtkButton *b, gpointer d) { (void)b; (void)d; open_dialog(); }

static gboolean on_drop(GtkDropTarget *t, const GValue *v, double x, double y,
                        gpointer d)
{
    (void)t; (void)x; (void)y; (void)d;
    GFile *f = NULL;
    if (G_VALUE_HOLDS(v, GDK_TYPE_FILE_LIST)) {
        GSList *l = gdk_file_list_get_files(g_value_get_boxed(v));
        if (l) f = l->data;
        g_slist_free(l);
    } else if (G_VALUE_HOLDS(v, G_TYPE_FILE)) {
        f = g_value_get_object(v);
    }
    char *p = f ? g_file_get_path(f) : NULL;
    if (p) player_open(p);
    g_free(p);
    return p != NULL;
}

/* ── keys ──────────────────────────────────────────────────────── */

static gboolean on_key(GtkEventControllerKey *c, guint kv, guint code,
                       GdkModifierType mods, gpointer d)
{
    (void)c; (void)code; (void)d;
    GdkModifierType m = mods & (GDK_CONTROL_MASK | GDK_ALT_MASK |
                                GDK_SUPER_MASK);
    if (m == GDK_CONTROL_MASK) {
        switch (kv) {
        case GDK_KEY_o: case GDK_KEY_O: open_dialog(); return TRUE;
        case GDK_KEY_q: case GDK_KEY_Q:
        case GDK_KEY_w: case GDK_KEY_W:
            gtk_window_close(GTK_WINDOW(A.win)); return TRUE;
        }
        return FALSE;
    }
    if (m) return FALSE;

    switch (kv) {
    case GDK_KEY_Escape:
        if (gtk_window_is_fullscreen(GTK_WINDOW(A.win)))
            gtk_window_unfullscreen(GTK_WINDOW(A.win));
        else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.edit_btn)))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.edit_btn), FALSE);
        else return FALSE;
        return TRUE;
    case GDK_KEY_f: case GDK_KEY_F: case GDK_KEY_F11:
        toggle_fullscreen(); return TRUE;
    }
    if (!A.file) return FALSE;

    switch (kv) {
    case GDK_KEY_space: case GDK_KEY_k: case GDK_KEY_K:
        toggle_pause(); return TRUE;
    case GDK_KEY_Left:  seek_rel(-5); controls_poke(); return TRUE;
    case GDK_KEY_Right: seek_rel(5);  controls_poke(); return TRUE;
    case GDK_KEY_Up:    change_volume(5);  return TRUE;
    case GDK_KEY_Down:  change_volume(-5); return TRUE;
    case GDK_KEY_m: case GDK_KEY_M: toggle_mute(); return TRUE;
    case GDK_KEY_bracketleft:  speed_step(-1); return TRUE;
    case GDK_KEY_bracketright: speed_step(1);  return TRUE;
    case GDK_KEY_n: case GDK_KEY_N: open_sibling(1);  return TRUE;
    case GDK_KEY_p: case GDK_KEY_P: open_sibling(-1); return TRUE;
    case GDK_KEY_period:
        /* frame-step pauses by itself; one frame at a time only makes
         * sense on a still picture. */
        A.play_sel = FALSE;
        cmd("frame-step", NULL, NULL); return TRUE;
    case GDK_KEY_comma:
        A.play_sel = FALSE;
        cmd("frame-back-step", NULL, NULL); return TRUE;
    case GDK_KEY_e: case GDK_KEY_E: toggle_edit(); return TRUE;
    case GDK_KEY_i: case GDK_KEY_I: set_start(); return TRUE;
    case GDK_KEY_o: case GDK_KEY_O: set_end(); return TRUE;
    }
    return FALSE;
}

/* ── building the window ───────────────────────────────────────── */

static const char *css =
    ".lp-video-bg { background-color: black; }\n"
    ".lp-controls { background-color: rgba(14,18,24,0.84);"
    "  border-radius: 14px; padding: 4px 10px; margin: 0 16px 16px 16px;"
    "  color: #f1f5f9; }\n"
    ".lp-controls button, .lp-controls menubutton > button { color: #f1f5f9; }\n"
    ".lp-controls scale { padding-top: 6px; padding-bottom: 6px; }\n"
    ".lp-time { font-feature-settings: \"tnum\"; min-width: 44px; }\n"
    ".lp-ph-title { font-size: 18pt; font-weight: bold; color: #f1f5f9; }\n"
    ".lp-ph-sub { color: rgba(241,245,249,0.65); }\n"
    ".lp-ph-icon { color: rgba(241,245,249,0.55); }\n"
    ".lp-empty-title { font-size: 20pt; font-weight: 800; }\n"
    ".lp-edit { padding: 8px 14px 10px 14px;"
    "  border-top: 1px solid rgba(255,255,255,0.08); }\n"
    ".lp-sel { font-feature-settings: \"tnum\"; }\n"
    ".lp-op-title { font-weight: bold; }\n"
    ".lp-ops flowboxchild { padding: 0; }\n";

static GtkWidget *build_controls(void)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(bar, "lp-controls");
    gtk_widget_set_valign(bar, GTK_ALIGN_END);

    gtk_box_append(GTK_BOX(bar), icon_button("media-seek-backward-symbolic",
        T("Back 10 seconds", "10초 뒤로"), G_CALLBACK(on_back10)));
    A.play_btn = icon_button("media-playback-start-symbolic",
        T("Play (Space)", "재생 (Space)"), G_CALLBACK(on_play));
    gtk_box_append(GTK_BOX(bar), A.play_btn);
    gtk_box_append(GTK_BOX(bar), icon_button("media-seek-forward-symbolic",
        T("Forward 10 seconds", "10초 앞으로"), G_CALLBACK(on_fwd10)));

    A.pos_lbl = gtk_label_new("0:00");
    gtk_widget_add_css_class(A.pos_lbl, "lp-time");
    gtk_widget_set_margin_start(A.pos_lbl, 6);
    gtk_box_append(GTK_BOX(bar), A.pos_lbl);

    A.seek = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.1);
    gtk_scale_set_draw_value(GTK_SCALE(A.seek), FALSE);
    gtk_widget_set_hexpand(A.seek, TRUE);
    gtk_widget_set_tooltip_text(A.seek, T("Position (← →: 5 seconds)",
                                          "위치 (← →: 5초)"));
    g_signal_connect(A.seek, "change-value", G_CALLBACK(on_seek_change), NULL);
    gtk_box_append(GTK_BOX(bar), A.seek);

    A.dur_lbl = gtk_label_new("0:00");
    gtk_widget_add_css_class(A.dur_lbl, "lp-time");
    gtk_widget_set_margin_end(A.dur_lbl, 6);
    gtk_box_append(GTK_BOX(bar), A.dur_lbl);

    A.mute_btn = icon_button("audio-volume-high-symbolic",
        T("Mute (M)", "음소거 (M)"), G_CALLBACK(on_mute));
    gtk_box_append(GTK_BOX(bar), A.mute_btn);
    A.vol = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(A.vol), FALSE);
    gtk_widget_set_size_request(A.vol, 96, -1);
    gtk_widget_set_tooltip_text(A.vol, T("Volume (↑ ↓)", "음량 (↑ ↓)"));
    gtk_range_set_value(GTK_RANGE(A.vol), 100);
    g_signal_connect(A.vol, "value-changed", G_CALLBACK(on_volume), NULL);
    gtk_box_append(GTK_BOX(bar), A.vol);

    GMenu *sm = g_menu_new();
    for (size_t i = 0; i < G_N_ELEMENTS(speeds); i++) {
        char buf[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(buf, sizeof buf, "%g", speeds[i]);
        char *label = speeds[i] == 1.0 ? g_strdup(T("Normal", "보통"))
                                       : g_strdup_printf("%s×", buf);
        /* Always with a decimal point: "win.speed(1)" would parse as an
         * integer and never match the action's double state. */
        char dbuf[G_ASCII_DTOSTR_BUF_SIZE];
        char *act = g_strdup_printf("win.speed(%s)",
            g_ascii_formatd(dbuf, sizeof dbuf, "%.2f", speeds[i]));
        g_menu_append(sm, label, act);
        g_free(label); g_free(act);
    }
    A.speed_btn = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(A.speed_btn), "1×");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(A.speed_btn),
                                   G_MENU_MODEL(sm));
    gtk_menu_button_set_direction(GTK_MENU_BUTTON(A.speed_btn), GTK_ARROW_UP);
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(A.speed_btn), FALSE);
    gtk_widget_set_tooltip_text(A.speed_btn, T("Playback speed ([ ])",
                                               "재생 속도 ([ ])"));
    g_object_unref(sm);
    gtk_box_append(GTK_BOX(bar), A.speed_btn);

    A.loop_btn = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(A.loop_btn),
                             "media-playlist-repeat-symbolic");
    gtk_widget_add_css_class(A.loop_btn, "flat");
    gtk_widget_set_tooltip_text(A.loop_btn, T("Repeat", "반복"));
    g_signal_connect(A.loop_btn, "toggled", G_CALLBACK(on_loop), NULL);
    gtk_box_append(GTK_BOX(bar), A.loop_btn);

    A.fs_btn = icon_button("view-fullscreen-symbolic",
        T("Full screen (F)", "전체 화면 (F)"), G_CALLBACK(on_fs));
    gtk_box_append(GTK_BOX(bar), A.fs_btn);

    GtkEventController *mc = gtk_event_controller_motion_new();
    g_signal_connect(mc, "enter", G_CALLBACK(on_controls_enter), NULL);
    g_signal_connect(mc, "leave", G_CALLBACK(on_controls_leave), NULL);
    gtk_widget_add_controller(bar, mc);
    return bar;
}

static GtkWidget *build_edit_panel(void)
{
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(v, "lp-edit");

    A.timeline = gtk_drawing_area_new();
    gtk_widget_set_size_request(A.timeline, -1, 44);
    gtk_widget_set_hexpand(A.timeline, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(A.timeline), tl_draw,
                                   NULL, NULL);
    gtk_widget_set_tooltip_text(A.timeline,
        T("Drag the handles to choose a part; click to jump there.",
          "손잡이를 끌어 구간을 고르고, 누르면 그 자리로 갑니다."));
    GtkGesture *g = gtk_gesture_drag_new();
    g_signal_connect(g, "drag-begin", G_CALLBACK(tl_begin), NULL);
    g_signal_connect(g, "drag-update", G_CALLBACK(tl_update), NULL);
    gtk_widget_add_controller(A.timeline, GTK_EVENT_CONTROLLER(g));
    GtkEventController *mc = gtk_event_controller_motion_new();
    g_signal_connect(mc, "motion", G_CALLBACK(tl_motion), NULL);
    gtk_widget_add_controller(A.timeline, mc);
    gtk_box_append(GTK_BOX(v), A.timeline);

    /* start ... selection ... end */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *bs = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(bs), label_icon_box("go-first-symbolic",
                         T("Set start", "시작 지점")));
    gtk_widget_set_tooltip_text(bs, T("Start the selection here (I)",
                                      "지금 위치를 구간의 시작으로 (I)"));
    g_signal_connect(bs, "clicked", G_CALLBACK(on_set_start), NULL);
    gtk_box_append(GTK_BOX(row), bs);
    A.start_lbl = gtk_label_new("");
    gtk_widget_add_css_class(A.start_lbl, "lp-sel");
    gtk_box_append(GTK_BOX(row), A.start_lbl);

    GtkWidget *mid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_hexpand(mid, TRUE);
    gtk_widget_set_halign(mid, GTK_ALIGN_CENTER);
    GtkWidget *bp = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(bp), label_icon_box(
        "media-playback-start-symbolic", T("Play selection", "구간 재생")));
    gtk_widget_set_tooltip_text(bp, T("Play only the selected part",
                                      "선택한 부분만 재생"));
    g_signal_connect(bp, "clicked", G_CALLBACK(on_play_sel), NULL);
    gtk_box_append(GTK_BOX(mid), bp);
    A.len_lbl = gtk_label_new("");
    gtk_widget_add_css_class(A.len_lbl, "lp-sel");
    gtk_widget_add_css_class(A.len_lbl, "dim-label");
    gtk_box_append(GTK_BOX(mid), A.len_lbl);
    GtkWidget *br = icon_button("edit-clear-symbolic",
        T("Select the whole file", "파일 전체 선택"), G_CALLBACK(on_reset_sel));
    gtk_box_append(GTK_BOX(mid), br);
    gtk_box_append(GTK_BOX(row), mid);

    A.end_lbl = gtk_label_new("");
    gtk_widget_add_css_class(A.end_lbl, "lp-sel");
    gtk_box_append(GTK_BOX(row), A.end_lbl);
    GtkWidget *be = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(be), label_icon_box("go-last-symbolic",
                         T("Set end", "끝 지점")));
    gtk_widget_set_tooltip_text(be, T("End the selection here (O)",
                                      "지금 위치를 구간의 끝으로 (O)"));
    g_signal_connect(be, "clicked", G_CALLBACK(on_set_end), NULL);
    gtk_box_append(GTK_BOX(row), be);
    gtk_box_append(GTK_BOX(v), row);

    /* the tools */
    GtkWidget *fb = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(fb), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(fb), 8);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(fb), 6);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(fb), 6);
    gtk_widget_add_css_class(fb, "lp-ops");
    /* One even row of tools across the panel; two rows of four when the
     * window is too narrow for one. */
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(fb), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(fb), 4);
    A.ops_box = fb;

    const item_t trim[] = {
        { OP_TRIM_FAST, T("Fast (no re-encode)", "빠르게 (다시 인코딩 안 함)"),
          T("Takes seconds and keeps full quality. The cut snaps to the "
            "nearest key frame, so it may start a little early.",
            "몇 초면 끝나고 화질이 그대로입니다. 자르는 자리가 가까운 "
            "키 프레임에 맞춰져서 조금 일찍 시작할 수 있습니다.") },
        { OP_TRIM_PRECISE, T("Precise", "정확하게"),
          T("Cuts exactly at the marks. Re-encodes, so it takes longer.",
            "표시한 자리에서 정확히 자릅니다. 다시 인코딩하므로 오래 걸립니다.") },
    };
    gtk_flow_box_append(GTK_FLOW_BOX(fb), op_menu("edit-cut-symbolic",
        T("Trim", "자르기"),
        T("Keep only the selected part. Fast cuts snap to key frames.",
          "선택한 부분만 남깁니다. 빠른 자르기는 키 프레임에 맞춰집니다."),
        trim, 2));

    gtk_flow_box_append(GTK_FLOW_BOX(fb), op_button(OP_CUT,
        "edit-delete-symbolic", T("Remove part", "부분 삭제"),
        T("Cut the selected part out and join the rest",
          "선택한 부분을 빼고 나머지를 이어 붙입니다")));

    const item_t gif[] = {
        { OP_GIF_480, T("Small (480 px wide)", "작게 (너비 480)"), NULL },
        { OP_GIF_720, T("Large (720 px wide)", "크게 (너비 720)"), NULL },
    };
    gtk_flow_box_append(GTK_FLOW_BOX(fb), op_menu("image-x-generic-symbolic",
        T("Make GIF", "GIF 만들기"),
        T("An animated GIF of the selected part (12 frames a second)",
          "선택한 부분으로 움직이는 GIF 를 만듭니다 (초당 12장)"), gif, 2));

    const item_t aud[] = {
        { OP_AUDIO_MP3, "MP3", T("Plays everywhere (192 kbit/s)",
                                 "어디서나 재생됩니다 (192 kbit/s)") },
        { OP_AUDIO_M4A, "M4A (AAC)", T("Smaller at the same quality",
                                       "같은 음질에 더 작습니다") },
    };
    gtk_flow_box_append(GTK_FLOW_BOX(fb), op_menu("audio-x-generic-symbolic",
        T("Extract audio", "소리 추출"),
        T("Save the sound of the selected part as a music file",
          "선택한 부분의 소리를 음악 파일로 저장합니다"), aud, 2));

    gtk_flow_box_append(GTK_FLOW_BOX(fb), op_button(OP_FRAME,
        "camera-photo-symbolic", T("Save frame", "장면 저장"),
        T("Save the picture at the current position as PNG",
          "지금 위치의 화면을 PNG 로 저장합니다")));

    const item_t rot[] = {
        { OP_ROT_LEFT, T("Rotate left", "왼쪽으로 돌리기"),
          T("90° counter-clockwise", "반시계 방향 90°") },
        { OP_ROT_RIGHT, T("Rotate right", "오른쪽으로 돌리기"),
          T("90° clockwise", "시계 방향 90°") },
    };
    gtk_flow_box_append(GTK_FLOW_BOX(fb), op_menu("object-rotate-right-symbolic",
        T("Rotate", "회전"),
        T("Turn a sideways video upright (whole file)",
          "옆으로 누운 동영상을 바로 세웁니다 (파일 전체)"), rot, 2));

    gtk_flow_box_append(GTK_FLOW_BOX(fb), op_button(OP_NO_AUDIO,
        "audio-volume-muted-symbolic", T("Remove audio", "소리 없애기"),
        T("The same video without sound (whole file, no re-encode)",
          "소리만 뺀 같은 동영상 (파일 전체, 다시 인코딩 안 함)")));

    const item_t size[] = {
        { OP_SIZE_1080, "1080p", T("Full HD", "풀 HD") },
        { OP_SIZE_720, "720p", T("HD - good for sharing", "HD - 공유하기 좋음") },
        { OP_SIZE_480, "480p", T("Smallest file", "가장 작은 파일") },
    };
    gtk_flow_box_append(GTK_FLOW_BOX(fb), op_menu("zoom-fit-best-symbolic",
        T("Resize", "크기 줄이기"),
        T("Make the file smaller: lower resolution, re-encoded (whole file)",
          "해상도를 낮추고 다시 인코딩해 파일을 줄입니다 (파일 전체)"),
        size, 3));
    gtk_box_append(GTK_BOX(v), fb);

    /* status line and "where to save" */
    GtkWidget *srow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    A.status = gtk_stack_new();
    gtk_widget_set_hexpand(A.status, TRUE);
    gtk_stack_set_transition_type(GTK_STACK(A.status),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    A.idle_lbl = gtk_label_new("");
    gtk_widget_add_css_class(A.idle_lbl, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(A.idle_lbl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(A.idle_lbl), PANGO_ELLIPSIZE_END);
    gtk_stack_add_named(GTK_STACK(A.status), A.idle_lbl, "idle");

    GtkWidget *busy = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    A.busy_lbl = gtk_label_new("");
    gtk_box_append(GTK_BOX(busy), A.busy_lbl);
    A.progress = gtk_progress_bar_new();
    gtk_widget_set_hexpand(A.progress, TRUE);
    gtk_widget_set_valign(A.progress, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(busy), A.progress);
    GtkWidget *cancel = gtk_button_new_with_label(T("Cancel", "취소"));
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel), NULL);
    gtk_box_append(GTK_BOX(busy), cancel);
    gtk_stack_add_named(GTK_STACK(A.status), busy, "busy");

    GtkWidget *done = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *ok = gtk_image_new_from_icon_name("emblem-ok-symbolic");
    gtk_widget_add_css_class(ok, "success");
    gtk_box_append(GTK_BOX(done), ok);
    A.done_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(A.done_lbl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(A.done_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(A.done_lbl, TRUE);
    gtk_box_append(GTK_BOX(done), A.done_lbl);
    GtkWidget *bf = gtk_button_new_with_label(T("Open folder", "폴더 열기"));
    g_signal_connect(bf, "clicked", G_CALLBACK(on_open_folder), NULL);
    gtk_box_append(GTK_BOX(done), bf);
    GtkWidget *bpr = gtk_button_new_with_label(T("Play result", "결과 보기"));
    gtk_widget_add_css_class(bpr, "suggested-action");
    g_signal_connect(bpr, "clicked", G_CALLBACK(on_play_result), NULL);
    gtk_box_append(GTK_BOX(done), bpr);
    gtk_box_append(GTK_BOX(done), icon_button("window-close-symbolic",
        T("Dismiss", "닫기"), G_CALLBACK(on_dismiss)));
    gtk_stack_add_named(GTK_STACK(A.status), done, "done");

    GtkWidget *errb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *ei = gtk_image_new_from_icon_name("dialog-error-symbolic");
    gtk_widget_add_css_class(ei, "error");
    gtk_widget_set_valign(ei, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(errb), ei);
    A.err_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(A.err_lbl), 0);
    gtk_label_set_wrap(GTK_LABEL(A.err_lbl), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(A.err_lbl), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(A.err_lbl), TRUE);
    gtk_label_set_lines(GTK_LABEL(A.err_lbl), 5);
    gtk_label_set_ellipsize(GTK_LABEL(A.err_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(A.err_lbl, TRUE);
    gtk_box_append(GTK_BOX(errb), A.err_lbl);
    GtkWidget *dismiss = icon_button("window-close-symbolic",
        T("Dismiss", "닫기"), G_CALLBACK(on_dismiss));
    gtk_widget_set_valign(dismiss, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(errb), dismiss);
    gtk_stack_add_named(GTK_STACK(A.status), errb, "error");
    gtk_box_append(GTK_BOX(srow), A.status);

    A.saveas = gtk_check_button_new_with_label(T("Ask where to save",
                                                 "저장할 곳 묻기"));
    gtk_widget_set_tooltip_text(A.saveas,
        T("Off: results are saved next to the original, e.g. “name-trim.mp4”.",
          "끄면 결과를 원본 옆에 저장합니다 (예: “이름-trim.mp4”)."));
    gtk_widget_set_valign(A.saveas, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(srow), A.saveas);
    gtk_box_append(GTK_BOX(v), srow);

    A.edit_rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(A.edit_rev),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_child(GTK_REVEALER(A.edit_rev), v);
    return A.edit_rev;
}

static GtkWidget *build_empty(void)
{
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(v, GTK_ALIGN_CENTER);
    GtkWidget *im = gtk_image_new_from_icon_name("video-x-generic-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(im), 96);
    gtk_widget_add_css_class(im, "dim-label");
    gtk_box_append(GTK_BOX(v), im);
    GtkWidget *t = gtk_label_new(T("No video open", "열린 동영상이 없습니다"));
    gtk_widget_add_css_class(t, "lp-empty-title");
    gtk_box_append(GTK_BOX(v), t);
    GtkWidget *s = gtk_label_new(T("Open a video or music file, or drop one here.",
                                   "동영상이나 음악 파일을 열거나 여기에 끌어다 놓으세요."));
    gtk_widget_add_css_class(s, "dim-label");
    gtk_box_append(GTK_BOX(v), s);
    GtkWidget *b = gtk_button_new_with_mnemonic(T("_Open…", "열기(_O)…"));
    gtk_widget_add_css_class(b, "suggested-action");
    gtk_widget_add_css_class(b, "pill");
    gtk_widget_set_halign(b, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(b, 6);
    g_signal_connect(b, "clicked", G_CALLBACK(on_open), NULL);
    gtk_box_append(GTK_BOX(v), b);
    return v;
}

static void build_window(GtkApplication *gapp)
{
    if (A.win) return;
    A.op_menus = g_ptr_array_new();
    A.speed = 1.0;
    A.volume = 100;
    A.paused = TRUE;

    /* A player is looked at in the dark: ask the theme for its dark
     * variant. A theme that is dark already is unaffected. */
    g_object_set(gtk_settings_get_default(),
                 "gtk-application-prefer-dark-theme", TRUE, NULL);

    GtkCssProvider *cp = gtk_css_provider_new();
    gtk_css_provider_load_from_data(cp, css, -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(cp), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(cp);

    A.win = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(A.win), T("Videos", "동영상"));
    lp_fit_default_size(GTK_WINDOW(A.win), 1100, 700);
    gtk_widget_set_size_request(A.win, 560, 400);

    A.header = gtk_header_bar_new();
    A.title = gtk_label_new(T("Videos", "동영상"));
    gtk_widget_add_css_class(A.title, "title");
    gtk_label_set_ellipsize(GTK_LABEL(A.title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(A.title), 60);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(A.header), A.title);
    GtkWidget *ob = gtk_button_new_from_icon_name("document-open-symbolic");
    gtk_widget_set_tooltip_text(ob, T("Open a file (Ctrl+O)", "파일 열기 (Ctrl+O)"));
    g_signal_connect(ob, "clicked", G_CALLBACK(on_open), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(A.header), ob);
    A.edit_btn = gtk_toggle_button_new();
    gtk_button_set_child(GTK_BUTTON(A.edit_btn),
                         label_icon_box("edit-cut-symbolic", T("Edit", "편집")));
    gtk_widget_set_tooltip_text(A.edit_btn,
        T("Trim, cut out, convert (E)", "자르기, 부분 삭제, 변환 (E)"));
    gtk_widget_set_sensitive(A.edit_btn, FALSE);
    g_signal_connect(A.edit_btn, "toggled", G_CALLBACK(on_edit_toggled), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(A.header), A.edit_btn);
    gtk_window_set_titlebar(GTK_WINDOW(A.win), A.header);

    /* The speed menu's radio items. */
    A.speed_action = g_simple_action_new_stateful("speed",
        G_VARIANT_TYPE_DOUBLE, g_variant_new_double(1.0));
    g_signal_connect(A.speed_action, "activate",
                     G_CALLBACK(on_speed_action), NULL);
    g_action_map_add_action(G_ACTION_MAP(A.win), G_ACTION(A.speed_action));

    A.stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(A.stack), build_empty(), "empty");

    GtkWidget *pv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    A.overlay = gtk_overlay_new();
    gtk_widget_add_css_class(A.overlay, "lp-video-bg");
    gtk_widget_set_vexpand(A.overlay, TRUE);
    make_video_widget();

    /* Over the picture: the placeholder for music / errors ... */
    A.ph = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(A.ph, "lp-video-bg");
    gtk_widget_set_hexpand(A.ph, TRUE);
    gtk_widget_set_vexpand(A.ph, TRUE);
    GtkWidget *phc = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_valign(phc, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(phc, TRUE);
    gtk_widget_set_margin_bottom(phc, 60);
    A.ph_icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(A.ph_icon), 128);
    gtk_widget_add_css_class(A.ph_icon, "lp-ph-icon");
    gtk_box_append(GTK_BOX(phc), A.ph_icon);
    A.ph_title = gtk_label_new("");
    gtk_widget_add_css_class(A.ph_title, "lp-ph-title");
    gtk_label_set_wrap(GTK_LABEL(A.ph_title), TRUE);
    gtk_label_set_justify(GTK_LABEL(A.ph_title), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(phc), A.ph_title);
    A.ph_sub = gtk_label_new("");
    gtk_widget_add_css_class(A.ph_sub, "lp-ph-sub");
    gtk_label_set_wrap(GTK_LABEL(A.ph_sub), TRUE);
    gtk_label_set_justify(GTK_LABEL(A.ph_sub), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(A.ph_sub), 60);
    gtk_box_append(GTK_BOX(phc), A.ph_sub);
    gtk_box_append(GTK_BOX(A.ph), phc);
    gtk_widget_set_visible(A.ph, FALSE);
    GtkGesture *pg = gtk_gesture_click_new();
    g_signal_connect(pg, "pressed", G_CALLBACK(on_video_click), NULL);
    gtk_widget_add_controller(A.ph, GTK_EVENT_CONTROLLER(pg));
    gtk_overlay_add_overlay(GTK_OVERLAY(A.overlay), A.ph);

    /* ... and the floating controls. */
    A.controls_rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(A.controls_rev),
                                     GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_revealer_set_transition_duration(GTK_REVEALER(A.controls_rev), 250);
    gtk_widget_set_valign(A.controls_rev, GTK_ALIGN_END);
    A.controls = build_controls();
    gtk_revealer_set_child(GTK_REVEALER(A.controls_rev), A.controls);
    gtk_revealer_set_reveal_child(GTK_REVEALER(A.controls_rev), TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(A.overlay), A.controls_rev);

    GtkEventController *mc = gtk_event_controller_motion_new();
    g_signal_connect(mc, "motion", G_CALLBACK(on_motion), NULL);
    gtk_widget_add_controller(A.overlay, mc);

    gtk_box_append(GTK_BOX(pv), A.overlay);
    gtk_box_append(GTK_BOX(pv), build_edit_panel());
    gtk_stack_add_named(GTK_STACK(A.stack), pv, "player");
    gtk_stack_set_visible_child_name(GTK_STACK(A.stack), "empty");
    gtk_window_set_child(GTK_WINDOW(A.win), A.stack);

    GtkDropTarget *dt = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY);
    GType types[] = { GDK_TYPE_FILE_LIST, G_TYPE_FILE };
    gtk_drop_target_set_gtypes(dt, types, G_N_ELEMENTS(types));
    g_signal_connect(dt, "drop", G_CALLBACK(on_drop), NULL);
    gtk_widget_add_controller(A.win, GTK_EVENT_CONTROLLER(dt));

    /* Capture phase: the keys are the player's even when a button has
     * the focus - Space must pause, not press that button again. */
    GtkEventController *kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), NULL);
    gtk_widget_add_controller(A.win, kc);

    g_signal_connect(A.win, "notify::fullscreened",
                     G_CALLBACK(on_fullscreen_changed), NULL);

    sync_play_icon();
    sync_volume();
    sync_selection();
    on_dismiss(NULL, NULL);
    update_ops();
}

static void on_activate(GtkApplication *gapp, gpointer d)
{
    (void)d;
    build_window(gapp);
    gtk_window_present(GTK_WINDOW(A.win));
}

static void on_app_open(GApplication *gapp, GFile **files, int n,
                        const char *hint, gpointer d)
{
    (void)hint; (void)d;
    build_window(GTK_APPLICATION(gapp));
    gtk_window_present(GTK_WINDOW(A.win));
    if (n > 0) {
        char *p = g_file_get_path(files[0]);
        if (p) player_open(p);
        g_free(p);
    }
}

static void on_startup(GApplication *gapp, gpointer d)
{
    (void)gapp; (void)d;
    player_init();
}

static void on_shutdown(GApplication *gapp, gpointer d)
{
    (void)gapp; (void)d;
    if (A.job) {
        A.job_cancelled = TRUE;
        g_subprocess_force_exit(A.job);
        if (A.job_out) g_unlink(A.job_out);
        if (A.job_err) g_unlink(A.job_err);
    }
    /* The render context first: mpv_terminate_destroy waits for the
     * video output, which waits for the render context to go. */
    if (A.rctx) { M.render_context_free(A.rctx); A.rctx = NULL; }
    if (A.mpv) { M.terminate_destroy(A.mpv); A.mpv = NULL; }
}

int main(int argc, char **argv)
{
    if (g_getenv("LP_VIDEO_SELFTEST")) {
        int rc = 0;
        for (int i = 1; i < argc; i++) rc |= edit_selftest(argv[i]);
        return argc > 1 ? rc : 2;
    }
    GtkApplication *app = gtk_application_new("org.lpzero.Video",
                                              G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_app_open), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return r;
}
