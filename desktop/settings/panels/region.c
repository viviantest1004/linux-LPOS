/*
 * region.c - Region & language: the language of the screens, the formats
 * of dates and numbers, the physical keyboard's layout, and the way to
 * the input sources.
 *
 * ── Per person, at the next login ──
 *
 * The language is the session's, not this window's: every program reads
 * LANG once, when it starts, so a change can only take effect for
 * programs started afterwards - which is honestly "the next time you
 * sign in", and the row says so rather than half-translating the desktop
 * now. The choice is one line in a file the session reads before it
 * starts anything (desktop/session/session-run):
 *
 *     ~/.config/lp/locale        en_US.UTF-8 | ko_KR.UTF-8   -> LANG, LC_MESSAGES
 *     ~/.config/lp/formats       en_US.UTF-8 | ko_KR.UTF-8   -> LC_TIME, LC_NUMERIC,
 *                                                                LC_MONETARY, LC_PAPER,
 *                                                                LC_MEASUREMENT
 *
 * Home, not /etc/default/locale, because a standard account chooses its
 * own language without an administrator (the spec's permission table),
 * and two people on one machine may read different ones. What the login
 * screen, the console and new accounts use is the system's
 * (`localectl set-locale`), which needs an administrator and is a
 * separate, explicit button. localectl only asks systemd-localed, which
 * LP's init does not run - it answered every time with "Failed to connect
 * to bus" - so when it cannot reach it the same two lines are written to
 * /etc/default/locale directly, the file localed itself keeps and the
 * installer writes.
 *
 * English is the default: no file means en_US.UTF-8.
 *
 * ── The physical keys ──
 *
 * Which character each key makes is the compositor's xkb layout, in
 * wayfire.ini [input] xkb_layout / xkb_options, applied at once. Korean
 * laptops without a dedicated 한/영 key use Right Alt for it; xkb's
 * korean:ralt_hangul option makes that key send Hangul, which the input
 * method then treats as the switch key.
 */
#include "core.h"

#include <langinfo.h>
#include <locale.h>
#include <string.h>
#include <time.h>

static const char *LOCALES[] = { "en_US.UTF-8", "ko_KR.UTF-8", NULL };

static char *read_choice(const char *name)
{
    char *p = lp_config_path(name);
    char *v = lp_slurp(p);
    g_free(p);
    if (v) g_strstrip(v);
    if (!v || !*v) { g_free(v); return NULL; }
    return v;
}

static int locale_index(const char *v)
{
    if (v && g_str_has_prefix(v, "ko")) return 1;
    return 0;
}

static void on_language(GtkWidget *seg, int i, gpointer p)
{
    (void)seg; (void)p;
    char *path = lp_config_path("locale");
    char *line = g_strdup_printf("%s\n", LOCALES[i]);
    gboolean ok = lp_write_file(path, line);
    g_free(line); g_free(path);
    if (ok)
        lp_toast(FALSE, i == 1 ? "다음에 로그인하면 한국어로 바뀝니다 (The language changes to Korean at your next sign-in)"
                               : "English from your next sign-in (다음 로그인부터 영어)");
}

/* Today's date and a number, the way a locale writes them, without
 * switching this program's own locale. */
static char *sample(const char *loc)
{
    locale_t l = newlocale(LC_ALL_MASK, loc, (locale_t)0);
    if (!l) return g_strdup(T("This format is not installed", "이 형식이 설치되어 있지 않습니다"));
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char date[128] = "";
    strftime_l(date, sizeof date, "%x (%a)", &tm, l);
    const char *dot = nl_langinfo_l(RADIXCHAR, l);
    const char *sep = nl_langinfo_l(THOUSEP, l);
    char *r = g_strdup_printf("%s · 1%s234%s56", date, sep && *sep ? sep : ",", dot && *dot ? dot : ".");
    freelocale(l);
    return r;
}

static void on_formats(GtkWidget *seg, int i, gpointer p)
{
    GtkWidget *row = p;
    char *path = lp_config_path("formats");
    char *line = g_strdup_printf("%s\n", LOCALES[i]);
    gboolean ok = lp_write_file(path, line);
    g_free(line); g_free(path);
    (void)seg;
    if (!ok) return;
    char *s = sample(LOCALES[i]);
    row_set_detail(row, s);
    g_free(s);
    lp_toast(FALSE, T("Dates and numbers change at your next sign-in",
                      "날짜와 숫자 형식은 다음 로그인부터 바뀝니다"));
}

