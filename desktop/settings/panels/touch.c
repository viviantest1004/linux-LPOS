/*
 * touch.c - Touch & mouse: the pointer's size (access.c's row, the same
 * setting as Accessibility's), the touchpad, a mouse, the touchscreen,
 * and when the on-screen keyboard comes up.
 *
 * ── Pointer settings are wayfire's input options ──
 *
 * Wayfire keeps libinput's settings in the [input] section of its config
 * and applies a change the moment the file changes, so the file is both
 * the live change and the record:
 *
 *     [input]
 *     tap_to_click = true
 *     natural_scroll = true
 *     touchpad_cursor_speed = 0.200000       -1 .. 1
 *     mouse_cursor_speed = 0.000000          -1 .. 1
 *     disable_touchpad_while_typing = true
 *     left_handed_mode = false
 *     click_method = clickfinger             or button-areas
 *
 * Under sway (the fallback session, and the headless test rig) the same
 * change is also sent with `swaymsg input type:touchpad ...`, because
 * sway does not read wayfire.ini.
 *
 * ── The touchscreen ──
 *
 * Turning it off is a sway command (`input type:touch events disabled`).
 * Wayfire 0.7 has no option for it - its [input-device:NAME] section only
 * maps a device to an output - so under wayfire the row says that instead
 * of pretending. The choice is kept in ~/.config/lp/touch.conf
 * (touchscreen=on|off) and put back at login.
 *
 * ── The on-screen keyboard ──
 *
 * lp-osk's own file, ~/.config/lp/osk.ini, [keyboard] auto-show =
 * touch|always|off. Only that key is written; the keyboard's height and
 * layouts, which lp-osk writes itself, stay as they were.
 */
#include "core.h"

#include <math.h>
#include <string.h>

static char *touch_conf(void) { return lp_config_path("touch.conf"); }

static gboolean wf_bool(const char *key, gboolean dflt)
{
    char *ini = wayfire_ini();
    char *v = ini_get(ini, "input", key);
    g_free(ini);
    gboolean r = v ? (!strcmp(v, "true") || !strcmp(v, "1")) : dflt;
    g_free(v);
    return r;
}

static double wf_num(const char *key, double dflt)
{
    char *ini = wayfire_ini();
    char *v = ini_get(ini, "input", key);
    g_free(ini);
    double r = v ? g_ascii_strtod(v, NULL) : dflt;
    g_free(v);
    return r;
}

static void wf_set(const char *key, const char *value)
{
    char *ini = wayfire_ini();
    ini_set(ini, "input", key, value);
    g_free(ini);
}

/* A switch whose data is the wayfire key and, after a '|', the sway
 * "input type:X KEY" words to send, e.g. "tap_to_click|touchpad tap". */
static void on_wf_switch(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    char **parts = g_strsplit(p, "|", 2);
    wf_set(parts[0], on ? "true" : "false");
    if (parts[1]) {
        char **w = g_strsplit(parts[1], " ", 2);
        char *type = g_strdup_printf("type:%s", w[0]);
        const char *a[] = { "input", type, w[1], on ? "enabled" : "disabled", NULL };
        lp_swaymsg(a);
        g_free(type);
        g_strfreev(w);
    }
    g_strfreev(parts);
    GtkWidget *row = g_object_get_data(sw, "lp-row");
    const char *title = row ? g_object_get_data(G_OBJECT(row), "lp-title") : "";
    lp_toast(FALSE, "%s: %s", title, on ? T("on", "켬") : T("off", "끔"));
}

typedef struct { char *key, *value; } wf_write_t;

static void wf_write_now(gpointer p)
{
    wf_write_t *w = p;
    wf_set(w->key, w->value);
}

static void wf_write_free(gpointer p)
{
    wf_write_t *w = p;
    g_free(w->key); g_free(w->value); g_free(w);
}

/* Under sway the speed follows the finger (swaymsg, which costs
 * nothing); wayfire.ini is written once the slider has been still for a
 * fifth of a second, because every write makes wayfire re-read its whole
 * config. */
static void on_speed(GtkRange *r, gpointer p)
{
    const char *which = p;                /* "touchpad" or "mouse" */
    double v = gtk_range_get_value(r);
    wf_write_t *w = g_new0(wf_write_t, 1);
    char s[32];
    g_ascii_formatd(s, sizeof s, "%.6f", v);
    w->key = g_strdup_printf("%s_cursor_speed", which);
    w->value = g_strdup(s);
    lp_later(w->key, 200, wf_write_now, w, wf_write_free);
    char *type = g_strdup_printf("type:%s", !strcmp(which, "mouse") ? "pointer" : "touchpad");
    char sp[32];
    g_ascii_formatd(sp, sizeof sp, "%.2f", v);
    const char *a[] = { "input", type, "pointer_accel", sp, NULL };
    lp_swaymsg(a);
    g_free(type);
}

static char *speed_words(double v)
{
    if (fabs(v) < 0.05) return g_strdup(T("Default", "기본"));
    return g_strdup_printf("%+d", (int)lround(v * 10));
}

