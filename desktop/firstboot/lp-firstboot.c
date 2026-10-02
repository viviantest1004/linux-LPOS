/*
 * lp-firstboot - the setup that runs once, on the first boot of an
 * installed LP.
 *
 * The installer has already asked what had to be known before the disk
 * was written: the language, the account and its password (which is also
 * the recovery password), the time zone and the computer's name. What is
 * left is what only makes sense on the machine itself - the keyboard it
 * is typed on and the network it is in - and, for a system that was not
 * installed by the window (a scripted `lp-install`, an image written to
 * the disk by hand), the questions the installer would have asked.
 * /etc/lp/firstboot lists under done= what was answered already, and
 * those pages are skipped.
 *
 * The pages:
 *
 *   language   English or 한국어, preselected from the installer's choice
 *   keyboard   English (US), or English (US) with Korean 2-beolsik
 *              (한/영 on right Alt, the XPS's US keyboard has no key)
 *   wifi       the networks lp-net sees; tap one, give the password,
 *              connect - or skip, it can be done from the top bar later
 *   account    only if the installer did not make one
 *   region     only if the installer did not set them
 *   finish     lp-install configure ... --finish applies everything in
 *              one call, as root through the setup gate, and removes
 *              /etc/lp/firstboot: this window never comes back
 *
 * Every write is lp-install's (the same code the installer uses on the
 * new disk, `configure`), and it reaches root through the setup gate's
 * relay (LP_INSTALL, LP_NET - lp-setup-gate explains why the window is
 * not root itself). Nothing is written until the last page, so a power
 * cut half way through the questions just asks them again.
 *
 * Touch first and bilingual exactly as the installer is: setup-ui.c for
 * the look and the language switch, setup-pages.c for account and
 * region.
 *
 *   lp-firstboot [--korean] [--windowed]
 *   LP_SETUP_PAGE=<page>    start on a page (screenshots, tests)
 */
#include "setup-ui.h"
#include "setup-pages.h"
#include "lp-json.h"
#include "lp-motion.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    GtkApplication *app;
    GtkWidget  *win, *stack, *card;
    gboolean    windowed;
    char       *done;                   /* "account,hostname,..." */

    GtkWidget  *en, *ko;                /* language */
    GtkWidget  *kb_us, *kb_ko;          /* keyboard */

    /* wifi */
    GtkWidget  *wifi_box, *wifi_status, *wifi_pw, *wifi_pw_row, *wifi_connect;
    char        wifi_ssid[128];
    gboolean    wifi_secured;
    GSubprocess *wifi_proc;

    SuAccount   acct;
    SuRegion    region;

    /* finish */
    GtkWidget  *fin_title_ok, *fin_text, *fin_go, *fin_retry;
    GtkWidget  *fin_spinner;
    GSubprocess *fin_proc;
    char        fin_error[1024];

    LpSpring    card_in;
    LpMotion   *card_m;
} App;

static App A;

static const char *backend(const char *env, const char *def)
{
    const char *b = g_getenv(env);
    return b && *b ? b : def;
}

static gboolean asked(const char *key)
{
    /* A key under done= was answered by the installer. */
    if (!A.done)
        return FALSE;
    char **v = g_strsplit(A.done, ",", -1);
    gboolean hit = g_strv_contains((const char *const *)v, key);
    g_strfreev(v);
    return hit;
}

static gboolean need_account(void) { return !asked("account"); }
static gboolean need_region(void)  { return !asked("timezone") || !asked("hostname"); }

/* The page after `from`, going forward, skipping what is not needed. */
static const char *next_page(const char *from)
{
    static const char *const ORDER[] = { "language", "keyboard", "wifi",
                                         "account", "region", "finish", NULL };
    gboolean past = FALSE;
    for (int i = 0; ORDER[i]; i++) {
        if (past) {
            if (!strcmp(ORDER[i], "account") && !need_account()) continue;
            if (!strcmp(ORDER[i], "region") && !need_region()) continue;
            return ORDER[i];
        }
        if (!strcmp(ORDER[i], from))
            past = TRUE;
    }
    return "finish";
}

