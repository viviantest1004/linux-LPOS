/*
 * datetime.c - Date & time: automatic time, the time zone, setting the
 * clock by hand, and the 24-hour clock.
 *
 * The clock and the time zone are the machine's, not the person's, so
 * changing them goes through timedatectl as administrator (the password
 * dialog, sys.c): `timedatectl set-ntp true|false`, `set-timezone
 * Area/City`, `set-time "YYYY-MM-DD HH:MM:SS"`. What is in force is read
 * back from `timedatectl show` (Timezone=, NTP=, NTPSynchronized=), the
 * systemd shape the CLI track's timedatectl answers in; if that is not
 * there, /etc/localtime says the zone.
 *
 * The 12/24-hour choice is the person's: ~/.config/lp/clock ("24" or
 * "12", the file the top bar's clock reads) and GNOME's clock-format key
 * for apps that follow it.
 *
 * Setting the clock by hand is offered only while automatic time is off,
 * because the time service would put it back a minute later and the
 * change would look like it had not worked.
 */
#include "core.h"

#include <string.h>
#include <time.h>

typedef struct {
    GtkWidget *page;
    GtkWidget *now_row;
    GtkWidget *auto_row, *tz_row, *set_row;
    char      *tz;
    gboolean   ntp;
    guint      tick;
} dt_t;

static dt_t *DT;

static void dt_free(gpointer p)
{
    dt_t *d = p;
    if (d->tick) g_source_remove(d->tick);
    g_free(d->tz);
    if (DT == d) DT = NULL;
    g_free(d);
}

static gboolean clock12(void)
{
    char *p = lp_config_path("clock");
    char *v = lp_slurp(p);
    g_free(p);
    gboolean r = v && !strcmp(g_strstrip(v), "12");
    g_free(v);
    return r;
}

static char *now_words(void)
{
    GDateTime *now = g_date_time_new_now_local();
    char *s = g_date_time_format(now, clock12() ? T("%A %e %B %Y, %l:%M:%S %p", "%Y년 %-m월 %-d일 (%a) %p %l:%M:%S")
                                                : T("%A %e %B %Y, %H:%M:%S", "%Y년 %-m월 %-d일 (%a) %H:%M:%S"));
    g_date_time_unref(now);
    return s;
}

static gboolean tick(gpointer p)
{
    dt_t *d = p;
    char *s = now_words();
    row_set_value(d->now_row, s);
    g_free(s);
    return G_SOURCE_CONTINUE;
}

static void reload(dt_t *d);

static void admin_done(int st, const char *out, const char *err, gpointer p)
{
    char *ok = p;
    if (st == 0) {
        lp_toast(FALSE, "%s", ok);
        /* The time zone changed under this process: tzset reads it again. */
        tzset();
    } else if (st != -2) {
        char *why = st == -1 ? g_strdup(T("timedatectl is not installed", "timedatectl 이 설치되어 있지 않습니다"))
                             : lp_first_line(err, out);
        lp_toast(TRUE, T("The clock did not change: %s", "시계를 바꾸지 못했습니다: %s"), why);
        g_free(why);
    }
    g_free(ok);
    if (DT) reload(DT);
}

static void on_auto(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    const char *v[] = { "timedatectl", "set-ntp", on ? "true" : "false", NULL };
    lp_admin_run(v, NULL, T("Changing how this machine keeps its clock needs an administrator.",
                            "이 기계가 시계를 맞추는 방법을 바꾸려면 관리자 권한이 필요합니다."),
                 GTK_WIDGET(sw), admin_done,
                 g_strdup(on ? T("The clock sets itself from the internet", "인터넷으로 시계를 자동으로 맞춥니다")
                             : T("Automatic time is off", "자동 시간 맞추기를 껐습니다")));
}

/* ── the time zone picker ───────────────────────────────────────────── */

/* Korean names for the zones people here look for, so "서울" finds
 * Asia/Seoul. */
static const char *const TZ_KO[] = {
    "Asia/Seoul", "서울 한국", "Asia/Tokyo", "도쿄 일본", "Asia/Shanghai", "상하이 베이징 중국",
    "Asia/Hong_Kong", "홍콩", "Asia/Taipei", "타이베이 대만", "Asia/Singapore", "싱가포르",
    "Asia/Bangkok", "방콕 태국", "Asia/Ho_Chi_Minh", "호찌민 베트남", "Asia/Kolkata", "인도",
    "Europe/London", "런던 영국", "Europe/Paris", "파리 프랑스", "Europe/Berlin", "베를린 독일",
    "America/New_York", "뉴욕 미국 동부", "America/Chicago", "시카고 미국 중부",
    "America/Denver", "덴버 미국 산악", "America/Los_Angeles", "로스앤젤레스 미국 서부",
    "America/Vancouver", "밴쿠버 캐나다", "Australia/Sydney", "시드니 호주", "Pacific/Auckland", "오클랜드 뉴질랜드",
    "UTC", "협정 세계시", NULL
};

