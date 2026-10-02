/*
 * apps.c - Apps: which app opens web links, mail, music, video, photos,
 * documents and text; what starts with the desktop; and what is
 * installed.
 *
 * ── Default apps are mimeapps.list ──
 *
 * The freedesktop answer to "what opens a PDF" is
 * ~/.config/mimeapps.list, [Default Applications] type=app.desktop, and
 * GIO (every GTK app, the file manager), xdg-open and xdg-mime all read
 * it. This panel writes that file directly, one key per type, atomically
 * and durably like every other file here - `xdg-mime default` would do
 * the same edit in place, and a power cut in the middle of that loses
 * every association at once. A category is several types (a browser is
 * http, https and text/html), all set together.
 *
 * The apps offered for a category are the ones whose .desktop file says
 * they can open its first type; the one shown is what GIO resolves now,
 * so a choice made with xdg-mime in a terminal shows here too.
 *
 * ── Startup apps are wayfire's [autostart] ──
 *
 * This session starts programs from wayfire.ini's [autostart] section,
 * not from XDG autostart files (nothing here runs those). Turning one off
 * takes its line out and keeps it in ~/.config/lp/autostart-off.conf, so
 * turning it on again puts back exactly the command that was there. The
 * desktop's own parts (the bar, the dock, the keyboard, the notification
 * daemon, our restore at login) are not listed: switching one of them off
 * from here would break the desktop this window is drawn on.
 */
#include "core.h"

#include <gio/gdesktopappinfo.h>
#include <string.h>

typedef struct { const char *en, *ko; const char *types[6]; } category_t;

static const category_t CATS[] = {
    { "Web browser", "웹 브라우저", { "x-scheme-handler/http", "x-scheme-handler/https", "text/html",
                                      "application/xhtml+xml", NULL } },
    { "Mail", "메일", { "x-scheme-handler/mailto", NULL } },
    { "Music", "음악", { "audio/mpeg", "audio/flac", "audio/x-vorbis+ogg", "audio/ogg", "audio/x-wav", NULL } },
    { "Video", "동영상", { "video/mp4", "video/x-matroska", "video/webm", "video/quicktime", NULL } },
    { "Photos", "사진", { "image/jpeg", "image/png", "image/webp", "image/gif", NULL } },
    { "Documents (PDF)", "문서 (PDF)", { "application/pdf", NULL } },
    { "Text", "텍스트", { "text/plain", NULL } },
    { "Files and folders", "파일과 폴더", { "inode/directory", NULL } },
};

static char *mimeapps(void)
{
    return g_build_filename(g_get_user_config_dir(), "mimeapps.list", NULL);
}

static void on_default(GObject *dd, GParamSpec *ps, gpointer p)
{
    (void)ps;
    const category_t *c = p;
    GPtrArray *ids = g_object_get_data(dd, "lp-ids");
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (!ids || i >= ids->len) return;
    const char *id = g_ptr_array_index(ids, i);
    char *path = mimeapps();
    gboolean ok = TRUE;
    for (int k = 0; c->types[k]; k++)
        ok &= ini_set(path, "Default Applications", c->types[k], id);
    g_free(path);
    GAppInfo *info = G_APP_INFO(g_desktop_app_info_new(id));
    if (ok)
        lp_toast(FALSE, T("%s opens with %s", "%s 은(는) 이제 %s 로 엽니다"), T(c->en, c->ko),
                 info ? g_app_info_get_display_name(info) : id);
    if (info) g_object_unref(info);
}

static GtkWidget *default_row(GtkWidget *list, const category_t *c)
{
    GList *all = g_app_info_get_all_for_type(c->types[0]);
    GAppInfo *cur = g_app_info_get_default_for_type(c->types[0], FALSE);
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    guint sel = 0;
    for (GList *l = all; l; l = l->next) {
        GAppInfo *a = l->data;
        const char *id = g_app_info_get_id(a);
        if (!id || !g_app_info_should_show(a)) continue;
        gboolean dup = FALSE;
        for (guint k = 0; k < ids->len; k++) dup |= !strcmp(g_ptr_array_index(ids, k), id);
        if (dup) continue;
        if (cur && !g_strcmp0(g_app_info_get_id(cur), id)) sel = ids->len;
        g_ptr_array_add(ids, g_strdup(id));
        g_ptr_array_add(names, g_strdup(g_app_info_get_display_name(a)));
    }
    g_list_free_full(all, g_object_unref);
    GtkWidget *r;
    if (!ids->len) {
        r = row_value(list, T(c->en, c->ko), NULL, T("No app installed", "설치된 앱 없음"));
        g_ptr_array_free(ids, TRUE);
    } else {
        g_ptr_array_add(names, NULL);
        r = row_choice(list, T(c->en, c->ko), NULL, (const char *const *)names->pdata, sel,
                       G_CALLBACK(on_default), (gpointer)c);
        g_object_set_data_full(G_OBJECT(row_control(r)), "lp-ids", ids, (GDestroyNotify)g_ptr_array_unref);
    }
    g_ptr_array_free(names, TRUE);
    if (cur) g_object_unref(cur);
    return r;
}