static const char *prev_page(const char *from)
{
    static const char *const ORDER[] = { "language", "keyboard", "wifi",
                                         "account", "region", NULL };
    const char *prev = "language";
    for (int i = 0; ORDER[i]; i++) {
        if (!strcmp(ORDER[i], from))
            return prev;
        if (!strcmp(ORDER[i], "account") && !need_account()) continue;
        if (!strcmp(ORDER[i], "region") && !need_region()) continue;
        prev = ORDER[i];
    }
    return prev;
}

static void start_finish(void);
static void scan_wifi(void);

static void go_next(GtkButton *b, gpointer page)
{
    (void)b;
    const char *to = next_page(page);
    if (!strcmp(page, "account"))
        su_region_suggest_host(&A.region, su_account_login(&A.acct));
    su_go(A.stack, to, TRUE);
    if (!strcmp(to, "wifi"))
        scan_wifi();
    if (!strcmp(to, "finish"))
        start_finish();
}

static void go_back(GtkButton *b, gpointer page)
{
    (void)b;
    su_go(A.stack, prev_page(page), FALSE);
}

static void nav(SuPage *p, const char *page, gboolean back)
{
    if (back) {
        GtkWidget *b = su_button("Back", "뒤로", "su-secondary");
        g_signal_connect(b, "clicked", G_CALLBACK(go_back), (gpointer)page);
        gtk_box_append(GTK_BOX(p->left), b);
    }
    GtkWidget *n = su_button("Next", "다음", "su-primary");
    g_signal_connect(n, "clicked", G_CALLBACK(go_next), (gpointer)page);
    gtk_box_append(GTK_BOX(p->right), n);
}

/* ── language ──────────────────────────────────────────────────────── */

static void on_language(GtkToggleButton *b, gpointer d)
{
    (void)d;
    if (!gtk_toggle_button_get_active(b))
        return;
    su_set_korean(GTK_WIDGET(b) == A.ko);
    su_region_language_changed(&A.region);
    /* Korean chosen: the keyboard page starts on the Korean layout. */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(su_korean ? A.kb_ko : A.kb_us), TRUE);
}

static void page_language(void)
{
    SuPage p;
    su_page(&p, "Welcome to LP", "LP 에 오신 것을 환영합니다",
            "A few questions, once, and LP is ready. Choose a language.",
            "몇 가지만 한 번 물어보면 LP 를 쓸 수 있습니다. 언어를 고르세요.");
    A.en = su_choice("English", "English", "English (United States)", "영어 (미국)", NULL);
    A.ko = su_choice("한국어", "한국어", "Korean", "한국어 (대한민국)", NULL);
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(A.ko), GTK_TOGGLE_BUTTON(A.en));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(su_korean ? A.ko : A.en), TRUE);
    g_signal_connect(A.en, "toggled", G_CALLBACK(on_language), NULL);
    g_signal_connect(A.ko, "toggled", G_CALLBACK(on_language), NULL);
    gtk_box_append(GTK_BOX(p.body), A.en);
    gtk_box_append(GTK_BOX(p.body), A.ko);
    gtk_box_append(GTK_BOX(p.body), su_label(
        "Each account can change its language later in Settings -> Region & "
        "Language; it applies at the next sign-in.",
        "계정마다 나중에 설정 -> 지역과 언어에서 바꿀 수 있고, 다음 로그인부터 "
        "적용됩니다.", "su-note"));
    nav(&p, "language", FALSE);
    gtk_stack_add_named(GTK_STACK(A.stack), p.root, "language");
}

/* ── keyboard ──────────────────────────────────────────────────────── */

static void page_keyboard(void)
{
    SuPage p;
    su_page(&p, "Keyboard", "키보드",
            "How do you type? The on-screen keyboard follows the same choice.",
            "어떻게 입력하나요? 화면 키보드도 같은 설정을 따릅니다.");
    A.kb_us = su_choice("English (US)", "영어 (미국)",
                        "The laptop's keys as printed.", "노트북 자판에 적힌 그대로.",
                        "input-keyboard");
    A.kb_ko = su_choice("English (US) and Korean", "영어 (미국) 와 한국어",
                        "Korean 2-beolsik alongside English. Switch with 한/영 - "
                        "the right Alt key on a keyboard without one.",
                        "영어와 한국어 두벌식. 한/영 으로 바꿉니다 - 한/영 키가 없는 "
                        "자판에서는 오른쪽 Alt 키.", "input-keyboard");
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(A.kb_ko), GTK_TOGGLE_BUTTON(A.kb_us));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(su_korean ? A.kb_ko : A.kb_us), TRUE);
    gtk_box_append(GTK_BOX(p.body), A.kb_us);
    gtk_box_append(GTK_BOX(p.body), A.kb_ko);
    nav(&p, "keyboard", TRUE);
    gtk_stack_add_named(GTK_STACK(A.stack), p.root, "keyboard");
}

