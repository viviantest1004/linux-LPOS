/*
 * quick.c - lp-quick, quick settings: the panel under the status icons.
 *
 *     ┌──────────────────────────────────────┐
 *     │ 🔊 ───────────────────────●   100%   │
 *     │ ☀  ─────────●──────────────    40%   │
 *     │ ◔  CAFE_FREE_WIFI   27% · no LAN   › │
 *     │    CPU     ▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬     12%   │
 *     │    Memory  ▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬     34%   │
 *     │    Disk    ▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬     51%   │
 *     │ ( Network On )  ( Dark Style )       │
 *     │ ( Settings   )  ( Lock       )       │
 *     │ ──────────────────────────────────── │
 *     │ (U) User Name                        │
 *     │     Log Out                          │
 *     │     Restart…                         │
 *     │     Power Off                        │
 *     └──────────────────────────────────────┘
 *
 * That is the owner's mockup, top to bottom, and this file draws exactly
 * it. The two toggle tiles are orange when on. The Wi-Fi row opens a
 * page with the networks around; everything else acts where it is.
 *
 * ── why this is GTK 3 now ──
 *
 * The first quick menu was a GTK 4 window that asked sway, after it had
 * appeared, to move it under the bar - gtk4-layer-shell is not in Debian
 * 12, so it could not be a layer surface. That meant it could not slide
 * (a toplevel is placed by the compositor, not drawn where the panel
 * wants), it could not be dragged down from the top edge, and on wayfire,
 * which has no IPC to move a window, it opened wherever wayfire put it.
 * GTK 3 has gtk-layer-shell 0.8, and every other part of the shell is
 * already GTK 3, so this is one of them now: a sheet (desktop/common/
 * lp-sheet.c) anchored under the bar at the right edge.
 *
 * ── motion ──
 *
 * The panel slides down and fades on the sheet spring (260 ms in, 182 ms
 * out) and can be caught and turned round half way. The top bar hands it
 * a finger dragging down from the top edge (drag-px / release-px below),
 * which it follows 1:1 and lets go of with the finger's velocity. The two
 * toggle tiles fill on the knob spring - with its one pixel of overshoot -
 * rather than switching colour in a frame. The sliders are GtkScale, which
 * already follows the finger 1:1; what this file adds is that dragging
 * never queues more than one wpctl or lp-tune behind the finger. The three
 * meters are sampled once a second, only while the panel is open, and
 * each bar glides to its new value on a spring instead of jumping; on
 * opening they start at their current values rather than growing in,
 * because a graph that animates in every time is noise.
 *
 * ── where each thing comes from ──
 *
 *   volume        wpctl get-volume / set-volume / set-mute
 *   brightness    lp-tune status --json, lp-tune brightness set (lp-tune
 *                 is the one backlight writer and remembers the value)
 *   Wi-Fi         lp-net status --json, scan --json, connect, radio
 *   CPU/mem/disk  /proc/stat, /proc/meminfo, statvfs("/")
 *   dark style    ~/.config/lp/style, via lp_style_set_light()
 *   power         lp-logout, lp-power restart|off, lp-reboot-recovery
 *
 * Every command runs asynchronously; the panel never waits on one.
 *
 * ── what it remembers ──
 *
 * The last volume and mute, in ~/.config/lp/volume ("0.40 unmuted"),
 * written atomically and durably a moment after the slider stops, and put
 * back once per session when this program first starts - WirePlumber
 * keeps its own copy too, but without an fsync of the directory, and a
 * power cut soon after a change can lose it. Dark style is lp-shell's
 * file. Brightness is lp-tune's.
 *
 * ── the power actions ──
 *
 * Each one asks first, in a dialog that dims the screen. Restart into
 * Recovery is there only when the boot-recovery track's
 * lp-reboot-recovery is installed, and it asks for nothing itself: that
 * command goes through lp-privd, and lp-privd is what asks for the
 * person's password (COMMON.md, Administrator rights). A button that
 * could reboot into a root shell without that would be the hole.
 *
 * Commands (lp-quick is one process; running it again hands it argv):
 *   lp-quick [toggle [MON]] | show [MON] | hide | daemon | refresh
 *   lp-quick drag-px PX [MON] | release-px VELOCITY_PX_PER_S
 */
#define _GNU_SOURCE 1

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

#include "lp-json.h"
#include "lp-motion.h"
#include "lp-sheet.h"
#include "lp-shell.h"

#define CARD_WIDTH 372          /* the card itself; CSS adds the shadow room */

/* ── meters ──────────────────────────────────────────────────────── */

typedef struct {
    GtkWidget *area;
    GtkWidget *value;
    LpSpring   s;               /* fraction 0..1, gliding between samples */
    double     hot;             /* above this the bar turns amber */
} Meter;

/* ── the knob-spring tiles ───────────────────────────────────────── */

typedef struct {
    GtkWidget *button;
    GtkWidget *label;
    LpSpring   on;
    LpMotion  *motion;
    gboolean   state;
} Tile;

static struct {
    LpSheet   *sheet;
    GtkWidget *card;
    GtkWidget *stack;

    GtkWidget *vol_icon, *vol_scale, *vol_pct;
    gboolean   vol_known, vol_muted, vol_busy, vol_quiet;
    double     vol_pending;
    guint      vol_save_id;

    GtkWidget *bri_row, *bri_scale, *bri_pct;
    gboolean   bri_busy, bri_quiet;
    int        bri_pending;

    GtkWidget *wifi_icon, *wifi_name, *wifi_value;
    gboolean   radio_on, net_busy;

    Meter      meter[3];
    LpMotion  *meters_motion;
    guint      sample_id;
    unsigned long long cpu_total, cpu_idle;

    Tile       net, dark;
    GtkWidget *recovery;

    GtkWidget *net_list, *net_msg, *pw_box, *pw_entry, *pw_label;
    char      *pw_ssid;

    int        drag_mon;
    gboolean   dragging;
} Q = { .vol_pending = -1, .bri_pending = -1, .drag_mon = -1 };

static void refresh(void);
static void confirm(const char *what);

/* ── tell the top bar ────────────────────────────────────────────── */

static void tell_panel(gboolean open)
{
    const char *a[] = { "lp-panel", "open", "quick", open ? "1" : "0", NULL };
    lp_send("panel", a);
}

static void on_closed(LpSheet *sh, gpointer d)
{
    (void)sh; (void)d;
    tell_panel(FALSE);
    if (Q.sample_id) {
        g_source_remove(Q.sample_id);
        Q.sample_id = 0;
    }
    /* Back to the first page for next time, with no transition: nobody
     * is looking. */
    gtk_stack_set_visible_child_full(GTK_STACK(Q.stack), "main",
                                     GTK_STACK_TRANSITION_TYPE_NONE);
}

/* ── volume ──────────────────────────────────────────────────────── */

