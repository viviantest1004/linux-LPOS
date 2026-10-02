/*
 * reset.c - Reset: six areas put back one at a time, all of them at
 * once, and reinstalling LP.
 *
 * reset-and-factory-reset.md is the design and this file follows it:
 *
 *   - Seven rows: shortcuts, keyboard, display, sound, network, web, and
 *     "All settings" in a section of its own, so an eye running down six
 *     rows does not stop on the seventh by accident.
 *   - Each row says what comes back and what stays, and the confirmation
 *     says the same two sentences - they come from one table (AREAS), so
 *     the row and the dialog cannot say different things.
 *   - Every reset is confirmed; none asks for a password, because each can
 *     be undone by doing the same thing again, and a password asked seven
 *     times makes the eighth - the one that erases - cheap (3-2).
 *   - Network and All settings carry the amber warning: the Wi-Fi
 *     passwords go, and with them this machine's way back onto the
 *     network.
 *   - "All settings" is not a factory reset, and says so twice.
 *
 * Reset is an administrator's (the spec's default policy): a standard
 * account sees every row locked, saying who can.
 *
 * ── Reinstalling ──
 *
 * The factory reset of this machine is "Reinstall LP" in the recovery
 * system (the disk layout contract: LP-RECOVERY holds the whole OS
 * payload, and its menu offers "Keep my files" or "Erase everything"
 * with a typed confirmation of its own). This panel's part is getting
 * there, in the spec's order (3-4): first the facts, with nothing to type;
 * then the administrator password; then the typed word - "reset", or in
 * Korean "초기화" - compared exactly as typed, not trimmed, because
 * trimming would accept something the person did not write. Then
 * `lp-reboot-recovery`. Nothing is erased by this window.
 */
#include "core.h"

#include <string.h>

void     lp_keyboard_reset_shortcuts(void);
void     lp_keyboard_reset_input(void);
gboolean lp_display_reset(char **why);
gboolean lp_sound_reset(char **why);

typedef gboolean (*reset_fn)(char **why);

static gboolean reset_shortcuts(char **why) { (void)why; lp_keyboard_reset_shortcuts(); return TRUE; }
static gboolean reset_keyboard(char **why) { (void)why; lp_keyboard_reset_input(); return TRUE; }

/* Every saved Wi-Fi network lp-net reports. The contract has no "list
 * saved networks" verb, so these are the saved ones in range now; one
 * saved somewhere else stays until it is seen (a follow-up for the
 * networking track: `lp-net forget --all`). */
static gboolean reset_network(char **why)
{
    static const char *const v[] = { "lp-net", "scan", "--json", NULL };
    char *out = NULL, *err = NULL;
    /* A scan takes seconds by nature. */
    int st = lp_run_full_timeout(v, NULL, &out, &err, 30 * 1000);
    if (st != 0) {
        if (why) *why = lp_first_line(err, out);
        g_free(out); g_free(err);
        return FALSE;
    }
    jnode_t *j = json_parse(out);
    int n = 0;
    gboolean ok = TRUE;
    for (jnode_t *c = j ? j->child : NULL; c; c = c->next) {
        if (!json_bool(c, "saved", FALSE)) continue;
        const char *ssid = json_str(c, "ssid", "");
        if (!*ssid) continue;
        const char *f[] = { "lp-net", "forget", ssid, NULL };
        char *o2 = NULL, *e2 = NULL;
        if (lp_run_full(f, NULL, &o2, &e2) != 0) {
            ok = FALSE;
            if (why && !*why) *why = lp_first_line(e2, o2);
        }
        n++;
        g_free(o2); g_free(e2);
    }
    json_free(j);
    g_free(out); g_free(err);
    return ok;
}

typedef struct {
    const char *id, *en, *ko;
    const char *back_en, *back_ko;      /* what comes back */
    const char *stays_en, *stays_ko;    /* what stays */
    gboolean    amber;
    reset_fn    fn;
} area_t;