/* ── wifi ──────────────────────────────────────────────────────────── */

static void wifi_say(const char *en, const char *ko)
{
    su_retext(A.wifi_status, en, ko);
}

static void on_network(GtkToggleButton *b, gpointer d)
{
    (void)d;
    if (!gtk_toggle_button_get_active(b))
        return;
    g_strlcpy(A.wifi_ssid, g_object_get_data(G_OBJECT(b), "ssid"), sizeof A.wifi_ssid);
    A.wifi_secured = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "secured"));
    gtk_revealer_set_reveal_child(GTK_REVEALER(A.wifi_pw_row), TRUE);
    gtk_widget_set_visible(A.wifi_pw, A.wifi_secured);
    gtk_widget_set_sensitive(A.wifi_connect, TRUE);
    if (A.wifi_secured)
        gtk_widget_grab_focus(A.wifi_pw);
}

static gboolean reveal_one(gpointer data)
{
    gtk_revealer_set_reveal_child(GTK_REVEALER(data), TRUE);
    return G_SOURCE_REMOVE;
}

static void scan_wifi(void)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(A.wifi_box)))
        gtk_box_remove(GTK_BOX(A.wifi_box), c);
    gtk_revealer_set_reveal_child(GTK_REVEALER(A.wifi_pw_row), FALSE);
    gtk_widget_set_sensitive(A.wifi_connect, FALSE);

    const char *argv[] = { backend("LP_NET", "lp-net"), "scan", "--json", NULL };
    int st = -1;
    char *out = su_run(argv, &st);
    LpJson *j = out && st == 0 ? lp_json_parse(out) : NULL;
    int n = j ? lp_json_len(j) : 0;
    GtkWidget *group = NULL;
    for (int i = 0; i < n && i < 12; i++) {
        LpJson *w = lp_json_at(j, i);
        const char *ssid = lp_json_str(w, "ssid", "");
        if (!*ssid)
            continue;               /* hidden networks: not from a list */
        const char *sec = lp_json_str(w, "security", "open");
        int q = (int)lp_json_num(w, "quality", 0);
        gboolean secured = strcmp(sec, "open") != 0;
        char *den = g_strdup_printf("%d%%  ·  %s", q, secured ? sec : "open");
        char *dko = g_strdup_printf("%d%%  ·  %s", q, secured ? sec : "암호 없음");
        GtkWidget *b = su_choice(ssid, ssid, den, dko,
                                 secured ? "network-wireless-encrypted-symbolic"
                                         : "network-wireless-symbolic");
        g_free(den); g_free(dko);
        g_object_set_data_full(G_OBJECT(b), "ssid", g_strdup(ssid), g_free);
        g_object_set_data(G_OBJECT(b), "secured", GINT_TO_POINTER(secured));
        if (group)
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(group));
        else
            group = b;
        g_signal_connect(b, "toggled", G_CALLBACK(on_network), NULL);
        /* Rows come in one after another on the insert spring's time. */
        GtkWidget *rv = gtk_revealer_new();
        gtk_revealer_set_transition_type(GTK_REVEALER(rv),
            lp_motion_reduced() ? GTK_REVEALER_TRANSITION_TYPE_CROSSFADE
                                : GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
        gtk_revealer_set_transition_duration(GTK_REVEALER(rv),
            lp_motion_reduced() ? 90 : lp_spring_ms(LP_SPRING_INSERT, FALSE));
        gtk_revealer_set_child(GTK_REVEALER(rv), b);
        gtk_box_append(GTK_BOX(A.wifi_box), rv);
        g_timeout_add(120 + 40 * i, reveal_one, rv);
    }
    if (!j)
        wifi_say("No Wi-Fi adapter answered. You can connect later from the top bar.",
                 "Wi-Fi 장치가 응답하지 않습니다. 나중에 상단바에서 연결할 수 있습니다.");
    else if (!group)
        wifi_say("No networks in range. You can connect later from the top bar.",
                 "잡히는 네트워크가 없습니다. 나중에 상단바에서 연결할 수 있습니다.");
    else
        wifi_say("Choose a network, or skip this and connect later.",
                 "네트워크를 고르거나, 건너뛰고 나중에 연결하세요.");
    if (j)
        lp_json_free(j);
    g_free(out);
}

