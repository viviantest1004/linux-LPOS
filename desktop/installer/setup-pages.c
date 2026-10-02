/*
 * setup-pages.c - the account and region pages (setup-pages.h says why
 * they are shared).
 *
 * The rules checked here are lp-install's own (LOGIN_RE, HOST_RE, a
 * zone that exists under /usr/share/zoneinfo), repeated so the window
 * can say what is wrong while the person types instead of after a disk
 * has been chosen. lp-install checks again before it writes anything;
 * this copy is for the person, that one is for the machine.
 */
#include "setup-pages.h"

#include <ctype.h>
#include <string.h>

/* A caption above a field: a label the size of the body text. */
static GtkWidget *caption(const char *en, const char *ko)
{
    GtkWidget *l = su_label(en, ko, "su-caption");
    gtk_widget_set_margin_top(l, 4);
    return l;
}

static void nav(SuPage *p, GtkWidget **back, GtkWidget **next)
{
    *back = su_button("Back", "뒤로", "su-secondary");
    gtk_box_append(GTK_BOX(p->left), *back);
    *next = su_button("Next", "다음", "su-primary");
    gtk_widget_set_sensitive(*next, FALSE);
    gtk_box_append(GTK_BOX(p->right), *next);
}

/* ── account ───────────────────────────────────────────────────────── */

static gboolean login_ok(const char *s)
{
    if (!s || !*s || strlen(s) > 32)
        return FALSE;
    if (!(islower((unsigned char)s[0]) || s[0] == '_'))
        return FALSE;
    for (const char *p = s; *p; p++)
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) ||
              *p == '_' || *p == '-'))
            return FALSE;
    /* Names the base system already uses for its own accounts. */
    static const char *const TAKEN[] = {
        "root", "daemon", "bin", "sys", "sync", "games", "man", "lp", "mail",
        "news", "uucp", "proxy", "www-data", "backup", "list", "irc", "gnats",
        "nobody", "messagebus", "polkitd", "_apt", "sudo", "admin", NULL
    };
    for (int i = 0; TAKEN[i]; i++)
        if (!strcmp(s, TAKEN[i]))
            return FALSE;
    return TRUE;
}

/* "Alex Kim" -> "alex". A name written in Hangul has no ASCII in
 * it, and then the field stays empty for the person to fill: guessing
 * a romanisation of somebody's name is not a thing to get wrong for
 * them. */
static char *derive_login(const char *full)
{
    GString *s = g_string_new(NULL);
    for (const char *p = full; *p && s->len < 32; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' && s->len)
            break;
        if (isalpha(c))
            g_string_append_c(s, (char)tolower(c));
        else if (isdigit(c) && s->len)
            g_string_append_c(s, (char)c);
    }
    return g_string_free(s, FALSE);
}

static void account_check(SuAccount *a)
{
    const char *login = gtk_editable_get_text(GTK_EDITABLE(a->login));
    const char *p1 = gtk_editable_get_text(GTK_EDITABLE(a->pw1));
    const char *p2 = gtk_editable_get_text(GTK_EDITABLE(a->pw2));
    const char *en = NULL, *ko = NULL;

    if (!*login) {
        en = "Choose a user name.";
        ko = "사용자 이름을 정하세요.";
    } else if (!login_ok(login)) {
        en = "A user name uses lower-case letters, digits, - and _, and starts with a letter.";
        ko = "사용자 이름은 영어 소문자, 숫자, - 와 _ 로 쓰고 영어 글자로 시작합니다.";
    } else if (!*p1) {
        en = "Choose a password. Every account on LP has one.";
        ko = "암호를 정하세요. LP 의 모든 계정에는 암호가 있습니다.";
    } else if (!*p2) {
        en = "Type the password again to confirm it.";
        ko = "확인을 위해 암호를 한 번 더 입력하세요.";
    } else if (strcmp(p1, p2) != 0) {
        en = "The two passwords are different.";
        ko = "두 암호가 서로 다릅니다.";
    }
    gtk_widget_set_sensitive(a->next, en == NULL);
    if (en) {
        su_retext(a->hint, en, ko);
        gtk_widget_remove_css_class(a->hint, "su-ok");
    } else {
        su_retext(a->hint, "Ready.", "준비됐습니다.");
        gtk_widget_add_css_class(a->hint, "su-ok");
    }
}

static void on_fullname(GtkEditable *e, gpointer data)
{
    SuAccount *a = data;
    if (!a->login_edited) {
        char *l = derive_login(gtk_editable_get_text(e));
        a->updating = TRUE;
        gtk_editable_set_text(GTK_EDITABLE(a->login), l);
        a->updating = FALSE;
        g_free(l);
    }
    account_check(a);
}

static void on_login(GtkEditable *e, gpointer data)
{
    SuAccount *a = data;
    (void)e;
    if (!a->updating)
        a->login_edited = TRUE;
    account_check(a);
}

static void on_pw(GtkEditable *e, gpointer data)
{
    (void)e;
    account_check(data);
}