static void vol_paint(double v)
{
    const char *icon = Q.vol_muted || v <= 0.001 ? "audio-volume-muted-symbolic"
                     : v < 0.34 ? "audio-volume-low-symbolic"
                     : v < 0.67 ? "audio-volume-medium-symbolic"
                                : "audio-volume-high-symbolic";
    gtk_image_set_from_icon_name(GTK_IMAGE(Q.vol_icon), icon, GTK_ICON_SIZE_BUTTON);
    char pct[16];
    g_snprintf(pct, sizeof pct, "%d%%", (int)lround(v * 100));
    gtk_label_set_text(GTK_LABEL(Q.vol_pct), pct);
}

static gboolean vol_save(gpointer d)
{
    (void)d;
    Q.vol_save_id = 0;
    char buf[48];
    g_snprintf(buf, sizeof buf, "%.2f %s\n",
               gtk_range_get_value(GTK_RANGE(Q.vol_scale)) / 100.0,
               Q.vol_muted ? "muted" : "unmuted");
    lp_config_write("volume", buf);
    return G_SOURCE_REMOVE;
}

static void vol_save_soon(void)
{
    /* After the finger has stopped, not on every step of the drag: a
     * durable write is an fsync, and a slider drag is a hundred values. */
    if (Q.vol_save_id)
        g_source_remove(Q.vol_save_id);
    Q.vol_save_id = g_timeout_add(1200, vol_save, NULL);
}

static void vol_apply(void);

static void vol_set_done(const char *out, gpointer d)
{
    (void)out; (void)d;
    Q.vol_busy = FALSE;
    vol_apply();
}

/* One wpctl at a time; whatever the finger did meanwhile collapses into
 * the newest value. Without this a fast drag queued forty processes and
 * the sound kept changing for a second after the finger stopped. */
static void vol_apply(void)
{
    if (Q.vol_busy || Q.vol_pending < 0)
        return;
    char v[16];
    g_snprintf(v, sizeof v, "%.2f", Q.vol_pending);
    Q.vol_pending = -1;
    Q.vol_busy = TRUE;
    const char *a[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", v, NULL };
    lp_run_async(a, vol_set_done, NULL);
}

static void on_vol_changed(GtkRange *r, gpointer d)
{
    (void)d;
    double v = gtk_range_get_value(r) / 100.0;
    vol_paint(v);
    if (Q.vol_quiet)
        return;
    Q.vol_pending = v;
    vol_apply();
    vol_save_soon();
}

static void mute_done(const char *out, gpointer d)
{
    (void)out; (void)d;
    const char *a[] = { "lp-panel", "refresh", NULL };
    lp_send("panel", a);
}

static void on_vol_icon(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!Q.vol_known)
        return;
    Q.vol_muted = !Q.vol_muted;
    vol_paint(gtk_range_get_value(GTK_RANGE(Q.vol_scale)) / 100.0);
    const char *a[] = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@",
                        Q.vol_muted ? "1" : "0", NULL };
    lp_run_async(a, mute_done, NULL);
    vol_save_soon();
}

static void vol_got(const char *out, gpointer d)
{
    (void)d;
    double v = 0;
    Q.vol_known = out && sscanf(out, "Volume: %lf", &v) == 1;
    Q.vol_muted = out && strstr(out, "MUTED");
    gtk_widget_set_sensitive(Q.vol_scale, Q.vol_known);
    Q.vol_quiet = TRUE;
    gtk_range_set_value(GTK_RANGE(Q.vol_scale), v * 100.0);
    Q.vol_quiet = FALSE;
    vol_paint(v);
    lp_sheet_invalidate(Q.sheet);
}

/* Once per session: put back the volume this panel last saved. The
 * runtime-dir marker is what makes it once - lp-quick restarted after a
 * crash must not undo a change made since login. */
static int restore_tries;

static gboolean restore_try(gpointer d);

static void restore_answer(const char *out, gpointer d)
{
    (void)d;
    if (!out) {
        if (++restore_tries < 10)
            g_timeout_add_seconds(1, restore_try, NULL);
        return;
    }
    char *saved = lp_config_read("volume");
    double v;
    char state[16] = "";
    if (saved && sscanf(saved, "%lf %15s", &v, state) >= 1 && v >= 0 && v <= 1.5) {
        char vs[16];
        g_snprintf(vs, sizeof vs, "%.2f", v);
        const char *a[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", vs, NULL };
        lp_spawn(a);
        const char *m[] = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@",
                            strcmp(state, "muted") == 0 ? "1" : "0", NULL };
        lp_spawn(m);
    }
    g_free(saved);
    char *mark = g_build_filename(g_get_user_runtime_dir(), "lp-volume-restored", NULL);
    g_file_set_contents(mark, "", 0, NULL);
    g_free(mark);
}

static gboolean restore_try(gpointer d)
{
    (void)d;
    const char *a[] = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL };
    lp_run_async(a, restore_answer, NULL);
    return G_SOURCE_REMOVE;
}

static void restore_volume(void)
{
    char *mark = g_build_filename(g_get_user_runtime_dir(), "lp-volume-restored", NULL);
    gboolean done = g_file_test(mark, G_FILE_TEST_EXISTS);
    g_free(mark);
    if (!done && lp_have("wpctl"))
        restore_try(NULL);
}

/* ── brightness ──────────────────────────────────────────────────── */

static void bri_apply(void);

static void bri_set_done(const char *out, gpointer d)
{
    (void)out; (void)d;
    Q.bri_busy = FALSE;
    bri_apply();
}

static void bri_apply(void)
{
    if (Q.bri_busy || Q.bri_pending < 0)
        return;
    char v[8];
    g_snprintf(v, sizeof v, "%d", Q.bri_pending);
    Q.bri_pending = -1;
    Q.bri_busy = TRUE;
    const char *a[] = { "lp-tune", "brightness", "set", v, NULL };
    lp_run_async(a, bri_set_done, NULL);
}

static void on_bri_changed(GtkRange *r, gpointer d)
{
    (void)d;
    int v = (int)lround(gtk_range_get_value(r));
    char pct[16];
    g_snprintf(pct, sizeof pct, "%d%%", v);
    gtk_label_set_text(GTK_LABEL(Q.bri_pct), pct);
    if (Q.bri_quiet)
        return;
    Q.bri_pending = v;
    bri_apply();
}

static void tune_got(const char *out, gpointer d)
{
    (void)d;
    LpJson *j = lp_json_parse(out);
    /* No lp-tune, or a machine with no backlight: no slider at all. A
     * slider that moves nothing reads as broken. */
    double b = j ? lp_json_num(j, "brightness", -1) : -1;
    gtk_widget_set_visible(Q.bri_row, b >= 0);
    if (b >= 0) {
        Q.bri_quiet = TRUE;
        gtk_range_set_value(GTK_RANGE(Q.bri_scale), b);
        Q.bri_quiet = FALSE;
        on_bri_changed(GTK_RANGE(Q.bri_scale), NULL);
    }
    lp_json_free(j);
    lp_sheet_invalidate(Q.sheet);
}

/* ── Wi-Fi row ───────────────────────────────────────────────────── */

static void tile_set(Tile *t, gboolean on, gboolean animate);

