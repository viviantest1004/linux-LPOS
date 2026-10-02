/*
 * panel.c - lp-panel, the top bar.
 *
 *   [LP] Files               Sat Sep 27  23:47          [⌨] [▾ ◖ ▮ ⏻]
 *
 * Left to right: the LP mark, which opens the LP menu (about this
 * computer, the everyday apps, lock, log out, restart, shut down), then
 * the name of the application in front; the date and time in the
 * middle, which opens a calendar with a way into Date & Time settings;
 * on the right the apps running in the background (tray.c), the
 * input language - EN or 한, which a click switches - the on-screen
 * keyboard button, and the status area - Wi-Fi, volume, battery,
 * power - which is one button and opens quick settings.
 *
 * The bar used to start with a text button, 현재 활동 ("Activities"),
 * which is GNOME's and Ubuntu's; the LP mark and its menu are LP's own.
 *
 * ── why a program and not waybar ──
 *
 * The bar used to be waybar with a config. waybar cannot draw this one:
 * it has no module for "the focused application's name" (its taskbar
 * lists every window), no calendar popover, and it draws the status
 * icons as separate modules with separate click targets where the
 * mockup has one area that opens one panel. Getting close meant custom
 * scripts polled every two seconds, which is what the old lp-bar-label
 * was. A GTK 3 layer-shell program is a few hundred lines, draws the
 * mockup exactly, and polls nothing faster than every ten seconds.
 *
 * ── what it asks, and how often ──
 *
 *   the focused window    wlr-foreign-toplevel events, no polling
 *   the clock             woken once a minute, on the minute, and at
 *                         once when ~/.config/lp/clock (12 or 24 hours)
 *                         or /etc/localtime (the zone) changes
 *   Wi-Fi                 `lp-net status --json`, every 10 s
 *   volume                `wpctl get-volume`, every 10 s
 *   battery               `lp-tune status --json`, every 30 s
 *   the input language    `lp-osk watch`, pushed at every change; under
 *                         wayfire fcitx5's State over D-Bus, every 300 ms
 *
 * and immediately on `lp-panel refresh`, which the volume keys and the
 * quick settings panel send after they change something, so the icons
 * never wait out a poll to catch up with a key press. A query still
 * running when the next one is due is not started twice.
 *
 * ── motion ──
 *
 * The calendar is a sheet (desktop/common/lp-sheet.c) that slides and
 * fades down from under the clock on the sheet spring, 260 ms in and
 * 182 ms out, and turns round if it is tapped again half way. It used to
 * be a GtkPopover, whose own transition is a fixed 150 ms timeline that
 * cannot be interrupted and ignores the springs every other panel uses.
 *
 * A finger that lands on the bar and pulls down is quick settings coming
 * down from the top edge: past 10 px of mostly-vertical travel the bar
 * takes the touch away from whatever button it started on, and from then
 * on hands the distance to lp-quick (drag-px), which follows it 1:1, and
 * on release the finger's speed (release-px), which flings it open or
 * shut. The panel draws nothing of that itself.
 */
#define _GNU_SOURCE 1

#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/timerfd.h>

#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>

#include "lp-apps.h"
#include "lp-json.h"
#include "lp-motion.h"
#include "lp-sheet.h"
#include "lp-shell.h"
#include "lp-toplevel.h"
#include "tray.h"

#define BAR_HEIGHT 36

typedef struct {
    GtkWindow *win;
    GdkMonitor *mon;
    GtkWidget *activities;
    GtkWidget *appname;
    GtkWidget *clock;
    GtkWidget *clock_label;
    GtkWidget *status;
    GtkWidget *lang;
    GtkWidget *lang_label;
    GtkGesture *pull;           /* the top-edge drag */
    LpVelocity pull_v;
    gboolean   pulling;
    GtkWidget *wifi_icon;
    GtkWidget *vol_icon;
    GtkWidget *bat_icon;
    GtkWidget *bat_label;
} Bar;

static GList *bars;
static GHashTable *app_cache;   /* app_id -> GDesktopAppInfo or NULL */

/* One calendar, moved to whichever output's clock was tapped. */
static struct {
    LpSheet   *sheet;
    GtkWidget *day, *date, *calendar;
} cal;

/* The state every bar shows. */
static char *wifi_icon_name, *vol_icon_name, *bat_icon_name, *bat_text;
static int bat_state;           /* 0 plain, 1 warn, 2 bad */
static gboolean busy_net, busy_vol, busy_bat;

/* ── the focused application ─────────────────────────────────────── */

static const char *name_for(LpToplevel *t)
{
    if (t->app_id && *t->app_id) {
        GDesktopAppInfo *d;
        if (!g_hash_table_lookup_extended(app_cache, t->app_id, NULL,
                                          (gpointer *)&d)) {
            d = lp_app_for_id(t->app_id);
            g_hash_table_insert(app_cache, g_strdup(t->app_id), d);
        }
        if (d)
            return lp_app_name(G_APP_INFO(d));
    }
    return t->title ? t->title : "";
}

static void on_toplevels(gpointer data)
{
    (void)data;
    const char *name = "";
    for (GList *l = lp_toplevels(); l; l = l->next) {
        LpToplevel *t = l->data;
        if (t->done && t->activated && !t->minimized)
            name = name_for(t);
    }
    for (GList *b = bars; b; b = b->next)
        gtk_label_set_text(GTK_LABEL(((Bar *)b->data)->appname), name);
    lp_tray_toplevels_changed();
}