static const area_t AREAS[] = {
    { "shortcuts", "Shortcuts", "단축키",
      "Every shortcut goes back to what LP ships with.", "모든 단축키가 LP 의 처음 값으로 돌아갑니다.",
      "Input sources and the switch key stay.", "입력 소스와 전환 키는 그대로입니다.",
      FALSE, reset_shortcuts },
    { "keyboard", "Keyboard", "키보드 설정",
      "Input sources, the switch key, and key repeat go back.", "입력 소스, 전환 키, 키 반복이 돌아갑니다.",
      "Shortcuts stay, and English (US) is always kept.", "단축키는 그대로이고, 영어 (US) 는 언제나 남습니다.",
      FALSE, reset_keyboard },
    { "display", "Display", "화면",
      "Resolution, refresh rate, scale and rotation go back to what each display prefers; night light turns off.",
      "해상도, 주사율, 배율, 방향이 각 화면이 권하는 값으로 돌아가고 야간 모드가 꺼집니다.",
      "The wallpaper stays.", "배경 화면은 그대로입니다.",
      FALSE, lp_display_reset },
    { "sound", "Sound", "소리",
      "Output and input volume, each app's volume, and the balance go back.",
      "출력·입력 볼륨, 앱별 볼륨, 좌우 균형이 돌아갑니다.",
      "The chosen output and input devices stay.", "골라 둔 출력·입력 장치는 그대로입니다.",
      FALSE, lp_sound_reset },
    { "network", "Network", "네트워크",
      "Saved Wi-Fi networks and their passwords are forgotten.", "저장된 Wi-Fi 와 그 비밀번호를 지웁니다.",
      "Files, accounts and other settings stay.", "파일, 계정, 다른 설정은 그대로입니다.",
      TRUE, reset_network },
};

#define AMBER_NET_EN "This computer drops off the network. To join again, the Wi-Fi password has to be typed once more."
#define AMBER_NET_KO "초기화하면 이 컴퓨터가 네트워크에서 떨어집니다. 다시 연결하려면 Wi-Fi 비밀번호를 한 번 더 입력해야 합니다."

static void run_area(const area_t *a, GString *failures)
{
    char *why = NULL;
    if (!a->fn(&why)) {
        if (failures->len) g_string_append(failures, "; ");
        g_string_append_printf(failures, "%s: %s", T(a->en, a->ko),
                               why ? why : T("failed", "실패"));
    }
    g_free(why);
}

static void area_ok(lp_dialog_t *d, gpointer p)
{
    const area_t *a = p;
    GString *f = g_string_new(NULL);
    run_area(a, f);
    lp_dialog_close(d);
    if (f->len) lp_toast(TRUE, T("Reset, with problems: %s", "초기화했지만 문제가 있었습니다: %s"), f->str);
    else lp_toast(FALSE, T("%s is reset", "%s 을(를) 초기화했습니다"), T(a->en, a->ko));
    g_string_free(f, TRUE);
}

/* Amber, not red: a limit on what the machine can do next, not a
 * failure (shell-design-spec 1-3). */
static void amber(lp_dialog_t *d)
{
    lp_dialog_text(d, T("▲ " AMBER_NET_EN, "▲ " AMBER_NET_KO), "lp-warn");
}

static void on_area(GtkButton *b, gpointer p)
{
    (void)b;
    const area_t *a = p;
    char *title = g_strdup_printf(T("Reset %s?", "%s 을(를) 초기화할까요?"), T(a->en, a->ko));
    lp_dialog_t *d = lp_dialog_new(title, T("Reset", "초기화"), FALSE, area_ok, (gpointer)a);
    lp_dialog_text(d, T(a->back_en, a->back_ko), NULL);
    lp_dialog_text(d, T(a->stays_en, a->stays_ko), "lp-note");
    if (a->amber) amber(d);
    lp_dialog_present(d);
    g_free(title);
}

/* ── all settings ───────────────────────────────────────────────────── */

static void all_ok(lp_dialog_t *d, gpointer p)
{
    (void)p;
    GString *f = g_string_new(NULL);
    /* The six, in the table's order: the list of what "all" means is
     * written once, here, not a second time (spec 2-6). */
    for (guint i = 0; i < G_N_ELEMENTS(AREAS); i++)
        run_area(&AREAS[i], f);
    lp_dialog_close(d);
    if (f->len) lp_toast(TRUE, T("Reset, with problems: %s", "초기화했지만 문제가 있었습니다: %s"), f->str);
    else lp_toast(FALSE, T("All settings are reset", "모든 설정을 초기화했습니다"));
    g_string_free(f, TRUE);
}

static void on_all(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    lp_dialog_t *d = lp_dialog_new(T("Reset all settings?", "모든 설정을 초기화할까요?"),
                                   T("Reset all", "모두 초기화"), FALSE, all_ok, NULL);
    lp_dialog_text(d, T("Shortcuts, keyboard, display, sound and network settings go back.",
                        "단축키, 키보드, 화면, 소리, 네트워크 설정이 돌아갑니다."), NULL);
    lp_dialog_text(d, T("This is not a factory reset: accounts and passwords, your documents and photos, "
                        "and the apps you installed all stay.",
                        "공장 초기화가 아닙니다. 계정과 비밀번호, 문서와 사진, 설치한 앱은 모두 그대로입니다."),
                   "lp-note");
    amber(d);
    lp_dialog_present(d);
}