static void net_got(const char *out, gpointer d)
{
    (void)d;
    Q.net_busy = FALSE;
    LpJson *j = lp_json_parse(out);
    const char *icon = "network-wireless-offline-symbolic";
    char *name = NULL, *value = NULL;
    if (!j) {
        name = g_strdup(T("Networking unavailable", "네트워크를 쓸 수 없음"));
        value = g_strdup("");
        Q.radio_on = FALSE;
    } else {
        gboolean wired = FALSE;
        LpJson *w = lp_json_get(j, "wired");
        for (int i = 0; i < lp_json_len(w); i++)
            if (lp_json_bool(lp_json_at(w, i), "carrier", 0))
                wired = TRUE;
        const char *lan = wired ? T("LAN", "유선 연결") : T("no LAN", "유선 없음");
        Q.radio_on = g_strcmp0(lp_json_str(j, "radio", "on"), "off") != 0;
        const char *st = lp_json_str(j, "wifi.state", "disconnected");
        int qual = (int)lp_json_num(j, "wifi.quality", 0);
        if (!Q.radio_on) {
            name = g_strdup(T("Wi-Fi Off", "Wi-Fi 꺼짐"));
            value = g_strdup(lan);
            icon = "network-wireless-disabled-symbolic";
        } else if (g_strcmp0(st, "connected") == 0) {
            name = g_strdup(lp_json_str(j, "wifi.ssid", "Wi-Fi"));
            value = g_strdup_printf("%d%% · %s", qual, lan);
            icon = qual >= 75 ? "network-wireless-signal-excellent-symbolic"
                 : qual >= 50 ? "network-wireless-signal-good-symbolic"
                 : qual >= 25 ? "network-wireless-signal-ok-symbolic"
                              : "network-wireless-signal-weak-symbolic";
        } else if (g_strcmp0(st, "disconnected") && g_strcmp0(st, "failed")) {
            name = g_strdup(lp_json_str(j, "wifi.ssid", "Wi-Fi"));
            value = g_strdup_printf("%s · %s", T("Connecting…", "연결 중…"), lan);
            icon = "network-wireless-acquiring-symbolic";
        } else {
            name = g_strdup(T("Not Connected", "연결 안 됨"));
            value = g_strdup(lan);
        }
        lp_json_free(j);
    }
    gtk_image_set_from_icon_name(GTK_IMAGE(Q.wifi_icon), icon, GTK_ICON_SIZE_BUTTON);
    gtk_label_set_text(GTK_LABEL(Q.wifi_name), name);
    gtk_label_set_text(GTK_LABEL(Q.wifi_value), value);
    tile_set(&Q.net, Q.radio_on, lp_sheet_shown(Q.sheet));
    g_free(name);
    g_free(value);
    lp_sheet_invalidate(Q.sheet);
}

static void net_refresh(void)
{
    if (Q.net_busy)
        return;
    Q.net_busy = TRUE;
    const char *a[] = { "lp-net", "status", "--json", NULL };
    lp_run_async(a, net_got, NULL);
}

/* ── the Wi-Fi page ──────────────────────────────────────────────── */

static void set_msg(const char *text, gboolean bad)
{
    gtk_label_set_text(GTK_LABEL(Q.net_msg), text ? text : "");
    gtk_widget_set_visible(Q.net_msg, text && *text);
    GtkStyleContext *sc = gtk_widget_get_style_context(Q.net_msg);
    if (bad)
        gtk_style_context_add_class(sc, "lp-bad");
    else
        gtk_style_context_remove_class(sc, "lp-bad");
}

static void connect_done(const char *out, gpointer d)
{
    char *ssid = d;
    if (out) {
        char *m = g_strdup_printf(T("Connected to %s.", "%s 에 연결했습니다."), ssid);
        set_msg(m, FALSE);
        g_free(m);
    } else {
        set_msg(T("Could not connect. Check the password and try again.",
                  "연결하지 못했습니다. 비밀번호를 확인하고 다시 해 보세요."), TRUE);
    }
    g_free(ssid);
    net_refresh();
    const char *a[] = { "lp-panel", "refresh", NULL };
    lp_send("panel", a);
}

static void do_connect(const char *ssid, const char *password)
{
    char *m = g_strdup_printf(T("Connecting to %s…", "%s 에 연결하는 중…"), ssid);
    set_msg(m, FALSE);
    g_free(m);
    if (password && *password) {
        const char *a[] = { "lp-net", "connect", ssid, "--password", password, NULL };
        lp_run_async(a, connect_done, g_strdup(ssid));
    } else {
        const char *a[] = { "lp-net", "connect", ssid, NULL };
        lp_run_async(a, connect_done, g_strdup(ssid));
    }
}

static void on_pw_go(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (!Q.pw_ssid)
        return;
    do_connect(Q.pw_ssid, gtk_entry_get_text(GTK_ENTRY(Q.pw_entry)));
    gtk_entry_set_text(GTK_ENTRY(Q.pw_entry), "");
    gtk_widget_hide(Q.pw_box);
}

static void on_net_item(GtkButton *b, gpointer d)
{
    (void)d;
    const char *ssid = g_object_get_data(G_OBJECT(b), "ssid");
    gboolean need_pw = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "need-pw"));
    if (!ssid)
        return;
    if (!need_pw) {
        gtk_widget_hide(Q.pw_box);
        do_connect(ssid, NULL);
        return;
    }
    g_free(Q.pw_ssid);
    Q.pw_ssid = g_strdup(ssid);
    char *l = g_strdup_printf(T("Password for %s", "%s 의 비밀번호"), ssid);
    gtk_label_set_text(GTK_LABEL(Q.pw_label), l);
    g_free(l);
    gtk_widget_show(Q.pw_box);
    /* Focus puts the text-input on it, and that is what brings up the
     * on-screen keyboard for a finger (the OSK's auto-show). */
    gtk_widget_grab_focus(Q.pw_entry);
}

static void scan_got(const char *out, gpointer d)
{
    (void)d;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(Q.net_list));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);

    LpJson *j = lp_json_parse(out);
    int n = j ? lp_json_len(j) : 0;
    if (!j)
        set_msg(T("Could not look for networks.", "네트워크를 찾지 못했습니다."), TRUE);
    else if (n == 0)
        set_msg(T("No networks found.", "찾은 네트워크가 없습니다."), FALSE);
    else
        set_msg(NULL, FALSE);
    for (int i = 0; i < n; i++) {
        LpJson *e = lp_json_at(j, i);
        const char *ssid = lp_json_str(e, "ssid", "");
        if (!*ssid)
            continue;           /* hidden networks are Settings' business */
        int qual = (int)lp_json_num(e, "quality", 0);
        gboolean open = g_strcmp0(lp_json_str(e, "security", "open"), "open") == 0;
        gboolean saved = lp_json_bool(e, "saved", 0);
        gboolean conn = lp_json_bool(e, "connected", 0);

        GtkWidget *b = gtk_button_new();
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_pack_start(GTK_BOX(row), lp_icon(
            qual >= 75 ? "network-wireless-signal-excellent-symbolic"
          : qual >= 50 ? "network-wireless-signal-good-symbolic"
          : qual >= 25 ? "network-wireless-signal-ok-symbolic"
                       : "network-wireless-signal-weak-symbolic", 16), FALSE, FALSE, 0);
        GtkWidget *name = gtk_label_new(ssid);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_widget_set_halign(name, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), name, TRUE, TRUE, 0);
        if (conn) {
            GtkWidget *c = gtk_label_new(T("Connected", "연결됨"));
            gtk_style_context_add_class(gtk_widget_get_style_context(c), "lp-dim");
            gtk_box_pack_start(GTK_BOX(row), c, FALSE, FALSE, 0);
        }
        if (!open)
            gtk_box_pack_start(GTK_BOX(row), lp_icon("changes-prevent-symbolic", 14),
                               FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(b), row);
        g_object_set_data_full(G_OBJECT(b), "ssid", g_strdup(ssid), g_free);
        g_object_set_data(G_OBJECT(b), "need-pw", GINT_TO_POINTER(!open && !saved));
        g_signal_connect(b, "clicked", G_CALLBACK(on_net_item), NULL);
        gtk_box_pack_start(GTK_BOX(Q.net_list), b, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(Q.net_list);
    lp_json_free(j);
}

