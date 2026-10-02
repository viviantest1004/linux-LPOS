/*
 * storage.c - Storage: every disk and partition, how full each one is,
 * the trash and the cache, and the way to the Disks app.
 *
 * The layout comes from `lsblk --json --bytes` (util-linux, in the base):
 * disks, their partitions, filesystem, label and where each is mounted.
 * How full a mounted one is comes from statvfs on its mount point, which
 * is the kernel's own count and the same number `df` prints. A partition
 * that is not mounted has no "free" to report, and says so.
 *
 * The partitions this OS is made of are named for what they are - the
 * disk layout contract fixes PARTLABEL=LP-ESP, LP-RECOVERY and LP-ROOT -
 * so the rows say "System", "Recovery", "Boot" rather than nvme0n1p3.
 *
 * Changing disks (formatting, partitioning, mounting) is the Disks app's
 * job (lp-disks, through lp-diskd); this panel only reports and opens it.
 * The two things it does itself are the person's own files: emptying the
 * trash and clearing ~/.cache, both after a confirmation that says how
 * much comes back.
 */
#include "core.h"

#include <glib/gstdio.h>
#include <sys/statvfs.h>
#include <string.h>

typedef struct {
    GtkWidget *page;
    GtkWidget *disks;        /* box the disk groups go into */
    GtkWidget *trash_row, *cache_row;
} stor_t;

static stor_t *ST;

static void stor_free(gpointer p)
{
    if (ST == p) ST = NULL;
    g_free(p);
}

/* ── sizes of the person's own folders ──────────────────────────────── */

static guint64 tree_size(const char *path, int depth)
{
    GStatBuf st;
    if (g_lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (guint64)st.st_blocks * 512;
    if (depth > 32) return 0;
    guint64 total = 0;
    GDir *d = g_dir_open(path, 0, NULL);
    if (!d) return 0;
    const char *n;
    while ((n = g_dir_read_name(d))) {
        char *p = g_build_filename(path, n, NULL);
        total += tree_size(p, depth + 1);
        g_free(p);
    }
    g_dir_close(d);
    return total;
}

static gboolean remove_tree(const char *path, gboolean keep_top)
{
    GStatBuf st;
    if (g_lstat(path, &st) != 0) return TRUE;
    gboolean ok = TRUE;
    if (S_ISDIR(st.st_mode)) {
        GDir *d = g_dir_open(path, 0, NULL);
        if (d) {
            const char *n;
            while ((n = g_dir_read_name(d))) {
                char *p = g_build_filename(path, n, NULL);
                ok &= remove_tree(p, FALSE);
                g_free(p);
            }
            g_dir_close(d);
        }
        if (!keep_top) ok &= g_rmdir(path) == 0;
    } else if (!keep_top) {
        ok &= g_unlink(path) == 0;
    }
    return ok;
}

static char *trash_dir(void) { return g_build_filename(g_get_user_data_dir(), "Trash", NULL); }

typedef struct { char *path; guint64 size; GtkWidget *row; } sized_t;

static void size_thread(GTask *t, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    sized_t *s = data;
    s->size = tree_size(s->path, 0);
    g_task_return_boolean(t, TRUE);
}

static void sized_free(gpointer p)
{
    sized_t *s = p;
    g_free(s->path);
    g_free(s);
}

static void size_done(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)src; (void)d;
    sized_t *s = g_task_get_task_data(G_TASK(res));
    GtkWidget *row = s->row;
    char *h = lp_human(s->size);
    row_set_detail(row, h);
    g_free(h);
    GtkWidget *b = row_control(row);
    if (b) gtk_widget_set_sensitive(b, s->size > 0);
    g_object_set_data(G_OBJECT(row), "lp-size", GSIZE_TO_POINTER((gsize)(s->size / 1024)));
}

/* Measured in a thread: a cache of a few hundred thousand files takes
 * seconds to walk. */