static void on_connected(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    char *out = NULL;
    GError *err = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out,
                                                       NULL, &err) &&
                  g_subprocess_get_if_exited(G_SUBPROCESS(src)) &&
                  g_subprocess_get_exit_status(G_SUBPROCESS(src)) == 0;
    g_clear_error(&err);
    g_clear_object(&A.wifi_proc);
    gtk_widget_set_sensitive(A.wifi_connect, TRUE);
    if (ok) {
        char *en = g_strdup_printf("Connected to %s.", A.wifi_ssid);
        char *ko = g_strdup_printf("%s 에 연결했습니다.", A.wifi_ssid);
        wifi_say(en, ko);
        g_free(en); g_free(ko);
    } else {
        char *line = out ? g_strstrip(out) : NULL;
        char *en = g_strdup_printf("Could not connect to %s%s%s", A.wifi_ssid,
                                   line && *line ? ": " : ".", line && *line ? line : "");
        char *ko = g_strdup_printf("%s 에 연결하지 못했습니다%s%s", A.wifi_ssid,
                                   line && *line ? ": " : ".", line && *line ? line : "");
        wifi_say(en, ko);
        g_free(en); g_free(ko);
    }
    g_free(out);
}

static void on_connect(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!A.wifi_ssid[0] || A.wifi_proc)
        return;
    const char *pw = gtk_editable_get_text(GTK_EDITABLE(A.wifi_pw));
    GError *err = NULL;
    /* lp-net's contract (COMMON.md) takes the password as an option, so
     * for the second the connect takes it is in the process list. At
     * first boot there is nobody else on the machine to read it; a
     * --password-stdin in lp-net would close even that (asked of the
     * networking track). */
    if (A.wifi_secured && *pw)
        A.wifi_proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                       G_SUBPROCESS_FLAGS_STDERR_MERGE, &err,
                                       backend("LP_NET", "lp-net"), "connect",
                                       A.wifi_ssid, "--password", pw, NULL);
    else
        A.wifi_proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                       G_SUBPROCESS_FLAGS_STDERR_MERGE, &err,
                                       backend("LP_NET", "lp-net"), "connect",
                                       A.wifi_ssid, NULL);
    if (!A.wifi_proc) {
        wifi_say(err ? err->message : "lp-net did not start",
                 err ? err->message : "lp-net 이 시작되지 않았습니다");
        g_clear_error(&err);
        return;
    }
    gtk_widget_set_sensitive(A.wifi_connect, FALSE);
    char *en = g_strdup_printf("Connecting to %s…", A.wifi_ssid);
    char *ko = g_strdup_printf("%s 에 연결하는 중…", A.wifi_ssid);
    wifi_say(en, ko);
    g_free(en); g_free(ko);
    g_subprocess_communicate_utf8_async(A.wifi_proc, NULL, NULL, on_connected, NULL);
}

static void on_rescan(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    scan_wifi();
}