static void scan(void)
{
    set_msg(T("Looking for networks…", "네트워크를 찾는 중…"), FALSE);
    const char *a[] = { "lp-net", "scan", "--json", NULL };
    lp_run_async(a, scan_got, NULL);
}

static void on_wifi_row(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gtk_widget_hide(Q.pw_box);
    gtk_stack_set_visible_child_name(GTK_STACK(Q.stack), "wifi");
    scan();
}

static void on_back(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gtk_stack_set_visible_child_name(GTK_STACK(Q.stack), "main");
}

static void on_rescan(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    scan();
}

/* ── meters ──────────────────────────────────────────────────────── */

static gboolean meter_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    Meter *m = d;
    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);
    double h = 6, y = (H - h) / 2.0, r = h / 2.0;
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    GdkRGBA trough = { 0.23, 0.23, 0.23, 1 }, fill = { 0.91, 0.91, 0.91, 1 },
            amber = { 0.94, 0.70, 0.31, 1 };
    gtk_style_context_lookup_color(sc, "lp_track", &trough);
    gtk_style_context_lookup_color(sc, "lp_t1", &fill);
    gtk_style_context_lookup_color(sc, "lp_amber", &amber);

    cairo_new_sub_path(cr);
    cairo_arc(cr, r, y + r, r, G_PI / 2, 3 * G_PI / 2);
    cairo_arc(cr, W - r, y + r, r, -G_PI / 2, G_PI / 2);
    cairo_close_path(cr);
    gdk_cairo_set_source_rgba(cr, &trough);
    cairo_fill(cr);

    double f = m->s.x < 0 ? 0 : m->s.x > 1 ? 1 : m->s.x;
    double fw = f * W;
    if (fw >= 1) {
        double rr = MIN(r, fw / 2);
        cairo_new_sub_path(cr);
        cairo_arc(cr, rr, y + r, rr, G_PI / 2, 3 * G_PI / 2);
        cairo_arc(cr, fw - rr, y + r, rr, -G_PI / 2, G_PI / 2);
        cairo_close_path(cr);
        gdk_cairo_set_source_rgba(cr, f >= m->hot ? &amber : &fill);
        cairo_fill(cr);
    }
    return TRUE;
}

static void meters_frame(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    for (int i = 0; i < 3; i++)
        gtk_widget_queue_draw(Q.meter[i].area);
}

static void meter_set(Meter *m, double f, gboolean glide)
{
    char pct[16];
    g_snprintf(pct, sizeof pct, "%d%%", (int)lround(f * 100));
    gtk_label_set_text(GTK_LABEL(m->value), pct);
    if (glide)
        lp_spring_set_target(&m->s, f);
    else
        lp_spring_jump(&m->s, f);
}

static gboolean read_cpu(unsigned long long *total, unsigned long long *idle)
{
    char *s = NULL;
    if (!g_file_get_contents("/proc/stat", &s, NULL, NULL))
        return FALSE;
    unsigned long long v[8] = { 0 };
    int n = sscanf(s, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    g_free(s);
    if (n < 4)
        return FALSE;
    *total = 0;
    for (int i = 0; i < 8; i++)
        *total += v[i];
    *idle = v[3] + v[4];       /* idle + iowait */
    return TRUE;
}

static double read_mem(void)
{
    char *s = NULL;
    if (!g_file_get_contents("/proc/meminfo", &s, NULL, NULL))
        return 0;
    unsigned long long tot = 0, avail = 0;
    char *p = strstr(s, "MemTotal:");
    if (p) sscanf(p, "MemTotal: %llu", &tot);
    p = strstr(s, "MemAvailable:");
    if (p) sscanf(p, "MemAvailable: %llu", &avail);
    g_free(s);
    return tot ? 1.0 - (double)avail / tot : 0;
}

static double read_disk(void)
{
    struct statvfs v;
    if (statvfs("/", &v) != 0 || v.f_blocks == 0)
        return 0;
    /* What df prints: used over used + available to this user. */
    double used = (double)(v.f_blocks - v.f_bfree);
    double denom = used + (double)v.f_bavail;
    return denom > 0 ? used / denom : 0;
}

static void sample(gboolean glide)
{
    unsigned long long t, i;
    if (read_cpu(&t, &i)) {
        if (Q.cpu_total && t > Q.cpu_total) {
            double busy = 1.0 - (double)(i - Q.cpu_idle) / (double)(t - Q.cpu_total);
            meter_set(&Q.meter[0], CLAMP(busy, 0, 1), glide);
        }
        Q.cpu_total = t;
        Q.cpu_idle = i;
    }
    meter_set(&Q.meter[1], read_mem(), glide);
    meter_set(&Q.meter[2], read_disk(), glide);
    lp_motion_kick(Q.meters_motion);
}

static gboolean sample_tick(gpointer d)
{
    (void)d;
    sample(TRUE);
    return G_SOURCE_CONTINUE;
}

/* The CPU figure needs two readings. The first is taken as the panel
 * starts to open and the second a quarter of a second later, which lands
 * the bars at their values without an animation from zero; from then on
 * once a second, gliding. */
static gboolean sample_first(gpointer d)
{
    (void)d;
    sample(FALSE);
    Q.sample_id = g_timeout_add_seconds(1, sample_tick, NULL);
    return G_SOURCE_REMOVE;
}

static void start_sampling(void)
{
    if (Q.sample_id)
        return;
    unsigned long long t, i;
    if (read_cpu(&t, &i)) {
        Q.cpu_total = t;
        Q.cpu_idle = i;
    }
    Q.sample_id = g_timeout_add(250, sample_first, NULL);
}

/* ── tiles: fill on the knob spring ──────────────────────────────── */

static void pill(cairo_t *cr, double x, double y, double w, double h)
{
    double r = h / 2.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + r, y + r, r, G_PI / 2, 3 * G_PI / 2);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, G_PI / 2);
    cairo_close_path(cr);
}

/* Runs before GtkButton's own draw, which then paints only the press
 * highlight (CSS) and the label on top. The raised grey is always there;
 * the accent is laid over it at the spring's value and grows out of a
 * three-pixel inset, so a tile switching on swells into its colour with
 * the knob's small overshoot instead of flipping. */