static void measure(GtkWidget *row, const char *path)
{
    sized_t *s = g_new0(sized_t, 1);
    s->path = g_strdup(path);
    s->row = row;
    row_set_detail(row, T("Measuring…", "재는 중…"));
    GTask *t = g_task_new(row, NULL, size_done, NULL);
    g_task_set_task_data(t, s, sized_free);
    g_task_run_in_thread(t, size_thread);
    g_object_unref(t);
}

static void empty_ok(lp_dialog_t *d, gpointer p)
{
    const char *which = p;
    gboolean trash = !strcmp(which, "trash");
    char *dir = trash ? trash_dir() : g_strdup(g_get_user_cache_dir());
    gboolean ok = TRUE;
    if (trash) {
        char *f = g_build_filename(dir, "files", NULL), *i = g_build_filename(dir, "info", NULL);
        ok = remove_tree(f, TRUE) & remove_tree(i, TRUE);
        g_free(f); g_free(i);
    } else {
        ok = remove_tree(dir, TRUE);
    }
    g_free(dir);
    lp_dialog_close(d);
    if (ok)
        lp_toast(FALSE, trash ? T("The trash is empty", "휴지통을 비웠습니다") : T("The cache is cleared", "캐시를 지웠습니다"));
    else
        lp_toast(TRUE, T("Some files could not be removed", "지우지 못한 파일이 있습니다"));
    if (ST) {
        char *t = trash_dir();
        measure(ST->trash_row, t);
        g_free(t);
        measure(ST->cache_row, g_get_user_cache_dir());
    }
}

static void on_empty(GtkButton *b, gpointer p)
{
    const char *which = p;
    gboolean trash = !strcmp(which, "trash");
    GtkWidget *row = g_object_get_data(G_OBJECT(b), "lp-row");
    char *h = lp_human((guint64)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "lp-size")) * 1024);
    char *t = g_strdup_printf(trash ? T("Empty the trash? (%s)", "휴지통을 비울까요? (%s)")
                                    : T("Clear the cache? (%s)", "캐시를 지울까요? (%s)"), h);
    lp_dialog_t *d = lp_dialog_new(t, trash ? T("Empty", "비우기") : T("Clear", "지우기"), trash,
                                   empty_ok, (gpointer)which);
    lp_dialog_text(d, trash ? T("Everything in the trash is deleted for good.",
                                "휴지통의 모든 것이 영구히 지워집니다.")
                            : T("Apps keep copies here to start faster; they make them again. "
                                "Nothing you made is in it.",
                                "앱이 빨리 뜨려고 만들어 두는 사본이고 다시 만들어집니다. "
                                "직접 만든 것은 들어 있지 않습니다."), NULL);
    lp_dialog_present(d);
    g_free(t); g_free(h);
}

/* ── disks ──────────────────────────────────────────────────────────── */

static const char *role(const char *partlabel, const char *mount)
{
    if (partlabel && !strcmp(partlabel, "LP-ROOT")) return T("System", "시스템");
    if (partlabel && !strcmp(partlabel, "LP-RECOVERY")) return T("Recovery", "복구");
    if (partlabel && !strcmp(partlabel, "LP-ESP")) return T("Boot", "부팅");
    if (mount && !strcmp(mount, "/")) return T("System", "시스템");
    if (mount && !strcmp(mount, "/boot/efi")) return T("Boot", "부팅");
    if (mount && !strcmp(mount, "[SWAP]")) return T("Swap", "스왑");
    return NULL;
}