static void page_wifi(void)
{
    SuPage p;
    su_page(&p, "Wi-Fi", "Wi-Fi",
            "Connect to a network now, or later from the top bar.",
            "지금 네트워크에 연결하거나, 나중에 상단바에서 연결하세요.");
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_widget_add_css_class(sw, "su-list");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), 300);
    A.wifi_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), A.wifi_box);
    gtk_box_append(GTK_BOX(p.body), sw);

    A.wifi_pw_row = gtk_revealer_new();
    gtk_revealer_set_transition_duration(GTK_REVEALER(A.wifi_pw_row),
        lp_motion_reduced() ? 90 : lp_spring_ms(LP_SPRING_EXPAND, FALSE));
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    A.wifi_pw = gtk_password_entry_new();
    su_osk_attach(A.wifi_pw);
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(A.wifi_pw), TRUE);
    gtk_widget_add_css_class(A.wifi_pw, "su-entry");
    gtk_widget_set_hexpand(A.wifi_pw, TRUE);
    gtk_box_append(GTK_BOX(row), A.wifi_pw);
    A.wifi_connect = su_button("Connect", "연결", "su-primary");
    g_signal_connect(A.wifi_connect, "clicked", G_CALLBACK(on_connect), NULL);
    gtk_box_append(GTK_BOX(row), A.wifi_connect);
    gtk_revealer_set_child(GTK_REVEALER(A.wifi_pw_row), row);
    gtk_box_append(GTK_BOX(p.body), A.wifi_pw_row);

    A.wifi_status = su_label("", "", "su-hint");
    gtk_box_append(GTK_BOX(p.body), A.wifi_status);

    GtkWidget *back = su_button("Back", "뒤로", "su-secondary");
    g_signal_connect(back, "clicked", G_CALLBACK(go_back), (gpointer)"wifi");
    gtk_box_append(GTK_BOX(p.left), back);
    GtkWidget *again = su_button("Scan again", "다시 찾기", "su-secondary");
    g_signal_connect(again, "clicked", G_CALLBACK(on_rescan), NULL);
    gtk_box_append(GTK_BOX(p.left), again);
    GtkWidget *n = su_button("Next", "다음", "su-primary");
    g_signal_connect(n, "clicked", G_CALLBACK(go_next), (gpointer)"wifi");
    gtk_box_append(GTK_BOX(p.right), n);
    gtk_stack_add_named(GTK_STACK(A.stack), p.root, "wifi");
}

/* ── account, region ───────────────────────────────────────────────── */

static void page_account(void)
{
    su_account_build(&A.acct, FALSE);
    g_signal_connect(A.acct.back, "clicked", G_CALLBACK(go_back), (gpointer)"account");
    g_signal_connect(A.acct.next, "clicked", G_CALLBACK(go_next), (gpointer)"account");
    gtk_stack_add_named(GTK_STACK(A.stack), A.acct.page.root, "account");
}

static void page_region(void)
{
    su_region_build(&A.region);
    g_signal_connect(A.region.back, "clicked", G_CALLBACK(go_back), (gpointer)"region");
    g_signal_connect(A.region.next, "clicked", G_CALLBACK(go_next), (gpointer)"region");
    gtk_stack_add_named(GTK_STACK(A.stack), A.region.page.root, "region");
}

/* ── finish ────────────────────────────────────────────────────────── */

static void finish_state(int state)       /* 0 working, 1 done, 2 failed */
{
    gtk_widget_set_visible(A.fin_spinner, state == 0);
    gtk_spinner_set_spinning(GTK_SPINNER(A.fin_spinner), state == 0);
    gtk_widget_set_visible(A.fin_go, state == 1);
    gtk_widget_set_visible(A.fin_retry, state == 2);
    if (state == 0) {
        su_retext(A.fin_title_ok, "Setting up…", "설정하는 중…");
        su_retext(A.fin_text, "Applying your choices.", "고른 것을 적용하고 있습니다.");
    } else if (state == 1) {
        su_retext(A.fin_title_ok, "LP is ready", "LP 를 쓸 준비가 됐습니다");
        su_retext(A.fin_text,
                  "Everything you chose is saved on the disk. Settings can change any "
                  "of it later.",
                  "고른 것은 모두 디스크에 저장됐습니다. 언제든 설정에서 바꿀 수 있습니다.");
    } else {
        char *en = g_strdup_printf("It could not be saved: %s", A.fin_error);
        char *ko = g_strdup_printf("저장하지 못했습니다: %s", A.fin_error);
        su_retext(A.fin_title_ok, "Something went wrong", "문제가 생겼습니다");
        su_retext(A.fin_text, en, ko);
        g_free(en); g_free(ko);
    }
}