static gboolean tile_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    Tile *t = d;
    double W = gtk_widget_get_allocated_width(w);
    double H = gtk_widget_get_allocated_height(w);
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    GdkRGBA base = { 0.17, 0.17, 0.17, 1 }, acc = { 0.91, 0.33, 0.13, 1 };
    gtk_style_context_lookup_color(sc, "lp_raised", &base);
    gtk_style_context_lookup_color(sc, "lp_accent", &acc);
    pill(cr, 0, 0, W, H);
    gdk_cairo_set_source_rgba(cr, &base);
    cairo_fill(cr);
    double x = t->on.x;
    if (x > 0.002) {
        double inset = CLAMP((1.0 - x) * 3.0, -1.0, 3.0);
        pill(cr, inset, inset, W - 2 * inset, H - 2 * inset);
        acc.alpha = CLAMP(x, 0, 1);
        gdk_cairo_set_source_rgba(cr, &acc);
        cairo_fill(cr);
    }
    return FALSE;
}

static void tile_frame(GtkWidget *w, gpointer d)
{
    (void)d;
    gtk_widget_queue_draw(w);
}

static void tile_set(Tile *t, gboolean on, gboolean animate)
{
    if (!t->button)
        return;
    t->state = on;
    GtkStyleContext *sc = gtk_widget_get_style_context(t->button);
    if (on)
        gtk_style_context_add_class(sc, "lp-on");
    else
        gtk_style_context_remove_class(sc, "lp-on");
    if (!animate) {
        lp_spring_jump(&t->on, on ? 1.0 : 0.0);
        gtk_widget_queue_draw(t->button);
        return;
    }
    if (on)
        lp_spring_set_target(&t->on, 1.0);
    else
        lp_spring_set_target_out(&t->on, 0.0);
    lp_motion_kick(t->motion);
}

static GtkWidget *tile_new(Tile *t, const char *icon, const char *text,
                           GCallback cb)
{
    t->button = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(t->button), "lp-tile");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(row), lp_icon(icon, 18), FALSE, FALSE, 0);
    t->label = gtk_label_new(text);
    gtk_label_set_ellipsize(GTK_LABEL(t->label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(t->label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), t->label, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(t->button), row);
    gtk_widget_set_hexpand(t->button, TRUE);
    g_signal_connect(t->button, "clicked", cb, t);
    g_signal_connect(t->button, "draw", G_CALLBACK(tile_draw), t);
    lp_spring_init(&t->on, LP_SPRING_KNOB, 0.0);
    t->motion = lp_motion_new(t->button, tile_frame, t);
    lp_motion_add(t->motion, &t->on);
    return t->button;
}

static void radio_done(const char *out, gpointer d)
{
    (void)out; (void)d;
    net_refresh();
    const char *a[] = { "lp-panel", "refresh", NULL };
    lp_send("panel", a);
}

static void on_net_tile(GtkButton *b, gpointer d)
{
    (void)b;
    Tile *t = d;
    gboolean on = !t->state;
    tile_set(t, on, TRUE);          /* answer the finger now, not in a second */
    const char *a[] = { "lp-net", "radio", on ? "on" : "off", NULL };
    lp_run_async(a, radio_done, NULL);
}

static void on_dark_tile(GtkButton *b, gpointer d)
{
    (void)b;
    Tile *t = d;
    gboolean dark = !t->state;
    tile_set(t, dark, TRUE);
    lp_style_set_light(!dark);
}

static void on_settings_tile(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    lp_sheet_hide(Q.sheet);
    const char *a[] = { "lp-settings", NULL };
    lp_spawn(a);
}

static void on_lock_tile(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    lp_sheet_hide(Q.sheet);
    const char *a[] = { "lp-lock", NULL };
    lp_spawn(a);
}

/* ── the confirm dialog ──────────────────────────────────────────── */

static struct {
    GtkWindow *win;
    GtkWidget *root, *card, *title, *body, *go;
    LpSpring   s;
    LpMotion  *motion;
    char      *what;
} D;

/* Everything is drawn here: the dim at the spring's value over the whole
 * output, and the card scaled from 0.96 and faded with it (COMMON.md:
 * appearing things start at 0.96 and transparent, never from nothing).
 * Under reduced motion the scale stays 1 and only the fade is left. */
static cairo_surface_t *dialog_snap;

static gboolean dialog_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    gint64 t0 = lp_trace_now();
    double x = CLAMP(D.s.x, 0.0, 1.0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.55 * x);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    GtkAllocation a;
    gtk_widget_get_allocation(D.card, &a);
    if (x >= 0.999 && !D.s.moving) {
        if (dialog_snap)
            cairo_surface_destroy(dialog_snap);
        dialog_snap = NULL;
        gtk_container_propagate_draw(GTK_CONTAINER(w), D.card, cr);
        lp_trace_draw("dialog-live", t0);
        return TRUE;
    }
    /* Moving: a picture of the card, for the same reason as lp-sheet.c -
     * its blurred CSS shadow is most of the cost of drawing it. */
    if (!dialog_snap)
        dialog_snap = lp_widget_snapshot(D.card);
    if (!dialog_snap)
        return TRUE;
    double sc = lp_motion_reduced() ? 1.0 : 0.96 + 0.04 * D.s.x;
    double cx = a.x + a.width / 2.0, cy = a.y + a.height / 2.0;
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, sc, sc);
    cairo_translate(cr, -cx, -cy);
    cairo_set_source_surface(cr, dialog_snap, a.x, a.y);
    cairo_paint_with_alpha(cr, x);
    lp_trace_draw("dialog", t0);
    return TRUE;
}

static void dialog_frame(GtkWidget *w, gpointer d)
{
    (void)d;
    gtk_widget_queue_draw(w);
    if (!D.s.moving && D.s.x <= 0.001 && gtk_widget_get_visible(GTK_WIDGET(D.win)))
        gtk_widget_hide(GTK_WIDGET(D.win));
}

static void dialog_close(void)
{
    lp_spring_set_target_out(&D.s, 0.0);
    lp_motion_kick(D.motion);
}

static void on_cancel(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    dialog_close();
}

static void on_go(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    const char *w = D.what;
    if (g_strcmp0(w, "logout") == 0) {
        const char *a[] = { "lp-logout", NULL };
        lp_spawn(a);
    } else if (g_strcmp0(w, "restart") == 0) {
        const char *a[] = { "lp-power", "restart", NULL };
        lp_spawn(a);
    } else if (g_strcmp0(w, "off") == 0) {
        const char *a[] = { "lp-power", "off", NULL };
        lp_spawn(a);
    } else if (g_strcmp0(w, "recovery") == 0) {
        /* lp-privd asks for the password; this only asks for the reboot. */
        const char *a[] = { "lp-reboot-recovery", NULL };
        lp_spawn(a);
    }
    dialog_close();
}

static gboolean dialog_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w; (void)d;
    if (ev->keyval == GDK_KEY_Escape) {
        dialog_close();
        return TRUE;
    }
    return FALSE;
}