static void part_row(GtkWidget *list, const jnode_t *p)
{
    const char *name = json_str(p, "name", "?");
    const char *fs = json_str(p, "fstype", NULL);
    const char *label = json_str(p, "label", NULL);
    const char *pl = json_str(p, "partlabel", NULL);
    const char *mnt = json_str(p, "mountpoint", NULL);
    double size = json_num(p, "size", 0);
    const char *r = role(pl, mnt);
    char *title = r ? g_strdup(r) : g_strdup(label ? label : pl ? pl : name);
    char *hs = lp_human((guint64)size);
    GString *detail = g_string_new(NULL);
    g_string_append_printf(detail, "%s · %s", name, hs);
    if (fs) g_string_append_printf(detail, " · %s", fs);
    if (mnt) g_string_append_printf(detail, " · %s", mnt);
    g_free(hs);

    struct statvfs sv;
    if (mnt && mnt[0] == '/' && statvfs(mnt, &sv) == 0 && sv.f_blocks) {
        guint64 total = (guint64)sv.f_blocks * sv.f_frsize;
        guint64 avail = (guint64)sv.f_bavail * sv.f_frsize;
        guint64 used = total - (guint64)sv.f_bfree * sv.f_frsize;
        GtkWidget *bar = gtk_level_bar_new_for_interval(0, 1);
        gtk_level_bar_set_value(GTK_LEVEL_BAR(bar), total ? (double)used / total : 0);
        gtk_widget_set_size_request(bar, 180, -1);
        /* Spec 2-2: under 15% free is a warning, in colour and in words. */
        gboolean low = total && avail * 100 / total < 15;
        if (low) gtk_widget_add_css_class(bar, "lp-full");
        GtkWidget *row = row_widget(list, title, detail->str, bar);
        char *fa = lp_human(avail);
        char *v = g_strdup_printf(low ? T("%s free · nearly full", "%s 남음 · 곧 가득 참") : T("%s free", "%s 남음"), fa);
        GtkWidget *l = gtk_label_new(v);
        gtk_widget_add_css_class(l, low ? "lp-warn" : "lp-value");
        gtk_box_append(GTK_BOX(row_box(row)), l);
        g_free(v); g_free(fa);
    } else {
        row_value(list, title, detail->str, mnt ? NULL : T("Not in use", "쓰지 않음"));
    }
    g_string_free(detail, TRUE);
    g_free(title);
}

static void on_lsblk(int st, const char *out, const char *err, gpointer p)
{
    stor_t *s = p;
    jnode_t *j = st == 0 ? json_parse(out) : NULL;
    const jnode_t *devs = json_get(j, "blockdevices");
    guint64 all_total = 0, all_free = 0;
    int ndisks = 0;
    for (const jnode_t *d = devs ? devs->child : NULL; d; d = d->next) {
        const char *type = json_str(d, "type", "");
        if (strcmp(type, "disk") != 0) continue;       /* loop, rom */
        /* zram is a "disk" to lsblk, but it is memory - the swap. */
        if (g_str_has_prefix(json_str(d, "name", ""), "zram")) continue;
        const char *model = json_str(d, "model", NULL);
        gboolean removable = json_bool(d, "rm", FALSE) || json_num(d, "rm", 0) > 0;
        char *hs = lp_human((guint64)json_num(d, "size", 0));
        char *m = g_strstrip(g_strdup(model ? model : json_str(d, "name", "?")));
        char *head = g_strdup_printf("%s%s · %s", m, removable ? T(" (removable)", " (이동식)") : "", hs);
        g_free(m);
        GtkWidget *label = gtk_label_new(head);
        gtk_widget_add_css_class(label, "lp-heading");
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_widget_set_margin_top(label, 22);
        gtk_widget_set_margin_bottom(label, 8);
        gtk_widget_set_margin_start(label, 4);
        gtk_box_append(GTK_BOX(s->disks), label);
        GtkWidget *list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
        gtk_widget_add_css_class(list, "lp-group");
        gtk_box_append(GTK_BOX(s->disks), list);
        g_free(head); g_free(hs);
        int parts = 0;
        for (const jnode_t *c = json_get(d, "children") ? json_get(d, "children")->child : NULL; c; c = c->next) {
            part_row(list, c);
            parts++;
        }
        if (!parts) part_row(list, d);
        ndisks++;
    }
    json_free(j);
    if (!ndisks) {
        GtkWidget *list = group_new(s->disks, NULL);
        char *why = st == -1 ? g_strdup(T("lsblk is not installed", "lsblk 이 설치되어 있지 않습니다"))
                             : lp_first_line(err, out);
        row_value(list, T("Could not read the disks", "디스크를 읽지 못했습니다"), why, NULL);
        g_free(why);
    }
    struct statvfs sv;
    if (statvfs("/", &sv) == 0) {
        all_total = (guint64)sv.f_blocks * sv.f_frsize;
        all_free = (guint64)sv.f_bavail * sv.f_frsize;
        char *a = lp_human(all_free), *b = lp_human(all_total);
        /* The two languages put the numbers in the opposite order. */
        char *sub = lp_korean() ? g_strdup_printf("시스템: %s 중 %s 남음", b, a)
                                : g_strdup_printf("System: %s free of %s", a, b);
        page_set_subtitle(s->page, sub);
        g_free(sub); g_free(a); g_free(b);
    }
}