/* ── the clock ───────────────────────────────────────────────────── */

/* Settings > Date & time's "24-hour clock" is ~/.config/lp/clock, "24" or
 * "12" (desktop/settings/panels/datetime.c), and its row says the top
 * bar follows it. The bar used to print %H:%M whatever the file said, so
 * the switch moved, the file changed, and the clock on the screen - the
 * only one most people look at - stayed where it was, restart or no
 * restart. Read at every tick, and at once when the file changes. */
static gboolean clock_12h(void)
{
    char *v = lp_config_read("clock");
    gboolean twelve = v && strcmp(v, "12") == 0;
    g_free(v);
    return twelve;
}

static void set_clock(void)
{
    GDateTime *now = g_date_time_new_now_local();
    /* The mockup's "Sat 23:47". %a is the locale's own short weekday,
     * so a Korean session reads 토 23:47 - the same shape, in its own
     * words - without a second format string to keep in step. */
    char *s = g_date_time_format(now, clock_12h()
                                 ? T("%a %b %-d  %-l:%M %p", "%-m월 %-d일 (%a)  %p %-l:%M")
                                 : T("%a %b %-d  %H:%M", "%-m월 %-d일 (%a)  %H:%M"));
    for (GList *b = bars; b; b = b->next)
        gtk_label_set_text(GTK_LABEL(((Bar *)b->data)->clock_label), s);
    g_free(s);
    g_date_time_unref(now);
}

static guint clock_source;

static gboolean clock_tick(gpointer d)
{
    (void)d;
    set_clock();
    /* Re-aim at the next minute boundary every time rather than adding
     * 60 s forever: a suspend, or a clock set by NTP, would otherwise
     * leave the bar a minute behind until the next restart. */
    GDateTime *now = g_date_time_new_now_local();
    int wait = 60 - g_date_time_get_second(now);
    g_date_time_unref(now);
    clock_source = g_timeout_add_seconds(wait > 0 ? wait : 60, clock_tick, NULL);
    return G_SOURCE_REMOVE;
}

/* The clock itself being set - Settings > Date & time's manual time, or
 * ntp. The minute timer runs on the monotonic clock and knew nothing of
 * it: the bar showed the old time for up to a minute and then kept its
 * minute changing at the old second. A CLOCK_REALTIME timerfd armed with
 * TFD_TIMER_CANCEL_ON_SET is the kernel's way to hear of it: its read
 * fails with ECANCELED the moment anyone sets the time. */
static int clock_set_fd = -1;

static void clock_set_arm(void)
{
    struct itimerspec far = { .it_value = { .tv_sec = 0x7fffffff } };
    timerfd_settime(clock_set_fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET,
                    &far, NULL);
}

static gboolean on_clock_set(gint fd, GIOCondition c, gpointer d)
{
    (void)c; (void)d;
    uint64_t n;
    if (read(fd, &n, sizeof n) < 0 && errno != ECANCELED && errno != EAGAIN)
        return G_SOURCE_REMOVE;
    clock_set_arm();
    if (clock_source)
        g_source_remove(clock_source);
    clock_source = 0;
    clock_tick(NULL);
    return G_SOURCE_CONTINUE;
}

static void clock_set_watch(void)
{
    clock_set_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
    if (clock_set_fd < 0)
        return;
    clock_set_arm();
    g_unix_fd_add(clock_set_fd, G_IO_IN, on_clock_set, NULL);
}

static void on_clock_file(GFileMonitor *m, GFile *f, GFile *o,
                          GFileMonitorEvent ev, gpointer d)
{
    (void)m; (void)f; (void)o; (void)ev; (void)d;
    set_clock();
}

/* The 12/24-hour file, and the time zone: timedatectl set-timezone puts a
 * new /etc/localtime link in place, and GLib reads the zone again for the
 * next GDateTime - so the bar can show it now rather than at the next
 * minute. The monitors live as long as the bar. */
static void clock_watch(const char *path)
{
    GFile *f = g_file_new_for_path(path);
    GFileMonitor *m = g_file_monitor_file(f, G_FILE_MONITOR_NONE, NULL, NULL);
    g_object_unref(f);
    if (m)
        g_signal_connect(m, "changed", G_CALLBACK(on_clock_file), NULL);
}

static void fill_calendar(void)
{
    GDateTime *now = g_date_time_new_now_local();
    char *day = g_date_time_format(now, "%A");
    char *date = g_date_time_format(now, T("%B %-d, %Y", "%Y년 %-m월 %-d일"));
    gtk_label_set_text(GTK_LABEL(cal.day), day);
    gtk_label_set_text(GTK_LABEL(cal.date), date);
    gtk_calendar_select_month(GTK_CALENDAR(cal.calendar),
                              g_date_time_get_month(now) - 1,
                              g_date_time_get_year(now));
    gtk_calendar_select_day(GTK_CALENDAR(cal.calendar),
                            g_date_time_get_day_of_month(now));
    g_free(day);
    g_free(date);
    g_date_time_unref(now);
}

static void on_datetime_settings(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    lp_sheet_hide(cal.sheet);
    const char *a[] = { "lp-settings", "datetime", NULL };
    lp_spawn(a);
}

