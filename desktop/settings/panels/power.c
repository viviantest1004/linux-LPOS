/*
 * power.c - Power: the power mode, the battery, when the screen goes
 * dark and the machine goes to sleep, and what the lid and the power
 * button do.
 *
 * ── Two owners, and this panel is neither ──
 *
 * lp-tune (the main loop's daemon) owns power: the profile, the battery
 * reading, suspend, the lid and the power button. It keeps its own file,
 * /etc/lp-tune.conf, and takes changes over its socket from anyone at the
 * machine - `lp-tune set PROFILE`, `lp-tune config set KEY VALUE` - so
 * none of these rows needs an administrator, and none of them writes
 * that file directly: lp-tune validates the value and knows the format.
 *
 * The screen going dark is the session's (desktop/session/lp-idle, which
 * runs swayidle; and wayfire's own idle plugin under wayfire). Neither
 * can tell battery from mains by itself, so the record is ours:
 *
 *     ~/.config/lp/power.conf
 *       blank_ac=600          seconds of no input before the screen goes
 *       blank_battery=300     dark, plugged in / on battery; 0 = never
 *
 * and this panel turns it into what the two readers understand today:
 * ~/.config/lp/idle (one number, the seconds for the power source in use
 * now - what lp-idle reads, then restarted so the new value takes), and
 * [idle] dpms_timeout in wayfire.ini (wayfire re-reads it by itself;
 * -1 is never). `lp-settings --restore` does the same at login, so the
 * value that applies is the one for the power source the session starts
 * on. Reading power.conf directly, and switching when the charger goes in
 * or out, is lp-idle's to add (the desktop-shell track's file).
 *
 * ── Only what this machine has ──
 *
 * A desktop or a virtual machine without a battery is not shown a
 * battery, "on battery" rows or a lid; they would be controls for
 * nothing. The machine is asked (lp-tune status --json) every time the
 * panel is opened.
 */
#include "core.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    GtkWidget *page;
    GtkWidget *battery;          /* group */
    GtkWidget *mode;             /* group */
    GtkWidget *screen;           /* group */
    GtkWidget *sleep;            /* group */
    jnode_t   *status;
    gboolean   has_battery;
} pwr_t;

static pwr_t *PW;

static void pwr_free(gpointer p)
{
    pwr_t *w = p;
    json_free(w->status);
    if (PW == w) PW = NULL;
    g_free(w);
}

/* ── the screen-off time ────────────────────────────────────────────── */

static const lp_opt_t BLANK[] = {
    { "60",   "1 minute",   "1분" },
    { "120",  "2 minutes",  "2분" },
    { "180",  "3 minutes",  "3분" },
    { "300",  "5 minutes",  "5분" },
    { "600",  "10 minutes", "10분" },
    { "900",  "15 minutes", "15분" },
    { "1800", "30 minutes", "30분" },
    { "0",    "Never",      "끄지 않음" },
    { NULL, NULL, NULL }
};

static char *power_conf(void) { return lp_config_path("power.conf"); }

/* The seconds for a power source, from power.conf; the defaults are
 * GNOME's (5 minutes on battery, 10 plugged in). */
static int blank_for(gboolean battery)
{
    char *c = power_conf();
    int v = kv_get_int(c, battery ? "blank_battery" : "blank_ac", battery ? 300 : 600);
    g_free(c);
    return v < 0 ? 0 : v;
}

/* Is the machine on battery right now? Asked of lp-tune; a machine that
 * cannot say is treated as plugged in. */
static gboolean on_battery_now(void)
{
    static const char *const v[] = { "lp-tune", "status", "--json", NULL };
    char *out = lp_run(v);
    jnode_t *j = json_parse(out);
    gboolean bat = j && !json_bool(j, "ac", TRUE) && json_bool(j, "battery.present", FALSE);
    json_free(j);
    g_free(out);
    return bat;
}