static void on_left_handed(GtkWidget *seg, int i, gpointer p)
{
    (void)seg; (void)p;
    wf_set("left_handed_mode", i == 1 ? "true" : "false");
    const char *a[] = { "input", "type:pointer", "left_handed", i == 1 ? "enabled" : "disabled", NULL };
    lp_swaymsg(a);
    lp_toast(FALSE, i == 1 ? T("The right button is now the primary one", "이제 오른쪽 버튼이 주 버튼입니다")
                           : T("The left button is now the primary one", "이제 왼쪽 버튼이 주 버튼입니다"));
}

/* The secondary (right) click on a touchpad with no buttons of its own:
 * two fingers pressed down anywhere, as on a Mac and on Windows' precision
 * touchpads, or the bottom-right corner pressed (libinput's "button
 * areas", its own default on most PC clickpads - where a two-finger press
 * is a plain left click, and the right button seemed not to exist).
 * A two-finger tap is a right click either way while tapping is on. */
static int click_method_index(void)
{
    char *ini = wayfire_ini();
    char *v = ini_get(ini, "input", "click_method");
    g_free(ini);
    int r = v && !strcmp(v, "button-areas") ? 1 : 0;
    g_free(v);
    return r;
}

static void on_click_method(GtkWidget *seg, int i, gpointer p)
{
    (void)seg; (void)p;
    wf_set("click_method", i == 1 ? "button-areas" : "clickfinger");
    const char *a[] = { "input", "type:touchpad", "click_method",
                        i == 1 ? "button_areas" : "clickfinger", NULL };
    lp_swaymsg(a);
    lp_toast(FALSE, i == 1 ? T("Right click: press the bottom-right corner",
                               "오른쪽 클릭: 오른쪽 아래 모서리를 누르기")
                           : T("Right click: press with two fingers",
                               "오른쪽 클릭: 두 손가락으로 누르기"));
}

/* ── touchscreen ────────────────────────────────────────────────────── */

static void apply_touchscreen(gboolean on)
{
    const char *a[] = { "input", "type:touch", "events", on ? "enabled" : "disabled", NULL };
    lp_swaymsg(a);
}

static void on_touchscreen(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    char *c = touch_conf();
    gboolean ok = kv_set(c, "touchscreen", on ? "on" : "off");
    g_free(c);
    if (!ok) { LP_QUIET(gtk_switch_set_active(GTK_SWITCH(sw), !on)); return; }
    apply_touchscreen(on);
    lp_toast(FALSE, on ? T("The touchscreen is on", "터치스크린을 켰습니다")
                       : T("The touchscreen is off - the touchpad and keyboard still work",
                           "터치스크린을 껐습니다 - 터치패드와 키보드는 그대로 씁니다"));
}

/* ── on-screen keyboard ─────────────────────────────────────────────── */

static const lp_opt_t OSK[] = {
    { "touch",  "When I use the touchscreen", "터치스크린을 쓸 때" },
    { "always", "Whenever I type",            "입력할 때마다" },
    { "off",    "Only when I ask",            "부를 때만" },
    { NULL, NULL, NULL }
};

static void on_osk(GObject *dd, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    const char *v = row_option_value(dd);
    if (!v) return;
    char *ini = lp_config_path("osk.ini");
    if (ini_set(ini, "keyboard", "auto-show", v))
        lp_toast(FALSE, !strcmp(v, "off") ? T("The keyboard comes up only from its button in the top bar",
                                              "화상 키보드는 상단바의 단추로만 나타납니다")
                                          : T("Saved", "저장했습니다"));
    g_free(ini);
}

static void on_osk_now(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    static const char *const v[] = { "lp-osk", "show", NULL };
    lp_spawn_bg(v);
}