static const char *tz_ko(const char *z)
{
    for (int i = 0; TZ_KO[i]; i += 2)
        if (!strcmp(TZ_KO[i], z)) return TZ_KO[i + 1];
    return NULL;
}

static gboolean tz_filter(GtkListBoxRow *r, gpointer p)
{
    GtkEditable *e = p;
    const char *q = gtk_editable_get_text(e);
    if (!q || !*q) return TRUE;
    const char *z = g_object_get_data(G_OBJECT(r), "lp-tz");
    const char *ko = tz_ko(z);
    char *a = g_utf8_casefold(z, -1), *b = g_utf8_casefold(q, -1);
    /* "new york" finds America/New_York. */
    for (char *c = b; *c; c++) if (*c == ' ') *c = '_';
    gboolean hit = strstr(a, b) != NULL || (ko && strstr(ko, q));
    g_free(a); g_free(b);
    return hit;
}

static void tz_search_changed(GtkEditable *e, gpointer list)
{
    (void)e;
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(list));
}

static void tz_chosen(lp_dialog_t *dl, gpointer p)
{
    (void)p;
    GtkWidget *list = lp_dialog_get_data(dl, "lp-list");
    GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
    if (!r) {
        lp_dialog_error(dl, T("Choose a time zone in the list.", "목록에서 시간대를 고르십시오."));
        return;
    }
    const char *z = g_object_get_data(G_OBJECT(r), "lp-tz");
    const char *v[] = { "timedatectl", "set-timezone", z, NULL };
    char *ok = g_strdup_printf(T("Time zone: %s", "시간대: %s"), z);
    lp_admin_run(v, NULL, T("Changing the time zone needs an administrator.",
                            "시간대를 바꾸려면 관리자 권한이 필요합니다."),
                 NULL, admin_done, ok);
    lp_dialog_close(dl);
}

static void tz_activated(GtkListBox *b, GtkListBoxRow *r, gpointer p)
{
    (void)b; (void)r;
    tz_chosen(p, NULL);
}

static void on_tz(GtkWidget *row, gpointer p)
{
    (void)row; (void)p;
    static const char *const v[] = { "timedatectl", "list-timezones", NULL };
    char *out = lp_run(v);
    if (!out) {
        /* Without timedatectl the zones are the files under zoneinfo. */
        static const char *const f[] = { "sh", "-c", "cd /usr/share/zoneinfo && find . -type f "
                                         "| sed 's|^./||' | grep -E '^[A-Z][a-z]+/' | sort", NULL };
        out = lp_run(f);
    }
    if (!out || !*out) {
        lp_toast(TRUE, T("Could not list the time zones", "시간대 목록을 읽지 못했습니다"));
        g_free(out);
        return;
    }
    lp_dialog_t *dl = lp_dialog_new(T("Time zone", "시간대"), T("Use", "사용"), FALSE, tz_chosen, NULL);
    GtkWidget *search = lp_dialog_entry(dl, T("Search: a city or a region", "검색: 도시나 지역"), NULL, FALSE);
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(list, "lp-group");
    char **zones = g_strsplit(out, "\n", -1);
    GtkListBoxRow *sel = NULL;
    for (int i = 0; zones[i]; i++) {
        char *z = g_strstrip(zones[i]);
        if (!*z) continue;
        GtkWidget *r = gtk_list_box_row_new();
        const char *ko = tz_ko(z);
        char *label = ko && lp_korean() ? g_strdup_printf("%s  ·  %s", z, ko) : g_strdup(z);
        GtkWidget *l = gtk_label_new(label);
        g_free(label);
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_widget_set_margin_start(l, 12);
        gtk_widget_set_size_request(r, -1, 44);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(r), l);
        g_object_set_data_full(G_OBJECT(r), "lp-tz", g_strdup(z), g_free);
        g_object_set_data_full(G_OBJECT(r), "lp-title", g_strdup(z), g_free);
        gtk_list_box_append(GTK_LIST_BOX(list), r);
        if (DT && DT->tz && !strcmp(DT->tz, z)) sel = GTK_LIST_BOX_ROW(r);
    }
    g_strfreev(zones);
    g_free(out);
    if (sel) gtk_list_box_select_row(GTK_LIST_BOX(list), sel);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(list), tz_filter, search, NULL);
    g_signal_connect(search, "changed", G_CALLBACK(tz_search_changed), list);
    g_signal_connect(list, "row-activated", G_CALLBACK(tz_activated), dl);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sw, 480, lp_dialog_fit(420, 320));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), list);
    gtk_box_append(GTK_BOX(lp_dialog_body(dl)), sw);
    lp_dialog_set_data(dl, "lp-list", list, NULL);
    lp_dialog_present(dl);
}