/* A tap on the dim, outside the card, is Cancel. */
static gboolean dialog_press(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    (void)d;
    GtkAllocation a;
    gtk_widget_get_allocation(D.card, &a);
    int x, y;
    gtk_widget_translate_coordinates(w, D.root, (int)ev->x, (int)ev->y, &x, &y);
    if (x < a.x || y < a.y || x >= a.x + a.width || y >= a.y + a.height)
        dialog_close();
    return FALSE;
}

static void dialog_build(void)
{
    D.win = lp_layer_window("lp-confirm", GTK_LAYER_SHELL_LAYER_OVERLAY,
                            LP_EDGE_TOP | LP_EDGE_BOTTOM | LP_EDGE_LEFT | LP_EDGE_RIGHT);
    gtk_layer_set_exclusive_zone(D.win, -1);
    gtk_layer_set_keyboard_mode(D.win, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    g_signal_connect(D.win, "key-press-event", G_CALLBACK(dialog_key), NULL);
    gtk_widget_add_events(GTK_WIDGET(D.win), GDK_BUTTON_PRESS_MASK);
    g_signal_connect(D.win, "button-press-event", G_CALLBACK(dialog_press), NULL);

    D.root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_app_paintable(D.root, TRUE);
    g_signal_connect(D.root, "draw", G_CALLBACK(dialog_draw), NULL);
    gtk_container_add(GTK_CONTAINER(D.win), D.root);

    D.card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(D.card), "lp-dialog");
    gtk_widget_set_halign(D.card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(D.card, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(D.card, TRUE);
    gtk_widget_set_size_request(D.card, 440, -1);
    D.title = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(D.title), "lp-dialog-title");
    gtk_widget_set_halign(D.title, GTK_ALIGN_START);
    D.body = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(D.body), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(D.body), 44);
    gtk_label_set_xalign(GTK_LABEL(D.body), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(D.body), "lp-dialog-body");
    gtk_box_pack_start(GTK_BOX(D.card), D.title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(D.card), D.body, FALSE, FALSE, 0);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_set_homogeneous(GTK_BOX(btns), TRUE);
    GtkWidget *cancel = gtk_button_new_with_label(T("Cancel", "취소"));
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel), NULL);
    D.go = gtk_button_new_with_label("");
    gtk_style_context_add_class(gtk_widget_get_style_context(D.go), "lp-go");
    g_signal_connect(D.go, "clicked", G_CALLBACK(on_go), NULL);
    gtk_box_pack_start(GTK_BOX(btns), cancel, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(btns), D.go, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(D.card), btns, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(D.root), D.card, TRUE, FALSE, 0);
    /* Cancel has the focus, so Enter on a physical keyboard is the safe
     * answer and the destructive one always takes a deliberate tap. */
    g_object_set_data(G_OBJECT(D.win), "cancel", cancel);

    lp_spring_init(&D.s, LP_SPRING_SHEET, 0.0);
    D.motion = lp_motion_new(D.root, dialog_frame, NULL);
    lp_motion_add(D.motion, &D.s);
}

static void confirm(const char *what)
{
    if (!D.win)
        dialog_build();
    g_free(D.what);
    D.what = g_strdup(what);
    const char *title = "", *body = "", *go = "";
    gboolean danger = FALSE;
    if (!strcmp(what, "logout")) {
        title = T("Log Out?", "로그아웃할까요?");
        body = T("Open applications will be closed. Unsaved work in them will be lost.",
                 "열린 앱이 모두 닫힙니다. 저장하지 않은 내용은 사라집니다.");
        go = T("Log Out", "로그아웃");
    } else if (!strcmp(what, "restart")) {
        title = T("Restart?", "다시 시작할까요?");
        body = T("LP will close every application and start again.",
                 "모든 앱을 닫고 LP 를 다시 시작합니다.");
        go = T("Restart", "다시 시작");
    } else if (!strcmp(what, "recovery")) {
        title = T("Restart into Recovery?", "복구 모드로 다시 시작할까요?");
        body = T("Recovery can check the disks or reinstall LP. You will be asked "
                 "for your password first.",
                 "복구 모드에서는 디스크를 검사하거나 LP 를 다시 설치할 수 있습니다. "
                 "먼저 비밀번호를 묻습니다.");
        go = T("Restart", "다시 시작");
    } else {
        title = T("Power Off?", "전원을 끌까요?");
        body = T("LP will close every application and switch the computer off.",
                 "모든 앱을 닫고 컴퓨터의 전원을 끕니다.");
        go = T("Power Off", "전원 끄기");
        danger = TRUE;
    }
    if (dialog_snap)
        cairo_surface_destroy(dialog_snap);
    dialog_snap = NULL;
    gtk_label_set_text(GTK_LABEL(D.title), title);
    gtk_label_set_text(GTK_LABEL(D.body), body);
    gtk_button_set_label(GTK_BUTTON(D.go), go);
    GtkStyleContext *sc = gtk_widget_get_style_context(D.go);
    if (danger)
        gtk_style_context_add_class(sc, "lp-danger");
    else
        gtk_style_context_remove_class(sc, "lp-danger");

    GdkMonitor *m = gtk_layer_get_monitor(lp_sheet_window(Q.sheet));
    if (m && !gtk_widget_get_visible(GTK_WIDGET(D.win)))
        gtk_layer_set_monitor(D.win, m);
    lp_sheet_hide(Q.sheet);
    gtk_widget_show_all(GTK_WIDGET(D.win));
    gtk_widget_grab_focus(g_object_get_data(G_OBJECT(D.win), "cancel"));
    lp_spring_set_target(&D.s, 1.0);
    lp_motion_kick(D.motion);
}

static void on_action(GtkButton *b, gpointer d)
{
    (void)b;
    confirm(d);
}

/* ── building the panel ──────────────────────────────────────────── */

static GtkWidget *slider_row(GtkWidget **icon, GtkWidget **scale, GtkWidget **pct,
                             const char *icon_name, GCallback changed,
                             GCallback icon_clicked)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "lp-slider-row");
    GtkWidget *ib = gtk_button_new();
    *icon = lp_icon(icon_name, 18);
    gtk_container_add(GTK_CONTAINER(ib), *icon);
    if (icon_clicked)
        g_signal_connect(ib, "clicked", icon_clicked, NULL);
    else
        gtk_widget_set_can_focus(ib, FALSE);
    gtk_box_pack_start(GTK_BOX(row), ib, FALSE, FALSE, 0);
    *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(*scale), FALSE);
    gtk_widget_set_hexpand(*scale, TRUE);
    g_signal_connect(*scale, "value-changed", changed, NULL);
    gtk_box_pack_start(GTK_BOX(row), *scale, TRUE, TRUE, 0);
    *pct = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(*pct), 1.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(*pct), "lp-pct");
    gtk_box_pack_start(GTK_BOX(row), *pct, FALSE, FALSE, 0);
    return row;
}