static void on_applied(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    char *out = NULL;
    GError *err = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out,
                                                       NULL, &err) &&
                  g_subprocess_get_if_exited(G_SUBPROCESS(src)) &&
                  g_subprocess_get_exit_status(G_SUBPROCESS(src)) == 0;
    g_strlcpy(A.fin_error, err ? err->message : (out ? g_strstrip(out) : "?"),
              sizeof A.fin_error);
    g_clear_error(&err);
    g_free(out);
    g_clear_object(&A.fin_proc);
    finish_state(ok ? 1 : 2);
}

static void start_finish(void)
{
    if (A.fin_proc)
        return;
    finish_state(0);
    GPtrArray *av = g_ptr_array_new();
    g_ptr_array_add(av, (gpointer)backend("LP_INSTALL", "/usr/local/bin/lp-install"));
    g_ptr_array_add(av, "configure");
    g_ptr_array_add(av, "--lang");
    g_ptr_array_add(av, su_korean ? "ko_KR.UTF-8" : "en_US.UTF-8");
    g_ptr_array_add(av, "--keyboard");
    g_ptr_array_add(av, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.kb_ko)) ? "us,kr" : "us");
    if (need_account()) {
        g_ptr_array_add(av, "--user");
        g_ptr_array_add(av, (gpointer)su_account_login(&A.acct));
        g_ptr_array_add(av, "--fullname");
        g_ptr_array_add(av, (gpointer)su_account_fullname(&A.acct));
        g_ptr_array_add(av, "--password-stdin");
    }
    if (need_region()) {
        g_ptr_array_add(av, "--timezone");
        g_ptr_array_add(av, (gpointer)su_region_timezone(&A.region));
        g_ptr_array_add(av, "--hostname");
        g_ptr_array_add(av, (gpointer)su_region_hostname(&A.region));
    }
    g_ptr_array_add(av, "--finish");
    g_ptr_array_add(av, NULL);
    GError *err = NULL;
    A.fin_proc = g_subprocess_newv((const char *const *)av->pdata,
                                   G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                   G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                   G_SUBPROCESS_FLAGS_STDERR_MERGE, &err);
    g_ptr_array_free(av, TRUE);
    if (!A.fin_proc) {
        g_strlcpy(A.fin_error, err ? err->message : "?", sizeof A.fin_error);
        g_clear_error(&err);
        finish_state(2);
        return;
    }
    char *in = need_account() ? g_strdup_printf("%s\n", su_account_password(&A.acct))
                              : g_strdup("");
    g_subprocess_communicate_utf8_async(A.fin_proc, in, NULL, on_applied, NULL);
    g_free(in);
}

static void on_start(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gtk_window_destroy(GTK_WINDOW(A.win));
}

static void on_retry(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    start_finish();
}

static void page_finish(void)
{
    SuPage p;
    su_page(&p, "", "", NULL, NULL);
    gtk_widget_set_visible(p.title, FALSE);
    A.fin_title_ok = su_label("", "", "su-title");
    gtk_box_prepend(GTK_BOX(p.body), A.fin_title_ok);
    A.fin_spinner = gtk_spinner_new();
    gtk_widget_set_size_request(A.fin_spinner, 48, 48);
    gtk_widget_set_halign(A.fin_spinner, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(p.body), A.fin_spinner);
    A.fin_text = su_label("", "", "su-body");
    gtk_box_append(GTK_BOX(p.body), A.fin_text);
    A.fin_retry = su_button("Try again", "다시 시도", "su-secondary");
    g_signal_connect(A.fin_retry, "clicked", G_CALLBACK(on_retry), NULL);
    gtk_box_append(GTK_BOX(p.left), A.fin_retry);
    A.fin_go = su_button("Start using LP", "LP 시작하기", "su-primary");
    g_signal_connect(A.fin_go, "clicked", G_CALLBACK(on_start), NULL);
    gtk_box_append(GTK_BOX(p.right), A.fin_go);
    gtk_stack_add_named(GTK_STACK(A.stack), p.root, "finish");
    finish_state(0);
}

/* ── the window ────────────────────────────────────────────────────── */