static const char *WHY_SYSTEM_LOCALE;

static void system_done(int st, const char *out, const char *err, gpointer p)
{
    char *body = p;
    /* No localed to talk to (no systemd): the file it would have written. */
    if (st != 0 && body && err &&
        (strstr(err, "Failed to connect to bus") || strstr(err, "not been booted with systemd"))) {
        lp_admin_write_file("/etc/default/locale", body, WHY_SYSTEM_LOCALE, NULL,
                            system_done, NULL);
        g_free(body);
        return;
    }
    g_free(body);
    if (st == 0)
        lp_toast(FALSE, T("The login screen and new accounts use this language now",
                          "이제 로그인 화면과 새 계정이 이 언어를 씁니다"));
    else if (st != -2) {
        char *why = lp_first_line(err, out);
        lp_toast(TRUE, T("The system language did not change: %s", "시스템 언어를 바꾸지 못했습니다: %s"), why);
        g_free(why);
    }
}

static void on_system_wide(GtkButton *b, gpointer p)
{
    (void)p;
    char *lang = read_choice("locale");
    char *fmt = read_choice("formats");
    char *a1 = g_strdup_printf("LANG=%s", lang ? lang : "en_US.UTF-8");
    char *a2 = g_strdup_printf("LC_TIME=%s", fmt ? fmt : (lang ? lang : "en_US.UTF-8"));
    const char *v[] = { "localectl", "set-locale", a1, a2, NULL };
    WHY_SYSTEM_LOCALE = T("Changing the language of the login screen and of new accounts needs "
                          "an administrator.",
                          "로그인 화면과 새 계정의 언어를 바꾸려면 관리자 권한이 필요합니다.");
    lp_admin_run(v, NULL, WHY_SYSTEM_LOCALE, GTK_WIDGET(b), system_done,
                 g_strdup_printf("%s\n%s\n", a1, a2));
    g_free(a1); g_free(a2); g_free(lang); g_free(fmt);
}

static void on_logout(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    static const char *const v[] = { "lp-logout", NULL };
    lp_spawn_bg(v);
}

/* ── the physical layout ────────────────────────────────────────────── */

static const lp_opt_t LAYOUTS[] = {
    { "us", "English (US)", "영어 (US)" },
    { "kr", "Korean", "한국어" },
    { "gb", "English (UK)", "영어 (UK)" },
    { "de", "German", "독일어" },
    { "fr", "French", "프랑스어" },
    { "jp", "Japanese", "일본어" },
    { NULL, NULL, NULL }
};

static char *wf_input(const char *key)
{
    char *ini = wayfire_ini();
    char *v = ini_get(ini, "input", key);
    g_free(ini);
    return v;
}

static void on_layout(GObject *dd, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    const char *v = row_option_value(dd);
    if (!v) return;
    char *ini = wayfire_ini();
    gboolean ok = ini_set(ini, "input", "xkb_layout", v);
    g_free(ini);
    const char *a[] = { "input", "type:keyboard", "xkb_layout", v, NULL };
    lp_swaymsg(a);
    if (ok) lp_toast(FALSE, T("Keyboard layout: %s", "키보드 배열: %s"), v);
}

static void on_ralt(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    char *opts = wf_input("xkb_options");
    GString *out = g_string_new(NULL);
    char **parts = g_strsplit(opts ? opts : "", ",", -1);
    for (int i = 0; parts[i]; i++) {
        char *o = g_strstrip(parts[i]);
        if (!*o || !strcmp(o, "korean:ralt_hangul")) continue;
        if (out->len) g_string_append_c(out, ',');
        g_string_append(out, o);
    }
    if (on) {
        if (out->len) g_string_append_c(out, ',');
        g_string_append(out, "korean:ralt_hangul");
    }
    g_strfreev(parts);
    g_free(opts);
    char *ini = wayfire_ini();
    gboolean ok = out->len ? ini_set(ini, "input", "xkb_options", out->str)
                           : ini_unset(ini, "input", "xkb_options");
    g_free(ini);
    const char *a[] = { "input", "type:keyboard", "xkb_options", out->str, NULL };
    lp_swaymsg(a);
    g_string_free(out, TRUE);
    if (ok) lp_toast(FALSE, on ? T("Right Alt now switches between English and Korean",
                                   "이제 오른쪽 Alt 가 한/영 키입니다")
                               : T("Right Alt is Alt again", "오른쪽 Alt 가 다시 Alt 입니다"));
}