static GtkWidget *entry(void)
{
    GtkWidget *e = gtk_entry_new();
    gtk_widget_add_css_class(e, "su-entry");
    gtk_entry_set_activates_default(GTK_ENTRY(e), FALSE);
    su_osk_attach(e);
    return e;
}

static GtkWidget *password(void)
{
    GtkWidget *e = gtk_password_entry_new();
    gtk_widget_add_css_class(e, "su-entry");
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(e), TRUE);
    su_osk_attach(e);
    return e;
}

void su_account_build(SuAccount *a, gboolean recovery_note)
{
    memset(a, 0, sizeof *a);
    su_page(&a->page, "Who will use this computer?", "이 컴퓨터를 누가 쓰나요?",
            "This account is the computer's administrator.",
            "이 계정이 컴퓨터의 관리자가 됩니다.");

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);

    a->fullname = entry();
    a->login = entry();
    a->pw1 = password();
    a->pw2 = password();
    gtk_widget_set_hexpand(a->fullname, TRUE);
    gtk_widget_set_hexpand(a->login, TRUE);

    gtk_grid_attach(GTK_GRID(grid), caption("Your name", "이름"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), caption("User name", "사용자 이름"), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), a->fullname, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), a->login, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), caption("Password", "암호"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), caption("Confirm password", "암호 확인"), 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), a->pw1, 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), a->pw2, 1, 3, 1, 1);
    gtk_box_append(GTK_BOX(a->page.body), grid);

    a->hint = su_label("", "", "su-hint");
    gtk_box_append(GTK_BOX(a->page.body), a->hint);

    GtkWidget *note = su_label(
        "You will use this password to sign in and to make changes to the "
        "system (sudo).",
        "로그인할 때와 시스템을 바꿀 때(sudo) 이 암호를 씁니다.", "su-note");
    gtk_box_append(GTK_BOX(a->page.body), note);
    if (recovery_note)
        gtk_box_append(GTK_BOX(a->page.body), su_label(
            "Your password also unlocks Recovery.",
            "이 암호로 복구 모드도 열립니다.", "su-note"));

    g_signal_connect(a->fullname, "changed", G_CALLBACK(on_fullname), a);
    g_signal_connect(a->login, "changed", G_CALLBACK(on_login), a);
    g_signal_connect(a->pw1, "changed", G_CALLBACK(on_pw), a);
    g_signal_connect(a->pw2, "changed", G_CALLBACK(on_pw), a);
    nav(&a->page, &a->back, &a->next);
    account_check(a);
}

const char *su_account_login(SuAccount *a)
{
    return gtk_editable_get_text(GTK_EDITABLE(a->login));
}

const char *su_account_fullname(SuAccount *a)
{
    return gtk_editable_get_text(GTK_EDITABLE(a->fullname));
}

const char *su_account_password(SuAccount *a)
{
    return gtk_editable_get_text(GTK_EDITABLE(a->pw1));
}

/* ── region ────────────────────────────────────────────────────────── */

static gboolean host_ok(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > 63 || s[0] == '-' || s[n - 1] == '-')
        return FALSE;
    for (size_t i = 0; i < n; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '-'))
            return FALSE;
    return TRUE;
}

static void region_check(SuRegion *r)
{
    const char *h = gtk_editable_get_text(GTK_EDITABLE(r->host));
    gboolean ok = host_ok(h) && su_region_timezone(r) != NULL;
    gtk_widget_set_sensitive(r->next, ok);
    if (ok) {
        su_retext(r->hint, "Ready.", "준비됐습니다.");
        gtk_widget_add_css_class(r->hint, "su-ok");
    } else {
        su_retext(r->hint,
                  "A computer name uses letters, digits and -, up to 63 of them.",
                  "컴퓨터 이름은 영어 글자, 숫자, - 로 63자까지 쓸 수 있습니다.");
        gtk_widget_remove_css_class(r->hint, "su-ok");
    }
}

static void on_host(GtkEditable *e, gpointer data)
{
    SuRegion *r = data;
    (void)e;
    if (!r->updating)
        r->host_edited = TRUE;
    region_check(r);
}

static void on_tz(GObject *o, GParamSpec *ps, gpointer data)
{
    SuRegion *r = data;
    (void)o; (void)ps;
    if (!r->updating)
        r->tz_touched = TRUE;
    region_check(r);
}