/* The disks app: LP's own when there is one, else GNOME Disks, else
 * GParted - whichever the image carries. */
static const char *disks_app(void)
{
    if (lp_have("lp-disks")) return "lp-disks";
    if (lp_have("gnome-disks")) return "gnome-disks";
    if (lp_have("gparted")) return "gparted";
    return NULL;
}

static void on_disks(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    const char *app = disks_app();
    if (!app)
        return;
    const char *const v[] = { app, NULL };
    if (lp_spawn_bg(v))
        lp_toast(FALSE, T("Opening Disks", "디스크 앱을 엽니다"));
}

static void on_browse(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    static const char *const v[] = { "lp-files", "/", NULL };
    if (lp_spawn_bg(v))
        lp_toast(FALSE, T("Opening Files", "파일 앱을 엽니다"));
}

static GtkWidget *build(void)
{
    stor_t *s = g_new0(stor_t, 1);
    ST = s;
    s->page = page_new(T("Storage", "저장 공간"), T("Reading the disks…", "디스크를 읽는 중…"));
    g_object_set_data_full(G_OBJECT(s->page), "lp-storage", s, stor_free);

    GtkWidget *g = group_new(s->page, NULL);
    GtkWidget *disks = row_button(g, T("Disks", "디스크"),
                                  T("Format, partition, check and mount drives", "드라이브 포맷, 파티션, 검사, 마운트"),
                                  T("Open Disks", "디스크 열기"), G_CALLBACK(on_disks), NULL);
    if (!disks_app())
        row_set_detail(disks, T("The Disks app is not installed", "디스크 앱이 설치되어 있지 않습니다"));
    row_button(g, T("Browse", "둘러보기"),
               T("The system disk's folders in Files", "시스템 디스크의 폴더를 파일 앱에서"),
               T("Open Files", "파일 열기"), G_CALLBACK(on_browse), NULL);
    s->trash_row = row_button(g, T("Trash", "휴지통"), NULL, T("Empty…", "비우기…"),
                              G_CALLBACK(on_empty), (gpointer)"trash");
    s->cache_row = row_button(g, T("Cache", "캐시"), NULL, T("Clear…", "지우기…"),
                              G_CALLBACK(on_empty), (gpointer)"cache");
    char *t = trash_dir();
    measure(s->trash_row, t);
    g_free(t);
    measure(s->cache_row, g_get_user_cache_dir());

    s->disks = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(s->page), s->disks);
    /* util-linux's lsblk: /bin/lsblk, first on PATH, is LP's own and has
     * no --json - its table came back as "Could not read the disks". */
    const char *v[] = { lp_base_tool("lsblk"), "--json", "--bytes", "--output",
        "NAME,SIZE,TYPE,FSTYPE,LABEL,PARTLABEL,MOUNTPOINT,MODEL,RM", NULL };
    lp_run_async(v, NULL, s->page, on_lsblk, s);
    return s->page;
}

static const char *const KEYS[] = {
    "Disks", "디스크",
    "Trash", "휴지통",
    "Cache", "캐시",
    "System", "시스템",
    "Recovery", "복구",
    "Free space", "남은 공간",
    NULL
};

const lp_panel_t lp_panel_storage = {
    "storage", "Storage", "저장 공간", "drive-harddisk-symbolic", build, KEYS, NULL
};