static void cal_build(void)
{
    GtkWidget *cbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(cbox), "lp-cal");
    cal.day = gtk_label_new("");
    cal.date = gtk_label_new("");
    gtk_widget_set_halign(cal.day, GTK_ALIGN_START);
    gtk_widget_set_halign(cal.date, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(cal.day), "lp-cal-day");
    gtk_style_context_add_class(gtk_widget_get_style_context(cal.date), "lp-cal-date");
    cal.calendar = gtk_calendar_new();
    gtk_box_pack_start(GTK_BOX(cbox), cal.day, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cbox), cal.date, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cbox), cal.calendar, FALSE, FALSE, 0);
    /* Where the time is set: the clock is the first place anyone looks
     * for that. */
    GtkWidget *dt = gtk_button_new_with_label(T("Date & Time Settings…", "날짜 및 시간 설정…"));
    gtk_style_context_add_class(gtk_widget_get_style_context(dt), "lp-cal-settings");
    g_signal_connect(dt, "clicked", G_CALLBACK(on_datetime_settings), NULL);
    gtk_box_pack_start(GTK_BOX(cbox), dt, FALSE, FALSE, 0);
    gtk_widget_show_all(cbox);
    /* Anchored to the top edge only, so the compositor centres it under
     * the clock - which is in the middle of the bar. */
    cal.sheet = lp_sheet_new("lp-calendar", LP_EDGE_TOP, cbox);
    lp_sheet_set_dismiss(cal.sheet, TRUE);
}

static void on_clock(GtkButton *btn, gpointer d)
{
    (void)btn;
    Bar *b = d;
    if (lp_sheet_shown(cal.sheet)) {
        lp_sheet_hide(cal.sheet);
        return;
    }
    fill_calendar();
    lp_sheet_set_monitor(cal.sheet, b->mon);
    lp_sheet_show(cal.sheet);
}

/* ── status: Wi-Fi, volume, battery ─────────────────────────────── */

/* Each of these touches the bar only when what it shows changes. The
 * polls below come every 15 and 30 seconds and nearly always bring the
 * same answer, and setting an icon again - even the same one - redraws
 * the bar: a new frame of the whole screen from the compositor every 15
 * seconds on a desktop nobody touched, which under KVM went to the host
 * as a new screen each time. */
static void show_icon(GtkWidget *w, const char *name)
{
    const char *cur = NULL;
    if (gtk_image_get_storage_type(GTK_IMAGE(w)) == GTK_IMAGE_ICON_NAME)
        gtk_image_get_icon_name(GTK_IMAGE(w), &cur, NULL);
    if (g_strcmp0(cur, name) != 0)
        gtk_image_set_from_icon_name(GTK_IMAGE(w), name, GTK_ICON_SIZE_BUTTON);
}

static void show_class(GtkWidget *w, const char *cls, gboolean on)
{
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    if (gtk_style_context_has_class(sc, cls) == on)
        return;
    if (on)
        gtk_style_context_add_class(sc, cls);
    else
        gtk_style_context_remove_class(sc, cls);
}

static void paint_status(void)
{
    for (GList *l = bars; l; l = l->next) {
        Bar *b = l->data;
        show_icon(b->wifi_icon,
            wifi_icon_name ? wifi_icon_name : "network-wireless-offline-symbolic");
        show_icon(b->vol_icon,
            vol_icon_name ? vol_icon_name : "audio-volume-muted-symbolic");
        gtk_widget_set_visible(b->bat_icon, bat_icon_name != NULL);
        gtk_widget_set_visible(b->bat_label, bat_text != NULL);
        if (bat_icon_name)
            show_icon(b->bat_icon, bat_icon_name);
        if (bat_text && g_strcmp0(gtk_label_get_text(GTK_LABEL(b->bat_label)), bat_text) != 0)
            gtk_label_set_text(GTK_LABEL(b->bat_label), bat_text);
        show_class(b->bat_label, "lp-warn", bat_state == 1);
        show_class(b->bat_label, "lp-bad", bat_state == 2);
    }
}

static void set_str(char **slot, const char *v)
{
    g_free(*slot);
    *slot = v ? g_strdup(v) : NULL;
}

static void net_done(const char *out, gpointer d)
{
    (void)d;
    busy_net = FALSE;
    LpJson *j = lp_json_parse(out);
    const char *icon = "network-wireless-offline-symbolic";
    if (j) {
        const char *st = lp_json_str(j, "wifi.state", "");
        int q = (int)lp_json_num(j, "wifi.quality", 0);
        gboolean wired = FALSE;
        LpJson *w = lp_json_get(j, "wired");
        for (int i = 0; i < lp_json_len(w); i++)
            if (lp_json_bool(lp_json_at(w, i), "carrier", 0))
                wired = TRUE;
        if (g_strcmp0(lp_json_str(j, "radio", "on"), "off") == 0)
            icon = wired ? "network-wired-symbolic"
                         : "network-wireless-disabled-symbolic";
        else if (g_strcmp0(st, "connected") == 0)
            icon = q >= 75 ? "network-wireless-signal-excellent-symbolic"
                 : q >= 50 ? "network-wireless-signal-good-symbolic"
                 : q >= 25 ? "network-wireless-signal-ok-symbolic"
                           : "network-wireless-signal-weak-symbolic";
        else if (wired)
            icon = "network-wired-symbolic";
        else if (g_strcmp0(st, "disconnected") && g_strcmp0(st, "failed"))
            icon = "network-wireless-acquiring-symbolic";
        lp_json_free(j);
    }
    set_str(&wifi_icon_name, icon);
    paint_status();
}