/* power.conf into ~/.config/lp/idle, wayfire.ini and a fresh lp-idle. */
static void apply_blank(gboolean battery, gboolean restart_idle)
{
    int secs = blank_for(battery);
    char v[16];
    g_snprintf(v, sizeof v, "%d", secs);

    char *idle = lp_config_path("idle");
    lp_write_file(idle, v);
    g_free(idle);

    char *ini = wayfire_ini();
    char w[16];
    g_snprintf(w, sizeof w, "%d", secs > 0 ? secs : -1);
    ini_set(ini, "idle", "dpms_timeout", w);
    g_free(ini);

    /* lp-idle replaces the running swayidle with one that has the new
     * time; there is no other way to change it (swayidle reads its
     * arguments once). */
    if (restart_idle && lp_have("lp-idle")) {
        static const char *const a[] = { "lp-idle", NULL };
        lp_spawn_bg(a);
    }
}

static void on_blank(GObject *dd, GParamSpec *ps, gpointer p)
{
    (void)ps;
    gboolean battery = GPOINTER_TO_INT(p);
    const char *val = row_option_value(dd);
    if (!val) return;
    char *c = power_conf();
    gboolean ok = kv_set(c, battery ? "blank_battery" : "blank_ac", val);
    g_free(c);
    if (!ok) return;
    /* Only the value for the power source in use goes live; the other
     * one waits in power.conf for the next login or the next change. */
    gboolean now_bat = PW && PW->has_battery && !json_bool(PW->status, "ac", TRUE);
    if (now_bat == battery || !PW || !PW->has_battery)
        apply_blank(battery, TRUE);
    int secs = atoi(val);
    if (secs == 0)
        lp_toast(FALSE, T("The screen will stay on", "화면을 끄지 않습니다"));
    else
        lp_toast(FALSE, T("The screen goes dark after %d min without input",
                          "입력이 없으면 %d분 뒤에 화면을 끕니다"), secs / 60);
}

/* ── lp-tune ────────────────────────────────────────────────────────── */

static void tune_done(int st, const char *out, const char *err, gpointer p)
{
    char *ok = p;
    if (st == 0) {
        lp_toast(FALSE, "%s", ok);
    } else {
        char *why = st == -1 ? g_strdup(T("lp-tune is not installed", "lp-tune 이 설치되어 있지 않습니다"))
                             : lp_first_line(err, out);
        lp_toast(TRUE, T("Power settings did not change: %s", "전원 설정을 바꾸지 못했습니다: %s"), why);
        g_free(why);
    }
    g_free(ok);
}

static void tune_config(const char *key, const char *value, const char *ok)
{
    const char *v[] = { "lp-tune", "config", "set", key, value, NULL };
    lp_run_async(v, NULL, PW ? PW->page : NULL, tune_done, g_strdup(ok));
}

static const char *const PROFILES[] = { "auto", "saver", "balanced", "performance", NULL };

static void on_profile(GtkWidget *seg, int i, gpointer p)
{
    (void)seg; (void)p;
    const char *v[] = { "lp-tune", "set", PROFILES[i], NULL };
    const char *words[] = {
        T("Power mode: automatic", "전원 모드: 자동"),
        T("Power mode: power saver", "전원 모드: 절전"),
        T("Power mode: balanced", "전원 모드: 균형"),
        T("Power mode: performance", "전원 모드: 성능"),
    };
    lp_run_async(v, NULL, PW ? PW->page : NULL, tune_done, g_strdup(words[i]));
}

static const lp_opt_t SUSPEND[] = {
    { "0",  "Never",      "하지 않음" },
    { "5",  "5 minutes",  "5분" },
    { "10", "10 minutes", "10분" },
    { "15", "15 minutes", "15분" },
    { "20", "20 minutes", "20분" },
    { "30", "30 minutes", "30분" },
    { "60", "1 hour",     "1시간" },
    { NULL, NULL, NULL }
};

static const lp_opt_t LID[] = {
    { "suspend",  "Sleep",     "절전 대기" },
    { "nothing",  "Do nothing", "아무것도 하지 않음" },
    { "poweroff", "Shut down", "시스템 종료" },
    { NULL, NULL, NULL }
};

static const lp_opt_t BUTTON[] = {
    { "suspend",  "Sleep",      "절전 대기" },
    { "poweroff", "Shut down",  "시스템 종료" },
    { "nothing",  "Do nothing", "아무것도 하지 않음" },
    { NULL, NULL, NULL }
};

/* The drop-down's data is the lp-tune key it sets. */
static void on_tune_choice(GObject *dd, GParamSpec *ps, gpointer p)
{
    (void)ps;
    const char *key = p;
    const char *val = row_option_value(dd);
    if (!val) return;
    lp_toast(FALSE, T("Saving…", "저장하는 중…"));
    tune_config(key, val, T("Saved", "저장했습니다"));
}