/* ── the web ────────────────────────────────────────────────────────── */

static void on_web(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    /* Firefox keeps its own settings in its profile, and resets itself
     * better than any outside edit could ("Refresh Firefox", which keeps
     * bookmarks and saved passwords). This opens the page it is on. */
    static const char *const v[] = { "firefox-esr", "about:support", NULL };
    if (lp_spawn_bg(v))
        lp_toast(FALSE, T("In Firefox, choose \"Refresh Firefox…\" on the page that opened",
                          "열린 Firefox 페이지에서 \"Firefox 새로 고침…\" 을 고르십시오"));
}

/* ── reinstall ──────────────────────────────────────────────────────── */

static void reinstall_done(int st, const char *out, const char *err, gpointer p)
{
    (void)p;
    if (st == 0)
        lp_toast(FALSE, T("This computer is restarting into Recovery", "이 컴퓨터가 복구 모드로 다시 시작합니다"));
    else if (st != -2) {
        char *why = lp_first_line(err, out);
        lp_toast(TRUE, "%s", why);
        g_free(why);
    }
}

static void phrase_ok(lp_dialog_t *d, gpointer p)
{
    (void)p;
    GtkWidget *e = lp_dialog_get_data(d, "lp-phrase");
    const char *want = T("reset", "초기화");
    /* Exactly as typed: no trimming, no case folding (spec 3-3). */
    if (strcmp(gtk_editable_get_text(GTK_EDITABLE(e)), want) != 0) {
        char *m = g_strdup_printf(T("Type %s exactly, to go on.", "계속하려면 %s 를 그대로 입력하십시오."), want);
        lp_dialog_error(d, m);
        g_free(m);
        return;
    }
    lp_dialog_close(d);
    static const char *const v[] = { "lp-reboot-recovery", NULL };
    /* Within the five minutes of the password just typed: no second
     * prompt. */
    lp_admin_run(v, NULL, T("Restarting into Recovery needs an administrator.",
                            "복구 모드로 다시 시작하려면 관리자 권한이 필요합니다."),
                 NULL, reinstall_done, NULL);
}

static void ask_phrase(void)
{
    lp_dialog_t *d = lp_dialog_new(T("Type to confirm", "입력해서 확인"), T("Restart into Recovery", "복구 모드로 다시 시작"),
                                   TRUE, phrase_ok, NULL);
    lp_dialog_text(d, T("Type reset to restart into Recovery, where \"Reinstall LP\" is.",
                        "초기화 라고 입력하면 \"LP 다시 설치\" 가 있는 복구 모드로 다시 시작합니다."), NULL);
    GtkWidget *e = lp_dialog_entry(d, T("Type reset", "초기화 입력"), NULL, FALSE);
    lp_dialog_set_data(d, "lp-phrase", e, NULL);
    lp_dialog_present(d);
}

static void password_checked(int st, const char *out, const char *err, gpointer p)
{
    (void)out; (void)err; (void)p;
    if (st == 0) ask_phrase();
}

static void facts_ok(lp_dialog_t *d, gpointer p)
{
    (void)p;
    lp_dialog_close(d);
    /* The password step: a command that does nothing, run as
     * administrator, is the password check - and it starts the five
     * minutes the last step runs in. */
    static const char *const v[] = { "true", NULL };
    lp_admin_run(v, NULL, T("Reinstalling LP needs an administrator.", "LP 를 다시 설치하려면 관리자 권한이 필요합니다."),
                 NULL, password_checked, NULL);
}

static void on_reinstall(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    lp_dialog_t *d = lp_dialog_new(T("Reinstall LP", "LP 다시 설치"), T("Continue", "계속"), TRUE, facts_ok, NULL);
    lp_dialog_text(d, T("The computer restarts into Recovery. Choose \"Reinstall LP\" there, then one of:",
                        "컴퓨터가 복구 모드로 다시 시작합니다. 그곳에서 \"LP 다시 설치\" 를 고른 뒤 둘 중 하나를 고릅니다:"), NULL);
    lp_dialog_text(d, T("• Keep my files - the system is put back as shipped; /home and the accounts stay.\n"
                        "• Erase everything - accounts, documents, photos, installed apps and every setting "
                        "are erased.",
                        "• 내 파일 남기기 - 시스템만 출고 상태로 돌아가고 /home 과 계정은 남습니다.\n"
                        "• 모두 지우기 - 계정, 문서, 사진, 설치한 앱, 모든 설정이 지워집니다."), NULL);
    lp_dialog_text(d, T("What comes back is the version on the recovery partition, not yesterday: "
                        "updates since then are installed again afterwards. Keep the power on while it runs.",
                        "돌아오는 것은 복구 파티션의 판이지 어제가 아닙니다. 그 뒤의 업데이트는 다시 받아야 "
                        "합니다. 진행 중에는 전원을 끄지 마십시오."), "lp-note");
    lp_dialog_present(d);
}