static GtkWidget *meter_row(Meter *m, const char *name, double hot)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *l = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "lp-meter-name");
    gtk_box_pack_start(GTK_BOX(row), l, FALSE, FALSE, 0);
    m->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(m->area, -1, 22);
    gtk_widget_set_hexpand(m->area, TRUE);
    g_signal_connect(m->area, "draw", G_CALLBACK(meter_draw), m);
    gtk_box_pack_start(GTK_BOX(row), m->area, TRUE, TRUE, 0);
    m->value = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(m->value), 1.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(m->value), "lp-meter-val");
    gtk_box_pack_start(GTK_BOX(row), m->value, FALSE, FALSE, 0);
    m->hot = hot;
    lp_spring_init(&m->s, LP_SPRING_SLIDE, 0.0);
    return row;
}

static GtkWidget *action_button(const char *label, const char *what, gboolean danger)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    GtkStyleContext *sc = gtk_widget_get_style_context(b);
    gtk_style_context_add_class(sc, "lp-action");
    if (danger)
        gtk_style_context_add_class(sc, "lp-danger");
    GtkWidget *l = gtk_bin_get_child(GTK_BIN(b));
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    g_signal_connect(b, "clicked", G_CALLBACK(on_action), (gpointer)what);
    return b;
}

static GtkWidget *build_main(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_box_pack_start(GTK_BOX(box),
        slider_row(&Q.vol_icon, &Q.vol_scale, &Q.vol_pct, "audio-volume-high-symbolic",
                   G_CALLBACK(on_vol_changed), G_CALLBACK(on_vol_icon)),
        FALSE, FALSE, 0);
    GtkWidget *bri_icon;
    Q.bri_row = slider_row(&bri_icon, &Q.bri_scale, &Q.bri_pct,
                           "display-brightness-symbolic",
                           G_CALLBACK(on_bri_changed), NULL);
    /* Children shown first: show_all skips a no-show-all widget
     * entirely, children included, and the row came up as an empty
     * 48px gap the first time it was made visible. */
    gtk_widget_show_all(Q.bri_row);
    gtk_widget_set_no_show_all(Q.bri_row, TRUE);
    gtk_widget_hide(Q.bri_row);
    gtk_box_pack_start(GTK_BOX(box), Q.bri_row, FALSE, FALSE, 0);

    /* Wi-Fi: name on the left, signal and wired state on the right. */
    GtkWidget *wifi = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(wifi), "lp-row");
    GtkWidget *wr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    Q.wifi_icon = lp_icon("network-wireless-offline-symbolic", 18);
    gtk_box_pack_start(GTK_BOX(wr), Q.wifi_icon, FALSE, FALSE, 0);
    Q.wifi_name = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(Q.wifi_name), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(Q.wifi_name), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(Q.wifi_name), "lp-row-title");
    gtk_box_pack_start(GTK_BOX(wr), Q.wifi_name, TRUE, TRUE, 0);
    Q.wifi_value = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(Q.wifi_value), "lp-row-value");
    gtk_box_pack_start(GTK_BOX(wr), Q.wifi_value, FALSE, FALSE, 0);
    GtkWidget *chev = lp_icon("go-next-symbolic", 14);
    gtk_style_context_add_class(gtk_widget_get_style_context(chev), "lp-chev");
    gtk_box_pack_start(GTK_BOX(wr), chev, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(wifi), wr);
    g_signal_connect(wifi, "clicked", G_CALLBACK(on_wifi_row), NULL);
    gtk_box_pack_start(GTK_BOX(box), wifi, FALSE, FALSE, 0);

    GtkWidget *meters = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(meters), "lp-meters");
    gtk_box_pack_start(GTK_BOX(meters), meter_row(&Q.meter[0], T("CPU", "CPU"), 0.90), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(meters), meter_row(&Q.meter[1], T("Memory", "메모리"), 0.90), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(meters), meter_row(&Q.meter[2], T("Disk", "디스크"), 0.90), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), meters, FALSE, FALSE, 0);
    Q.meters_motion = lp_motion_new(meters, meters_frame, NULL);
    for (int i = 0; i < 3; i++)
        lp_motion_add(Q.meters_motion, &Q.meter[i].s);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_widget_set_margin_top(grid, 6);
    gtk_grid_attach(GTK_GRID(grid), tile_new(&Q.net, "network-wireless-symbolic",
                    T("Network On", "네트워크 켬"), G_CALLBACK(on_net_tile)), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), tile_new(&Q.dark, "weather-clear-night-symbolic",
                    T("Dark Style", "어두운 스타일"), G_CALLBACK(on_dark_tile)), 1, 0, 1, 1);
    static Tile settings, lock;
    gtk_grid_attach(GTK_GRID(grid), tile_new(&settings, "emblem-system-symbolic",
                    T("Settings", "설정"), G_CALLBACK(on_settings_tile)), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), tile_new(&lock, "system-lock-screen-symbolic",
                    T("Lock", "잠금"), G_CALLBACK(on_lock_tile)), 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    GtkWidget *user = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(user), "lp-user");
    char *name = lp_user_display_name();
    char initial[8] = "?";
    gunichar c = g_utf8_get_char_validated(name, -1);
    if (c != (gunichar)-1 && c != (gunichar)-2 && c)
        initial[g_unichar_to_utf8(g_unichar_toupper(c), initial)] = '\0';
    GtkWidget *av = gtk_label_new(initial);
    gtk_style_context_add_class(gtk_widget_get_style_context(av), "lp-avatar");
    gtk_widget_set_valign(av, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(user), av, FALSE, FALSE, 0);
    GtkWidget *nl = gtk_label_new(name);
    gtk_label_set_ellipsize(GTK_LABEL(nl), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(nl), 0);
    gtk_box_pack_start(GTK_BOX(user), nl, TRUE, TRUE, 0);
    g_free(name);
    gtk_box_pack_start(GTK_BOX(box), user, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), action_button(T("Log Out", "로그아웃"), "logout", FALSE),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), action_button(T("Restart…", "다시 시작..."), "restart", FALSE),
                       FALSE, FALSE, 0);
    Q.recovery = action_button(T("Restart into Recovery…", "복구 모드로 다시 시작..."),
                               "recovery", FALSE);
    gtk_widget_show_all(Q.recovery);
    gtk_widget_set_no_show_all(Q.recovery, TRUE);
    gtk_widget_hide(Q.recovery);
    gtk_box_pack_start(GTK_BOX(box), Q.recovery, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), action_button(T("Power Off", "전원 끄기"), "off", TRUE),
                       FALSE, FALSE, 0);
    return box;
}