static void on_lock_before(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    tune_config("lock_before_suspend", on ? "yes" : "no",
                on ? T("The screen locks before the machine sleeps",
                       "잠들기 전에 화면을 잠급니다")
                   : T("The screen no longer locks before sleep",
                       "잠들기 전에 화면을 잠그지 않습니다"));
}

static void on_sleep_now(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    static const char *const v[] = { "lp-tune", "suspend", NULL };
    lp_run_async(v, NULL, NULL, tune_done, g_strdup(T("Going to sleep", "절전 대기로 들어갑니다")));
}

/* ── building ───────────────────────────────────────────────────────── */

static char *minutes_words(int m)
{
    if (m < 60) return g_strdup_printf(T("%d min", "%d분"), m);
    return g_strdup_printf(T("%d h %02d min", "%d시간 %02d분"), m / 60, m % 60);
}

static void fill_battery(pwr_t *w)
{
    const jnode_t *j = w->status;
    int pc = (int)json_num(j, "battery.percent", 0);
    const char *state = json_str(j, "battery.state", "unknown");
    int left = (int)json_num(j, "battery.minutes_left", -1);

    GtkWidget *lvl = gtk_level_bar_new_for_interval(0, 100);
    gtk_level_bar_set_value(GTK_LEVEL_BAR(lvl), pc);
    gtk_level_bar_set_mode(GTK_LEVEL_BAR(lvl), GTK_LEVEL_BAR_MODE_CONTINUOUS);
    gtk_widget_set_size_request(lvl, 200, -1);
    if (pc <= 15) gtk_widget_add_css_class(lvl, "lp-full");
    char *pct = g_strdup_printf("%d%%", pc);
    GtkWidget *r = row_widget(w->battery, T("Battery level", "배터리 잔량"), NULL, lvl);
    GtkWidget *l = gtk_label_new(pct);
    gtk_widget_add_css_class(l, "lp-value");
    gtk_widget_add_css_class(l, "lp-readout");
    gtk_box_append(GTK_BOX(row_box(r)), l);
    g_free(pct);

    const char *sw = !strcmp(state, "charging") ? T("Charging", "충전 중")
                   : !strcmp(state, "discharging") ? T("On battery", "배터리 사용 중")
                   : !strcmp(state, "full") ? T("Fully charged", "완전히 충전됨")
                   : T("Unknown", "알 수 없음");
    row_value(w->battery, T("State", "상태"), NULL, sw);
    if (left > 0) {
        char *t = minutes_words(left);
        row_value(w->battery, !strcmp(state, "charging") ? T("Until full", "완충까지")
                                                         : T("Time left", "남은 시간"), NULL, t);
        g_free(t);
    }
}

static char *subtitle(pwr_t *w)
{
    const jnode_t *j = w->status;
    if (!w->has_battery)
        return g_strdup(T("Plugged in", "전원 연결됨"));
    int pc = (int)json_num(j, "battery.percent", 0);
    int left = (int)json_num(j, "battery.minutes_left", -1);
    const char *state = json_str(j, "battery.state", "");
    if (!strcmp(state, "charging"))
        return g_strdup_printf(T("Battery %d%% · charging", "배터리 %d%% · 충전 중"), pc);
    if (left > 0) {
        char *t = minutes_words(left);
        /* Spec 2-1's own example: "배터리 38% · 1시간 24분 남음". */
        char *s = g_strdup_printf(T("Battery %d%% · %s left", "배터리 %d%% · %s 남음"), pc, t);
        g_free(t);
        return s;
    }
    return g_strdup_printf(T("Battery %d%%", "배터리 %d%%"), pc);
}

