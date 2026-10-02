/*
 * notify.c - Notifications: do not disturb, and clearing what is on
 * screen.
 *
 * mako draws the notifications, and "do not disturb" is one of mako's
 * modes: with it in mako's list new notifications are held back, without
 * it they come through. What a mode does is in mako's config, which is the desktop-shell
 * track's (desktop/notify) - including the rule the spec asks for, that
 * critical notifications (battery nearly empty, disk full) come through
 * even so.
 *
 * mako forgets its modes when it restarts, so the choice is also kept in
 * ~/.config/lp/notify.conf (dnd=yes|no) and `lp-settings --restore` puts
 * it back at login. The switch shows what mako says when mako is running
 * - someone may have used makoctl from a terminal - and the file when it
 * is not.
 *
 * ── Why busctl and not makoctl ──
 *
 * The list is read and written over D-Bus, ListModes and SetModes on
 * mako's own interface - exactly the calls makoctl makes. makoctl's
 * `mode` pipes them through jq, which the base does not carry, so every
 * `makoctl mode` failed: the page said the notification service was not
 * running while mako was drawing on screen, and the switch changed
 * nothing. busctl is systemd's, in the base, and needs nothing else.
 */
#include "core.h"

#include <string.h>

static char *notify_conf(void) { return lp_config_path("notify.conf"); }

static gboolean dnd_saved(void)
{
    char *c = notify_conf();
    char *v = kv_get(c, "dnd");
    gboolean on = v && !strcmp(v, "yes");
    g_free(v); g_free(c);
    return on;
}

static void dnd_done(int st, const char *out, const char *err, gpointer p)
{
    gboolean on = GPOINTER_TO_INT(p);
    if (st == 0)
        lp_toast(FALSE, on ? T("Do not disturb is on", "방해 금지를 켰습니다")
                           : T("Do not disturb is off", "방해 금지를 껐습니다"));
    else {
        /* The file is written either way: at the next login mako gets it. */
        char *why = st == -1 ? g_strdup(T("busctl is not installed", "busctl 이 설치되어 있지 않습니다"))
                             : lp_first_line(err, out);
        lp_toast(TRUE, T("Saved, but the notification service did not take it now: %s",
                         "저장했지만 알림 서비스가 지금 받아들이지 않았습니다: %s"), why);
        g_free(why);
    }
}

#define MAKO "org.freedesktop.Notifications", "/fr/emersion/Mako", "fr.emersion.Mako"
#define DND  "do-not-disturb"
#define MAKO_MS (5 * 1000)

static const char *const LIST_MODES[] = { "busctl", "--user", "call", MAKO, "ListModes", NULL };

/* busctl's answer to ListModes: `as 2 "default" "do-not-disturb"`. */
static char **parse_modes(const char *out)
{
    int argc = 0;
    char **argv = NULL;
    if (!g_shell_parse_argv(out, &argc, &argv, NULL) || argc < 2 || strcmp(argv[0], "as")) {
        g_strfreev(argv);
        return NULL;
    }
    char **m = g_strdupv(argv + 2);
    g_strfreev(argv);
    return m;
}

typedef struct { GWeakRef owner; gboolean on, report; } dnd_t;

/* Step two: mako's list with do-not-disturb added or taken out, and
 * every other mode left as it was. */
static void on_modes_for_dnd(int st, const char *out, const char *err, gpointer p)
{
    dnd_t *d = p;
    GObject *owner = g_weak_ref_get(&d->owner);
    char **cur = st == 0 ? parse_modes(out) : NULL;
    if (!cur) {
        if (d->report && owner)
            dnd_done(st == 0 ? 1 : st, out, err, GINT_TO_POINTER(d->on));
    } else {
        GPtrArray *v = g_ptr_array_new();
        const char *head[] = { "busctl", "--user", "call", MAKO, "SetModes", "as", NULL };
        for (int i = 0; head[i]; i++) g_ptr_array_add(v, (gpointer)head[i]);
        GPtrArray *keep = g_ptr_array_new();
        for (int i = 0; cur[i]; i++)
            if (strcmp(cur[i], DND)) g_ptr_array_add(keep, cur[i]);
        if (d->on) g_ptr_array_add(keep, DND);
        char n[16];
        g_snprintf(n, sizeof n, "%u", keep->len);
        g_ptr_array_add(v, n);
        for (guint i = 0; i < keep->len; i++) g_ptr_array_add(v, g_ptr_array_index(keep, i));
        g_ptr_array_add(v, NULL);
        lp_run_async_timeout((const char *const *)v->pdata, NULL, MAKO_MS, GTK_WIDGET(owner),
                             d->report ? dnd_done : NULL, GINT_TO_POINTER(d->on));
        g_ptr_array_free(keep, TRUE);
        g_ptr_array_free(v, TRUE);
    }
    g_strfreev(cur);
    if (owner) g_object_unref(owner);
    g_weak_ref_clear(&d->owner);
    g_free(d);
}