static int cmp_str(gconstpointer a, gconstpointer b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Every zone the base carries, from tzdata's own list of them. */
static GtkStringList *load_zones(void)
{
    GPtrArray *z = g_ptr_array_new_with_free_func(g_free);
    char *text = NULL;
    if (g_file_get_contents("/usr/share/zoneinfo/zone1970.tab", &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (char **l = lines; *l; l++) {
            if (**l == '#' || !**l)
                continue;
            char **f = g_strsplit(*l, "\t", 4);
            if (f[0] && f[1] && f[2])
                g_ptr_array_add(z, g_strdup(f[2]));
            g_strfreev(f);
        }
        g_strfreev(lines);
        g_free(text);
    }
    g_ptr_array_add(z, g_strdup("UTC"));
    g_ptr_array_sort(z, cmp_str);
    GtkStringList *sl = gtk_string_list_new(NULL);
    for (guint i = 0; i < z->len; i++)
        gtk_string_list_append(sl, g_ptr_array_index(z, i));
    g_ptr_array_free(z, TRUE);
    return sl;
}

/* Debian writes "Etc/UTC" to /etc/timezone, and zone1970.tab lists no
 * alias, so a name that is not in the list lands on UTC - never on
 * whatever happens to sort first (Africa/Abidjan). */
static void select_zone(SuRegion *r, const char *tz)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(r->zones));
    guint utc = 0;
    for (guint i = 0; i < n; i++) {
        const char *z = gtk_string_list_get_string(r->zones, i);
        if (!strcmp(z, "UTC"))
            utc = i;
        if (!strcmp(z, tz)) {
            r->updating = TRUE;
            gtk_drop_down_set_selected(GTK_DROP_DOWN(r->tz), i);
            r->updating = FALSE;
            return;
        }
    }
    r->updating = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(r->tz), utc);
    r->updating = FALSE;
}

void su_region_build(SuRegion *r)
{
    memset(r, 0, sizeof *r);
    su_page(&r->page, "Time zone and computer name", "시간대와 컴퓨터 이름",
            "The clock follows the time zone. Other computers on the network "
            "see this computer by its name.",
            "시계는 시간대를 따릅니다. 네트워크의 다른 컴퓨터는 이 이름으로 이 "
            "컴퓨터를 봅니다.");

    r->zones = load_zones();
    GtkExpression *ex = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string");
    r->tz = gtk_drop_down_new(G_LIST_MODEL(r->zones), ex);
    gtk_drop_down_set_enable_search(GTK_DROP_DOWN(r->tz), TRUE);
    gtk_widget_add_css_class(r->tz, "su-drop");
    r->host = entry();

    gtk_box_append(GTK_BOX(r->page.body), caption("Time zone", "시간대"));
    gtk_box_append(GTK_BOX(r->page.body), r->tz);
    gtk_box_append(GTK_BOX(r->page.body), caption("Computer name", "컴퓨터 이름"));
    gtk_box_append(GTK_BOX(r->page.body), r->host);
    r->hint = su_label("", "", "su-hint");
    gtk_box_append(GTK_BOX(r->page.body), r->hint);

    /* The zone this system is set to now (the image ships UTC), or
     * Seoul for a person who chose Korean. */
    char *cur = NULL;
    if (g_file_get_contents("/etc/timezone", &cur, NULL, NULL))
        g_strstrip(cur);
    select_zone(r, su_korean ? "Asia/Seoul" : (cur && *cur ? cur : "UTC"));
    g_free(cur);
    r->updating = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(r->host), "linux-lp");
    r->updating = FALSE;

    g_signal_connect(r->tz, "notify::selected", G_CALLBACK(on_tz), r);
    g_signal_connect(r->host, "changed", G_CALLBACK(on_host), r);
    nav(&r->page, &r->back, &r->next);
    region_check(r);
}

const char *su_region_timezone(SuRegion *r)
{
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(r->tz));
    if (i == GTK_INVALID_LIST_POSITION)
        return NULL;
    return gtk_string_list_get_string(r->zones, i);
}

const char *su_region_hostname(SuRegion *r)
{
    return gtk_editable_get_text(GTK_EDITABLE(r->host));
}

void su_region_suggest_host(SuRegion *r, const char *login)
{
    if (r->host_edited || !login || !*login)
        return;
    /* "alex-xps-15-9550": whose, and which machine - the name a
     * person would pick out of a list of devices on a network. */
    char *model = NULL;
    GString *s = g_string_new(login);
    if (g_file_get_contents("/sys/class/dmi/id/product_name", &model, NULL, NULL)) {
        g_strstrip(model);
        GString *m = g_string_new(NULL);
        for (const char *p = model; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (isalnum(c))
                g_string_append_c(m, (char)tolower(c));
            else if (m->len && m->str[m->len - 1] != '-')
                g_string_append_c(m, '-');
        }
        while (m->len && m->str[m->len - 1] == '-')
            g_string_truncate(m, m->len - 1);
        /* QEMU calls itself "Standard PC (Q35 + ICH9, 2009)", which is
         * no name for anything. */
        if (m->len && !g_str_has_prefix(m->str, "standard-pc"))
            g_string_append_printf(s, "-%s", m->str);
        else
            g_string_append(s, "-lp");
        g_string_free(m, TRUE);
    } else {
        g_string_append(s, "-lp");
    }
    g_free(model);
    if (s->len > 63)
        g_string_truncate(s, 63);
    while (s->len && s->str[s->len - 1] == '-')
        g_string_truncate(s, s->len - 1);
    r->updating = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(r->host), s->str);
    r->updating = FALSE;
    g_string_free(s, TRUE);
    region_check(r);
}

void su_region_language_changed(SuRegion *r)
{
    if (!r->tz_touched)
        select_zone(r, su_korean ? "Asia/Seoul" : "UTC");
}