static void vol_done(const char *out, gpointer d)
{
    (void)d;
    busy_vol = FALSE;
    const char *icon = "audio-volume-muted-symbolic";
    double v;
    /* "Volume: 0.40" or "Volume: 0.40 [MUTED]" */
    if (out && sscanf(out, "Volume: %lf", &v) == 1 && !strstr(out, "MUTED"))
        icon = v <= 0.001 ? "audio-volume-muted-symbolic"
             : v < 0.34  ? "audio-volume-low-symbolic"
             : v < 0.67  ? "audio-volume-medium-symbolic"
                         : "audio-volume-high-symbolic";
    set_str(&vol_icon_name, icon);
    paint_status();
}

static void bat_done(const char *out, gpointer d)
{
    (void)d;
    busy_bat = FALSE;
    LpJson *j = lp_json_parse(out);
    if (!j || !lp_json_bool(j, "battery.present", 0)) {
        /* No battery - a desktop, or a VM - so no battery icon at all.
         * An icon that always says "unknown" is noise in the one place
         * the eye goes to for how long the machine has left. */
        set_str(&bat_icon_name, NULL);
        set_str(&bat_text, NULL);
        lp_json_free(j);
        paint_status();
        return;
    }
    int pct = (int)lp_json_num(j, "battery.percent", 0);
    const char *st = lp_json_str(j, "battery.state", "unknown");
    gboolean charging = g_strcmp0(st, "charging") == 0 ||
                        g_strcmp0(st, "full") == 0;
    int lvl = ((pct + 5) / 10) * 10;
    if (lvl > 100) lvl = 100;
    char *icon = g_strdup_printf("battery-level-%d%s-symbolic", lvl,
        lvl == 100 && charging ? "-charged" : charging ? "-charging" : "");
    char *txt = g_strdup_printf("%d%%", pct);
    set_str(&bat_icon_name, icon);
    set_str(&bat_text, txt);
    /* Amber at 20, red at 10: spending red at 20 leaves nothing to say
     * at the moment it really matters (§1-3). */
    bat_state = charging ? 0 : pct <= 10 ? 2 : pct <= 20 ? 1 : 0;
    g_free(icon);
    g_free(txt);
    lp_json_free(j);
    paint_status();
}

static gboolean poll_net(gpointer d)
{
    (void)d;
    if (!busy_net) {
        busy_net = TRUE;
        const char *a[] = { "lp-net", "status", "--json", NULL };
        lp_run_async(a, net_done, NULL);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean poll_vol(gpointer d)
{
    (void)d;
    if (!busy_vol) {
        busy_vol = TRUE;
        const char *a[] = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL };
        lp_run_async(a, vol_done, NULL);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean poll_bat(gpointer d)
{
    (void)d;
    if (!busy_bat) {
        busy_bat = TRUE;
        const char *a[] = { "lp-tune", "status", "--json", NULL };
        lp_run_async(a, bat_done, NULL);
    }
    return G_SOURCE_CONTINUE;
}

/* ── the input language ──────────────────────────────────────────── */

/* EN or 한, whichever typing on the keys produces now, and a click
 * switches it - the one piece of state a person otherwise has to find
 * out by typing a letter and deleting it.
 *
 * Who knows the answer depends on the compositor. Under sway the input
 * method is lp-osk, for the laptop's keys and the on-screen keyboard
 * alike, and `lp-osk watch` (its socket, kept open) pushes a line of
 * JSON at every change - no polling. Under wayfire the laptop's keys go
 * through fcitx5 (session-run says why), which has no signal for its
 * state, so while fcitx5 is on the bus its State is asked every 300 ms:
 * one D-Bus round trip, no process started. Its state is shared by
 * every window (ShareInputState=All), so one answer is the answer.
 *
 * A click, or a choice from the right-click menu, sets both: fcitx5 for
 * the keys and lp-osk for the on-screen keyboard, so the two never
 * disagree about which language comes out. */

static GDBusConnection *bus;
static gboolean fcitx_up, fcitx_busy;
static guint fcitx_timer;
static int lang_now = -1;                   /* 0 EN, 1 한, -1 not known yet */
static GSocketConnection *osk_conn;
static GDataInputStream *osk_in;
static GtkWidget *lang_menu;

static void paint_lang(void)
{
    for (GList *l = bars; l; l = l->next) {
        Bar *b = l->data;
        gtk_label_set_text(GTK_LABEL(b->lang_label), lang_now == 1 ? "한" : "EN");
        gtk_widget_set_tooltip_text(b->lang, lang_now == 1
            ? T("Korean (click for English)", "한국어 (누르면 영어)")
            : T("English (click for Korean)", "영어 (누르면 한국어)"));
    }
}

static void set_lang(int ko)
{
    if (ko == lang_now)
        return;
    lang_now = ko;
    paint_lang();
}

static void fcitx_done(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    fcitx_busy = FALSE;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (!v)
        return;
    int st = 0;
    g_variant_get(v, "(i)", &st);
    g_variant_unref(v);
    set_lang(st == 2);                      /* 1 inactive (keys as printed), 2 active */
}

static gboolean poll_fcitx(gpointer d)
{
    (void)d;
    if (fcitx_up && !fcitx_busy) {
        fcitx_busy = TRUE;
        g_dbus_connection_call(bus, "org.fcitx.Fcitx5", "/controller",
                               "org.fcitx.Fcitx.Controller1", "State", NULL,
                               G_VARIANT_TYPE("(i)"), G_DBUS_CALL_FLAGS_NONE, 1000,
                               NULL, fcitx_done, NULL);
    }
    return G_SOURCE_CONTINUE;
}

static void fcitx_appeared(GDBusConnection *c, const char *name, const char *owner, gpointer d)
{
    (void)c; (void)name; (void)owner; (void)d;
    fcitx_up = TRUE;
    if (!fcitx_timer)
        fcitx_timer = g_timeout_add(300, poll_fcitx, NULL);
    poll_fcitx(NULL);
}

static void fcitx_vanished(GDBusConnection *c, const char *name, gpointer d)
{
    (void)c; (void)name; (void)d;
    fcitx_up = FALSE;
    if (fcitx_timer) {
        g_source_remove(fcitx_timer);
        fcitx_timer = 0;
    }
}

static char *osk_socket(void)
{
    return g_build_filename(g_get_user_runtime_dir(), "lp-osk.sock", NULL);
}

static void osk_watch(void);

static gboolean osk_retry(gpointer d)
{
    (void)d;
    osk_watch();
    return G_SOURCE_REMOVE;
}

static void osk_line(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)src; (void)d;
    char *line = g_data_input_stream_read_line_finish_utf8(osk_in, res, NULL, NULL);
    if (!line) {
        /* lp-osk went away (it is kept, so it is coming back). */
        g_clear_object(&osk_in);
        g_clear_object(&osk_conn);
        g_timeout_add_seconds(2, osk_retry, NULL);
        return;
    }
    LpJson *j = lp_json_parse(line);
    if (j && !fcitx_up)
        set_lang(g_strcmp0(lp_json_str(j, "language", "en"), "ko") == 0);
    lp_json_free(j);
    g_free(line);
    g_data_input_stream_read_line_async(osk_in, G_PRIORITY_DEFAULT, NULL, osk_line, NULL);
}

static void osk_connected(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    osk_conn = g_socket_client_connect_finish(G_SOCKET_CLIENT(src), res, NULL);
    g_object_unref(src);
    if (!osk_conn) {
        g_timeout_add_seconds(2, osk_retry, NULL);
        return;
    }
    GOutputStream *os = g_io_stream_get_output_stream(G_IO_STREAM(osk_conn));
    g_output_stream_write_all(os, "watch\n", 6, NULL, NULL, NULL);
    osk_in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(osk_conn)));
    g_data_input_stream_read_line_async(osk_in, G_PRIORITY_DEFAULT, NULL, osk_line, NULL);
}