static void on_sources(GtkWidget *row, gpointer p)
{
    (void)row; (void)p;
    lp_show_panel("keyboard", T("Input sources", "입력 소스"));
}

static GtkWidget *build(void)
{
    char *lang = read_choice("locale");
    char *fmt = read_choice("formats");
    int li = locale_index(lang), fi = fmt ? locale_index(fmt) : li;
    /* The subtitle is what is in force now, which after a change is not
     * yet what was chosen. */
    GtkWidget *page = page_new(T("Region & language", "지역과 언어"),
                               lp_korean() ? "한국어 · ko_KR.UTF-8" : "English · en_US.UTF-8");

    GtkWidget *g = group_new(page, T("Language", "언어"));
    /* The two names are each in their own language, so someone who
     * cannot read the current one can still find theirs. */
    const char *langs[] = { "English", "한국어", NULL };
    row_segmented(g, T("Language", "언어"),
                  T("Menus, windows and messages. Applies the next time you sign in.",
                    "메뉴, 창, 안내문의 언어입니다. 다음에 로그인할 때 적용됩니다."),
                  langs, li, G_CALLBACK(on_language), NULL);
    const char *fmts[] = { T("United States", "미국"), T("South Korea", "대한민국"), NULL };
    char *s = sample(LOCALES[fi]);
    GtkWidget *fr = row_shell(T("Formats", "형식"), s);
    g_free(s);
    row_add(g, fr);
    GtkWidget *tmp = row_segmented(NULL, "", NULL, fmts, fi, G_CALLBACK(on_formats), fr);
    /* The segmented control moves into the formats row, whose detail
     * line is the live sample the handler updates. */
    GtkWidget *seg = row_control(tmp);
    g_object_ref(seg);
    gtk_box_remove(GTK_BOX(row_box(tmp)), seg);
    gtk_widget_set_valign(seg, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row_box(fr)), seg);
    g_object_set_data(G_OBJECT(fr), "lp-control", seg);
    g_object_set_data(G_OBJECT(seg), "lp-row", fr);
    g_object_unref(seg);
    g_object_ref_sink(tmp);
    g_object_unref(tmp);

    if (lp_have("lp-logout"))
        row_button(g, T("Sign out to apply", "적용하려면 로그아웃"), NULL, T("Log out", "로그아웃"),
                   G_CALLBACK(on_logout), NULL);
    GtkWidget *sw = row_button(g, T("Login screen and new accounts", "로그인 화면과 새 계정"),
                               T("Use this language and these formats for the whole system too",
                                 "이 언어와 형식을 시스템 전체에도 씁니다"),
                               T("Apply…", "적용…"), G_CALLBACK(on_system_wide), NULL);
    if (!lp_is_admin()) row_lock(sw);

    GtkWidget *k = group_new(page, T("Keyboard", "키보드"));
    char *layout = wf_input("xkb_layout");
    char *opts = wf_input("xkb_options");
    row_options(k, T("Keyboard layout", "키보드 배열"),
                T("What the keys themselves type", "키를 눌렀을 때 나오는 글자"),
                LAYOUTS, layout, 0, G_CALLBACK(on_layout), NULL);
    row_switch(k, T("Right Alt is the 한/영 key", "오른쪽 Alt 를 한/영 키로"),
               T("For keyboards without a 한/영 key of their own", "한/영 키가 따로 없는 키보드용"),
               opts && strstr(opts, "korean:ralt_hangul"), G_CALLBACK(on_ralt), NULL);
    row_chevron(k, T("Input sources", "입력 소스"), T("English, Korean, and switching between them",
                                                     "영어, 한국어, 그리고 둘 사이 전환"),
                NULL, G_CALLBACK(on_sources), NULL);
    g_free(layout); g_free(opts); g_free(lang); g_free(fmt);
    return page;
}

static const char *const KEYS[] = {
    "Language", "언어",
    "Formats", "형식",
    "Keyboard layout", "키보드 배열",
    "Right Alt is the 한/영 key", "오른쪽 Alt 를 한/영 키로",
    "Input sources", "입력 소스",
    "Korean", "한국어",
    "English", "영어",
    NULL
};

const lp_panel_t lp_panel_region = {
    "region", "Region & language", "지역과 언어", "preferences-desktop-locale-symbolic",
    build, KEYS, NULL
};