/* ── building ───────────────────────────────────────────────────────── */

static GtkWidget *build(void)
{
    gboolean admin = lp_is_admin();
    GtkWidget *page = page_new(T("Reset", "초기화"),
                               T("Put settings back the way LP ships them", "설정을 LP 의 처음 상태로 되돌립니다"));
    GtkWidget *g = group_new(page, NULL);
    for (guint i = 0; i < G_N_ELEMENTS(AREAS); i++) {
        const area_t *a = &AREAS[i];
        char *detail = g_strdup_printf("%s %s", T(a->back_en, a->back_ko), T(a->stays_en, a->stays_ko));
        GtkWidget *r = row_button(g, T(a->en, a->ko), detail, T("Reset…", "초기화…"),
                                  G_CALLBACK(on_area), (gpointer)a);
        g_free(detail);
        if (a->amber) gtk_widget_add_css_class(row_control(r), "lp-warn");
        if (!admin) row_lock(r);
    }
    GtkWidget *w = row_button(g, T("Web", "웹"),
                              T("Firefox resets itself and keeps bookmarks and saved passwords; you are signed "
                                "out of websites.",
                                "Firefox 가 직접 초기화하며 즐겨찾기와 저장한 비밀번호는 남습니다. 웹사이트에서는 "
                                "로그아웃됩니다."),
                              T("Open Firefox…", "Firefox 열기…"), G_CALLBACK(on_web), NULL);
    if (!lp_have("firefox-esr")) gtk_widget_set_sensitive(row_control(w), FALSE);

    GtkWidget *all = group_new(page, T("Everything", "전체"));
    GtkWidget *ar = row_button(all, T("All settings", "모든 설정"),
                               T("The five above at once. Not a factory reset: accounts, files and apps stay.",
                                 "위의 다섯을 한 번에. 공장 초기화가 아닙니다: 계정, 파일, 앱은 남습니다."),
                               T("Reset all…", "모두 초기화…"), G_CALLBACK(on_all), NULL);
    if (!admin) row_lock(ar);

    GtkWidget *zone = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(zone, "lp-zone");
    gtk_widget_set_margin_top(zone, 28);
    GtkWidget *zt = gtk_label_new(T("Reinstall LP", "LP 다시 설치"));
    gtk_widget_add_css_class(zt, "lp-zone-title");
    gtk_widget_set_halign(zt, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(zone), zt);
    GtkWidget *zl = gtk_label_new(T("The factory reset. It happens in Recovery, where you choose whether your "
                                    "files are kept. Administrators only.",
                                    "공장 초기화입니다. 복구 모드에서 하며, 파일을 남길지 그곳에서 고릅니다. "
                                    "관리자만 할 수 있습니다."));
    gtk_label_set_wrap(GTK_LABEL(zl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(zl), 0);
    gtk_widget_add_css_class(zl, "lp-note");
    gtk_box_append(GTK_BOX(zone), zl);
    GtkWidget *zb = gtk_button_new_with_label(T("Reinstall LP…", "LP 다시 설치…"));
    gtk_widget_add_css_class(zb, "destructive-action");
    gtk_widget_set_halign(zb, GTK_ALIGN_START);
    g_signal_connect(zb, "clicked", G_CALLBACK(on_reinstall), NULL);
    gtk_box_append(GTK_BOX(zone), zb);
    if (!admin || !lp_have("lp-reboot-recovery")) {
        gtk_widget_set_sensitive(zb, FALSE);
        GtkWidget *why = gtk_label_new(!admin ? T("Only an administrator can change this", "관리자만 바꿀 수 있습니다")
                                              : T("Not available: lp-reboot-recovery is not installed",
                                                  "쓸 수 없음: lp-reboot-recovery 가 설치되어 있지 않습니다"));
        gtk_widget_add_css_class(why, "lp-detail");
        gtk_widget_set_halign(why, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(zone), why);
    }
    gtk_box_append(GTK_BOX(page), zone);
    return page;
}

static const char *const KEYS[] = {
    "Shortcuts", "단축키",
    "Keyboard", "키보드 설정",
    "Display", "화면",
    "Sound", "소리",
    "Network", "네트워크",
    "All settings", "모든 설정",
    "Factory reset", "공장 초기화",
    "Reinstall LP", "LP 다시 설치",
    NULL
};

const lp_panel_t lp_panel_reset = {
    "reset", "Reset", "초기화", "edit-undo-symbolic", build, KEYS, NULL
};
