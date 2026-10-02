/*
 * lp-apps.c - matching windows to applications. See lp-apps.h.
 *
 * Tried in order, first match wins:
 *   1. <app_id>.desktop, as given and in lower case
 *   2. a .desktop file whose StartupWMClass equals the app_id
 *   3. a .desktop file whose Exec program is named like the app_id,
 *      or like its last dotted component ("org.gnome.Calculator" and
 *      gnome-calculator do not match; "org.lpzero.Files" and lp-files
 *      do not either - that is what rule 2 is for)
 */
#include "lp-apps.h"

#include <gdk/gdk.h>
#include <gio/gdesktopappinfo.h>
#include <string.h>

static char *exec_base(GAppInfo *info)
{
    const char *ex = g_app_info_get_executable(info);
    return ex ? g_path_get_basename(ex) : NULL;
}

gboolean lp_app_owns(GDesktopAppInfo *info, const char *app_id)
{
    if (!info || !app_id || !*app_id)
        return FALSE;
    const char *id = g_app_info_get_id(G_APP_INFO(info));
    if (id) {
        size_t n = strlen(app_id);
        if (g_ascii_strncasecmp(id, app_id, n) == 0 &&
            strcmp(id + n, ".desktop") == 0)
            return TRUE;
    }
    const char *wm = g_desktop_app_info_get_startup_wm_class(info);
    if (wm && g_ascii_strcasecmp(wm, app_id) == 0)
        return TRUE;
    char *ex = exec_base(G_APP_INFO(info));
    gboolean ok = FALSE;
    if (ex) {
        const char *last = strrchr(app_id, '.');
        ok = g_ascii_strcasecmp(ex, app_id) == 0 ||
             (last && g_ascii_strcasecmp(ex, last + 1) == 0);
        g_free(ex);
    }
    return ok;
}

GDesktopAppInfo *lp_app_for_id(const char *app_id)
{
    if (!app_id || !*app_id)
        return NULL;
    char *name = g_strconcat(app_id, ".desktop", NULL);
    GDesktopAppInfo *d = g_desktop_app_info_new(name);
    g_free(name);
    if (d)
        return d;
    char *low = g_ascii_strdown(app_id, -1);
    name = g_strconcat(low, ".desktop", NULL);
    d = g_desktop_app_info_new(name);
    g_free(name);
    g_free(low);
    if (d)
        return d;

    GList *all = g_app_info_get_all();
    GDesktopAppInfo *found = NULL;
    for (GList *l = all; l && !found; l = l->next)
        if (G_IS_DESKTOP_APP_INFO(l->data) &&
            lp_app_owns(G_DESKTOP_APP_INFO(l->data), app_id))
            found = g_object_ref(l->data);
    g_list_free_full(all, g_object_unref);
    return found;
}

/* Terminal=true programs (htop, and the like) run inside the terminal -
 * Console (kgx), whose window has the same title bar as every other,
 * or foot where Console is missing. GLib looks for a terminal from its
 * own fixed list - gnome-terminal, xterm and so on - which neither is on,
 * and the program then started with nothing to show it in: pressing it
 * did nothing. */
static gboolean launch_in_terminal(GAppInfo *info)
{
    if (!G_IS_DESKTOP_APP_INFO(info) ||
        !g_desktop_app_info_get_boolean(G_DESKTOP_APP_INFO(info), "Terminal"))
        return FALSE;
    const char *cmd = g_app_info_get_commandline(info);
    if (!cmd || !*cmd)
        return FALSE;
    GString *c = g_string_new(NULL);
    for (const char *p = cmd; *p; p++) {
        if (*p == '%' && p[1]) {           /* a field code: %f %U ... */
            p++;
            if (*p == '%')
                g_string_append_c(c, '%');
            continue;
        }
        g_string_append_c(c, *p);
    }
    char *kgx = g_find_program_in_path("kgx");
    char *argv_kgx[] = { (char *)"kgx", (char *)"--", (char *)"sh", (char *)"-c",
                         c->str, NULL };
    char *argv_foot[] = { (char *)"foot", (char *)"-e", (char *)"sh", (char *)"-c",
                          c->str, NULL };
    char **argv = kgx ? argv_kgx : argv_foot;
    g_free(kgx);
    GError *err = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err)) {
        g_printerr("lp-shell: %s in %s: %s\n", g_app_info_get_id(info), argv[0], err->message);
        g_clear_error(&err);
    }
    g_string_free(c, TRUE);
    return TRUE;
}

void lp_app_launch(GAppInfo *info)
{
    if (launch_in_terminal(info))
        return;
    GdkAppLaunchContext *ctx =
        gdk_display_get_app_launch_context(gdk_display_get_default());
    GError *err = NULL;
    if (!g_app_info_launch(info, NULL, G_APP_LAUNCH_CONTEXT(ctx), &err)) {
        g_printerr("lp-shell: %s: %s\n", g_app_info_get_id(info),
                   err->message);
        g_clear_error(&err);
    }
    g_object_unref(ctx);
}

const char *lp_app_name(GAppInfo *info)
{
    const char *n = g_app_info_get_display_name(info);
    return n ? n : g_app_info_get_id(info);
}