static void osk_watch(void)
{
    char *path = osk_socket();
    GSocketAddress *a = g_unix_socket_address_new(path);
    g_free(path);
    g_socket_client_connect_async(g_socket_client_new(), G_SOCKET_CONNECTABLE(a), NULL,
                                  osk_connected, NULL);
    g_object_unref(a);
}

/* One request to lp-osk over its socket - a local connect, microseconds -
 * and only if it is not running, the command as a process. */
static void osk_tell(const char *lang)
{
    char *path = osk_socket();
    GSocketAddress *a = g_unix_socket_address_new(path);
    g_free(path);
    GSocketClient *cl = g_socket_client_new();
    GSocketConnection *c = g_socket_client_connect(cl, G_SOCKET_CONNECTABLE(a), NULL, NULL);
    g_object_unref(a);
    g_object_unref(cl);
    if (c) {
        char *req = g_strdup_printf("lang\t%s\n", lang);
        g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(c)),
                                  req, strlen(req), NULL, NULL, NULL);
        g_free(req);
        g_object_unref(c);
    } else {
        const char *argv[] = { "lp-osk", "lang", lang, NULL };
        lp_spawn(argv);
    }
}

static void choose_lang(int ko)
{
    if (fcitx_up)
        g_dbus_connection_call(bus, "org.fcitx.Fcitx5", "/controller",
                               "org.fcitx.Fcitx.Controller1", ko ? "Activate" : "Deactivate",
                               NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL, NULL);
    osk_tell(ko ? "ko" : "en");
    set_lang(ko);                           /* the next report confirms it */
}

static void on_lang(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    choose_lang(lang_now == 1 ? 0 : 1);
}

static void m_lang(GtkMenuItem *m, gpointer d)
{
    (void)m;
    choose_lang(GPOINTER_TO_INT(d));
}

static void m_keyboard_settings(GtkMenuItem *m, gpointer d)
{
    (void)m; (void)d;
    const char *a[] = { "lp-settings", "keyboard", NULL };
    lp_spawn(a);
}

/* The right button's press is only swallowed (so the button does not
 * toggle); the menu comes on its release, as lp_on_hold's do - opened on
 * the press, the release that follows closes it again (lp-shell.c). */
static gboolean on_lang_press(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    (void)w; (void)d;
    return e->button == GDK_BUTTON_SECONDARY;
}