static void apply_dnd(gboolean on, GtkWidget *owner, gboolean report)
{
    dnd_t *d = g_new0(dnd_t, 1);
    g_weak_ref_init(&d->owner, owner);
    d->on = on;
    d->report = report && owner;
    /* No owner here: the change goes to mako even if the page is gone
     * by the time the list comes back; only the banner needs the page. */
    lp_run_async_timeout(LIST_MODES, NULL, MAKO_MS, NULL, on_modes_for_dnd, d);
}

static void on_dnd(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    char *c = notify_conf();
    kv_set(c, "dnd", on ? "yes" : "no");
    g_free(c);
    apply_dnd(on, GTK_WIDGET(sw), TRUE);
    /* The subtitle says the state too; it was only written at build. */
    GtkWidget *page = GTK_WIDGET(sw);
    while (page && !g_object_get_data(G_OBJECT(page), "lp-dnd"))
        page = gtk_widget_get_parent(page);
    if (page)
        page_set_subtitle(page, on ? T("Do not disturb is on", "방해 금지가 켜져 있습니다")
                                   : T("Notifications appear at the top of the screen",
                                       "알림은 화면 위쪽에 나타납니다"));
}

static void on_clear(GtkButton *b, gpointer p)
{
    (void)p;
    static const char *const v[] = { "busctl", "--user", "call", MAKO, "DismissAllNotifications", NULL };
    char *err = NULL, *out = NULL;
    int st = lp_run_full(v, NULL, &out, &err);
    if (st == 0)
        lp_toast(FALSE, T("Cleared the notifications on screen", "화면의 알림을 모두 지웠습니다"));
    else {
        char *why = lp_first_line(err, out);
        lp_toast(TRUE, "%s", why);
        g_free(why);
    }
    (void)b;
    g_free(err); g_free(out);
}

static void on_modes(int st, const char *out, const char *err, gpointer p)
{
    (void)err;
    GtkWidget *page = p;
    GtkWidget *sw = g_object_get_data(G_OBJECT(page), "lp-dnd");
    GtkWidget *clear = g_object_get_data(G_OBJECT(page), "lp-clear");
    gboolean running = st == 0;
    gboolean on = running ? strstr(out, "do-not-disturb") != NULL : dnd_saved();
    LP_QUIET(gtk_switch_set_active(GTK_SWITCH(row_control(sw)), on));
    gtk_widget_set_sensitive(row_control(clear), running);
    page_set_subtitle(page, !running ? T("The notification service is not running; the choice "
                                         "below is kept for the next login",
                                         "알림 서비스가 돌고 있지 않습니다. 아래 선택은 다음 "
                                         "로그인 때 적용됩니다")
                          : on ? T("Do not disturb is on", "방해 금지가 켜져 있습니다")
                               : T("Notifications appear at the top of the screen",
                                   "알림은 화면 위쪽에 나타납니다"));
}

static GtkWidget *build(void)
{
    GtkWidget *page = page_new(T("Notifications", "알림"), NULL);
    GtkWidget *g = group_new(page, NULL);
    GtkWidget *sw = row_switch(g, T("Do not disturb", "방해 금지"),
                               T("New notifications are held back instead of shown",
                                 "새 알림이 뜨지 않고 쌓입니다"),
                               dnd_saved(), G_CALLBACK(on_dnd), NULL);
    g_object_set_data(G_OBJECT(page), "lp-dnd", sw);
    GtkWidget *clear = row_button(g, T("Notifications on screen", "화면의 알림"), NULL,
                                  T("Clear all", "모두 지우기"), G_CALLBACK(on_clear), NULL);
    g_object_set_data(G_OBJECT(page), "lp-clear", clear);
    page_note(page, T("Urgent notifications - the battery nearly empty, the disk full - always "
                      "come through, even with Do not disturb on.",
                      "배터리 부족, 디스크 부족 같은 긴급 알림은 방해 금지 중에도 항상 "
                      "나타납니다."));

    lp_run_async_timeout(LIST_MODES, NULL, MAKO_MS, page, on_modes, page);
    return page;
}

static void restore(void)
{
    if (dnd_saved())
        apply_dnd(TRUE, NULL, FALSE);
}

static const char *const KEYS[] = {
    "Do not disturb", "방해 금지",
    "Notifications on screen", "화면의 알림",
    NULL
};

const lp_panel_t lp_panel_notify = {
    "notify", "Notifications", "알림", "preferences-system-notifications-symbolic",
    build, KEYS, restore
};