/* ── startup ────────────────────────────────────────────────────────── */

/* The desktop's own parts, by the program a line starts. */
static gboolean is_desktop_part(const char *key, const char *cmd)
{
    static const char *const parts[] = { "lp-panel", "lp-dock", "lp-osk", "lp-quick", "lp-bar",
                                         "lp-settings", "lp-idle", "lp-notify", "lp-desktop", "mako",
                                         "wlsunset", "swayidle", "lp-audio-start", "pipewire",
                                         "wireplumber", "lp-lock", NULL };
    if (!strcmp(key, "autostart_wf_shell")) return TRUE;
    char *first = g_strdup(cmd);
    char *sp = strpbrk(first, " \t");
    if (sp) *sp = '\0';
    char *base = g_path_get_basename(first);
    gboolean r = FALSE;
    for (int i = 0; parts[i] && !r; i++)
        r = g_str_has_prefix(base, parts[i]);
    g_free(base); g_free(first);
    return r;
}

static char *off_conf(void) { return lp_config_path("autostart-off.conf"); }

static void on_startup(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps;
    const char *key = p;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    char *ini = wayfire_ini(), *off = off_conf();
    gboolean ok;
    if (on) {
        char *cmd = kv_get(off, key);
        ok = cmd && ini_set(ini, "autostart", key, cmd);
        g_free(cmd);
        if (ok) {
            /* Leave the command in the record; an empty value marks it on. */
            kv_set(off, key, "");
        }
    } else {
        char *cmd = ini_get(ini, "autostart", key);
        /* The record first: if the power goes between the two writes,
         * the command is still somewhere. */
        ok = cmd && kv_set(off, key, cmd) && ini_unset(ini, "autostart", key);
        g_free(cmd);
    }
    if (ok)
        lp_toast(FALSE, on ? T("%s starts with the desktop", "%s 이(가) 데스크탑과 함께 시작합니다")
                           : T("%s no longer starts with the desktop", "%s 이(가) 이제 데스크탑과 함께 시작하지 않습니다"),
                 key);
    else
        LP_QUIET(gtk_switch_set_active(GTK_SWITCH(sw), !on));
    g_free(ini); g_free(off);
}

/* A new startup entry from an installed app: its Exec without the %f/%u
 * field codes, which have nothing to fill them at login. */
static void add_startup_ok(lp_dialog_t *d, gpointer p)
{
    (void)p;
    GtkWidget *dd = lp_dialog_get_data(d, "lp-dd");
    GPtrArray *ids = lp_dialog_get_data(d, "lp-ids");
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (i >= ids->len) return;
    GDesktopAppInfo *a = g_desktop_app_info_new(g_ptr_array_index(ids, i));
    if (!a) return;
    const char *exec = g_app_info_get_commandline(G_APP_INFO(a));
    GString *cmd = g_string_new(NULL);
    char **words = g_strsplit(exec ? exec : "", " ", -1);
    for (int k = 0; words[k]; k++) {
        if (words[k][0] == '%' && words[k][1]) continue;
        if (cmd->len) g_string_append_c(cmd, ' ');
        g_string_append(cmd, words[k]);
    }
    g_strfreev(words);
    char *key = g_strdup_printf("lp_%s", (const char *)g_ptr_array_index(ids, i));
    char *dot = strrchr(key, '.');
    if (dot && !strcmp(dot, ".desktop")) *dot = '\0';
    for (char *c = key; *c; c++) if (!g_ascii_isalnum(*c) && *c != '_') *c = '_';
    char *ini = wayfire_ini();
    if (ini_set(ini, "autostart", key, cmd->str))
        lp_toast(FALSE, T("%s starts with the desktop", "%s 이(가) 데스크탑과 함께 시작합니다"),
                 g_app_info_get_display_name(G_APP_INFO(a)));
    g_free(ini); g_free(key);
    g_string_free(cmd, TRUE);
    g_object_unref(a);
    lp_dialog_close(d);
    lp_refresh();
}

static gint by_name(gconstpointer a, gconstpointer b)
{
    return g_utf8_collate(g_app_info_get_display_name(G_APP_INFO(a)),
                          g_app_info_get_display_name(G_APP_INFO(b)));
}

static GList *visible_apps(void)
{
    GList *all = g_app_info_get_all(), *out = NULL;
    for (GList *l = all; l; l = l->next)
        if (g_app_info_should_show(l->data))
            out = g_list_prepend(out, g_object_ref(l->data));
    g_list_free_full(all, g_object_unref);
    return g_list_sort(out, by_name);
}