static void on_config(int st, const char *out, const char *err, gpointer p)
{
    (void)err;
    pwr_t *w = p;
    char **lines = st == 0 ? g_strsplit(out, "\n", -1) : NULL;
    GHashTable *kv = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    for (int i = 0; lines && lines[i]; i++) {
        char *eq = strchr(lines[i], '=');
        if (eq)
            g_hash_table_insert(kv, g_strndup(lines[i], eq - lines[i]), g_strdup(eq + 1));
    }
    g_strfreev(lines);

    GtkWidget *g = w->sleep;
    GtkWidget *r;
    if (w->has_battery) {
        r = row_options(g, T("Automatic sleep on battery", "배터리일 때 자동 절전"),
                        T("After this long without input", "입력이 없는 채로 이만큼 지나면"),
                        SUSPEND, g_hash_table_lookup(kv, "idle_suspend_minutes_battery"), 3,
                        G_CALLBACK(on_tune_choice), (gpointer)"idle_suspend_minutes_battery");
        if (st != 0) row_lock(r);
    }
    r = row_options(g, w->has_battery ? T("Automatic sleep when plugged in", "전원 연결 시 자동 절전")
                                      : T("Automatic sleep", "자동 절전"),
                    w->has_battery ? NULL : T("After this long without input", "입력이 없는 채로 이만큼 지나면"),
                    SUSPEND, g_hash_table_lookup(kv, "idle_suspend_minutes_ac"), 0,
                    G_CALLBACK(on_tune_choice), (gpointer)"idle_suspend_minutes_ac");
    if (st != 0) row_lock(r);
    if (w->has_battery) {
        /* A laptop: it has a lid. */
        r = row_options(g, T("Closing the lid on battery", "배터리일 때 덮개를 닫으면"), NULL, LID,
                        g_hash_table_lookup(kv, "lid_on_battery"), 0,
                        G_CALLBACK(on_tune_choice), (gpointer)"lid_on_battery");
        if (st != 0) row_lock(r);
        r = row_options(g, T("Closing the lid when plugged in", "전원 연결 시 덮개를 닫으면"),
                        T("With an external display connected, closing the lid never sleeps",
                          "외부 모니터가 연결되어 있으면 덮개를 닫아도 잠들지 않습니다"),
                        LID, g_hash_table_lookup(kv, "lid_on_ac"), 0,
                        G_CALLBACK(on_tune_choice), (gpointer)"lid_on_ac");
        if (st != 0) row_lock(r);
    }
    r = row_options(g, T("Power button", "전원 버튼"), NULL, BUTTON,
                    g_hash_table_lookup(kv, "power_button"), 0,
                    G_CALLBACK(on_tune_choice), (gpointer)"power_button");
    if (st != 0) row_lock(r);
    const char *lb = g_hash_table_lookup(kv, "lock_before_suspend");
    r = row_switch(g, T("Lock before sleep", "잠들기 전에 잠그기"),
                   T("Opening the lid shows the lock screen, never the desktop",
                     "덮개를 열면 바탕화면이 아니라 잠금 화면이 나옵니다"),
                   !lb || strcmp(lb, "no") != 0, G_CALLBACK(on_lock_before), NULL);
    if (st != 0) row_lock(r);
    if (st == 0)
        row_button(g, T("Sleep now", "지금 절전"), NULL, T("Sleep", "절전"),
                   G_CALLBACK(on_sleep_now), NULL);
    else
        row_set_detail(r, T("lp-tune is not answering, so these cannot be changed",
                            "lp-tune 이 답하지 않아 바꿀 수 없습니다"));
    g_hash_table_destroy(kv);
}