static gboolean on_lang_release(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    (void)d;
    if (e->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    if (!lang_menu) {
        lang_menu = gtk_menu_new();
        GtkWidget *en = gtk_menu_item_new_with_label(T("English", "영어"));
        GtkWidget *ko = gtk_menu_item_new_with_label(T("Korean (한국어)", "한국어"));
        GtkWidget *st = gtk_menu_item_new_with_label(T("Keyboard Settings…", "키보드 설정…"));
        g_signal_connect(en, "activate", G_CALLBACK(m_lang), GINT_TO_POINTER(0));
        g_signal_connect(ko, "activate", G_CALLBACK(m_lang), GINT_TO_POINTER(1));
        g_signal_connect(st, "activate", G_CALLBACK(m_keyboard_settings), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(lang_menu), en);
        gtk_menu_shell_append(GTK_MENU_SHELL(lang_menu), ko);
        gtk_menu_shell_append(GTK_MENU_SHELL(lang_menu), gtk_separator_menu_item_new());
        gtk_menu_shell_append(GTK_MENU_SHELL(lang_menu), st);
        gtk_style_context_add_class(gtk_widget_get_style_context(lang_menu), "lp-menu");
        gtk_widget_show_all(lang_menu);
    }
    gtk_menu_popup_at_widget(GTK_MENU(lang_menu), w, GDK_GRAVITY_SOUTH_EAST,
                             GDK_GRAVITY_NORTH_EAST, (GdkEvent *)e);
    return TRUE;
}

static void lang_init(void)
{
    bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (bus)
        g_bus_watch_name_on_connection(bus, "org.fcitx.Fcitx5", G_BUS_NAME_WATCHER_FLAGS_NONE,
                                       fcitx_appeared, fcitx_vanished, NULL, NULL);
    osk_watch();
}

/* ── buttons ─────────────────────────────────────────────────────── */

/* Ask a running component first, over its socket; start it only if it
 * is not there. A spawn is a fork, an exec and a GTK start-up before the
 * finger's tap shows anything; the socket is a few hundred microseconds. */
static void ask(const char *name, const char *const *argv)
{
    if (!lp_send(name, argv))
        lp_spawn(argv);
}

static int mon_index(Bar *bar)
{
    GdkDisplay *dpy = gdk_display_get_default();
    int n = gdk_display_get_n_monitors(dpy);
    for (int i = 0; i < n; i++)
        if (gdk_display_get_monitor(dpy, i) == bar->mon)
            return i;
    return 0;
}

/* ── the LP menu ─────────────────────────────────────────────────── */

static void run_argv(const char *const *a)
{
    lp_spawn(a);
}

static void m_about(GtkMenuItem *m, gpointer d)
{ (void)m; (void)d; const char *a[] = { "lp-settings", "system", NULL }; run_argv(a); }
static void m_apps(GtkMenuItem *m, gpointer d)
{ (void)m; (void)d; const char *a[] = { "lp-appgrid", "toggle", NULL }; ask("appgrid", a); }
static void m_files(GtkMenuItem *m, gpointer d)
{ (void)m; (void)d; const char *a[] = { "lp-files", NULL }; run_argv(a); }
static void m_terminal(GtkMenuItem *m, gpointer d)
{
    (void)m; (void)d;
    const char *kgx[] = { "kgx", NULL }, *foot[] = { "foot", NULL };
    char *p = g_find_program_in_path("kgx");
    run_argv(p ? kgx : foot);
    g_free(p);
}
static void m_settings(GtkMenuItem *m, gpointer d)
{ (void)m; (void)d; const char *a[] = { "lp-settings", NULL }; run_argv(a); }
static void m_software(GtkMenuItem *m, gpointer d)
{ (void)m; (void)d; const char *a[] = { "lp-software", NULL }; run_argv(a); }
static void m_tasks(GtkMenuItem *m, gpointer d)
{ (void)m; (void)d; const char *a[] = { "lp-tasks", NULL }; run_argv(a); }
static void m_lock(GtkMenuItem *m, gpointer d)
{ (void)m; (void)d; const char *a[] = { "lp-lock", NULL }; run_argv(a); }

/* Logging out, restarting and shutting down ask first, in quick
 * settings' own dialog: one question, asked the same way everywhere. */
static void m_confirm(GtkMenuItem *m, gpointer d)
{
    (void)m;
    const char *a[] = { "lp-quick", "confirm", d, NULL };
    ask("quick", a);
}

static void menu_item(GtkWidget *menu, const char *label, GCallback cb, gpointer data)
{
    GtkWidget *mi = gtk_menu_item_new_with_label(label);
    g_signal_connect(mi, "activate", cb, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

static void menu_sep(GtkWidget *menu)
{
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

static GtkWidget *lp_menu_build(void)
{
    GtkWidget *menu = gtk_menu_new();
    menu_item(menu, T("About This Computer", "이 컴퓨터에 관하여"), G_CALLBACK(m_about), NULL);
    menu_sep(menu);
    menu_item(menu, T("All Applications", "모든 앱"), G_CALLBACK(m_apps), NULL);
    menu_item(menu, T("Files", "파일"), G_CALLBACK(m_files), NULL);
    menu_item(menu, T("Terminal", "터미널"), G_CALLBACK(m_terminal), NULL);
    menu_item(menu, T("Software", "소프트웨어"), G_CALLBACK(m_software), NULL);
    menu_item(menu, T("Task Manager", "작업 관리자"), G_CALLBACK(m_tasks), NULL);
    menu_item(menu, T("Settings", "설정"), G_CALLBACK(m_settings), NULL);
    menu_sep(menu);
    menu_item(menu, T("Lock Screen", "화면 잠금"), G_CALLBACK(m_lock), NULL);
    menu_item(menu, T("Log Out…", "로그아웃…"), G_CALLBACK(m_confirm), (gpointer)"logout");
    menu_item(menu, T("Restart…", "다시 시작…"), G_CALLBACK(m_confirm), (gpointer)"restart");
    menu_item(menu, T("Shut Down…", "시스템 종료…"), G_CALLBACK(m_confirm), (gpointer)"off");
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "lp-menu");
    gtk_widget_show_all(menu);
    return menu;
}

static void on_activities(GtkButton *b, gpointer d)
{
    (void)d;
    if (lp_hold_consumed(GTK_WIDGET(b)))
        return;
    static GtkWidget *menu;
    if (!menu)
        menu = lp_menu_build();
    GtkWidget *w = GTK_WIDGET(b);
    gtk_menu_popup_at_widget(GTK_MENU(menu), w, GDK_GRAVITY_SOUTH_WEST,
                             GDK_GRAVITY_NORTH_WEST, NULL);
}

static void on_keyboard(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    const char *a[] = { "lp-osk", "toggle", NULL };
    lp_spawn(a);
}

static void on_status(GtkButton *b, gpointer d)
{
    (void)b;
    Bar *bar = d;
    /* Tell quick settings which monitor to open on: the one whose bar
     * was touched, not whichever the compositor happens to prefer. */
    char mon[16];
    g_snprintf(mon, sizeof mon, "%d", mon_index(bar));
    const char *a[] = { "lp-quick", "toggle", mon, NULL };
    ask("quick", a);
}

/* ── pulling quick settings down from the top edge ──────────────── */

static void pull_send(Bar *b, const char *verb, double v)
{
    char num[32], mon[16];
    g_ascii_formatd(num, sizeof num, "%.1f", v);
    g_snprintf(mon, sizeof mon, "%d", mon_index(b));
    const char *a[] = { "lp-quick", verb, num, mon, NULL };
    if (!lp_send("quick", a) && !strcmp(verb, "release-px") && v > 0) {
        /* Not running: the drag could not be shown, but the intent was
         * clear - open it. */
        const char *s[] = { "lp-quick", "show", mon, NULL };
        lp_spawn(s);
    }
}

static void on_pull_update(GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    Bar *b = d;
    if (!b->pulling) {
        /* Mostly downwards and past a threshold: a pull, not a tap that
         * wobbled. Only then is the touch taken from the button. */
        if (dy < 10 || dy < 1.5 * ABS(dx))
            return;
        b->pulling = TRUE;
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
        lp_velocity_reset(&b->pull_v);
    }
    double py = MAX(0.0, dy - 10);
    lp_velocity_add(&b->pull_v, g_get_monotonic_time(), py);
    pull_send(b, "drag-px", py);
}

static void on_pull_end(GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    (void)g; (void)dx;
    Bar *b = d;
    if (!b->pulling)
        return;
    b->pulling = FALSE;
    lp_velocity_add(&b->pull_v, g_get_monotonic_time(), MAX(0.0, dy - 10));
    pull_send(b, "release-px", lp_velocity_get(&b->pull_v));
}

static void on_pull_cancel(GtkGesture *g, GdkEventSequence *seq, gpointer d)
{
    (void)g; (void)seq;
    Bar *b = d;
    if (b->pulling) {
        b->pulling = FALSE;
        pull_send(b, "release-px", 0);
    }
}

/* ── commands from outside ───────────────────────────────────────── */

static void set_open(const char *what, gboolean open)
{
    for (GList *l = bars; l; l = l->next) {
        Bar *b = l->data;
        GtkWidget *w = g_strcmp0(what, "grid") == 0 ? b->activities
                     : g_strcmp0(what, "quick") == 0 ? b->status : NULL;
        if (!w)
            continue;
        GtkStyleContext *sc = gtk_widget_get_style_context(w);
        if (open)
            gtk_style_context_add_class(sc, "lp-open");
        else
            gtk_style_context_remove_class(sc, "lp-open");
    }
}

static void on_command(int argc, char **argv, gpointer d)
{
    (void)d;
    if (argc >= 2 && g_strcmp0(argv[1], "refresh") == 0) {
        poll_vol(NULL);
        poll_net(NULL);
        poll_bat(NULL);
    } else if (argc >= 2 && g_strcmp0(argv[1], "lang") == 0) {
        /* A switch key under wayfire: show it now, not at the next poll. */
        poll_fcitx(NULL);
    } else if (argc >= 4 && g_strcmp0(argv[1], "open") == 0) {
        set_open(argv[2], g_strcmp0(argv[3], "1") == 0);
    }
}

/* ── one bar per monitor ─────────────────────────────────────────── */

static GtkWidget *icon_img(const char *name)
{
    return lp_icon(name, 18);
}

static Bar *bar_new(GdkMonitor *mon)
{
    Bar *b = g_new0(Bar, 1);
    b->mon = mon;
    b->win = lp_layer_window("lp-panel", GTK_LAYER_SHELL_LAYER_TOP,
                             LP_EDGE_TOP | LP_EDGE_LEFT | LP_EDGE_RIGHT);
    gtk_layer_set_monitor(b->win, mon);
    gtk_layer_auto_exclusive_zone_enable(b->win);
    gtk_widget_set_size_request(GTK_WIDGET(b->win), -1, BAR_HEIGHT);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "lp-bar");
    gtk_container_add(GTK_CONTAINER(b->win), box);

    /* left */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    b->activities = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(b->activities), lp_icon("distributor-logo-lp", 22));
    gtk_widget_set_tooltip_text(b->activities, T("LP menu", "LP 메뉴"));
    gtk_style_context_add_class(gtk_widget_get_style_context(b->activities),
                                "lp-activities");
    g_signal_connect(b->activities, "clicked", G_CALLBACK(on_activities), b);
    gtk_box_pack_start(GTK_BOX(left), b->activities, FALSE, FALSE, 0);

    b->appname = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(b->appname), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(b->appname), 32);
    gtk_style_context_add_class(gtk_widget_get_style_context(b->appname),
                                "lp-appname");
    gtk_box_pack_start(GTK_BOX(left), b->appname, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), left, FALSE, FALSE, 0);

    /* centre */
    b->clock = gtk_button_new();
    b->clock_label = gtk_label_new("");
    gtk_container_add(GTK_CONTAINER(b->clock), b->clock_label);
    gtk_style_context_add_class(gtk_widget_get_style_context(b->clock),
                                "lp-clock");
    g_signal_connect(b->clock, "clicked", G_CALLBACK(on_clock), b);
    gtk_box_set_center_widget(GTK_BOX(box), b->clock);

    /* right */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(right), lp_tray_new(), FALSE, FALSE, 0);

    b->lang = gtk_button_new();
    b->lang_label = gtk_label_new("EN");
    gtk_container_add(GTK_CONTAINER(b->lang), b->lang_label);
    gtk_style_context_add_class(gtk_widget_get_style_context(b->lang), "lp-lang");
    g_signal_connect(b->lang, "clicked", G_CALLBACK(on_lang), b);
    g_signal_connect(b->lang, "button-press-event", G_CALLBACK(on_lang_press), b);
    g_signal_connect(b->lang, "button-release-event", G_CALLBACK(on_lang_release), b);
    gtk_box_pack_start(GTK_BOX(right), b->lang, FALSE, FALSE, 0);

    GtkWidget *kbd = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(kbd), icon_img("input-keyboard-symbolic"));
    gtk_style_context_add_class(gtk_widget_get_style_context(kbd), "lp-kbd");
    g_signal_connect(kbd, "clicked", G_CALLBACK(on_keyboard), b);
    gtk_box_pack_start(GTK_BOX(right), kbd, FALSE, FALSE, 0);

    b->status = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(b->status), "lp-status");
    GtkWidget *st = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    b->wifi_icon = icon_img("network-wireless-offline-symbolic");
    b->vol_icon = icon_img("audio-volume-muted-symbolic");
    b->bat_icon = icon_img("battery-missing-symbolic");
    b->bat_label = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(st), b->wifi_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(st), b->vol_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(st), b->bat_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(st), b->bat_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(st), icon_img("system-shutdown-symbolic"),
                       FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(b->status), st);
    g_signal_connect(b->status, "clicked", G_CALLBACK(on_status), b);
    gtk_box_pack_start(GTK_BOX(right), b->status, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), right, FALSE, FALSE, 0);

    /* Capture phase: the pull sees the touch before the button under it
     * does, and takes it only once it is clearly a pull. */
    b->pull = gtk_gesture_drag_new(box);
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(b->pull), FALSE);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(b->pull),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(b->pull, "drag-update", G_CALLBACK(on_pull_update), b);
    g_signal_connect(b->pull, "drag-end", G_CALLBACK(on_pull_end), b);
    g_signal_connect(b->pull, "cancel", G_CALLBACK(on_pull_cancel), b);

    gtk_widget_show_all(GTK_WIDGET(b->win));
    bars = g_list_append(bars, b);
    return b;
}