static GtkWidget *build_wifi(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *back = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(back), lp_icon("go-previous-symbolic", 18));
    g_signal_connect(back, "clicked", G_CALLBACK(on_back), NULL);
    gtk_box_pack_start(GTK_BOX(head), back, FALSE, FALSE, 0);
    GtkWidget *t = gtk_label_new(T("Wi-Fi", "Wi-Fi"));
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "lp-row-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_widget_set_margin_start(t, 6);
    gtk_box_pack_start(GTK_BOX(head), t, TRUE, TRUE, 0);
    GtkWidget *again = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(again), lp_icon("view-refresh-symbolic", 18));
    g_signal_connect(again, "clicked", G_CALLBACK(on_rescan), NULL);
    gtk_box_pack_end(GTK_BOX(head), again, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 0);

    Q.net_msg = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(Q.net_msg), TRUE);
    gtk_label_set_xalign(GTK_LABEL(Q.net_msg), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(Q.net_msg), "lp-msg");
    gtk_widget_set_no_show_all(Q.net_msg, TRUE);
    gtk_box_pack_start(GTK_BOX(box), Q.net_msg, FALSE, FALSE, 0);

    /* The list scrolls kinetically under a finger (GtkScrolledWindow's
     * default for touch), inside the page's fixed height. */
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_kinetic_scrolling(GTK_SCROLLED_WINDOW(sw), TRUE);
    gtk_widget_set_vexpand(sw, TRUE);
    Q.net_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(Q.net_list), "lp-nets");
    gtk_container_add(GTK_CONTAINER(sw), Q.net_list);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);

    Q.pw_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(Q.pw_box), "lp-nets");
    Q.pw_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(Q.pw_label), 0);
    gtk_widget_set_margin_start(Q.pw_label, 36);
    gtk_box_pack_start(GTK_BOX(Q.pw_box), Q.pw_label, FALSE, FALSE, 0);
    Q.pw_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(Q.pw_entry), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(Q.pw_entry), GTK_INPUT_PURPOSE_PASSWORD);
    g_signal_connect(Q.pw_entry, "activate", G_CALLBACK(on_pw_go), NULL);
    gtk_box_pack_start(GTK_BOX(Q.pw_box), Q.pw_entry, FALSE, FALSE, 0);
    GtkWidget *go = gtk_button_new_with_label(T("Connect", "연결"));
    gtk_style_context_add_class(gtk_widget_get_style_context(go), "lp-action");
    g_signal_connect(go, "clicked", G_CALLBACK(on_pw_go), NULL);
    gtk_box_pack_start(GTK_BOX(Q.pw_box), go, FALSE, FALSE, 0);
    gtk_widget_show_all(Q.pw_box);
    gtk_widget_set_no_show_all(Q.pw_box, TRUE);
    gtk_widget_hide(Q.pw_box);
    gtk_box_pack_start(GTK_BOX(box), Q.pw_box, FALSE, FALSE, 0);
    return box;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w; (void)d;
    if (ev->keyval == GDK_KEY_Escape) {
        lp_sheet_hide(Q.sheet);
        return TRUE;
    }
    return FALSE;
}

static void build(void)
{
    Q.card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(Q.card), "lp-quick");
    gtk_widget_set_size_request(Q.card, CARD_WIDTH, -1);

    /* Two pages of one fixed height: switching between them moves pixels
     * inside the surface and never resizes it (COMMON.md: no relayout of
     * a layer surface per frame). 260 ms is motion.css's stack page. */
    Q.stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(Q.stack),
                                  GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration(GTK_STACK(Q.stack), 260);
    gtk_stack_set_homogeneous(GTK_STACK(Q.stack), TRUE);
    gtk_stack_add_named(GTK_STACK(Q.stack), build_main(), "main");
    gtk_stack_add_named(GTK_STACK(Q.stack), build_wifi(), "wifi");
    gtk_box_pack_start(GTK_BOX(Q.card), Q.stack, TRUE, TRUE, 0);

    Q.sheet = lp_sheet_new("lp-quick", LP_EDGE_TOP, Q.card);
    GtkWindow *w = lp_sheet_window(Q.sheet);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    /* On demand: a physical keyboard's Escape reaches it, and the Wi-Fi
     * password field takes typing (and so the on-screen keyboard), but
     * the panel does not steal the keyboard from the window behind it
     * just by being open. */
    gtk_layer_set_keyboard_mode(w, GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    g_signal_connect(w, "key-press-event", G_CALLBACK(on_key), NULL);
    lp_sheet_set_dismiss(Q.sheet, TRUE);
    lp_sheet_on_closed(Q.sheet, on_closed, NULL);
    gtk_widget_show_all(Q.card);
}

/* ── showing ─────────────────────────────────────────────────────── */

static void refresh(void)
{
    const char *v[] = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL };
    lp_run_async(v, vol_got, NULL);
    const char *t[] = { "lp-tune", "status", "--json", NULL };
    lp_run_async(t, tune_got, NULL);
    net_refresh();
    tile_set(&Q.dark, !lp_style_light(), FALSE);
    gtk_widget_set_visible(Q.recovery, lp_have("lp-reboot-recovery"));
}

static void use_monitor(const char *arg)
{
    if (!arg)
        return;
    GdkDisplay *dpy = gdk_display_get_default();
    int i = atoi(arg);
    if (i >= 0 && i < gdk_display_get_n_monitors(dpy))
        lp_sheet_set_monitor(Q.sheet, gdk_display_get_monitor(dpy, i));
}

static void show(void)
{
    if (!lp_sheet_shown(Q.sheet)) {
        refresh();
        start_sampling();
    }
    lp_sheet_show(Q.sheet);
    tell_panel(TRUE);
}

static void on_command(int argc, char **argv, gpointer d)
{
    (void)d;
    const char *cmd = argc >= 2 ? argv[1] : "toggle";
    if (!strcmp(cmd, "toggle")) {
        if (lp_sheet_shown(Q.sheet)) {
            lp_sheet_hide(Q.sheet);
        } else {
            use_monitor(argc >= 3 ? argv[2] : NULL);
            show();
        }
    } else if (!strcmp(cmd, "show")) {
        use_monitor(argc >= 3 ? argv[2] : NULL);
        show();
    } else if (!strcmp(cmd, "hide")) {
        lp_sheet_hide(Q.sheet);
    } else if (!strcmp(cmd, "confirm") && argc >= 3 &&
               (!strcmp(argv[2], "logout") || !strcmp(argv[2], "restart") ||
                !strcmp(argv[2], "off") || !strcmp(argv[2], "recovery"))) {
        /* The top bar's LP menu asks for the same questions the power
         * buttons here ask, in the same dialog. */
        confirm(argv[2]);
    } else if (!strcmp(cmd, "refresh")) {
        if (lp_sheet_shown(Q.sheet))
            refresh();
    } else if (!strcmp(cmd, "drag-px") && argc >= 3) {
        /* A finger coming down from the top edge, in logical pixels from
         * where it started. The panel follows it exactly. */
        if (!Q.dragging) {
            Q.dragging = TRUE;
            use_monitor(argc >= 4 ? argv[3] : NULL);
            if (!lp_sheet_shown(Q.sheet)) {
                refresh();
                start_sampling();
            }
            tell_panel(TRUE);
        }
        lp_sheet_drag(Q.sheet, g_ascii_strtod(argv[2], NULL) / lp_sheet_extent(Q.sheet));
    } else if (!strcmp(cmd, "release-px") && argc >= 3) {
        Q.dragging = FALSE;
        lp_sheet_release(Q.sheet, g_ascii_strtod(argv[2], NULL) / lp_sheet_extent(Q.sheet));
    }
    /* "daemon": nothing to do - being started was the point. */
}

int main(int argc, char **argv)
{
    if (!lp_single_instance("quick", argc, argv, on_command, NULL))
        return 0;
    lp_shell_init(&argc, &argv);
    build();
    restore_volume();
    on_command(argc, argv, NULL);
    gtk_main();
    return 0;
}