static void on_status(int st, const char *out, const char *err, gpointer p)
{
    (void)err;
    pwr_t *w = p;
    json_free(w->status);
    w->status = st == 0 ? json_parse(out) : NULL;
    w->has_battery = json_bool(w->status, "battery.present", FALSE);

    /* The mode. */
    if (w->status) {
        const char *prof = json_str(w->status, "profile", "auto");
        int active = 0;
        for (int i = 0; PROFILES[i]; i++)
            if (!strcmp(PROFILES[i], prof)) active = i;
        const char *items[] = { T("Automatic", "자동"), T("Saver", "절전"),
                                T("Balanced", "균형"), T("Performance", "성능"), NULL };
        const char *eff = json_str(w->status, "effective", "");
        const char *effw = !strcmp(eff, "saver") ? T("power saver", "절전")
                         : !strcmp(eff, "performance") ? T("performance", "성능")
                         : T("balanced", "균형");
        char *detail = g_strdup_printf(T("Automatic saves power on battery and runs balanced when "
                                         "plugged in. In force now: %s.",
                                         "자동은 배터리일 때 절전, 전원을 꽂으면 균형으로 움직입니다. "
                                         "지금 적용 중: %s."), effw);
        row_segmented(w->mode, T("Power mode", "전원 모드"), detail, items, active,
                      G_CALLBACK(on_profile), NULL);
        g_free(detail);
    } else {
        char *why = st == -1 ? g_strdup(T("lp-tune is not installed", "lp-tune 이 설치되어 있지 않습니다"))
                             : lp_first_line(err, out);
        row_value(w->mode, T("Power mode", "전원 모드"), why, T("Unavailable", "쓸 수 없음"));
        g_free(why);
    }

    if (w->has_battery) {
        fill_battery(w);
        gtk_widget_set_visible(w->battery, TRUE);
        GtkWidget *h = g_object_get_data(G_OBJECT(w->battery), "lp-head");
        if (h) gtk_widget_set_visible(h, TRUE);
    }

    /* The screen. */
    char cur[16];
    if (w->has_battery) {
        g_snprintf(cur, sizeof cur, "%d", blank_for(TRUE));
        row_options(w->screen, T("Screen off on battery", "배터리일 때 화면 끄기"),
                    T("After this long without input", "입력이 없는 채로 이만큼 지나면"),
                    BLANK, cur, 3, G_CALLBACK(on_blank), GINT_TO_POINTER(1));
    }
    g_snprintf(cur, sizeof cur, "%d", blank_for(FALSE));
    row_options(w->screen, w->has_battery ? T("Screen off when plugged in", "전원 연결 시 화면 끄기")
                                          : T("Screen off", "화면 끄기"),
                w->has_battery ? NULL : T("After this long without input", "입력이 없는 채로 이만큼 지나면"),
                BLANK, cur, 4, G_CALLBACK(on_blank), GINT_TO_POINTER(0));

    char *sub = subtitle(w);
    page_set_subtitle(w->page, sub);
    g_free(sub);

    static const char *const v[] = { "lp-tune", "config", "get", NULL };
    lp_run_async(v, NULL, w->page, on_config, w);
}

static GtkWidget *build(void)
{
    pwr_t *w = g_new0(pwr_t, 1);
    PW = w;
    w->page = page_new(T("Power", "전원"), T("Reading the battery…", "배터리를 읽는 중…"));
    g_object_set_data_full(G_OBJECT(w->page), "lp-power", w, pwr_free);
    w->mode = group_new(w->page, NULL);

    /* Hidden until the machine says it has a battery. */
    GtkWidget *head = gtk_label_new(T("Battery", "배터리"));
    gtk_widget_add_css_class(head, "lp-heading");
    gtk_widget_set_halign(head, GTK_ALIGN_START);
    gtk_widget_set_margin_top(head, 22);
    gtk_widget_set_margin_bottom(head, 8);
    gtk_widget_set_margin_start(head, 4);
    gtk_widget_set_visible(head, FALSE);
    gtk_box_append(GTK_BOX(w->page), head);
    w->battery = group_new(w->page, NULL);
    gtk_widget_set_margin_top(w->battery, 0);
    gtk_widget_set_visible(w->battery, FALSE);
    g_object_set_data(G_OBJECT(w->battery), "lp-head", head);

    w->screen = group_new(w->page, T("Screen", "화면"));
    w->sleep = group_new(w->page, T("Sleep and buttons", "절전과 버튼"));

    static const char *const v[] = { "lp-tune", "status", "--json", NULL };
    lp_run_async(v, NULL, w->page, on_status, w);
    return w->page;
}

/* At login: the screen-off time for the power source the session starts
 * on. lp-idle is started by the session itself after this. */
static void restore(void)
{
    apply_blank(on_battery_now(), FALSE);
}

static const char *const KEYS[] = {
    "Power mode", "전원 모드",
    "Battery level", "배터리 잔량",
    "Screen off", "화면 끄기",
    "Screen off on battery", "배터리일 때 화면 끄기",
    "Automatic sleep", "자동 절전",
    "Closing the lid on battery", "배터리일 때 덮개를 닫으면",
    "Power button", "전원 버튼",
    "Lock before sleep", "잠들기 전에 잠그기",
    "Suspend", "절전 대기",
    NULL
};

const lp_panel_t lp_panel_power = {
    "power", "Power", "전원", "battery-good-symbolic", build, KEYS, restore
};