static void bar_free(Bar *b)
{
    bars = g_list_remove(bars, b);
    g_clear_object(&b->pull);
    gtk_widget_destroy(GTK_WIDGET(b->win));
    g_free(b);
}

static void on_monitor_added(GdkDisplay *d, GdkMonitor *m, gpointer u)
{
    (void)d; (void)u;
    bar_new(m);
    set_clock();
    paint_status();
    paint_lang();
    on_toplevels(NULL);
}

static void on_monitor_removed(GdkDisplay *d, GdkMonitor *m, gpointer u)
{
    (void)d; (void)u;
    for (GList *l = bars; l; l = l->next)
        if (((Bar *)l->data)->mon == m) {
            bar_free(l->data);
            break;
        }
}

int main(int argc, char **argv)
{
    if (!lp_single_instance("panel", argc, argv, on_command, NULL))
        return 0;
    lp_shell_init(&argc, &argv);

    /* Values are never freed: the cache lives as long as the bar, and a
     * missing app is cached as NULL, which g_object_unref would not take. */
    app_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    cal_build();
    lp_tray_init();

    GdkDisplay *dpy = gdk_display_get_default();
    for (int i = 0; i < gdk_display_get_n_monitors(dpy); i++)
        bar_new(gdk_display_get_monitor(dpy, i));
    g_signal_connect(dpy, "monitor-added", G_CALLBACK(on_monitor_added), NULL);
    g_signal_connect(dpy, "monitor-removed", G_CALLBACK(on_monitor_removed), NULL);

    if (lp_toplevels_init())
        lp_toplevels_watch(on_toplevels, NULL);
    on_toplevels(NULL);
    lang_init();
    paint_lang();

    clock_tick(NULL);
    clock_set_watch();
    char *clock_file = lp_config_path("clock");
    clock_watch(clock_file);
    g_free(clock_file);
    clock_watch("/etc/localtime");
    paint_status();
    poll_net(NULL);
    poll_vol(NULL);
    poll_bat(NULL);
    /* Every spawn is a process start, and on a slow or emulated machine
     * that is not free: volume only needs a poll for changes made
     * elsewhere (the keys and quick settings send `lp-panel refresh`). */
    g_timeout_add_seconds(15, poll_net, NULL);
    g_timeout_add_seconds(30, poll_vol, NULL);
    g_timeout_add_seconds(30, poll_bat, NULL);

    gtk_main();
    return 0;
}