/* ── setting the clock by hand ──────────────────────────────────────── */

static void set_time_ok(lp_dialog_t *dl, gpointer p)
{
    (void)p;
    GtkCalendar *cal = lp_dialog_get_data(dl, "lp-cal");
    GtkSpinButton *h = lp_dialog_get_data(dl, "lp-h"), *m = lp_dialog_get_data(dl, "lp-m");
    GDateTime *d = gtk_calendar_get_date(cal);
    char *when = g_strdup_printf("%04d-%02d-%02d %02d:%02d:00", g_date_time_get_year(d),
                                 g_date_time_get_month(d), g_date_time_get_day_of_month(d),
                                 gtk_spin_button_get_value_as_int(h), gtk_spin_button_get_value_as_int(m));
    g_date_time_unref(d);
    const char *v[] = { "timedatectl", "set-time", when, NULL };
    lp_admin_run(v, NULL, T("Setting the clock needs an administrator.",
                            "시계를 맞추려면 관리자 권한이 필요합니다."),
                 NULL, admin_done, g_strdup_printf(T("The clock is set to %s", "시계를 %s 로 맞췄습니다"), when));
    g_free(when);
    lp_dialog_close(dl);
}

static GtkWidget *spin(int max, int value, const char *title)
{
    GtkWidget *s = gtk_spin_button_new_with_range(0, max, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), value);
    gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(s), TRUE);
    gtk_orientable_set_orientation(GTK_ORIENTABLE(s), GTK_ORIENTATION_VERTICAL);
    g_object_set_data_full(G_OBJECT(s), "lp-title", g_strdup(title), g_free);
    return s;
}

static void on_set_time(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    lp_dialog_t *dl = lp_dialog_new(T("Set the date and time", "날짜와 시간 맞추기"), T("Set", "맞추기"),
                                    FALSE, set_time_ok, NULL);
    GtkWidget *cal = gtk_calendar_new();
    gtk_box_append(GTK_BOX(lp_dialog_body(dl)), cal);
    GDateTime *now = g_date_time_new_now_local();
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    GtkWidget *h = spin(23, g_date_time_get_hour(now), T("Hour", "시"));
    GtkWidget *m = spin(59, g_date_time_get_minute(now), T("Minute", "분"));
    GtkWidget *colon = gtk_label_new(":");
    gtk_widget_add_css_class(colon, "lp-big");
    gtk_box_append(GTK_BOX(row), h);
    gtk_box_append(GTK_BOX(row), colon);
    gtk_box_append(GTK_BOX(row), m);
    gtk_box_append(GTK_BOX(lp_dialog_body(dl)), row);
    g_date_time_unref(now);
    lp_dialog_set_data(dl, "lp-cal", cal, NULL);
    lp_dialog_set_data(dl, "lp-h", h, NULL);
    lp_dialog_set_data(dl, "lp-m", m, NULL);
    lp_dialog_present(dl);
}

/* ── 24-hour clock ──────────────────────────────────────────────────── */

static void on_24h(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    gboolean h24 = gtk_switch_get_active(GTK_SWITCH(sw));
    char *path = lp_config_path("clock");
    gboolean ok = lp_write_file(path, h24 ? "24\n" : "12\n");
    g_free(path);
    if (!ok) return;
    lp_gsettings_set("org.gnome.desktop.interface", "clock-format", h24 ? "24h" : "12h");
    if (DT) tick(DT);
    lp_toast(FALSE, h24 ? T("24-hour clock", "24시간제") : T("12-hour clock", "12시간제"));
}

/* ── reading ────────────────────────────────────────────────────────── */