static GtkWidget *build(void)
{
    GtkWidget *page = page_new(T("Touch & mouse", "터치와 마우스"), NULL);

    /* The same row as Accessibility's: one pointer size. */
    GtkWidget *pg = group_new(page, T("Pointer", "포인터"));
    lp_pointer_size_row(pg);

    GtkWidget *tp = group_new(page, T("Touchpad", "터치패드"));
    row_switch(tp, T("Tap to click", "탭하여 클릭"), NULL, wf_bool("tap_to_click", TRUE),
               G_CALLBACK(on_wf_switch), (gpointer)"tap_to_click|touchpad tap");
    row_switch(tp, T("Natural scrolling", "자연스러운 스크롤"),
               T("The content moves with your fingers, as on a phone",
                 "휴대전화처럼 내용이 손가락을 따라 움직입니다"),
               wf_bool("natural_scroll", FALSE),
               G_CALLBACK(on_wf_switch), (gpointer)"natural_scroll|touchpad natural_scroll");
    row_switch(tp, T("Off while typing", "입력 중에는 끄기"),
               T("A palm on the touchpad does not move the pointer while you type",
                 "입력하는 동안 손바닥이 닿아도 포인터가 움직이지 않습니다"),
               wf_bool("disable_touchpad_while_typing", TRUE),
               G_CALLBACK(on_wf_switch), (gpointer)"disable_touchpad_while_typing|touchpad dwt");
    const char *clicks[] = { T("Two fingers", "두 손가락으로 누르기"),
                             T("Bottom-right corner", "오른쪽 아래 모서리"), NULL };
    row_segmented(tp, T("Right click", "오른쪽 클릭"),
                  T("Tapping with two fingers is a right click too",
                    "두 손가락으로 톡 쳐도 오른쪽 클릭입니다"),
                  clicks, click_method_index(), G_CALLBACK(on_click_method), NULL);
    GtkWidget *r = row_scale(tp, T("Touchpad speed", "터치패드 속도"), NULL, -1, 1, 0.05,
                             wf_num("touchpad_cursor_speed", 0), G_CALLBACK(on_speed), (gpointer)"touchpad");
    g_object_set_data(G_OBJECT(row_control(r)), "lp-fmt", (gpointer)speed_words);
    gtk_scale_add_mark(GTK_SCALE(row_control(r)), 0, GTK_POS_BOTTOM, NULL);
    LP_QUIET(gtk_range_set_value(GTK_RANGE(row_control(r)), wf_num("touchpad_cursor_speed", 0) + 0.01));
    LP_QUIET(gtk_range_set_value(GTK_RANGE(row_control(r)), wf_num("touchpad_cursor_speed", 0)));

    GtkWidget *mg = group_new(page, T("Mouse", "마우스"));
    r = row_scale(mg, T("Mouse speed", "마우스 속도"), NULL, -1, 1, 0.05,
                  wf_num("mouse_cursor_speed", 0), G_CALLBACK(on_speed), (gpointer)"mouse");
    g_object_set_data(G_OBJECT(row_control(r)), "lp-fmt", (gpointer)speed_words);
    gtk_scale_add_mark(GTK_SCALE(row_control(r)), 0, GTK_POS_BOTTOM, NULL);
    LP_QUIET(gtk_range_set_value(GTK_RANGE(row_control(r)), wf_num("mouse_cursor_speed", 0) + 0.01));
    LP_QUIET(gtk_range_set_value(GTK_RANGE(row_control(r)), wf_num("mouse_cursor_speed", 0)));
    const char *hands[] = { T("Left", "왼쪽"), T("Right", "오른쪽"), NULL };
    row_segmented(mg, T("Primary button", "주 버튼"), NULL, hands,
                  wf_bool("left_handed_mode", FALSE) ? 1 : 0, G_CALLBACK(on_left_handed), NULL);

    GtkWidget *ts = group_new(page, T("Touchscreen", "터치스크린"));
    char *c = touch_conf();
    char *tsv = kv_get(c, "touchscreen");
    g_free(c);
    r = row_switch(ts, T("Touchscreen", "터치스크린"), NULL, !tsv || strcmp(tsv, "off") != 0,
                   G_CALLBACK(on_touchscreen), NULL);
    g_free(tsv);
    if (!lp_sway())
        row_set_detail(r, T("Saved, but only a sway session can turn the touchscreen off; "
                            "under wayfire it stays on",
                            "저장은 되지만 터치스크린을 끄는 것은 sway 세션에서만 됩니다. "
                            "wayfire 에서는 켜진 채로 남습니다"));

    char *ini = lp_config_path("osk.ini");
    char *auto_show = ini_get(ini, "keyboard", "auto-show");
    g_free(ini);
    GtkWidget *og = group_new(page, T("On-screen keyboard", "화상 키보드"));
    row_options(og, T("Show the on-screen keyboard", "화상 키보드 보이기"),
                T("It always has a close button, and the top bar has a keyboard button",
                  "언제나 닫기 단추가 있고, 상단바에 키보드 단추가 있습니다"),
                OSK, auto_show, 0, G_CALLBACK(on_osk), NULL);
    g_free(auto_show);
    row_button(og, T("Try it", "시험해 보기"), NULL, T("Show keyboard", "키보드 보이기"),
               G_CALLBACK(on_osk_now), NULL);
    return page;
}

static void restore(void)
{
    char *c = touch_conf();
    char *v = kv_get(c, "touchscreen");
    if (v && !strcmp(v, "off"))
        apply_touchscreen(FALSE);
    g_free(v); g_free(c);
}

static const char *const KEYS[] = {
    "Pointer size", "포인터 크기",
    "Cursor size", "커서 크기",
    "Tap to click", "탭하여 클릭",
    "Natural scrolling", "자연스러운 스크롤",
    "Touchpad speed", "터치패드 속도",
    "Off while typing", "입력 중에는 끄기",
    "Mouse speed", "마우스 속도",
    "Primary button", "주 버튼",
    "Touchscreen", "터치스크린",
    "Show the on-screen keyboard", "화상 키보드 보이기",
    "Touchpad", "터치패드",
    NULL
};

const lp_panel_t lp_panel_touch = {
    "touch", "Touch & mouse", "터치와 마우스", "input-touchpad-symbolic", build, KEYS, restore
};