static void on_add_startup(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    GList *apps = visible_apps();
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *names = g_ptr_array_new();
    for (GList *l = apps; l; l = l->next) {
        g_ptr_array_add(ids, g_strdup(g_app_info_get_id(l->data)));
        g_ptr_array_add(names, (gpointer)g_app_info_get_display_name(l->data));
    }
    g_ptr_array_add(names, NULL);
    lp_dialog_t *d = lp_dialog_new(T("Start an app with the desktop", "데스크탑과 함께 시작할 앱"),
                                   T("Add", "더하기"), FALSE, add_startup_ok, NULL);
    GtkWidget *dd = gtk_drop_down_new_from_strings((const char *const *)names->pdata);
    g_object_set_data_full(G_OBJECT(dd), "lp-title", g_strdup(T("App", "앱")), g_free);
    gtk_drop_down_set_enable_search(GTK_DROP_DOWN(dd), TRUE);
    gtk_box_append(GTK_BOX(lp_dialog_body(d)), dd);
    lp_dialog_set_data(d, "lp-dd", dd, NULL);
    lp_dialog_set_data(d, "lp-ids", ids, (GDestroyNotify)g_ptr_array_unref);
    g_ptr_array_free(names, TRUE);
    g_list_free_full(apps, g_object_unref);
    lp_dialog_present(d);
}

static void on_software(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    static const char *const v[] = { "lp-software", NULL };
    lp_spawn_bg(v);
}

static GtkWidget *build(void)
{
    GList *apps = visible_apps();
    char *sub = g_strdup_printf(T("%u apps installed", "설치된 앱 %u개"), g_list_length(apps));
    GtkWidget *page = page_new(T("Apps", "앱"), sub);
    g_free(sub);

    GtkWidget *g = group_new(page, T("Default apps", "기본 앱"));
    for (guint i = 0; i < G_N_ELEMENTS(CATS); i++)
        default_row(g, &CATS[i]);

    GtkWidget *s = group_new(page, T("Start with the desktop", "데스크탑과 함께 시작"));
    char *ini = wayfire_ini(), *off = off_conf();
    char **keys = ini_keys(ini, "autostart");
    int shown = 0;
    for (int i = 0; keys[i]; i++) {
        char *cmd = ini_get(ini, "autostart", keys[i]);
        if (cmd && !is_desktop_part(keys[i], cmd)) {
            GtkWidget *r = row_switch(s, keys[i], cmd, TRUE, G_CALLBACK(on_startup), NULL);
            /* The key outlives the strv: it is the switch's data. */
            char *k = g_strdup(keys[i]);
            g_object_set_data_full(G_OBJECT(r), "lp-key", k, g_free);
            g_object_set_data(G_OBJECT(row_control(r)), "lp-cb-data", k);
            shown++;
        }
        g_free(cmd);
    }
    /* The ones switched off from here, so they can come back. */
    char **offk = NULL;
    {
        char *body = lp_slurp(off);
        char **l = g_strsplit(body ? body : "", "\n", -1);
        GPtrArray *a = g_ptr_array_new();
        for (int i = 0; l[i]; i++) {
            char *eq = strchr(l[i], '=');
            if (!eq || l[i][0] == '#' || !eq[1]) continue;
            g_ptr_array_add(a, g_strndup(l[i], eq - l[i]));
        }
        g_ptr_array_add(a, NULL);
        offk = (char **)g_ptr_array_free(a, FALSE);
        g_strfreev(l);
        g_free(body);
    }
    for (int i = 0; offk[i]; i++) {
        char *cmd = kv_get(off, offk[i]);
        char *k = g_strdup(offk[i]);
        GtkWidget *r = row_switch(s, k, cmd, FALSE, G_CALLBACK(on_startup), k);
        g_object_set_data_full(G_OBJECT(r), "lp-key", k, g_free);
        g_free(cmd);
        shown++;
    }
    g_strfreev(offk);
    g_strfreev(keys);
    g_free(ini); g_free(off);
    if (!shown)
        row_value(s, T("Nothing else starts with the desktop", "데스크탑과 함께 시작하는 앱이 없습니다"),
                  NULL, NULL);
    row_button(s, T("Add a startup app", "시작 앱 더하기"), NULL, T("Add…", "더하기…"),
               G_CALLBACK(on_add_startup), NULL);

    GtkWidget *ig = group_new(page, T("Installed", "설치된 앱"));
    row_button(ig, T("Install and remove apps", "앱 설치와 제거"), T("In the Software app", "소프트웨어 앱에서"),
               T("Open Software", "소프트웨어 열기"), G_CALLBACK(on_software), NULL);
    for (GList *l = apps; l; l = l->next) {
        GAppInfo *a = l->data;
        const char *desc = g_app_info_get_description(a);
        GtkWidget *r = row_value(ig, g_app_info_get_display_name(a), desc, NULL);
        GIcon *icon = g_app_info_get_icon(a);
        if (icon) {
            GtkWidget *img = gtk_image_new_from_gicon(icon);
            gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
            gtk_box_prepend(GTK_BOX(row_box(r)), img);
        }
    }
    g_list_free_full(apps, g_object_unref);
    return page;
}

static const char *const KEYS[] = {
    "Default apps", "기본 앱",
    "Web browser", "웹 브라우저",
    "Mail", "메일",
    "Music", "음악",
    "Video", "동영상",
    "Photos", "사진",
    "Documents (PDF)", "문서 (PDF)",
    "Add a startup app", "시작 앱 더하기",
    "Install and remove apps", "앱 설치와 제거",
    NULL
};

const lp_panel_t lp_panel_apps = {
    "apps", "Apps", "앱", "view-app-grid-symbolic", build, KEYS, NULL
};