static void on_show(int st, const char *out, const char *err, gpointer p)
{
    (void)err;
    dt_t *d = p;
    g_free(d->tz);
    d->tz = NULL;
    d->ntp = FALSE;
    gboolean synced = FALSE, known = st == 0;
    if (known) {
        char **l = g_strsplit(out, "\n", -1);
        for (int i = 0; l[i]; i++) {
            if (g_str_has_prefix(l[i], "Timezone=")) d->tz = g_strdup(l[i] + 9);
            else if (g_str_has_prefix(l[i], "NTP=")) d->ntp = !strcmp(l[i] + 4, "yes");
            else if (g_str_has_prefix(l[i], "NTPSynchronized=")) synced = !strcmp(l[i] + 16, "yes");
        }
        g_strfreev(l);
    }
    if (!d->tz) {
        char *link = g_file_read_link("/etc/localtime", NULL);
        const char *z = link ? strstr(link, "zoneinfo/") : NULL;
        d->tz = g_strdup(z ? z + 9 : "UTC");
        g_free(link);
    }
    LP_QUIET(gtk_switch_set_active(GTK_SWITCH(row_control(d->auto_row)), d->ntp));
    if (lp_is_admin() || !known) {
        const char *why = !known ? T("timedatectl is not answering", "timedatectl 이 답하지 않습니다")
                        : !d->ntp ? NULL
                        : synced ? T("Set from the internet; in step now",
                                     "인터넷으로 맞춥니다. 지금 맞춰져 있습니다")
                                 : T("Set from the internet once it is reachable",
                                     "인터넷에 연결되면 맞춥니다");
        row_set_detail(d->auto_row, why);
    }
    row_set_value(d->tz_row, d->tz);
    if (lp_is_admin()) {
        /* A standard account's rows stay locked, and say who can. */
        gtk_widget_set_sensitive(row_control(d->set_row), !d->ntp);
        row_set_detail(d->set_row, d->ntp ? T("Turn automatic time off first", "먼저 자동 시간 맞추기를 끄십시오") : NULL);
    }
    char *sub = g_strdup_printf("%s · %s", d->tz, d->ntp ? T("automatic", "자동") : T("manual", "수동"));
    page_set_subtitle(d->page, sub);
    g_free(sub);
}

static void reload(dt_t *d)
{
    static const char *const v[] = { "timedatectl", "show", NULL };
    lp_run_async(v, NULL, d->page, on_show, d);
}

static GtkWidget *build(void)
{
    dt_t *d = g_new0(dt_t, 1);
    DT = d;
    d->page = page_new(T("Date & time", "날짜와 시간"), NULL);
    g_object_set_data_full(G_OBJECT(d->page), "lp-dt", d, dt_free);
    GtkWidget *g = group_new(d->page, NULL);
    char *now = now_words();
    d->now_row = row_value(g, T("Now", "지금"), NULL, now);
    gtk_widget_add_css_class(row_control(d->now_row), "lp-readout");
    g_free(now);
    d->auto_row = row_switch(g, T("Automatic date and time", "자동 날짜와 시간"), NULL, FALSE,
                             G_CALLBACK(on_auto), NULL);
    d->tz_row = row_chevron(g, T("Time zone", "시간대"), NULL, "…", G_CALLBACK(on_tz), NULL);
    d->set_row = row_button(g, T("Date and time", "날짜와 시간"), NULL, T("Set…", "맞추기…"),
                            G_CALLBACK(on_set_time), NULL);
    GtkWidget *f = group_new(d->page, NULL);
    row_switch(f, T("24-hour clock", "24시간제"), T("The top bar's clock follows this", "상단바의 시계도 따릅니다"),
               !clock12(), G_CALLBACK(on_24h), NULL);
    if (!lp_is_admin()) {
        row_lock(d->auto_row);
        row_lock(d->tz_row);
        row_lock(d->set_row);
    }
    d->tick = g_timeout_add_seconds(1, tick, d);
    reload(d);
    return d->page;
}

static void restore(void)
{
    lp_gsettings_set("org.gnome.desktop.interface", "clock-format", clock12() ? "12h" : "24h");
}

static const char *const KEYS[] = {
    "Automatic date and time", "자동 날짜와 시간",
    "Time zone", "시간대",
    "Date and time", "날짜와 시간",
    "24-hour clock", "24시간제",
    "Clock", "시계",
    "NTP", "NTP",
    NULL
};

const lp_panel_t lp_panel_datetime = {
    "datetime", "Date & time", "날짜와 시간", "preferences-system-time-symbolic", build, KEYS, restore
};