static gboolean card_start(GtkWidget *w, GdkFrameClock *fc, gpointer d)
{
    static int frames;
    (void)w; (void)fc; (void)d;
    if (++frames < 3)
        return G_SOURCE_CONTINUE;
    lp_spring_set_target(&A.card_in, 1.0);
    lp_motion_kick(A.card_m);
    return G_SOURCE_REMOVE;
}

static void card_frame(GtkWidget *w, gpointer d)
{
    (void)d;
    gtk_widget_set_opacity(w, CLAMP(A.card_in.x, 0.0, 1.0));
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)w; (void)d;
    return A.fin_proc != NULL;        /* not while the answers are written */
}

static void activate(GtkApplication *app, gpointer d)
{
    (void)d;
    su_load_css();
    A.win = gtk_application_window_new(app);
    gtk_widget_add_css_class(A.win, "su-window");
    gtk_window_set_title(GTK_WINDOW(A.win), "Set up LP");
    gtk_window_set_default_size(GTK_WINDOW(A.win), 1100, 760);
    g_signal_connect(A.win, "close-request", G_CALLBACK(on_close), NULL);

    A.card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(A.card, "su-card");
    gtk_widget_set_size_request(A.card, 760, 560);
    gtk_widget_set_halign(A.card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(A.card, GTK_ALIGN_CENTER);
    A.stack = su_stack();
    gtk_widget_set_vexpand(A.stack, TRUE);
    gtk_box_append(GTK_BOX(A.card), A.stack);
    gtk_window_set_child(GTK_WINDOW(A.win), su_card_holder(A.card));
    su_osk_card(A.card);

    page_keyboard();          /* before language: on_language sets it */
    page_region();            /* before language: likewise the zone   */
    page_language();
    page_wifi();
    page_account();
    page_finish();
    /* Order in the stack is not the order of the pages; the names are. */
    su_set_korean(su_korean);
    gtk_stack_set_visible_child_name(GTK_STACK(A.stack), "language");

    if (!A.windowed)
        gtk_window_fullscreen(GTK_WINDOW(A.win));
    gtk_window_present(GTK_WINDOW(A.win));

    lp_spring_init(&A.card_in, LP_SPRING_WINDOW, lp_motion_reduced() ? 1.0 : 0.0);
    gtk_widget_set_opacity(A.card, A.card_in.x);
    A.card_m = lp_motion_new(A.card, card_frame, NULL);
    lp_motion_add(A.card_m, &A.card_in);
    /* Started from the third frame, not the first: the first frames of a
     * new window at 3840x2160 are its layout and its fonts loading, and
     * an animation that begins then loses its first 300 ms to them. */
    gtk_widget_add_tick_callback(A.card, card_start, NULL, NULL);

    const char *page = g_getenv("LP_SETUP_PAGE");
    if (page && *page) {
        gtk_stack_set_visible_child_name(GTK_STACK(A.stack), page);
        if (!strcmp(page, "wifi"))
            scan_wifi();
        if (!strcmp(page, "finish"))
            start_finish();
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--korean"))
            su_korean = TRUE;
        else if (!strcmp(argv[i], "--windowed"))
            A.windowed = TRUE;
    }
    const char *lang = g_getenv("LANG");
    if (lang && g_str_has_prefix(lang, "ko"))
        su_korean = TRUE;

    /* What the installer answered already. */
    char *fb = NULL;
    const char *path = g_getenv("LP_FIRSTBOOT_FILE");
    if (g_file_get_contents(path && *path ? path : "/etc/lp/firstboot", &fb, NULL, NULL)) {
        char **lines = g_strsplit(fb, "\n", -1);
        for (char **l = lines; *l; l++) {
            if (g_str_has_prefix(*l, "done="))
                A.done = g_strdup(*l + 5);
            if (g_str_has_prefix(*l, "lang=ko"))
                su_korean = TRUE;
        }
        g_strfreev(lines);
        g_free(fb);
    }

    A.app = gtk_application_new("org.lp.Firstboot", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(A.app, "activate", G_CALLBACK(activate), NULL);
    char *args[] = { argv[0], NULL };
    int st = g_application_run(G_APPLICATION(A.app), 1, args);
    g_object_unref(A.app);
    return st;
}
