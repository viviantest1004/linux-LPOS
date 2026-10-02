/*
 * updates.c - Updates: whether there are any, when the list was last
 * fetched, and the way to install them.
 *
 * Installing is the Software app's (lp-software, which does it through
 * lp-privd with a progress bar and the log); this panel does not have a
 * second "update all" button that could run beside it. What it can say
 * without an administrator it says: how many packages apt already knows
 * newer versions of (`apt list --upgradable`, read from apt's cache, no
 * network), and how old that knowledge is - the time stamp of apt's own
 * package cache, which apt rewrites every time the lists are fetched. A
 * list from three weeks ago is why "no updates" can be wrong, and the
 * subtitle says so.
 */
#include "core.h"

#include <glib/gstdio.h>
#include <string.h>

static void on_open(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    static const char *const v[] = { "lp-software", NULL };
    if (lp_spawn_bg(v))
        lp_toast(FALSE, T("Opening Software", "소프트웨어를 엽니다"));
}

static char *age_words(gint64 secs)
{
    if (secs < 3600) return g_strdup(T("less than an hour ago", "한 시간 안"));
    if (secs < 86400) return g_strdup_printf(T("%d hours ago", "%d시간 전"), (int)(secs / 3600));
    return g_strdup_printf(T("%d days ago", "%d일 전"), (int)(secs / 86400));
}

static void on_list(int st, const char *out, const char *err, gpointer p)
{
    (void)err;
    GtkWidget *page = p;
    GtkWidget *count_row = g_object_get_data(G_OBJECT(page), "lp-count");
    GtkWidget *list = g_object_get_data(G_OBJECT(page), "lp-list");
    if (st != 0) {
        row_set_value(count_row, T("Unknown", "알 수 없음"));
        row_set_detail(count_row, st == -1 ? T("apt is not installed", "apt 가 설치되어 있지 않습니다")
                                           : T("apt could not read its lists", "apt 가 목록을 읽지 못했습니다"));
        page_set_subtitle(page, T("Could not tell whether there are updates",
                                  "업데이트가 있는지 알 수 없습니다"));
        return;
    }
    if (g_object_get_data(G_OBJECT(page), "lp-never")) {
        /* No lists fetched yet: apt knows of nothing newer because it has
         * never looked, which is not the same as up to date. */
        row_set_value(count_row, T("Not checked yet", "아직 확인 안 함"));
        page_set_subtitle(page, T("Updates have not been checked for yet - Software checks when it opens",
                                  "아직 업데이트를 확인하지 않았습니다 - 소프트웨어 앱이 열리면 확인합니다"));
        return;
    }
    char **l = g_strsplit(out, "\n", -1);
    int n = 0;
    for (int i = 0; l[i]; i++) {
        /* "name/suite version arch [upgradable from: old]" */
        if (!strstr(l[i], "upgradable from")) continue;
        n++;
        if (n > 12) continue;
        char *slash = strchr(l[i], '/');
        char *name = slash ? g_strndup(l[i], slash - l[i]) : g_strdup(l[i]);
        char **f = g_strsplit(l[i], " ", -1);
        const char *to = f[0] && f[1] ? f[1] : "";
        const char *from = strstr(l[i], "from: ");
        char *old = from ? g_strndup(from + 6, strcspn(from + 6, "]")) : g_strdup("");
        char *v = g_strdup_printf("%s → %s", old, to);
        row_value(list, name, NULL, v);
        g_free(v); g_free(old); g_free(name);
        g_strfreev(f);
    }
    g_strfreev(l);
    if (n > 12) {
        char *more = g_strdup_printf(T("and %d more", "외 %d개"), n - 12);
        row_value(list, more, NULL, NULL);
        g_free(more);
    }
    gtk_widget_set_visible(list, n > 0);
    GtkWidget *head = gtk_widget_get_prev_sibling(list);     /* group_new's heading */
    if (head && GTK_IS_LABEL(head)) gtk_widget_set_visible(head, n > 0);
    char *v = n ? g_strdup_printf(T("%d available", "%d개 있음"), n) : g_strdup(T("Up to date", "최신"));
    row_set_value(count_row, v);
    g_free(v);
    page_set_subtitle(page, n ? T("Updates are ready to install", "설치할 업데이트가 있습니다")
                              : T("This system is up to date, as of the last check",
                                  "마지막 확인 기준으로 최신입니다"));
}

static GtkWidget *build(void)
{
    GtkWidget *page = page_new(T("Updates", "업데이트"), T("Checking…", "확인하는 중…"));
    GtkWidget *g = group_new(page, NULL);
    GtkWidget *count = row_value(g, T("Updates", "업데이트"), NULL, "…");
    g_object_set_data(G_OBJECT(page), "lp-count", count);
    GStatBuf st;
    char *when;
    if (g_stat("/var/cache/apt/pkgcache.bin", &st) == 0)
        when = age_words(g_get_real_time() / G_USEC_PER_SEC - st.st_mtime);
    else {
        when = g_strdup(T("never", "한 번도 안 함"));
        g_object_set_data(G_OBJECT(page), "lp-never", GINT_TO_POINTER(1));
    }
    row_value(g, T("Last checked", "마지막 확인"), NULL, when);
    g_free(when);
    GtkWidget *b = row_button(g, T("Install updates", "업데이트 설치"),
                              T("In the Software app, which also checks for new ones",
                                "소프트웨어 앱에서 합니다. 새 업데이트도 거기서 확인합니다"),
                              T("Open Software", "소프트웨어 열기"), G_CALLBACK(on_open), NULL);
    if (!lp_have("lp-software"))
        row_set_detail(b, T("The Software app is not installed", "소프트웨어 앱이 설치되어 있지 않습니다"));
    GtkWidget *list = group_new(page, T("Waiting to be installed", "설치를 기다리는 것"));
    gtk_widget_set_visible(list, FALSE);
    gtk_widget_set_visible(gtk_widget_get_prev_sibling(list), FALSE);
    g_object_set_data(G_OBJECT(page), "lp-list", list);
    /* The base's apt: /bin/apt is LP's own and answers only root. */
    const char *v[] = { lp_base_tool("apt"), "list", "--upgradable", NULL };
    lp_run_async_timeout(v, NULL, 60 * 1000, page, on_list, page);
    return page;
}

static const char *const KEYS[] = {
    "Updates", "업데이트",
    "Install updates", "업데이트 설치",
    "Last checked", "마지막 확인",
    "Software", "소프트웨어",
    NULL
};

const lp_panel_t lp_panel_updates = {
    "updates", "Updates", "업데이트", "software-update-available-symbolic", build, KEYS, NULL
};
