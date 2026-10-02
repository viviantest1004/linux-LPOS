/* files - the file manager.
 *
 * One directory at a time: a sidebar of places, a breadcrumb of the way
 * here, and a list of what is in front of you.
 *
 * Everything visible is a CSS class. The colours, the hairlines and the
 * shadow live in desktop/theme/gtk-4.0/gtk.css, so changing the look is
 * changing one stylesheet rather than rebuilding this program. Only the
 * numbers CSS cannot carry are set from here - the sidebar width, the
 * two fixed column widths, and the size the window opens at - and they
 * are named as constants so they are easy to find.
 *
 * Reading a directory is where this program gets lied to, and the three
 * lies are answered where they happen: a name that is not valid UTF-8, a
 * symlink whose target is gone, and a directory that changes underneath
 * the read. Each has a comment at the place the decision was made.
 *
 * GTK 4.8 is the floor. gtk_show_uri_full() and GtkMessageDialog are
 * current API there; both are deprecated from 4.10 onward, which is why
 * the Makefile does not turn warnings into errors.
 */
#include <gtk/gtk.h>
#include "lp-i18n.h"
#include "lp-fit.h"
#include <sys/statvfs.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#define APP_ID "org.lpzero.Files"

/* The mockup's numbers that CSS has no way to express. */
#define SIDEBAR_WIDTH   96
#define SIZE_COL_WIDTH  46
#define DATE_COL_WIDTH  52

/* Not from the mockup - it draws one window, not the size it opens at. */
#define WINDOW_WIDTH   900
#define WINDOW_HEIGHT  600

/* Below this much room left, the capacity bar turns amber. §1-3 gives
 * amber to limits and constraints, and a disk this full is one. */
#define LOW_SPACE_FRACTION 0.15

/* A directory that changed while we were looking at it is usually still
 * changing. Coalescing the monitor's events over this long turns a file
 * copy into one reload instead of one per block written. */
#define REFRESH_DEBOUNCE_MS 300

#define QUERY_ATTRS \
    G_FILE_ATTRIBUTE_STANDARD_NAME          "," \
    G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME  "," \
    G_FILE_ATTRIBUTE_STANDARD_TYPE          "," \
    G_FILE_ATTRIBUTE_STANDARD_SIZE          "," \
    G_FILE_ATTRIBUTE_ACCESS_CAN_READ        "," \
    G_FILE_ATTRIBUTE_TIME_MODIFIED

/* ------------------------------------------------------------------ */
/* One row                                                             */
/* ------------------------------------------------------------------ */

#define LPF_TYPE_ITEM (lpf_item_get_type())
G_DECLARE_FINAL_TYPE(LpfItem, lpf_item, LPF, ITEM, GObject)

struct _LpfItem {
    GObject   parent_instance;

    /* `name` is the bytes on disk and `display` is the text on screen,
     * and they are not always the same string. A filename is a byte
     * sequence, not text - it may be latin-1, or a fragment of a UTF-8
     * sequence that was cut in half by a broken copy. GIO's display-name
     * substitutes for whatever it cannot decode so the row is readable,
     * which means two different files can display identically. Anything
     * that touches the disk therefore uses `name`, never `display`. */
    char     *name;
    char     *display;

    char     *size_text;
    char     *date_text;
    char     *icon_name;
    char     *sort_key;
    goffset   size;
    /* 화면에 찍는 문자열(date_text)과 별개로 숫자를 들고 있다.
     * '어제' 와 '9월 4일' 을 문자열로 비교하면 정렬이 되지 않는다. */
    guint64   mtime;
    gboolean  is_dir;
    gboolean  readable;
    gboolean  dangling;   /* a symlink whose target is not there */
};

G_DEFINE_TYPE(LpfItem, lpf_item, G_TYPE_OBJECT)

static void lpf_item_finalize(GObject *object)
{
    LpfItem *self = LPF_ITEM(object);

    g_free(self->name);
    g_free(self->display);
    g_free(self->size_text);
    g_free(self->date_text);
    g_free(self->icon_name);
    g_free(self->sort_key);

    G_OBJECT_CLASS(lpf_item_parent_class)->finalize(object);
}

static void lpf_item_class_init(LpfItemClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = lpf_item_finalize;
}

static void lpf_item_init(LpfItem *self)
{
    (void)self;
}

/* ------------------------------------------------------------------ */
/* The window                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    GtkWidget *row;    /* the button, so its selected class can be set */
    char      *path;
} Place;

typedef struct {
    GtkWindow          *window;
    GtkWidget          *title;
    GtkWidget          *back;
    GtkWidget          *forward;
    GtkWidget          *crumbs;
    GtkWidget          *stack;
    GtkWidget          *search_bar;
    GtkWidget          *search_entry;
    GtkWidget          *status_left;
    GtkWidget          *status_right;
    GtkWidget          *capacity;
    GtkWidget          *capacity_label;

    GListStore         *store;
    GtkCustomFilter    *filter;
    GtkSelectionModel  *selection;

    /* Where we have been. `pos` is the current entry, so back and
     * forward are nothing more than moving it. */
    GPtrArray          *history;
    guint               pos;

    char               *path;
    GFileMonitor       *monitor;
    guint               refresh_id;
    gboolean            show_hidden;

    GPtrArray          *places;   /* Place*, for the selected row */

    /* ── 잘라내기·복사한 것 ──
     *
     * 이 창 안의 클립보드다. 시스템 클립보드에도 text/uri-list 로
     * 같이 실어 두므로 다른 앱에 붙여넣는 것도 되지만, 붙여넣기가
     * '옮기기' 인지 '복사' 인지는 이쪽에만 있다 - uri-list 에는 그것을
     * 적을 자리가 없고, 옮기기로 착각해 원본을 지우는 것보다는
     * 이 창을 벗어나면 복사가 되는 편이 안전하다. */
    GPtrArray          *clip;      /* GFile*, 참조를 들고 있다 */
    gboolean            clip_cut;

    GtkSortListModel   *sorted;
    GtkWidget          *list_view; /* GtkColumnView, 정렬을 물어보려고 */
    GtkWidget          *menu;      /* 우클릭 팝오버 */

    /* 파일을 옮기거나 지우는 동안 참이다. 그동안 같은 일을 다시
     * 시작하면 두 작업이 같은 파일을 두고 경쟁한다. */
    gboolean            busy;

    /* 하위 폴더까지 뒤지는 검색. 켜져 있으면 store 에 이 폴더가
     * 아니라 검색 결과가 들어간다. */
    gboolean            deep;
    GCancellable       *deep_cancel;
} App;

static void navigate(App *app, const char *path, gboolean push);
static void reload(App *app);
static void update_status(App *app);

/* ------------------------------------------------------------------ */
/* How the mockup writes numbers and dates                             */
/* ------------------------------------------------------------------ */

/* 4.2KB, 1.8MB, 35MB - one decimal below ten and none from ten up. Three
 * significant figures would give 4.19KB and 1.80MB; the mockup's rule is
 * shorter and it keeps the column from jittering as a copy grows. */
static char *format_size(goffset bytes)
{
    static const char *unit[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    double v = (double)bytes;
    int    u = 0;

    while (v >= 1024.0 && u < (int)G_N_ELEMENTS(unit) - 1) {
        v /= 1024.0;
        u++;
    }

    if (u == 0)
        return g_strdup_printf("%.0fB", v);
    if (v < 10.0)
        return g_strdup_printf("%.1f%s", v, unit[u]);
    return g_strdup_printf("%.0f%s", v, unit[u]);
}

/* "31GB 남음" for the capacity bar and the right of the status bar. */
static char *format_free(const char *path)
{
    struct statvfs vfs;

    if (statvfs(path, &vfs) != 0)
        return NULL;

    /* f_bavail, not f_bfree. ext4 holds a few percent back for root, and
     * the honest number is what an ordinary process may still use - the
     * same distinction df makes. */
    guint64 bytes = (guint64)vfs.f_bavail * (guint64)vfs.f_frsize;
    char   *size  = format_size((goffset)bytes);
    char   *text  = g_strdup_printf(T("%s free", "%s 남음"), size);

    g_free(size);
    return text;
}

/* How full the filesystem is, 0.0 to 1.0, for the capacity bar. */
static gboolean disk_usage(const char *path, double *used, double *free_part)
{
    struct statvfs vfs;

    if (statvfs(path, &vfs) != 0 || vfs.f_blocks == 0)
        return FALSE;

    double avail = (double)vfs.f_bavail / (double)vfs.f_blocks;

    /* The bar fills to what is gone from an ordinary process's point of
     * view, so the reserve reads as used. It is not available, and a bar
     * that says otherwise promises room that is not there. */
    *free_part = avail;
    *used      = 1.0 - avail;
    return TRUE;
}

/* Whole days between two moments, counted by calendar day and not by
 * dividing seconds - "어제" has to mean yesterday's date, not twenty-four
 * hours ago, or a file saved last night reads as today until noon. */
static int days_between(GDateTime *then, GDateTime *now)
{
    GDate a, b;

    g_date_set_dmy(&a, g_date_time_get_day_of_month(then),
                   g_date_time_get_month(then), g_date_time_get_year(then));
    g_date_set_dmy(&b, g_date_time_get_day_of_month(now),
                   g_date_time_get_month(now), g_date_time_get_year(now));

    return (int)g_date_days_between(&a, &b);
}

/* Today is a time, yesterday is 어제, this year is 9월 4일, and anything
 * older carries its year. The year form is written 2024. 9. 4. rather
 * than 2024년 9월 4일 because the column is 52px wide and the long form
 * does not fit in it. */
static char *format_date(guint64 seconds)
{
    GDateTime *when = g_date_time_new_from_unix_local((gint64)seconds);
    if (!when)
        return g_strdup("");

    GDateTime *now  = g_date_time_new_now_local();
    int        days = days_between(when, now);
    char      *text;

    /* days is negative for a file stamped in the future, which happens
     * with a wrong clock or a copy off a FAT drive in another timezone.
     * Such a file reads as today rather than as a date that has not
     * happened yet. */
    if (days <= 0)
        text = g_date_time_format(when, "%H:%M");
    else if (days == 1)
        text = g_strdup(T("yesterday", "어제"));
    else if (g_date_time_get_year(when) == g_date_time_get_year(now))
        /* "Sep 27" / "9월 27일". This was one printf format for both
         * languages, and the English one ("%d %b") handed the month's
         * number to %d and the day to %b, which printf does not have:
         * every date this year read "9 110". */
        text = lp_korean() ? g_strdup_printf("%d월 %d일", g_date_time_get_month(when),
                                             g_date_time_get_day_of_month(when))
                           : g_date_time_format(when, "%b %-d");
    else
        text = g_strdup_printf("%d. %d. %d.",
                               g_date_time_get_year(when),
                               g_date_time_get_month(when),
                               g_date_time_get_day_of_month(when));

    g_date_time_unref(now);
    g_date_time_unref(when);
    return text;
}

/* ------------------------------------------------------------------ */
/* Saying so                                                           */
/* ------------------------------------------------------------------ */

/* The name goes on its own line rather than into the sentence. Korean
 * would want 을 or 를 after it, and which one is right depends on the
 * last letter of the filename - which can be a digit, a bracket, or
 * Cyrillic. A sentence that cannot be built correctly is not built. */
static void say(App *app, const char *what, const char *name, const char *why)
{
    GtkWidget *dialog = gtk_message_dialog_new(app->window,
                                               GTK_DIALOG_MODAL |
                                               GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_OTHER,
                                               GTK_BUTTONS_NONE,
                                               "%s", what);

    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                             "%s\n\n%s", name, why);
    gtk_dialog_add_button(GTK_DIALOG(dialog), T("OK", "확인"), GTK_RESPONSE_CLOSE);

    g_signal_connect(dialog, "response",
                     G_CALLBACK(gtk_window_destroy), NULL);
    gtk_window_present(GTK_WINDOW(dialog));
}

/* ------------------------------------------------------------------ */
/* Reading a directory                                                 */
/* ------------------------------------------------------------------ */

/* 항목 24. The number has to agree with what you see after entering, so
 * it uses the same plain dotfile rule the listing uses rather than GIO's
 * is-hidden, which also honours .hidden files and backup names and would
 * quietly disagree with the rows.
 *
 * This costs one opendir per folder in the listing. It is readdir with
 * no stat per entry, so a folder of a thousand names is one page of I/O;
 * a directory holding hundreds of subdirectories is where it would show,
 * and that is the trade the mockup asked for by writing 항목 24. */
static gboolean count_entries(const char *path, gboolean show_hidden, guint *out)
{
    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir)
        return FALSE;   /* no permission: say nothing, rather than 0 */

    guint       n = 0;
    const char *name;

    while ((name = g_dir_read_name(dir)) != NULL)
        if (show_hidden || name[0] != '.')
            n++;

    g_dir_close(dir);
    *out = n;
    return TRUE;
}

/* Folders first, then by name. g_utf8_collate_key_for_filename is what
 * puts 2 before 10, sorts 사진 next to 사전 the way Korean expects, and
 * does not shove the dotfiles to the front when they are shown. */
static int compare_items(gconstpointer a, gconstpointer b)
{
    LpfItem *x = *(LpfItem *const *)a;
    LpfItem *y = *(LpfItem *const *)b;

    if (x->is_dir != y->is_dir)
        return x->is_dir ? -1 : 1;
    return strcmp(x->sort_key, y->sort_key);
}

static LpfItem *item_from_info(App *app, const char *dir_path, GFileInfo *info)
{
    LpfItem   *item = g_object_new(LPF_TYPE_ITEM, NULL);
    GFileType  type = g_file_info_get_file_type(info);

    item->name     = g_strdup(g_file_info_get_name(info));
    item->display  = g_strdup(g_file_info_get_display_name(info));
    item->sort_key = g_utf8_collate_key_for_filename(item->display, -1);
    item->readable = g_file_info_get_attribute_boolean(
                         info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ);

    /* We enumerate following symlinks, so a good link reports the type of
     * whatever it points at. A link still calling itself a link means the
     * stat of the target failed - the target is gone, or a loop, or on a
     * filesystem that is no longer mounted. It gets a row, because a
     * dangling link is a thing you want to see in order to delete it. */
    item->dangling = (type == G_FILE_TYPE_SYMBOLIC_LINK);
    item->is_dir   = (type == G_FILE_TYPE_DIRECTORY);

    if (item->is_dir) {
        char *child = g_build_filename(dir_path, item->name, NULL);
        guint n;

        item->icon_name = g_strdup("folder-symbolic");
        if (count_entries(child, app->show_hidden, &n))
            item->size_text = g_strdup_printf(T("%u items", "항목 %u"), n);
        else
            item->size_text = g_strdup("");
        g_free(child);
    } else if (item->dangling) {
        item->icon_name = g_strdup("dialog-question-symbolic");
        item->size_text = g_strdup("");
    } else {
        item->size      = g_file_info_get_size(info);
        item->icon_name = g_strdup("text-x-generic-symbolic");
        item->size_text = format_size(item->size);
    }

    /* A filesystem that carries no mtime, or an entry whose time could
     * not be read, would otherwise print 1970 with a straight face. */
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
        item->mtime = g_file_info_get_attribute_uint64(
            info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        item->date_text = format_date(item->mtime);
    } else {
        item->mtime     = 0;
        item->date_text = g_strdup("");
    }

    return item;
}

/* The names that were selected, so a reload does not lose the selection.
 * Keyed by the on-disk name: the row is a different object after the
 * reload, but it is the same file. */
static GHashTable *selected_names(App *app)
{
    GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
    GtkBitset  *bits  = gtk_selection_model_get_selection(app->selection);
    GtkBitsetIter iter;
    guint         pos;

    if (gtk_bitset_iter_init_first(&iter, bits, &pos)) {
        do {
            LpfItem *item = g_list_model_get_item(
                                G_LIST_MODEL(app->selection), pos);
            if (item) {
                g_hash_table_add(names, g_strdup(item->name));
                g_object_unref(item);
            }
        } while (gtk_bitset_iter_next(&iter, &pos));
    }

    gtk_bitset_unref(bits);
    return names;
}

static void restore_selection(App *app, GHashTable *names)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(app->selection));

    for (guint i = 0; i < n; i++) {
        LpfItem *item = g_list_model_get_item(G_LIST_MODEL(app->selection), i);

        if (item && g_hash_table_contains(names, item->name))
            gtk_selection_model_select_item(app->selection, i, FALSE);
        g_clear_object(&item);
    }
}

static gboolean refresh_now(gpointer data)
{
    App *app = data;

    app->refresh_id = 0;
    reload(app);
    return G_SOURCE_REMOVE;
}

static void schedule_refresh(App *app)
{
    if (app->refresh_id != 0)
        g_source_remove(app->refresh_id);
    app->refresh_id = g_timeout_add(REFRESH_DEBOUNCE_MS, refresh_now, app);
}

static void on_directory_changed(GFileMonitor *monitor, GFile *file,
                                 GFile *other, GFileMonitorEvent event,
                                 gpointer data)
{
    (void)monitor; (void)file; (void)other; (void)event;
    schedule_refresh(data);
}

/* Fills the list from `path`. Returns FALSE and sets `error` if the
 * directory could not be opened at all, in which case nothing on screen
 * has been touched yet and the caller can stay where it is. */
static gboolean fill_list(App *app, const char *path, GError **error)
{
    GFile           *dir = g_file_new_for_path(path);
    GFileEnumerator *e   = g_file_enumerate_children(dir, QUERY_ATTRS,
                                                     G_FILE_QUERY_INFO_NONE,
                                                     NULL, error);
    if (!e) {
        g_object_unref(dir);
        return FALSE;
    }

    GPtrArray *rows = g_ptr_array_new();
    gboolean   truncated = FALSE;

    for (;;) {
        GError    *one  = NULL;
        GFileInfo *info = g_file_enumerator_next_file(e, NULL, &one);

        if (!info) {
            /* A directory is not a snapshot. Between the enumerator
             * opening and the last entry arriving, names appear and
             * vanish, and one that vanished in that window comes back
             * here as an error instead of as a row. GIO does not promise
             * the enumeration can continue past it, so we keep what we
             * have and ask for another pass - the directory has just
             * proved it is changing, and one more read is cheaper than
             * showing half of it as if it were all of it. */
            if (one) {
                g_error_free(one);
                truncated = TRUE;
            }
            break;
        }

        const char *name = g_file_info_get_name(info);
        if (app->show_hidden || (name && name[0] != '.'))
            g_ptr_array_add(rows, item_from_info(app, path, info));

        g_object_unref(info);
    }

    g_object_unref(e);

    g_ptr_array_sort(rows, compare_items);

    g_list_store_remove_all(app->store);
    if (rows->len > 0)
        g_list_store_splice(app->store, 0, 0, rows->pdata, rows->len);

    /* The store took a reference of its own; ours goes back. */
    for (guint i = 0; i < rows->len; i++)
        g_object_unref(rows->pdata[i]);
    g_ptr_array_free(rows, TRUE);

    /* Watch this directory so someone else's copy, download or delete
     * shows up here without the user asking. */
    g_clear_object(&app->monitor);
    app->monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_NONE,
                                            NULL, NULL);
    if (app->monitor)
        g_signal_connect(app->monitor, "changed",
                         G_CALLBACK(on_directory_changed), app);

    g_object_unref(dir);

    if (truncated)
        schedule_refresh(app);
    return TRUE;
}

static void reload(App *app)
{
    if (!app->path)
        return;

    GHashTable *keep = selected_names(app);
    GError     *error = NULL;

    if (fill_list(app, app->path, &error)) {
        restore_selection(app, keep);
    } else {
        /* The directory we are standing in went away or closed to us
         * while we were in it. Emptying the list is the truth. */
        g_list_store_remove_all(app->store);
        g_clear_error(&error);
    }

    g_hash_table_unref(keep);
    update_status(app);
}

/* ------------------------------------------------------------------ */
/* The breadcrumb                                                      */
/* ------------------------------------------------------------------ */

static void on_crumb_clicked(GtkButton *button, gpointer data)
{
    App        *app  = data;
    const char *path = g_object_get_data(G_OBJECT(button), "path");

    if (g_strcmp0(path, app->path) != 0)
        navigate(app, path, TRUE);
}

static void crumb_append(App *app, const char *label, const char *path,
                         gboolean current)
{
    GtkWidget *button = gtk_button_new_with_label(label);

    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
    gtk_widget_add_css_class(button, current ? "lp-crumb-current" : "lp-crumb");
    g_object_set_data_full(G_OBJECT(button), "path", g_strdup(path), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(on_crumb_clicked), app);
    gtk_box_append(GTK_BOX(app->crumbs), button);
}

static void crumb_chevron(App *app)
{
    GtkWidget *chevron = gtk_image_new_from_icon_name("go-next-symbolic");

    gtk_widget_add_css_class(chevron, "lp-crumb-sep");
    gtk_box_append(GTK_BOX(app->crumbs), chevron);
}

/* The trail is built from the path itself, so it is always the way we
 * actually got here and not a story about it. Below the home directory
 * the first crumb is 홈 rather than /home/이름 - the sidebar calls it
 * 홈, and two names for one place is one too many. */
static void build_crumbs(App *app)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(app->crumbs)) != NULL)
        gtk_box_remove(GTK_BOX(app->crumbs), child);

    const char *home = g_get_home_dir();
    const char *rest = app->path;
    char       *base = NULL;

    if (home && g_str_has_prefix(app->path, home) &&
        (app->path[strlen(home)] == '\0' || app->path[strlen(home)] == '/')) {
        base = g_strdup(home);
        rest = app->path + strlen(home);
        crumb_append(app, T("Home", "홈"), home, rest[0] == '\0');
    } else {
        base = g_strdup("/");
        crumb_append(app, "/", "/", app->path[1] == '\0');
        rest = app->path;
    }

    char **parts = g_strsplit(rest, "/", -1);

    for (int i = 0; parts[i] != NULL; i++) {
        if (parts[i][0] == '\0')
            continue;

        char *here = g_build_filename(base, parts[i], NULL);
        crumb_chevron(app);
        crumb_append(app, parts[i], here, g_strcmp0(here, app->path) == 0);
        g_free(base);
        base = here;
    }

    g_strfreev(parts);
    g_free(base);
}

/* ------------------------------------------------------------------ */
/* Where we are                                                        */
/* ------------------------------------------------------------------ */

static void update_places(App *app)
{
    for (guint i = 0; i < app->places->len; i++) {
        Place *place = g_ptr_array_index(app->places, i);

        if (g_strcmp0(place->path, app->path) == 0)
            gtk_widget_add_css_class(place->row, "selected");
        else
            gtk_widget_remove_css_class(place->row, "selected");
    }
}

static void update_capacity(App *app)
{
    double used, free_part;

    /* The bar under 시스템 is the root filesystem, whatever directory the
     * list happens to be showing. That is what makes it a drive and not
     * a property of the folder. */
    if (!disk_usage("/", &used, &free_part)) {
        gtk_widget_set_visible(app->capacity, FALSE);
        gtk_widget_set_visible(app->capacity_label, FALSE);
        return;
    }

    gtk_widget_set_visible(app->capacity, TRUE);
    gtk_widget_set_visible(app->capacity_label, TRUE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->capacity), used);

    if (free_part < LOW_SPACE_FRACTION)
        gtk_widget_add_css_class(app->capacity, "warning");
    else
        gtk_widget_remove_css_class(app->capacity, "warning");

    char *text = format_free("/");
    gtk_label_set_text(GTK_LABEL(app->capacity_label), text ? text : "");
    g_free(text);
}

static void update_status(App *app)
{
    guint      total    = g_list_model_get_n_items(G_LIST_MODEL(app->selection));
    GtkBitset *bits     = gtk_selection_model_get_selection(app->selection);
    guint      chosen   = 0;
    goffset    bytes    = 0;
    GtkBitsetIter iter;
    guint         pos;

    if (gtk_bitset_iter_init_first(&iter, bits, &pos)) {
        do {
            LpfItem *item = g_list_model_get_item(
                                G_LIST_MODEL(app->selection), pos);
            if (item) {
                chosen++;
                /* A folder contributes nothing. Adding up what is inside
                 * it would mean walking the tree, and the number would
                 * change while it was being read. */
                if (!item->is_dir)
                    bytes += item->size;
                g_object_unref(item);
            }
        } while (gtk_bitset_iter_next(&iter, &pos));
    }
    gtk_bitset_unref(bits);

    /* With nothing selected the size is everything in the folder, which
     * is the question you are asking when you have not picked anything
     * yet. Selecting narrows both numbers to what you picked. */
    if (chosen == 0) {
        for (guint i = 0; i < total; i++) {
            LpfItem *item = g_list_model_get_item(
                                G_LIST_MODEL(app->selection), i);
            if (item) {
                if (!item->is_dir)
                    bytes += item->size;
                g_object_unref(item);
            }
        }
    }

    char *size = format_size(bytes);
    char *left;

    if (chosen > 0)
        left = g_strdup_printf(T("%u items · %u selected · %s", "항목 %u개 · %u개 선택 · %s"),
                               total, chosen, size);
    else
        left = g_strdup_printf(T("%u items · %s", "항목 %u개 · %s"), total, size);

    gtk_label_set_text(GTK_LABEL(app->status_left), left);
    g_free(left);
    g_free(size);

    char *right = app->path ? format_free(app->path) : NULL;
    gtk_label_set_text(GTK_LABEL(app->status_right), right ? right : "");
    g_free(right);
}

static void update_history_buttons(App *app)
{
    gtk_widget_set_sensitive(app->back, app->pos > 0);
    gtk_widget_set_sensitive(app->forward,
                             app->pos + 1 < app->history->len);
}

/* Moves to `path`. `push` is FALSE when back and forward are walking the
 * history, which is the whole difference between the two: they move the
 * cursor, everything else cuts the future off and writes a new entry. */
static void navigate(App *app, const char *path, gboolean push)
{
    char   *full  = g_canonicalize_filename(path, NULL);
    GError *error = NULL;

    /* Load before committing. If the directory will not open we have not
     * yet changed the title, the crumbs or the history, so refusing is
     * simply staying where we were. */
    char *previous = app->path;
    app->path = full;

    if (!fill_list(app, full, &error)) {
        char *name = g_path_get_basename(full);
        const char *why =
            g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)
                ? T("No permission to read it.", "읽기 권한이 없습니다.")
                : T("The folder could not be read.", "폴더를 읽지 못했습니다.");

        say(app, T("cannot be opened", "열 수 없습니다"), name, why);

        g_free(name);
        g_clear_error(&error);
        app->path = previous;
        g_free(full);
        return;
    }

    g_free(previous);

    if (push) {
        const char *here = app->history->len > 0
                               ? g_ptr_array_index(app->history, app->pos)
                               : NULL;

        /* Arriving where we already are changes nothing, and it must not
         * look like it does: clicking the sidebar row for the folder you
         * are standing in would otherwise throw the forward entries away
         * and grey out a chevron that was working a moment ago. */
        if (g_strcmp0(here, full) != 0) {
            /* Somewhere new from the middle of the history cuts the
             * future off, the way a browser does. */
            while (app->history->len > app->pos + 1)
                g_ptr_array_remove_index(app->history, app->history->len - 1);

            g_ptr_array_add(app->history, g_strdup(full));
            app->pos = app->history->len - 1;
        }
    }

    char *name = g_path_get_basename(full);
    gtk_label_set_text(GTK_LABEL(app->title), name);
    gtk_window_set_title(app->window, name);
    g_free(name);

    build_crumbs(app);
    update_places(app);
    update_capacity(app);
    update_history_buttons(app);
    update_status(app);
}

/* ------------------------------------------------------------------ */
/* Opening what is under the cursor                                    */
/* ------------------------------------------------------------------ */

/* Which file a launch was for. Two double-clicks in a row have two of
 * these in flight at once, so the name cannot live anywhere shared or
 * the second answer names the first file. */
typedef struct {
    App  *app;
    char *name;
} Launch;

static void on_launched(GObject *source, GAsyncResult *result, gpointer data)
{
    Launch *launch = data;
    GError *error  = NULL;

    (void)source;

    /* Without this the failure is a line in the journal and nothing on
     * screen: you double-click, and the machine does nothing at all. */
    if (!gtk_show_uri_full_finish(launch->app->window, result, &error)) {
        say(launch->app, T("cannot be opened", "열 수 없습니다"), launch->name,
            T("Nothing installed can open this file.", "이 파일을 열 수 있는 프로그램이 없습니다."));
        g_clear_error(&error);
    }

    g_free(launch->name);
    g_free(launch);
}

/* ── a program, opened by a tap ──────────────────────────────────────
 *
 * A file that is executable and is a program (an ELF binary, or a script
 * with a #! line) is not opened like a document: a tap asks first, and
 * says what it is. Running whatever was downloaded because someone
 * double-clicked it is how a desktop gets owned. "Run" starts it in a
 * terminal, so its output and its questions have somewhere to go;
 * "Open as text" shows a script's contents instead. */
static gboolean is_program(const char *path)
{
    if (!g_file_test(path, G_FILE_TEST_IS_EXECUTABLE) ||
        g_file_test(path, G_FILE_TEST_IS_DIR))
        return FALSE;
    char head[4] = { 0 };
    FILE *f = fopen(path, "rb");
    if (!f)
        return FALSE;
    size_t n = fread(head, 1, sizeof head, f);
    fclose(f);
    return (n >= 4 && !memcmp(head, "\177ELF", 4)) || (n >= 2 && head[0] == '#' && head[1] == '!');
}

typedef struct { App *app; char *path; } RunAsk;

static void on_run_response(GtkDialog *d, int response, gpointer data)
{
    RunAsk *r = data;
    if (response == 1) {
        char *dir = g_path_get_dirname(r->path);
        /* In Console, which keeps its window after the program ends only
         * if asked: the script's last words stay until Enter. */
        char *kgx = g_find_program_in_path("kgx");
        char *wd = g_strconcat("--working-directory=", dir, NULL);
        const char *argv_kgx[] = { "kgx", wd, "--", "sh", "-c",
            "\"$0\"; printf '\\n[%s]' \"$1\"; read x", r->path,
            T("Press Enter to close", "Enter 를 누르면 닫힙니다"), NULL };
        const char *argv_foot[] = { "foot", "--hold", "--working-directory", dir, r->path, NULL };
        const char **argv = kgx ? argv_kgx : argv_foot;
        g_free(kgx);
        GError *err = NULL;
        gboolean ok = g_spawn_async(dir, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err);
        g_free(wd);
        if (!ok) {
            say(r->app, T("could not be run", "실행하지 못했습니다"), r->path, err->message);
            g_clear_error(&err);
        }
        g_free(dir);
    } else if (response == 2) {
        const char *argv[] = { "gnome-text-editor", r->path, NULL };
        if (!g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL)) {
            const char *alt[] = { "gedit", r->path, NULL };
            g_spawn_async(NULL, (char **)alt, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        }
    }
    g_free(r->path);
    g_free(r);
    gtk_window_destroy(GTK_WINDOW(d));
}

static void ask_run(App *app, const char *path, const char *display)
{
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GtkWidget *d = gtk_message_dialog_new(app->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                          T("Run “%s”?", "“%s” 을(를) 실행할까요?"), display);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s",
        T("This file is a program. Run it only if you know where it came from.",
          "이 파일은 프로그램입니다. 어디서 온 것인지 알 때만 실행하십시오."));
    gtk_dialog_add_button(GTK_DIALOG(d), T("Cancel", "취소"), GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(d), T("Open as Text", "텍스트로 열기"), 2);
    gtk_dialog_add_button(GTK_DIALOG(d), T("Run", "실행"), 1);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    G_GNUC_END_IGNORE_DEPRECATIONS
    RunAsk *r = g_new0(RunAsk, 1);
    r->app = app;
    r->path = g_strdup(path);
    g_signal_connect(d, "response", G_CALLBACK(on_run_response), r);
    gtk_window_present(GTK_WINDOW(d));
}

static void open_item(App *app, LpfItem *item)
{
    char *path = g_build_filename(app->path, item->name, NULL);

    if (item->dangling) {
        say(app, T("cannot be opened", "열 수 없습니다"), item->display,
            T("The link points at nothing.", "링크가 가리키는 위치가 없습니다."));
    } else if (!item->readable) {
        /* Shown, not hidden. A file you may not read is still a fact
         * about the folder, and hiding it turns "no permission" into
         * "not there" - the second is a lie and it is the harder one to
         * debug. */
        say(app, T("cannot be opened", "열 수 없습니다"), item->display, T("No permission to read it.", "읽기 권한이 없습니다."));
    } else if (item->is_dir) {
        navigate(app, path, TRUE);
    } else if (is_program(path)) {
        ask_run(app, path, item->display);
    } else {
        GFile  *file   = g_file_new_for_path(path);
        char   *uri    = g_file_get_uri(file);
        Launch *launch = g_new0(Launch, 1);

        launch->app  = app;
        launch->name = g_strdup(item->display);

        gtk_show_uri_full(app->window, uri, GDK_CURRENT_TIME, NULL,
                          on_launched, launch);
        g_free(uri);
        g_object_unref(file);
    }

    g_free(path);
}

static void on_row_activated(GtkWidget *view, guint position, gpointer data)
{
    App     *app  = data;
    LpfItem *item = g_list_model_get_item(G_LIST_MODEL(app->selection),
                                          position);
    (void)view;

    if (item) {
        open_item(app, item);
        g_object_unref(item);
    }
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static void act_back(GSimpleAction *a, GVariant *p, gpointer data)
{
    App *app = data;
    (void)a; (void)p;

    if (app->pos > 0) {
        app->pos--;
        navigate(app, g_ptr_array_index(app->history, app->pos), FALSE);
    }
}

static void act_forward(GSimpleAction *a, GVariant *p, gpointer data)
{
    App *app = data;
    (void)a; (void)p;

    if (app->pos + 1 < app->history->len) {
        app->pos++;
        navigate(app, g_ptr_array_index(app->history, app->pos), FALSE);
    }
}

static void act_up(GSimpleAction *a, GVariant *p, gpointer data)
{
    App *app = data;
    (void)a; (void)p;

    if (g_strcmp0(app->path, "/") == 0)
        return;

    char *parent = g_path_get_dirname(app->path);
    navigate(app, parent, TRUE);
    g_free(parent);
}

static void act_home(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    navigate(data, g_get_home_dir(), TRUE);
}

static void act_refresh(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    reload(data);
}

static void act_show_hidden(GSimpleAction *action, GVariant *p, gpointer data)
{
    App     *app = data;
    GVariant *state = g_action_get_state(G_ACTION(action));
    gboolean  on = !g_variant_get_boolean(state);

    (void)p;
    g_variant_unref(state);

    g_simple_action_set_state(action, g_variant_new_boolean(on));
    app->show_hidden = on;
    reload(app);
}

/* ------------------------------------------------------------------ */
/* Search                                                              */
/* ------------------------------------------------------------------ */

static gboolean filter_match(gpointer object, gpointer data)
{
    LpfItem    *item  = object;
    App        *app   = data;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(app->search_entry));

    if (!query || query[0] == '\0')
        return TRUE;

    /* Casefold both sides so English matches regardless of case. Korean
     * has no case, so for 다운로드 this is a plain substring test. */
    char    *haystack = g_utf8_casefold(item->display, -1);
    char    *needle   = g_utf8_casefold(query, -1);
    gboolean hit      = (strstr(haystack, needle) != NULL);

    g_free(haystack);
    g_free(needle);
    return hit;
}

/* ── 하위 폴더까지 뒤지는 검색 ────────────────────────────────
 *
 * 평소의 검색은 지금 보고 있는 목록을 거르는 것뿐이라 즉시 끝난다.
 * 이쪽은 폴더를 걸어 내려가므로 시간이 든다.
 *
 * 두 가지 한도를 둔다: 항목 20000개와 깊이 12단계. 한도가 없으면
 * 홈에서 검색을 켜는 순간 창이 멈추고, 그 상태는 고장과 구별되지
 * 않는다. 한도에 걸리면 상태줄이 "여기까지" 라고 말한다 - 결과를
 * 전부 찾은 척하지 않는다. */
#define DEEP_MAX_ITEMS 20000
#define DEEP_MAX_DEPTH 12

static void deep_walk(App *app, const char *root, const char *rel,
                      const char *needle, GPtrArray *rows, int depth)
{
    if (depth > DEEP_MAX_DEPTH || rows->len >= DEEP_MAX_ITEMS)
        return;

    char  *here = rel[0] ? g_build_filename(root, rel, NULL) : g_strdup(root);
    GFile *dir  = g_file_new_for_path(here);
    GFileEnumerator *e = g_file_enumerate_children(
        dir, QUERY_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    g_object_unref(dir);

    if (!e) { g_free(here); return; }

    for (;;) {
        GFileInfo *info = g_file_enumerator_next_file(e, NULL, NULL);
        if (!info || rows->len >= DEEP_MAX_ITEMS) {
            if (info) g_object_unref(info);
            break;
        }

        const char *name = g_file_info_get_name(info);
        if (!name || (!app->show_hidden && name[0] == '.')) {
            g_object_unref(info);
            continue;
        }

        char *child_rel = rel[0] ? g_build_filename(rel, name, NULL)
                                 : g_strdup(name);

        char *folded = g_utf8_casefold(g_file_info_get_display_name(info), -1);
        if (strstr(folded, needle)) {
            LpfItem *item = item_from_info(app, here, info);
            /* name 은 지금 폴더에서의 상대 경로가 된다. open_item 이
             * app->path 와 이어 붙이므로 그대로 열린다. */
            g_free(item->name);
            item->name = g_strdup(child_rel);
            /* 어디서 찾았는지 보이지 않으면 같은 이름 열 개가 구별되지
             * 않는다. */
            if (rel[0]) {
                char *shown = g_strdup_printf("%s  —  %s", item->display, rel);
                g_free(item->display);
                item->display = shown;
            }
            g_ptr_array_add(rows, item);
        }
        g_free(folded);

        if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
            deep_walk(app, root, child_rel, needle, rows, depth + 1);

        g_free(child_rel);
        g_object_unref(info);
    }

    g_object_unref(e);
    g_free(here);
}

static void deep_search(App *app, const char *text)
{
    char *needle = g_utf8_casefold(text, -1);
    GPtrArray *rows = g_ptr_array_new();

    deep_walk(app, app->path, "", needle, rows, 0);
    g_free(needle);

    g_list_store_remove_all(app->store);
    g_list_store_splice(app->store, 0, 0, rows->pdata, rows->len);
    for (guint i = 0; i < rows->len; i++)
        g_object_unref(g_ptr_array_index(rows, i));

    char *msg = rows->len >= DEEP_MAX_ITEMS
        ? g_strdup_printf(T("First %u matches", "먼저 찾은 %u개"), rows->len)
        : g_strdup_printf(T("%u found under here", "이 아래에서 %u개"),
                          rows->len);
    gtk_label_set_text(GTK_LABEL(app->status_left), msg);
    g_free(msg);
    g_ptr_array_free(rows, TRUE);
}

static void on_search_changed(GtkEditable *entry, gpointer data)
{
    App *app = data;
    const char *text = gtk_editable_get_text(entry);

    if (app->deep && text && *text) {
        deep_search(app, text);
        /* 결과는 이미 걸러진 것이므로 필터는 통과시켜야 한다. */
        gtk_filter_changed(GTK_FILTER(app->filter), GTK_FILTER_CHANGE_LESS_STRICT);
        return;
    }

    if (app->deep)
        reload(app);      /* 검색어를 지웠으면 원래 목록으로 */

    gtk_filter_changed(GTK_FILTER(app->filter), GTK_FILTER_CHANGE_DIFFERENT);
    update_status(app);
}

static void act_deep_search(GSimpleAction *action, GVariant *value,
                            gpointer data)
{
    App *app = data;
    app->deep = g_variant_get_boolean(value);
    g_simple_action_set_state(action, value);

    const char *text = gtk_editable_get_text(GTK_EDITABLE(app->search_entry));
    if (app->deep && text && *text)
        deep_search(app, text);
    else
        reload(app);
}

/* 선택한 것을 연다. 더블클릭과 같은 일이고, 우클릭 메뉴의 첫 줄이
 * 그것이어야 해서 행동으로도 만들어 둔다.
 *
 * first_selected 는 파일을 다루는 절에 있고 그쪽이 아래에 있어서,
 * 여기서는 앞선 선언만 두고 정의는 그 옆에 둔다. */
static LpfItem *first_selected(App *app);

static void act_open(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    App *app = data;
    LpfItem *item = first_selected(app);
    if (!item) return;
    open_item(app, item);
    g_object_unref(item);
}

/* ------------------------------------------------------------------ */
/* Cells                                                               */
/* ------------------------------------------------------------------ */

static void name_setup(GtkSignalListItemFactory *f, GtkListItem *cell,
                       gpointer data)
{
    GtkWidget *box   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *icon  = gtk_image_new();
    GtkWidget *label = gtk_label_new(NULL);

    (void)f; (void)data;

    /* Spacing is left at 0 on purpose: GtkBox spacing is a property and
     * the theme cannot reach it, so the 9px gap is a margin in the
     * stylesheet instead, where the rest of the numbers are. */
    gtk_widget_add_css_class(box, "lp-cell");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    /* Middle, not end: two files often differ only in their extension or
     * their last few characters, and cutting the tail off hides exactly
     * the part that tells them apart. */
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(label, TRUE);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_item_set_child(cell, box);
}

static void name_bind(GtkSignalListItemFactory *f, GtkListItem *cell,
                      gpointer data)
{
    LpfItem   *item  = gtk_list_item_get_item(cell);
    GtkWidget *box   = gtk_list_item_get_child(cell);
    GtkWidget *icon  = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_last_child(box);

    (void)f; (void)data;

    gtk_image_set_from_icon_name(GTK_IMAGE(icon), item->icon_name);
    gtk_label_set_text(GTK_LABEL(label), item->display);
    g_object_set_data(G_OBJECT(box), "lpf-item", item);

    /* Row widgets are recycled as the list scrolls, so a class left by
     * whichever row this widget showed last is still on it. Take both
     * off before putting one on. */
    gtk_widget_remove_css_class(icon, "lp-icon-folder");
    gtk_widget_remove_css_class(icon, "lp-icon-file");
    gtk_widget_remove_css_class(label, "lp-name");
    gtk_widget_remove_css_class(label, "lp-name-file");

    gtk_widget_add_css_class(icon, item->is_dir ? "lp-icon-folder"
                                                : "lp-icon-file");
    gtk_widget_add_css_class(label, item->is_dir ? "lp-name" : "lp-name-file");
}

static void text_setup(GtkSignalListItemFactory *f, GtkListItem *cell,
                       gpointer data)
{
    GtkWidget *label = gtk_label_new(NULL);

    (void)f;

    gtk_label_set_xalign(GTK_LABEL(label), 1.0);
    gtk_widget_add_css_class(label, (const char *)data);
    gtk_list_item_set_child(cell, label);
}

static void size_bind(GtkSignalListItemFactory *f, GtkListItem *cell,
                      gpointer data)
{
    (void)f; (void)data;
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(cell)),
                       LPF_ITEM(gtk_list_item_get_item(cell))->size_text);
    g_object_set_data(G_OBJECT(gtk_list_item_get_child(cell)), "lpf-item",
                      gtk_list_item_get_item(cell));
}

static void date_bind(GtkSignalListItemFactory *f, GtkListItem *cell,
                      gpointer data)
{
    (void)f; (void)data;
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(cell)),
                       LPF_ITEM(gtk_list_item_get_item(cell))->date_text);
    g_object_set_data(G_OBJECT(gtk_list_item_get_child(cell)), "lpf-item",
                      gtk_list_item_get_item(cell));
}

static void tile_setup(GtkSignalListItemFactory *f, GtkListItem *cell,
                       gpointer data)
{
    GtkWidget *box   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *icon  = gtk_image_new();
    GtkWidget *label = gtk_label_new(NULL);

    (void)f; (void)data;

    gtk_widget_add_css_class(box, "lp-tile");
    gtk_widget_add_css_class(icon, "lp-tile-icon");
    gtk_widget_add_css_class(label, "lp-tile-name");
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_item_set_child(cell, box);
}

static void tile_bind(GtkSignalListItemFactory *f, GtkListItem *cell,
                      gpointer data)
{
    LpfItem   *item  = gtk_list_item_get_item(cell);
    GtkWidget *box   = gtk_list_item_get_child(cell);
    GtkWidget *icon  = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_last_child(box);

    (void)f; (void)data;

    gtk_image_set_from_icon_name(GTK_IMAGE(icon), item->icon_name);
    gtk_label_set_text(GTK_LABEL(label), item->display);
    g_object_set_data(G_OBJECT(box), "lpf-item", item);
}

/* ------------------------------------------------------------------ */
/* The sidebar                                                         */
/* ------------------------------------------------------------------ */

static void on_place_clicked(GtkButton *button, gpointer data)
{
    navigate(data, g_object_get_data(G_OBJECT(button), "path"), TRUE);
}

static GtkWidget *sidebar_heading(const char *text)
{
    GtkWidget *label = gtk_label_new(text);

    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_add_css_class(label, "lp-group");
    return label;
}

/* A row that leads somewhere. `icon_name` may be NULL, in which case the
 * row gets the tag dot instead - same row, different mark. */
static GtkWidget *sidebar_row(App *app, const char *label_text,
                              const char *icon_name, const char *path)
{
    GtkWidget *button = gtk_button_new();
    GtkWidget *box    = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *label  = gtk_label_new(label_text);
    GtkWidget *mark;

    if (icon_name) {
        mark = gtk_image_new_from_icon_name(icon_name);
        gtk_widget_add_css_class(mark, "lp-place-icon");
    } else {
        /* An empty box is the dot: the theme gives it 7px of min width
         * and height, a radius and a colour. Nothing to draw from here. */
        mark = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(mark, "lp-tag-dot");
        gtk_widget_set_valign(mark, GTK_ALIGN_CENTER);
    }

    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_add_css_class(label, "lp-place-label");

    gtk_box_append(GTK_BOX(box), mark);
    gtk_box_append(GTK_BOX(box), label);

    gtk_button_set_child(GTK_BUTTON(button), box);
    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
    gtk_widget_add_css_class(button, "lp-place");
    g_object_set_data_full(G_OBJECT(button), "path", g_strdup(path), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(on_place_clicked), app);

    Place *place = g_new0(Place, 1);
    place->row  = button;
    place->path = g_strdup(path);
    g_ptr_array_add(app->places, place);

    return button;
}

/* The place the sidebar names has to exist, or the row is a promise the
 * machine does not keep. XDG's own directory is used when the user has
 * one; otherwise the folder is created under the Korean name the sidebar
 * shows, so the label and the path say the same thing. */
/* Where one of the standard folders actually is.
 *
 * The name on disk and the name on the screen are two different things
 * and this used to conflate them: the label was passed in and used as
 * the directory name, so the sidebar's "다운로드" was also the folder's
 * name. That worked while the machine spoke one language. It stops
 * working the moment it speaks two - the English session would make and
 * use ~/Downloads while the Korean one made and used ~/다운로드, and the
 * same person's files would be in whichever folder matched the language
 * they were in when they saved.
 *
 * So the folder is named in English on disk, always, and what the
 * sidebar prints is the caller's business. `fallback` is that on-disk
 * name and is deliberately not translated.
 *
 * g_get_user_special_dir reads ~/.config/user-dirs.dirs, which the image
 * writes, so this normally answers from there and the fallback is only
 * for a home directory nobody set up. */
static char *place_dir(GUserDirectory which, const char *fallback)
{
    const char *xdg = g_get_user_special_dir(which);

    if (xdg && g_file_test(xdg, G_FILE_TEST_IS_DIR))
        return g_strdup(xdg);

    char *path = g_build_filename(g_get_home_dir(), fallback, NULL);
    g_mkdir_with_parents(path, 0755);
    return path;
}

/* The freedesktop trash, so that anything else on this system which
 * throws a file away puts it where this row looks for it. */
static char *trash_dir(void)
{
    char *path = g_build_filename(g_get_user_data_dir(), "Trash", "files", NULL);

    g_mkdir_with_parents(path, 0755);
    return path;
}

/* Drives the system mounted, not drives it might have. README's
 * automount puts USB storage under /media, so that is where we look; an
 * empty /media means no USB row, which is the honest answer. */
/* The block device mounted at `point`, from the kernel's own table. */
static char *device_at(const char *point)
{
    char *text = NULL, *dev = NULL;
    if (!g_file_get_contents("/proc/self/mounts", &text, NULL, NULL))
        return NULL;
    char **lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i] && !dev; i++) {
        char **f = g_strsplit(lines[i], " ", 3);
        if (f[0] && f[1] && !strcmp(f[1], point) && g_str_has_prefix(f[0], "/dev/"))
            dev = g_strdup(f[0]);
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(text);
    return dev;
}

/* Eject: unmount the drive through udisks, which asks polkit - and the
 * image's polkit rule (50-lp-removable.pkla) lets the person at the
 * desk (group plugdev) do it without a password, the way every desktop
 * does. Then the sidebar is rebuilt without it, and if the window was
 * looking inside it, it goes home. */
static void on_eject(GtkButton *b, gpointer data)
{
    App *app = data;
    const char *point = g_object_get_data(G_OBJECT(b), "lp-point");
    char *dev = point ? device_at(point) : NULL;
    if (!dev) {
        say(app, T("could not be ejected", "꺼내지 못했습니다"), point ? point : "?",
            T("It is not mounted any more.", "이미 마운트되어 있지 않습니다."));
        return;
    }
    const char *argv[] = { "udisksctl", "unmount", "--no-user-interaction", "-b", dev, NULL };
    char *err = NULL;
    int st = 1;
    g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                 NULL, NULL, NULL, &err, &st, NULL);
    if (!g_spawn_check_wait_status(st, NULL)) {
        say(app, T("could not be ejected", "꺼내지 못했습니다"), point,
            err && *err ? err : T("Something on it is still in use.", "안의 무언가가 아직 쓰이고 있습니다."));
    } else {
        if (app->path && g_str_has_prefix(app->path, point))
            navigate(app, g_get_home_dir(), TRUE);
        gtk_widget_set_visible(gtk_widget_get_parent(GTK_WIDGET(b)), FALSE);
        update_places(app);
    }
    g_free(err);
    g_free(dev);
}

static void add_usb_rows(App *app, GtkWidget *box);

static gboolean refill_usb(gpointer data)
{
    GtkWidget *usb = data;
    GFileMonitor *mm = g_object_get_data(G_OBJECT(usb), "lp-media-monitor");
    App *app = mm ? g_object_get_data(G_OBJECT(mm), "lp-app") : NULL;
    g_object_set_data(G_OBJECT(usb), "lp-refill", NULL);
    /* The window closed in the half second since the mount: the box is
     * held (below) but no longer in a window, and the App is gone. */
    if (!app || !gtk_widget_get_root(usb))
        return G_SOURCE_REMOVE;
    /* The rows about to go are also in app->places, which marks the
     * current place on every move. Left there, the next move set a class
     * on a freed button - a stick ejected or plugged in, then any click
     * in the sidebar, and Files could close on the spot. */
    for (guint i = app->places->len; i-- > 0;) {
        Place *p = g_ptr_array_index(app->places, i);
        if (gtk_widget_is_ancestor(p->row, usb))
            g_ptr_array_remove_index(app->places, i);
    }
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(usb)))
        gtk_box_remove(GTK_BOX(usb), c);
    add_usb_rows(app, usb);
    update_places(app);
    return G_SOURCE_REMOVE;
}

static void on_media_changed(GFileMonitor *m, GFile *f, GFile *o,
                             GFileMonitorEvent ev, gpointer data)
{
    (void)m; (void)f; (void)o; (void)ev;
    /* A mount is a burst of events; one refill a moment later. */
    if (!g_object_get_data(G_OBJECT(data), "lp-refill")) {
        g_object_set_data(G_OBJECT(data), "lp-refill", GINT_TO_POINTER(1));
        g_timeout_add_full(G_PRIORITY_DEFAULT, 500, refill_usb,
                           g_object_ref(data), g_object_unref);
    }
}

static void add_usb_rows(App *app, GtkWidget *box)
{
    GDir *media = g_dir_open("/media", 0, NULL);
    if (!media)
        return;

    const char *name;
    while ((name = g_dir_read_name(media)) != NULL) {
        char *path = g_build_filename("/media", name, NULL);

        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            GtkWidget *row = sidebar_row(app, name, "drive-removable-media-symbolic", path);
            gtk_widget_set_hexpand(row, TRUE);
            gtk_box_append(GTK_BOX(line), row);
            char *dev = device_at(path);
            if (dev) {
                GtkWidget *ej = gtk_button_new_from_icon_name("media-eject-symbolic");
                gtk_button_set_has_frame(GTK_BUTTON(ej), FALSE);
                gtk_widget_set_tooltip_text(ej, T("Eject", "꺼내기"));
                g_object_set_data_full(G_OBJECT(ej), "lp-point", g_strdup(path), g_free);
                g_signal_connect(ej, "clicked", G_CALLBACK(on_eject), app);
                gtk_box_append(GTK_BOX(line), ej);
                g_free(dev);
            }
            gtk_box_append(GTK_BOX(box), line);
        }
        g_free(path);
    }

    g_dir_close(media);
}

/* Tags come from ~/.config/lp-files/tags, one per line, name and path
 * separated by a tab. There is no tag database on this system to read,
 * and a group of dots that lead nowhere would break the same rule the
 * places follow - so with no file, there is no 태그 group. */
static void add_tag_rows(App *app, GtkWidget *box)
{
    char     *path = g_build_filename(g_get_user_config_dir(),
                                      "lp-files", "tags", NULL);
    char     *text = NULL;
    gboolean  read = g_file_get_contents(path, &text, NULL, NULL);

    g_free(path);
    if (!read)
        return;

    char     **lines  = g_strsplit(text, "\n", -1);
    GtkWidget *heading = NULL;

    for (int i = 0; lines[i] != NULL; i++) {
        char **field = g_strsplit(lines[i], "\t", 2);

        if (field[0] && field[0][0] != '\0' && field[1] &&
            g_file_test(field[1], G_FILE_TEST_IS_DIR)) {
            if (!heading) {
                heading = sidebar_heading(T("Tags", "태그"));
                gtk_box_append(GTK_BOX(box), heading);
            }
            gtk_box_append(GTK_BOX(box),
                           sidebar_row(app, field[0], NULL, field[1]));
        }
        g_strfreev(field);
    }

    g_strfreev(lines);
    g_free(text);
}

static GtkWidget *build_sidebar(App *app)
{
    GtkWidget *scroller = gtk_scrolled_window_new();
    GtkWidget *box      = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Both names. The theme styles .sidebar, which is what GTK's own
     * widgets carry and therefore what a theme is written against;
     * lp-sidebar stays so that a rule can reach this sidebar and not
     * every other one. */
    gtk_widget_add_css_class(box, "sidebar");
    gtk_widget_add_css_class(box, "lp-sidebar");

    /* Width, and it takes all three.
     *
     * set_size_request alone sets a minimum, and a child of a horizontal
     * box with nothing stopping it takes whatever is left over - which
     * is how a 96px sidebar comes out four hundred wide. The hexpand
     * says do not grow; size_request says do not shrink; and halign
     * FILL keeps it filling the width it was given rather than centring
     * inside it. */
    gtk_widget_set_size_request(box, SIDEBAR_WIDTH, -1);
    gtk_widget_set_hexpand(box, FALSE);
    gtk_widget_set_halign(box, GTK_ALIGN_FILL);

    gtk_box_append(GTK_BOX(box), sidebar_heading(T("Places", "위치")));

    char *documents = place_dir(G_USER_DIRECTORY_DOCUMENTS, "Documents");
    /* 다운로드, never 내려받기 - see the note at the top of the Makefile. */
    char *downloads = place_dir(G_USER_DIRECTORY_DOWNLOAD, "Downloads");
    char *pictures  = place_dir(G_USER_DIRECTORY_PICTURES, "Pictures");
    char *trash     = trash_dir();

    gtk_box_append(GTK_BOX(box), sidebar_row(app, T("Home", "홈"), "user-home-symbolic",
                                             g_get_home_dir()));
    gtk_box_append(GTK_BOX(box), sidebar_row(app, T("Documents", "문서"),
                                             "folder-documents-symbolic",
                                             documents));
    gtk_box_append(GTK_BOX(box), sidebar_row(app, T("Downloads", "다운로드"),
                                             "folder-download-symbolic",
                                             downloads));
    gtk_box_append(GTK_BOX(box), sidebar_row(app, T("Pictures", "사진"),
                                             "folder-pictures-symbolic",
                                             pictures));
    gtk_box_append(GTK_BOX(box), sidebar_row(app, T("Trash", "휴지통"),
                                             "user-trash-symbolic", trash));

    g_free(documents);
    g_free(downloads);
    g_free(pictures);
    g_free(trash);

    gtk_box_append(GTK_BOX(box), sidebar_heading(T("Drives", "드라이브")));
    gtk_box_append(GTK_BOX(box), sidebar_row(app, T("System", "시스템"),
                                             "drive-harddisk-symbolic", "/"));

    app->capacity = gtk_progress_bar_new();
    gtk_widget_add_css_class(app->capacity, "lp-capacity");
    gtk_box_append(GTK_BOX(box), app->capacity);

    app->capacity_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(app->capacity_label), 0.0);
    gtk_widget_add_css_class(app->capacity_label, "lp-capacity-label");
    gtk_box_append(GTK_BOX(box), app->capacity_label);

    /* The drives come and go: /media is watched, and the rows under
     * this heading are made again when a stick is mounted or taken out. */
    GtkWidget *usb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), usb);
    add_usb_rows(app, usb);
    GFile *mf = g_file_new_for_path("/media");
    GFileMonitor *mm = g_file_monitor_directory(mf, G_FILE_MONITOR_NONE, NULL, NULL);
    g_object_unref(mf);
    if (mm) {
        g_object_set_data(G_OBJECT(mm), "lp-app", app);
        g_signal_connect(mm, "changed", G_CALLBACK(on_media_changed), usb);
        g_object_set_data_full(G_OBJECT(usb), "lp-media-monitor", mm, g_object_unref);
    }
    add_tag_rows(app, box);

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), box);
    gtk_widget_add_css_class(scroller, "lp-sidebar-scroll");
    return scroller;
}

/* ------------------------------------------------------------------ */
/* The header bar                                                      */
/* ------------------------------------------------------------------ */

static GtkWidget *icon_button(const char *icon_name, const char *action)
{
    GtkWidget *button = gtk_button_new_from_icon_name(icon_name);

    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
    gtk_widget_add_css_class(button, "lp-icon");
    if (action)
        gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
    return button;
}

static void on_view_toggled(GtkToggleButton *button, gpointer data)
{
    App *app = data;

    gtk_stack_set_visible_child_name(GTK_STACK(app->stack),
                                     gtk_toggle_button_get_active(button)
                                         ? "list" : "grid");
}

static GtkWidget *build_header(App *app)
{
    GtkWidget *header = gtk_header_bar_new();

    /* The window's own controls - minimise, maximise, close - the same
     * three, drawn the same way (theme/gtk-4.0/gtk.css), as in every other
     * window. A close button packed by hand here was the one window on
     * the desktop with a different set. */
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    gtk_widget_add_css_class(header, "lp-header");

    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    app->back    = icon_button("go-previous-symbolic", "win.back");
    app->forward = icon_button("go-next-symbolic", "win.forward");
    gtk_widget_add_css_class(app->back, "lp-nav");
    gtk_widget_add_css_class(app->forward, "lp-nav");
    gtk_box_append(GTK_BOX(nav), app->back);
    gtk_box_append(GTK_BOX(nav), app->forward);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), nav);

    app->title = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(app->title), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(app->title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(app->title, TRUE);
    gtk_widget_add_css_class(app->title, "lp-title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), app->title);

    GtkWidget *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    GtkWidget *search = gtk_toggle_button_new();
    gtk_button_set_child(GTK_BUTTON(search),
                         gtk_image_new_from_icon_name("system-search-symbolic"));
    gtk_button_set_has_frame(GTK_BUTTON(search), FALSE);
    gtk_widget_add_css_class(search, "lp-icon");
    gtk_box_append(GTK_BOX(tools), search);

    GtkWidget *list_view = gtk_toggle_button_new();
    gtk_button_set_child(GTK_BUTTON(list_view),
                         gtk_image_new_from_icon_name("view-list-symbolic"));
    gtk_button_set_has_frame(GTK_BUTTON(list_view), FALSE);
    gtk_widget_add_css_class(list_view, "lp-icon");
    gtk_widget_add_css_class(list_view, "lp-view");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(list_view), TRUE);
    gtk_box_append(GTK_BOX(tools), list_view);

    GtkWidget *grid_view = gtk_toggle_button_new();
    gtk_button_set_child(GTK_BUTTON(grid_view),
                         gtk_image_new_from_icon_name("view-grid-symbolic"));
    gtk_button_set_has_frame(GTK_BUTTON(grid_view), FALSE);
    gtk_widget_add_css_class(grid_view, "lp-icon");
    gtk_widget_add_css_class(grid_view, "lp-view");
    /* Grouping them makes the pair exclusive, so :checked is the whole
     * active/inactive difference and the theme needs no help from here. */
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(grid_view),
                                GTK_TOGGLE_BUTTON(list_view));
    gtk_box_append(GTK_BOX(tools), grid_view);
    g_signal_connect(list_view, "toggled", G_CALLBACK(on_view_toggled), app);

    GMenu *menu = g_menu_new();

    GMenu *make = g_menu_new();
    g_menu_append(make, T("New folder", "새 폴더"), "win.new-folder");
    g_menu_append(make, T("Paste", "붙여넣기"), "win.paste");
    g_menu_append(make, T("Select all", "전체 선택"), "win.select-all");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(make));
    g_object_unref(make);

    GMenu *look = g_menu_new();
    g_menu_append(look, T("Hidden items", "숨김 항목"), "win.show-hidden");
    g_menu_append(look, T("Search sub-folders too", "하위 폴더까지 검색"),
                  "win.deep-search");
    g_menu_append(look, T("Refresh", "새로 고침"), "win.refresh");
    g_menu_append(look, T("Parent folder", "상위 폴더"), "win.up");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(look));
    g_object_unref(look);

    GtkWidget *menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button),
                                  "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button),
                                   G_MENU_MODEL(menu));
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(menu_button), FALSE);
    gtk_widget_add_css_class(menu_button, "lp-icon");
    gtk_box_append(GTK_BOX(tools), menu_button);
    g_object_unref(menu);

    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), tools);

    g_object_set_data(G_OBJECT(header), "search-button", search);
    return header;
}

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Doing things to files                                               */
/* ------------------------------------------------------------------ */

/* Everything below this line writes to the disk, and that is a different
 * kind of program from the one above it, which only read.
 *
 * Three rules hold for all of it.
 *
 * 1. Delete means the trash. g_file_trash() puts the file where it can
 *    be got back from, and that is what "delete" means to the person who
 *    pressed it. Permanent deletion exists - Shift+Delete - and it asks
 *    first, every time, and says how many things and from where.
 *
 * 2. Nothing is overwritten silently. A paste onto a name that exists
 *    makes "name (2)" instead. Overwriting is the one mistake that has
 *    no undo, and a file manager that does it quietly has destroyed
 *    something the person still believed they had.
 *
 * 3. The work happens off the main thread. Copying a folder of photos
 *    takes seconds to minutes, and a window that stops repainting for
 *    that long is a window that looks broken - people force-quit it, and
 *    then the copy really is half-finished. A thread keeps the window
 *    alive, the status line says what is happening, and the operation
 *    is refused rather than queued while another is running.
 */

typedef struct {
    App        *app;
    GPtrArray  *sources;    /* GFile* */
    GFile      *dest_dir;
    gboolean    move;
    gboolean    permanent;  /* delete without the trash */
    char       *error;      /* NULL when it went through */
    guint       done;
    guint       total;
} Op;

static void op_free(Op *op)
{
    if (op->sources) g_ptr_array_unref(op->sources);
    g_clear_object(&op->dest_dir);
    g_free(op->error);
    g_free(op);
}

/* A name that is not taken yet: "photo.png", then "photo (2).png".
 *
 * The number goes before the extension rather than after the whole name
 * because the extension is what decides which program opens it, and
 * "photo.png (2)" opens in nothing. */
static GFile *free_name(GFile *dir, const char *name)
{
    GFile *candidate = g_file_get_child(dir, name);

    if (!g_file_query_exists(candidate, NULL))
        return candidate;
    g_object_unref(candidate);

    const char *dot = strrchr(name, '.');
    /* A leading dot is the whole name of a hidden file, not an
     * extension: ".bashrc" must become ".bashrc (2)". */
    if (dot == name)
        dot = NULL;

    char *stem = dot ? g_strndup(name, (gsize)(dot - name)) : g_strdup(name);
    const char *ext = dot ? dot : "";

    for (int i = 2; i < 10000; i++) {
        char *try = g_strdup_printf("%s (%d)%s", stem, i, ext);
        GFile *f = g_file_get_child(dir, try);
        g_free(try);
        if (!g_file_query_exists(f, NULL)) {
            g_free(stem);
            return f;
        }
        g_object_unref(f);
    }
    g_free(stem);
    return g_file_get_child(dir, name);   /* 포기하고 원래 이름 */
}

/* GIO copies one file. A directory has to be walked, and this is that
 * walk. Errors stop it - a half-copied folder that reports success is
 * worse than one that says where it stopped. */
static gboolean copy_tree(GFile *src, GFile *dst, GError **error)
{
    GFileType type = g_file_query_file_type(src, G_FILE_QUERY_INFO_NONE, NULL);

    if (type != G_FILE_TYPE_DIRECTORY)
        return g_file_copy(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS,
                           NULL, NULL, NULL, error);

    if (!g_file_make_directory_with_parents(dst, NULL, error)) {
        if (!g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            return FALSE;
        g_clear_error(error);
    }

    GFileEnumerator *e = g_file_enumerate_children(
        src, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NONE,
        NULL, error);
    if (!e)
        return FALSE;

    gboolean ok = TRUE;
    for (;;) {
        GFileInfo *info = g_file_enumerator_next_file(e, NULL, error);
        if (!info) {
            ok = (*error == NULL);
            break;
        }
        GFile *child_src = g_file_get_child(src, g_file_info_get_name(info));
        GFile *child_dst = g_file_get_child(dst, g_file_info_get_name(info));
        ok = copy_tree(child_src, child_dst, error);
        g_object_unref(child_src);
        g_object_unref(child_dst);
        g_object_unref(info);
        if (!ok) break;
    }
    g_object_unref(e);
    return ok;
}

static gboolean delete_tree(GFile *f, GError **error)
{
    if (g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, NULL)
        == G_FILE_TYPE_DIRECTORY) {
        GFileEnumerator *e = g_file_enumerate_children(
            f, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NONE,
            NULL, error);
        if (!e) return FALSE;

        for (;;) {
            GFileInfo *info = g_file_enumerator_next_file(e, NULL, error);
            if (!info) break;
            GFile *child = g_file_get_child(f, g_file_info_get_name(info));
            gboolean ok = delete_tree(child, error);
            g_object_unref(child);
            g_object_unref(info);
            if (!ok) { g_object_unref(e); return FALSE; }
        }
        g_object_unref(e);
        if (*error) return FALSE;
    }
    return g_file_delete(f, NULL, error);
}

/* ── 스레드에서 도는 부분 ─────────────────────────────────────── */

static void op_thread(GTask *task, gpointer source, gpointer data,
                      GCancellable *cancel)
{
    (void)source; (void)cancel;
    Op *op = data;
    GError *error = NULL;

    for (guint i = 0; i < op->sources->len; i++) {
        GFile *src = g_ptr_array_index(op->sources, i);

        if (op->dest_dir) {
            char  *base = g_file_get_basename(src);
            GFile *dst  = free_name(op->dest_dir, base);
            g_free(base);

            if (op->move) {
                /* 같은 파일시스템이면 rename 한 번이라 즉시 끝난다.
                 * 아니면 GIO 가 복사한 뒤 지우는데, 그 판단을 여기서
                 * 다시 할 이유는 없다. */
                g_file_move(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS,
                            NULL, NULL, NULL, &error);
                if (g_error_matches(error, G_IO_ERROR,
                                    G_IO_ERROR_WOULD_RECURSE)) {
                    /* 폴더를 다른 파일시스템으로 옮기는 경우.
                     * g_file_move 가 못 하므로 손으로 복사하고 지운다. */
                    g_clear_error(&error);
                    if (copy_tree(src, dst, &error))
                        delete_tree(src, &error);
                }
            } else {
                copy_tree(src, dst, &error);
            }
            g_object_unref(dst);
        } else if (op->permanent) {
            delete_tree(src, &error);
        } else {
            g_file_trash(src, NULL, &error);
            if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
                /* 휴지통이 없는 파일시스템 - USB 의 FAT 이 그렇다.
                 * 조용히 영구 삭제로 넘어가지 않는다. 지우려던 사람은
                 * 되돌릴 수 있다고 믿고 눌렀다. */
                g_clear_error(&error);
                op->error = g_strdup(
                    T("This filesystem has no trash. Use Shift+Delete to"
                      " remove it for good.",
                      "이 파일시스템에는 휴지통이 없습니다. 영구히 지우려면"
                      " Shift+Delete 를 쓰십시오."));
                break;
            }
        }

        op->done++;
        if (error) {
            op->error = g_strdup(error->message);
            g_clear_error(&error);
            break;
        }
    }
    g_task_return_boolean(task, TRUE);
}

static void op_finished(GObject *source, GAsyncResult *res, gpointer data)
{
    (void)source; (void)res;
    Op  *op  = data;
    App *app = op->app;

    app->busy = FALSE;

    if (op->error) {
        char *how_far = op->done
            ? g_strdup_printf(T("%u of %u done", "%u개 중 %u개 처리함"),
                              op->done, op->total)
            : g_strdup(T("Nothing was changed", "아무것도 바뀌지 않았습니다"));

        say(app, T("The operation did not finish", "작업을 끝내지 못했습니다"),
            how_far, op->error);
        g_free(how_far);
    }

    reload(app);
    update_status(app);
    op_free(op);
}

static void op_start(App *app, GPtrArray *sources, GFile *dest_dir,
                     gboolean move, gboolean permanent)
{
    if (app->busy || sources->len == 0) {
        g_ptr_array_unref(sources);
        return;
    }
    app->busy = TRUE;

    Op *op = g_new0(Op, 1);
    op->app       = app;
    op->sources   = sources;
    op->dest_dir  = dest_dir ? g_object_ref(dest_dir) : NULL;
    op->move      = move;
    op->permanent = permanent;
    op->total     = sources->len;

    GTask *task = g_task_new(NULL, NULL, op_finished, op);
    g_task_set_task_data(task, op, NULL);
    g_task_run_in_thread(task, op_thread);
    g_object_unref(task);
}

/* ── 지금 선택된 것들 ─────────────────────────────────────────── */

/* 선택된 항목의 GFile 목록. 아무것도 선택되지 않았으면 빈 배열이다. */
static GPtrArray *selected_files(App *app)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
    GtkBitset *bits = gtk_selection_model_get_selection(app->selection);
    GtkBitsetIter iter;
    guint pos;

    if (gtk_bitset_iter_init_first(&iter, bits, &pos)) {
        do {
            LpfItem *item = g_list_model_get_item(
                G_LIST_MODEL(app->selection), pos);
            if (item) {
                char *path = g_build_filename(app->path, item->name, NULL);
                g_ptr_array_add(out, g_file_new_for_path(path));
                g_free(path);
                g_object_unref(item);
            }
        } while (gtk_bitset_iter_next(&iter, &pos));
    }
    gtk_bitset_unref(bits);
    return out;
}

static LpfItem *first_selected(App *app)
{
    GtkBitset *bits = gtk_selection_model_get_selection(app->selection);
    LpfItem   *item = NULL;
    GtkBitsetIter iter;
    guint pos;

    if (gtk_bitset_iter_init_first(&iter, bits, &pos))
        item = g_list_model_get_item(G_LIST_MODEL(app->selection), pos);
    gtk_bitset_unref(bits);
    return item;
}

/* ── 복사 · 잘라내기 · 붙여넣기 ───────────────────────────────── */

static void put_on_clipboard(App *app, GPtrArray *files)
{
    /* 시스템 클립보드에는 uri-list 로. 다른 앱에 붙여넣기가 되는 것은
     * 이것 때문이고, 그쪽에서는 언제나 복사로 취급된다. */
    GString *uris = g_string_new(NULL);
    for (guint i = 0; i < files->len; i++) {
        char *uri = g_file_get_uri(g_ptr_array_index(files, i));
        g_string_append(uris, uri);
        g_string_append_c(uris, '\n');
        g_free(uri);
    }
    /* Two shapes at once: a file list (text/uri-list - what other file
     * managers, the desktop and GTK apps paste as files) and plain text
     * (the paths as URIs, for a terminal or an editor). */
    GSList *list = NULL;
    for (guint i = files->len; i > 0; i--)
        list = g_slist_prepend(list, g_ptr_array_index(files, i - 1));
    GdkFileList *fl = gdk_file_list_new_from_list(list);
    g_slist_free(list);
    GdkContentProvider *parts[2] = {
        gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, fl),
        gdk_content_provider_new_typed(G_TYPE_STRING, uris->str),
    };
    g_boxed_free(GDK_TYPE_FILE_LIST, fl);
    GdkContentProvider *both = gdk_content_provider_new_union(parts, 2);
    GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(app->window));
    gdk_clipboard_set_content(cb, both);
    g_object_unref(both);
    g_string_free(uris, TRUE);
}

/* Files copied in another program - another Files window, the desktop,
 * a browser's download list - arrive on the system clipboard as a file
 * list. Pasting them here copies them in. */
static gboolean clipboard_has_files(App *app)
{
    GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(app->window));
    return gdk_content_formats_contain_gtype(gdk_clipboard_get_formats(cb), GDK_TYPE_FILE_LIST);
}

static void on_clip_files(GObject *src, GAsyncResult *res, gpointer data)
{
    App *app = data;
    const GValue *v = gdk_clipboard_read_value_finish(GDK_CLIPBOARD(src), res, NULL);
    if (!v || !G_VALUE_HOLDS(v, GDK_TYPE_FILE_LIST))
        return;
    GSList *l = gdk_file_list_get_files(g_value_get_boxed(v));
    GPtrArray *files = g_ptr_array_new_with_free_func(g_object_unref);
    for (GSList *n = l; n; n = n->next)
        g_ptr_array_add(files, g_object_ref(n->data));
    /* transfer container: the list is ours, the files are the list's owner's */
    g_slist_free(l);
    GFile *dest = g_file_new_for_path(app->path);
    op_start(app, files, dest, FALSE, FALSE);
    g_object_unref(dest);
}

static void set_clip(App *app, gboolean cut)
{
    GPtrArray *files = selected_files(app);
    if (files->len == 0) {
        g_ptr_array_unref(files);
        return;
    }

    if (app->clip) g_ptr_array_unref(app->clip);
    app->clip     = files;
    app->clip_cut = cut;

    put_on_clipboard(app, files);

    char *msg = g_strdup_printf(
        cut ? T("%u ready to move", "%u개를 옮길 준비가 되었습니다")
            : T("%u ready to copy", "%u개를 복사할 준비가 되었습니다"),
        files->len);
    gtk_label_set_text(GTK_LABEL(app->status_left), msg);
    g_free(msg);
}

static void act_copy(GSimpleAction *a, GVariant *p, gpointer data)
{ (void)a; (void)p; set_clip(data, FALSE); }

static void act_cut(GSimpleAction *a, GVariant *p, gpointer data)
{ (void)a; (void)p; set_clip(data, TRUE); }

static void act_paste(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    App *app = data;
    if (!app->clip || app->clip->len == 0) {
        if (clipboard_has_files(app))
            gdk_clipboard_read_value_async(gtk_widget_get_clipboard(GTK_WIDGET(app->window)),
                                           GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT, NULL,
                                           on_clip_files, app);
        return;
    }

    GFile *dest = g_file_new_for_path(app->path);
    GPtrArray *copy = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < app->clip->len; i++)
        g_ptr_array_add(copy, g_object_ref(g_ptr_array_index(app->clip, i)));

    op_start(app, copy, dest, app->clip_cut, FALSE);
    g_object_unref(dest);

    /* 잘라내기는 한 번만 붙는다. 두 번째 붙여넣기는 이미 없는 원본을
     * 찾게 되고, 그 오류는 사용자가 한 일과 이어지지 않는다. */
    if (app->clip_cut) {
        g_clear_pointer(&app->clip, g_ptr_array_unref);
        app->clip_cut = FALSE;
    }
}

/* ── 삭제 ─────────────────────────────────────────────────────── */

typedef struct { App *app; GPtrArray *files; } Confirm;

static void on_delete_response(GtkDialog *dialog, int response, gpointer data)
{
    Confirm *c = data;
    if (response == GTK_RESPONSE_ACCEPT)
        op_start(c->app, c->files, NULL, FALSE, TRUE);
    else
        g_ptr_array_unref(c->files);
    g_free(c);
    gtk_window_destroy(GTK_WINDOW(dialog));
}

/* ── the trash: back where it came from ──────────────────────────────
 *
 * g_file_trash() (Delete) moved the file to ~/.local/share/Trash/files
 * and wrote where it was in Trash/info/NAME.trashinfo, "Path=" and the
 * old place percent-encoded - the freedesktop.org trash spec. Restore
 * reads that line back, moves the file there (as "name (2)" if the name
 * has been taken since, never over it), and removes the .trashinfo. */
static void act_restore(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    App *app = data;
    GPtrArray *files = selected_files(app);
    char *base = g_build_filename(g_get_user_data_dir(), "Trash", NULL);
    guint ok = 0, bad = 0;
    for (guint i = 0; i < files->len; i++) {
        GFile *f = g_ptr_array_index(files, i);
        char *name = g_file_get_basename(f);
        char *info = g_strdup_printf("%s/info/%s.trashinfo", base, name);
        char *text = NULL, *orig = NULL;
        if (g_file_get_contents(info, &text, NULL, NULL)) {
            char **lines = g_strsplit(text, "\n", -1);
            for (int k = 0; lines[k]; k++)
                if (g_str_has_prefix(lines[k], "Path="))
                    orig = g_uri_unescape_string(lines[k] + 5, NULL);
            g_strfreev(lines);
        }
        gboolean moved = FALSE;
        if (orig && *orig == '/') {
            GFile *want = g_file_new_for_path(orig);
            GFile *dir = g_file_get_parent(want);
            if (dir) {
                g_file_make_directory_with_parents(dir, NULL, NULL);
                char *bn = g_file_get_basename(want);
                GFile *dst = free_name(dir, bn);
                moved = g_file_move(f, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, NULL);
                g_object_unref(dst);
                g_free(bn);
                g_object_unref(dir);
            }
            g_object_unref(want);
        }
        if (moved) {
            g_unlink(info);
            ok++;
        } else {
            bad++;
        }
        g_free(orig); g_free(text); g_free(info); g_free(name);
    }
    g_free(base);
    g_ptr_array_unref(files);
    if (bad)
        say(app, T("could not be restored", "복원하지 못했습니다"),
            T("Some items", "일부 항목"), T("Their original place is not known or not writable.",
                                            "원래 위치를 모르거나 쓸 수 없습니다."));
    (void)ok;
    reload(app);
}

static void act_trash(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    App *app = data;
    op_start(app, selected_files(app), NULL, FALSE, FALSE);
}

static void act_delete(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    App *app = data;
    GPtrArray *files = selected_files(app);
    if (files->len == 0) {
        g_ptr_array_unref(files);
        return;
    }

    /* 영구 삭제는 되돌릴 수 없는 유일한 동작이므로, 몇 개를 어디서
     * 지우는지 말하고 묻는다. 개수만 말하면 잘못 고른 것을 알 수
     * 없다. */
    char *what = g_strdup_printf(
        T("Delete %u item%s for good?", "%u개%s를 영구히 지울까요?"),
        files->len,
        lp_korean() ? "" : (files->len == 1 ? "" : "s"));

    GtkWidget *dialog = gtk_message_dialog_new(
        app->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", what);
    g_free(what);

    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog), "%s\n\n%s", app->path,
        T("They will not go to the trash and cannot be brought back.",
          "휴지통을 거치지 않으며, 되돌릴 수 없습니다."));

    gtk_dialog_add_button(GTK_DIALOG(dialog), T("Cancel", "취소"),
                          GTK_RESPONSE_CANCEL);
    GtkWidget *ok = gtk_dialog_add_button(GTK_DIALOG(dialog),
                                          T("Delete", "지우기"),
                                          GTK_RESPONSE_ACCEPT);
    gtk_widget_add_css_class(ok, "destructive-action");

    Confirm *c = g_new0(Confirm, 1);
    c->app = app;
    c->files = files;
    g_signal_connect(dialog, "response",
                     G_CALLBACK(on_delete_response), c);
    gtk_window_present(GTK_WINDOW(dialog));
}

/* ── 이름 바꾸기 · 새 폴더 ────────────────────────────────────── */

typedef struct {
    App       *app;
    GtkWidget *entry;
    char      *old_name;    /* NULL 이면 새 폴더 */
} NameDialog;

static void on_name_response(GtkDialog *dialog, int response, gpointer data)
{
    NameDialog *nd = data;
    App *app = nd->app;

    if (response == GTK_RESPONSE_ACCEPT) {
        const char *text = gtk_editable_get_text(GTK_EDITABLE(nd->entry));

        /* 슬래시가 들어가면 이름이 아니라 경로가 된다. 조용히 잘라
         * 붙이면 사용자가 의도하지 않은 자리에 파일이 생긴다. */
        if (!*text || strchr(text, '/')) {
            say(app, T("That name cannot be used", "쓸 수 없는 이름입니다"),
                *text ? text : T("(empty)", "(비어 있음)"),
                T("A name cannot be empty or contain a slash.",
                  "이름은 비어 있을 수 없고 / 를 담을 수 없습니다."));
        } else {
            GFile  *dir = g_file_new_for_path(app->path);
            GError *err = NULL;

            if (nd->old_name) {
                GFile *src = g_file_get_child(dir, nd->old_name);
                GFile *dst = g_file_get_child(dir, text);
                if (g_file_query_exists(dst, NULL)) {
                    say(app, T("That name is taken", "이미 있는 이름입니다"),
                        text, T("Something here already has it.",
                                "이 폴더에 같은 이름이 있습니다."));
                } else {
                    g_file_move(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS,
                                NULL, NULL, NULL, &err);
                }
                g_object_unref(src);
                g_object_unref(dst);
            } else {
                GFile *nf = g_file_get_child(dir, text);
                g_file_make_directory(nf, NULL, &err);
                g_object_unref(nf);
            }

            if (err) {
                say(app, nd->old_name
                        ? T("Could not rename it", "이름을 바꾸지 못했습니다")
                        : T("Could not make the folder", "폴더를 만들지 못했습니다"),
                    text, err->message);
                g_error_free(err);
            }
            g_object_unref(dir);
            reload(app);
        }
    }

    g_free(nd->old_name);
    g_free(nd);
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void name_dialog(App *app, const char *old_name)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        old_name ? T("Rename", "이름 바꾸기") : T("New folder", "새 폴더"),
        app->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        T("Cancel", "취소"), GTK_RESPONSE_CANCEL,
        old_name ? T("Rename", "바꾸기") : T("Create", "만들기"),
        GTK_RESPONSE_ACCEPT, NULL);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_widget_set_margin_start(entry, 16);
    gtk_widget_set_margin_end(entry, 16);
    gtk_widget_set_margin_top(entry, 12);
    gtk_widget_set_margin_bottom(entry, 12);
    gtk_widget_set_size_request(entry, 320, -1);

    if (old_name) {
        gtk_editable_set_text(GTK_EDITABLE(entry), old_name);
        /* 확장자를 뺀 부분만 고른다. 이름을 바꾸려는 사람이 바꾸고
         * 싶은 것은 대개 그쪽이고, 확장자까지 지워지면 파일이 열리지
         * 않게 된다. */
        const char *dot = strrchr(old_name, '.');
        if (dot && dot != old_name)
            gtk_editable_select_region(GTK_EDITABLE(entry), 0,
                                       (int)g_utf8_strlen(old_name,
                                                          dot - old_name));
        else
            gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
    } else {
        gtk_editable_set_text(GTK_EDITABLE(entry),
                              T("New folder", "새 폴더"));
        gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
    }

    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                   entry);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

    NameDialog *nd = g_new0(NameDialog, 1);
    nd->app = app;
    nd->entry = entry;
    nd->old_name = g_strdup(old_name);

    g_signal_connect(dialog, "response", G_CALLBACK(on_name_response), nd);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void act_rename(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    App *app = data;
    LpfItem *item = first_selected(app);
    if (!item) return;
    name_dialog(app, item->name);
    g_object_unref(item);
}

static void act_new_folder(GSimpleAction *a, GVariant *p, gpointer data)
{ (void)a; (void)p; name_dialog(data, NULL); }

static void act_select_all(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    App *app = data;
    gtk_selection_model_select_all(app->selection);
}

/* ── 속성 ─────────────────────────────────────────────────────── */

/* 폴더 하나의 크기를 세는 것은 그 안을 전부 걷는 일이라, 큰 폴더에서는
 * 오래 걸린다. 창을 먼저 띄우고 결과를 나중에 채워 넣는다. */
static goffset dir_size(GFile *dir, guint *files, guint *dirs)
{
    GFileEnumerator *e = g_file_enumerate_children(
        dir, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_SIZE
        "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    if (!e) return 0;

    goffset total = 0;
    for (;;) {
        GFileInfo *info = g_file_enumerator_next_file(e, NULL, NULL);
        if (!info) break;
        if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
            GFile *child = g_file_get_child(dir, g_file_info_get_name(info));
            (*dirs)++;
            total += dir_size(child, files, dirs);
            g_object_unref(child);
        } else {
            (*files)++;
            total += g_file_info_get_size(info);
        }
        g_object_unref(info);
    }
    g_object_unref(e);
    return total;
}

static void prop_row(GtkWidget *grid, int row, const char *label,
                     const char *value)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);

    GtkWidget *v = gtk_label_new(value);
    gtk_widget_set_halign(v, GTK_ALIGN_START);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_label_set_wrap(GTK_LABEL(v), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(v), 44);
    gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
}

/* ── permissions, editable ───────────────────────────────────────────
 *
 * Nine switches - read, write, run for the owner, the group and everyone
 * else - under the numbers. Each change is written at once with
 * g_file_set_attribute_uint32 (chmod), and a file this account does not
 * own cannot be changed: the kernel refuses, and the box goes back. */
typedef struct { GFile *file; guint32 bit; } PermBit;

static void perm_bit_free(gpointer p)
{
    PermBit *b = p;
    g_object_unref(b->file);
    g_free(b);
}

static void on_perm_toggled(GtkCheckButton *cb, gpointer data)
{
    PermBit *b = data;
    GFileInfo *fi = g_file_query_info(b->file, G_FILE_ATTRIBUTE_UNIX_MODE,
                                      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    if (!fi)
        return;
    guint32 mode = g_file_info_get_attribute_uint32(fi, G_FILE_ATTRIBUTE_UNIX_MODE);
    g_object_unref(fi);
    gboolean on = gtk_check_button_get_active(cb);
    guint32 want = on ? (mode | b->bit) : (mode & ~b->bit);
    if (want == mode)
        return;
    GError *err = NULL;
    if (!g_file_set_attribute_uint32(b->file, G_FILE_ATTRIBUTE_UNIX_MODE, want & 07777,
                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &err)) {
        g_signal_handlers_block_by_func(cb, on_perm_toggled, data);
        gtk_check_button_set_active(cb, !on);
        g_signal_handlers_unblock_by_func(cb, on_perm_toggled, data);
        gtk_widget_set_tooltip_text(GTK_WIDGET(cb), err->message);
        g_clear_error(&err);
    }
}

static int perm_editor(GtkWidget *grid, int r, GFile *file, guint32 mode)
{
    static const char *who_en[] = { "Owner", "Group", "Others" };
    static const char *who_ko[] = { "소유자", "그룹", "다른 사람" };
    static const char *what_en[] = { "Read", "Write", "Run" };
    static const char *what_ko[] = { "읽기", "쓰기", "실행" };
    for (int w = 0; w < 3; w++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        for (int k = 0; k < 3; k++) {
            GtkWidget *cb = gtk_check_button_new_with_label(T(what_en[k], what_ko[k]));
            guint32 bit = 1u << (8 - (w * 3 + k));
            gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), (mode & bit) != 0);
            PermBit *b = g_new0(PermBit, 1);
            b->file = g_object_ref(file);
            b->bit = bit;
            g_object_set_data_full(G_OBJECT(cb), "lp-perm", b, perm_bit_free);
            g_signal_connect(cb, "toggled", G_CALLBACK(on_perm_toggled), b);
            gtk_box_append(GTK_BOX(row), cb);
        }
        GtkWidget *label = gtk_label_new(T(who_en[w], who_ko[w]));
        gtk_label_set_xalign(GTK_LABEL(label), 1.0);
        gtk_widget_add_css_class(label, "dim-label");
        gtk_grid_attach(GTK_GRID(grid), label, 0, r, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), row, 1, r, 1, 1);
        r++;
    }
    return r;
}

static void act_properties(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    App *app = data;
    LpfItem *item = first_selected(app);
    if (!item) return;

    char  *path = g_build_filename(app->path, item->name, NULL);
    GFile *file = g_file_new_for_path(path);
    GFileInfo *info = g_file_query_info(
        file, "standard::*,time::modified,time::access,unix::mode,owner::*",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        T("Properties", "속성"), app->window,
        GTK_DIALOG_DESTROY_WITH_PARENT,
        T("Close", "닫기"), GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 440, -1);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_widget_set_margin_start(grid, 20);
    gtk_widget_set_margin_end(grid, 20);
    gtk_widget_set_margin_top(grid, 16);
    gtk_widget_set_margin_bottom(grid, 16);

    int r = 0;
    prop_row(grid, r++, T("Name", "이름"), item->display);
    prop_row(grid, r++, T("Where", "위치"), app->path);

    if (info) {
        prop_row(grid, r++, T("Type", "종류"),
                 g_file_info_get_content_type(info)
                     ? g_content_type_get_description(
                           g_file_info_get_content_type(info))
                     : T("unknown", "알 수 없음"));

        if (item->is_dir) {
            guint files = 0, dirs = 0;
            goffset total = dir_size(file, &files, &dirs);
            char *size = g_format_size(total);
            char *v = g_strdup_printf(
                T("%s · %u files in %u folders",
                  "%s · 폴더 %u개 안에 파일 %u개"),
                size, lp_korean() ? dirs : files, lp_korean() ? files : dirs);
            prop_row(grid, r++, T("Size", "크기"), v);
            g_free(v); g_free(size);
        } else {
            char *size = g_format_size(g_file_info_get_size(info));
            char *v = g_strdup_printf("%s (%" G_GOFFSET_FORMAT " bytes)",
                                      size, g_file_info_get_size(info));
            prop_row(grid, r++, T("Size", "크기"), v);
            g_free(v); g_free(size);
        }

        GDateTime *mod = g_file_info_get_modification_date_time(info);
        if (mod) {
            char *when = g_date_time_format(mod, "%Y-%m-%d %H:%M:%S");
            prop_row(grid, r++, T("Modified", "수정일"), when);
            g_free(when);
            g_date_time_unref(mod);
        }

        guint32 mode = g_file_info_get_attribute_uint32(
            info, G_FILE_ATTRIBUTE_UNIX_MODE);
        if (mode) {
            char perm[11] = "----------";
            perm[0] = item->is_dir ? 'd' : '-';
            const char *rwx = "rwxrwxrwx";
            for (int i = 0; i < 9; i++)
                perm[i + 1] = (mode & (1 << (8 - i))) ? rwx[i] : '-';
            char *v = g_strdup_printf("%s (%04o)", perm, mode & 07777);
            prop_row(grid, r++, T("Permissions", "권한"), v);
            g_free(v);
            r = perm_editor(grid, r, file, mode);
        }

        const char *owner = g_file_info_get_attribute_string(
            info, G_FILE_ATTRIBUTE_OWNER_USER);
        if (owner)
            prop_row(grid, r++, T("Owner", "소유자"), owner);

        g_object_unref(info);
    }

    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                   grid);
    g_signal_connect(dialog, "response", G_CALLBACK(gtk_window_destroy), NULL);
    gtk_window_present(GTK_WINDOW(dialog));

    g_object_unref(file);
    g_free(path);
    g_object_unref(item);
}

/* ── 압축 풀기 ────────────────────────────────────────────────── */

/* 직접 풀지 않고 file-roller 에게 넘긴다. zip, tar, 7z, rar 를 저마다
 * 다르게 다루는 코드를 여기 들이는 것은 이 앱이 할 일이 아니고,
 * file-roller 는 이미 이미지에 들어 있다. */
static void act_extract(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    App *app = data;
    LpfItem *item = first_selected(app);
    if (!item) return;

    char *path = g_build_filename(app->path, item->name, NULL);
    const char *argv[] = { "file-roller", "--extract-here", path, NULL };

    if (!g_spawn_async(app->path, (char **)argv, NULL,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                       G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL))
        say(app, T("Could not extract it", "압축을 풀지 못했습니다"),
            item->display,
            T("file-roller is not installed.",
              "file-roller 가 설치되어 있지 않습니다."));

    g_free(path);
    g_object_unref(item);
}

/* 압축 파일처럼 보이는가. 우클릭 메뉴에 '압축 풀기' 를 넣을지 정하는
 * 데만 쓰이므로, 확장자로 보는 것으로 충분하다. */
static gboolean looks_like_archive(const char *name)
{
    static const char *ext[] = {
        ".zip", ".tar", ".gz", ".tgz", ".bz2", ".xz", ".7z", ".rar",
        ".zst", ".tar.gz", ".tar.xz", ".tar.bz2", NULL
    };
    char *lower = g_ascii_strdown(name, -1);
    gboolean hit = FALSE;
    for (int i = 0; ext[i] && !hit; i++)
        hit = g_str_has_suffix(lower, ext[i]);
    g_free(lower);
    return hit;
}

/* ------------------------------------------------------------------ */
/* The right-click menu                                                */
/* ------------------------------------------------------------------ */

/* Built each time it opens rather than once at startup.
 *
 * What belongs on it depends on what is under the pointer: Extract only
 * for an archive, Rename only for exactly one thing, Paste only when
 * something was copied. A fixed menu with half its entries greyed out
 * makes a person read all of it to find the two that work. */
static void popup_menu(App *app, GtkWidget *over, double x, double y)
{
    GtkBitset *bits = gtk_selection_model_get_selection(app->selection);
    guint      n    = gtk_bitset_get_size(bits);
    gtk_bitset_unref(bits);

    GMenu *menu = g_menu_new();

    char *trash = trash_dir();
    gboolean in_trash = g_strcmp0(app->path, trash) == 0;
    g_free(trash);

    if (n >= 1 && in_trash) {
        GMenu *t = g_menu_new();
        g_menu_append(t, T("Restore", "복원"), "win.restore");
        g_menu_append(t, T("Delete for good", "영구히 지우기"), "win.delete");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(t));
        g_object_unref(t);
    } else if (n >= 1) {
        GMenu *first = g_menu_new();
        g_menu_append(first, T("Open", "열기"), "win.open");

        if (n == 1) {
            LpfItem *item = first_selected(app);
            if (item) {
                if (looks_like_archive(item->name))
                    g_menu_append(first, T("Extract here", "여기에 압축 풀기"),
                                  "win.extract");
                g_object_unref(item);
            }
        }
        g_menu_append_section(menu, NULL, G_MENU_MODEL(first));
        g_object_unref(first);

        GMenu *edit = g_menu_new();
        g_menu_append(edit, T("Copy", "복사"), "win.copy");
        g_menu_append(edit, T("Cut", "잘라내기"), "win.cut");
        if (app->clip && app->clip->len)
            g_menu_append(edit, T("Paste", "붙여넣기"), "win.paste");
        if (n == 1)
            g_menu_append(edit, T("Rename", "이름 바꾸기"), "win.rename");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(edit));
        g_object_unref(edit);

        GMenu *gone = g_menu_new();
        g_menu_append(gone, T("Move to trash", "휴지통으로"), "win.trash");
        g_menu_append(gone, T("Delete for good", "영구히 지우기"), "win.delete");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(gone));
        g_object_unref(gone);

        GMenu *info = g_menu_new();
        g_menu_append(info, T("Properties", "속성"), "win.properties");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(info));
        g_object_unref(info);
    } else {
        /* 빈 자리에서 눌렀다. 선택된 것이 없으므로 이 폴더에 대한
         * 것만 남는다. */
        GMenu *here = g_menu_new();
        if ((app->clip && app->clip->len) || clipboard_has_files(app))
            g_menu_append(here, T("Paste", "붙여넣기"), "win.paste");
        g_menu_append(here, T("New folder", "새 폴더"), "win.new-folder");
        g_menu_append(here, T("Select all", "전체 선택"), "win.select-all");
        g_menu_append(here, T("Refresh", "새로 고침"), "win.refresh");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(here));
        g_object_unref(here);
    }

    if (app->menu)
        gtk_widget_unparent(app->menu);

    app->menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(menu);

    gtk_popover_set_has_arrow(GTK_POPOVER(app->menu), FALSE);
    gtk_widget_set_halign(app->menu, GTK_ALIGN_START);
    /* GTK 4.8 에는 gtk_popover_set_parent 가 없다. 팝오버도 위젯이므로
     * 부모를 직접 붙인다 - 4.10 이 그 이름의 함수를 만들었을 뿐이고,
     * 하는 일은 같다. */
    gtk_widget_set_parent(app->menu, over);
    gtk_popover_set_pointing_to(GTK_POPOVER(app->menu),
                                &(GdkRectangle){ (int)x, (int)y, 1, 1 });
    gtk_popover_popup(GTK_POPOVER(app->menu));
}

/* What a right click is about, the way every file manager does it: on a
 * file that is not selected, that file alone becomes the selection; on a
 * selected one, the selection stays; on the empty space, nothing is
 * selected. Without this the menu was always the empty space's - New
 * folder, Select all - whatever was under the pointer. The cell widgets
 * carry their item ("lpf-item", set when bound); item_at reads it, and
 * dragging and dropping ask it the same question. */
static LpfItem *item_at(GtkWidget *over, double x, double y)
{
    LpfItem *hit = NULL;
    for (GtkWidget *w = gtk_widget_pick(over, x, y, GTK_PICK_DEFAULT);
         w && !hit; w = w == over ? NULL : gtk_widget_get_parent(w))
        hit = g_object_get_data(G_OBJECT(w), "lpf-item");
    return hit;
}

static void select_under(App *app, GtkWidget *over, double x, double y)
{
    LpfItem *hit = item_at(over, x, y);
    if (!hit) {
        gtk_selection_model_unselect_all(app->selection);
        return;
    }
    guint n = g_list_model_get_n_items(G_LIST_MODEL(app->selection));
    for (guint i = 0; i < n; i++) {
        gpointer it = g_list_model_get_item(G_LIST_MODEL(app->selection), i);
        g_object_unref(it);
        if (it == hit) {
            if (!gtk_selection_model_is_selected(app->selection, i))
                gtk_selection_model_select_item(app->selection, i, TRUE);
            return;
        }
    }
}

static void on_right_click(GtkGestureClick *gesture, int n_press,
                           double x, double y, gpointer data)
{
    (void)n_press;
    App *app = data;
    GtkWidget *over = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(gesture));
    select_under(app, over, x, y);
    popup_menu(app, over, x, y);
}

/* 터치스크린에는 오른쪽 버튼이 없다: 손가락으로 길게 누르면 같은 메뉴.
 * 시퀀스를 가져가야 손을 뗄 때 그 항목이 열리지 않는다. */
static void on_long_press(GtkGestureLongPress *gesture, double x, double y,
                          gpointer data)
{
    App *app = data;
    GtkWidget *over = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(gesture));
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    select_under(app, over, x, y);
    popup_menu(app, over, x, y);
}

/* 메뉴는 보기(view)에 붙어 있다. 창을 닫으면 보기가 먼저 사라지고
 * free_app 은 창이 끝날 때 돌아서, 거기서 메뉴를 떼면 이미 해제된 보기를
 * 건드렸다. 보기가 사라질 때 같이 뗀다. */
static void on_view_destroy(GtkWidget *view, gpointer data)
{
    App *app = data;
    if (app->menu && gtk_widget_get_parent(app->menu) == view) {
        gtk_widget_unparent(app->menu);
        app->menu = NULL;
    }
}

/* 오른쪽 버튼과 Menu 키 둘 다. 키보드만 쓰는 사람에게 메뉴가 없으면
 * 복사와 삭제는 단축키를 외운 사람만 쓸 수 있는 기능이 된다. */
static void attach_menu(App *app, GtkWidget *view)
{
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_right_click), app);
    gtk_widget_add_controller(view, GTK_EVENT_CONTROLLER(click));
    g_signal_connect(view, "destroy", G_CALLBACK(on_view_destroy), app);

    /* 터치만: 마우스로 누르고 있는 것은 끌기의 시작이다. 캡처 단계라야
     * 목록의 항목이 누름을 먼저 가져가기 전에 본다. */
    GtkGesture *hold = gtk_gesture_long_press_new();
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(hold), TRUE);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(hold),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(hold, "pressed", G_CALLBACK(on_long_press), app);
    gtk_widget_add_controller(view, GTK_EVENT_CONTROLLER(hold));
}

/* ------------------------------------------------------------------ */
/* Sorting by column                                                   */
/* ------------------------------------------------------------------ */

/* Folders stay above files whichever column is sorted, and that is not
 * a decoration: a folder and a file of the same name are different
 * kinds of thing, and mixing them by size puts a 4KB folder in the
 * middle of a list of small files where nobody looks for it. Every
 * comparison below therefore answers the directory question first. */
static int dirs_first(LpfItem *a, LpfItem *b)
{
    if (a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1;
    return 0;
}

static int sort_by_name(gconstpointer a, gconstpointer b, gpointer user)
{
    (void)user;
    LpfItem *x = (LpfItem *)a, *y = (LpfItem *)b;
    int d = dirs_first(x, y);
    return d ? d : strcmp(x->sort_key, y->sort_key);
}

static int sort_by_size(gconstpointer a, gconstpointer b, gpointer user)
{
    (void)user;
    LpfItem *x = (LpfItem *)a, *y = (LpfItem *)b;
    int d = dirs_first(x, y);
    if (d) return d;
    /* 폴더끼리는 크기가 없으므로 이름으로 떨어진다. 둘 다 0 이라고
     * 답하면 정렬이 안정적이지 않아 누를 때마다 순서가 바뀐다. */
    if (x->is_dir) return strcmp(x->sort_key, y->sort_key);
    if (x->size < y->size) return -1;
    if (x->size > y->size) return 1;
    return strcmp(x->sort_key, y->sort_key);
}

static int sort_by_date(gconstpointer a, gconstpointer b, gpointer user)
{
    (void)user;
    LpfItem *x = (LpfItem *)a, *y = (LpfItem *)b;
    int d = dirs_first(x, y);
    if (d) return d;
    if (x->mtime < y->mtime) return -1;
    if (x->mtime > y->mtime) return 1;
    return strcmp(x->sort_key, y->sort_key);
}

/* ------------------------------------------------------------------ */
/* Drag and drop                                                       */
/* ------------------------------------------------------------------ */

/* A drag that starts on a file or folder carries it; one that starts on
 * the empty space is the rubberband's, and prepare says so by returning
 * nothing.
 *
 * Why this runs in the capture phase and claims the press: the list's
 * rubberband is a drag gesture of GTK's own on the list inside the view,
 * and in GTK 4.8 it starts from anywhere, rows included. In the bubble
 * phase it saw every motion before this source (a child before its
 * parent), claimed it at the same threshold, and a drag out of Files
 * never began - pressing on a file and dragging it onto a folder drew a
 * selection rectangle instead (seen under wayfire; the order is GTK's,
 * not the compositor's). Claiming here, before it sees the motion,
 * cancels it for this one press. */
static GdkContentProvider *on_drag_prepare(GtkDragSource *source,
                                           double x, double y, gpointer data)
{
    App *app = data;
    GtkWidget *view = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(source));

    if (!item_at(view, x, y))
        return NULL;
    /* An unselected file is dragged on its own, a selected one with the
     * rest of the selection - the right click's rule. */
    select_under(app, view, x, y);

    GPtrArray *files = selected_files(app);
    if (files->len == 0) {
        g_ptr_array_unref(files);
        return NULL;
    }

    GSList *list = NULL;
    for (guint i = files->len; i > 0; i--)
        list = g_slist_prepend(list, g_ptr_array_index(files, i - 1));

    GdkFileList *fl = gdk_file_list_new_from_list(list);
    g_slist_free(list);
    g_ptr_array_unref(files);

    /* new_typed 가 boxed 값을 복사해 가므로 이쪽 것은 여기서 놓는다.
     * GdkFileList 는 boxed 타입이고 전용 unref 함수가 없다. */
    GdkContentProvider *p =
        gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, fl);
    g_boxed_free(GDK_TYPE_FILE_LIST, fl);
    gtk_gesture_set_state(GTK_GESTURE(source), GTK_EVENT_SEQUENCE_CLAIMED);
    return p;
}

static guint64 device_of(GFile *f)
{
    GFileInfo *fi = g_file_query_info(f, G_FILE_ATTRIBUTE_UNIX_DEVICE,
                                      G_FILE_QUERY_INFO_NONE, NULL, NULL);
    guint64 d = fi ? g_file_info_get_attribute_uint32(fi, G_FILE_ATTRIBUTE_UNIX_DEVICE) : 0;
    g_clear_object(&fi);
    return d;
}

static gboolean same_disk(GFile *a, GFile *b)
{
    guint64 da = device_of(a), db = device_of(b);
    return da && da == db;
}

/* Dropped on a folder's row, the files go into that folder; anywhere
 * else in the view, into the folder on screen. Without the first half
 * the only drop Files took was from outside it: a file dragged onto a
 * folder next to it landed back where it came from and nothing moved. */
static gboolean on_drop(GtkDropTarget *target, const GValue *value,
                        double x, double y, gpointer data)
{
    App *app = data;

    if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
        return FALSE;

    GtkWidget *view = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(target));
    LpfItem *hit = item_at(view, x, y);
    char *dest_path = hit && hit->is_dir
        ? g_build_filename(app->path, hit->name, NULL)
        : g_strdup(app->path);

    GdkFileList *fl = g_value_get_boxed(value);
    GSList      *l  = gdk_file_list_get_files(fl);

    GPtrArray *sources = g_ptr_array_new_with_free_func(g_object_unref);
    for (GSList *n = l; n; n = n->next) {
        GFile *f = n->data;
        /* 자기 자신이 있는 폴더로 끌어다 놓는 것은 아무 뜻이 없고,
         * "(2)" 사본만 만들어 낸다. 폴더를 자기 자신이나 자기 안으로
         * 옮기는 것도 뜻이 없다 - 폴더 줄 위에서 놓으면 끌던 폴더가
         * 바로 그 줄일 수 있다. */
        GFile *parent = g_file_get_parent(f);
        char  *pp = parent ? g_file_get_path(parent) : NULL;
        char  *fp = g_file_get_path(f);
        gboolean same = pp && g_strcmp0(pp, dest_path) == 0;
        if (fp && (g_strcmp0(fp, dest_path) == 0 ||
                   (g_str_has_prefix(dest_path, fp) &&
                    dest_path[strlen(fp)] == '/')))
            same = TRUE;
        g_free(fp);
        g_free(pp);
        g_clear_object(&parent);
        if (!same)
            g_ptr_array_add(sources, g_object_ref(f));
    }
    /* transfer container: unreffing the files too freed them under the
     * drag's own file list - on wayfire a drop from the desktop then moved
     * nothing (g_file_move: 'G_IS_FILE (source)' failed). */
    g_slist_free(l);

    if (sources->len == 0) {
        g_ptr_array_unref(sources);
        g_free(dest_path);
        return FALSE;
    }

    /* On the same disk a drop moves, onto another disk it copies - what
     * every file manager people have used does, and what "drag it into
     * that folder" means. (It used to always copy, and a drag inside the
     * home left the original behind every time.) A move is undone by
     * dragging back; a copy onto a USB stick leaves the original where
     * it was, which is what a stick is for. */
    GFile *dest = g_file_new_for_path(dest_path);
    g_free(dest_path);
    gboolean move = same_disk(g_ptr_array_index(sources, 0), dest);
    op_start(app, sources, dest, move, FALSE);
    g_object_unref(dest);
    return TRUE;
}

static void attach_dnd(App *app, GtkWidget *view)
{
    GtkDragSource *source = gtk_drag_source_new();
    gtk_drag_source_set_actions(source, GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect(source, "prepare", G_CALLBACK(on_drag_prepare), app);
    /* Ahead of the rubberband: see on_drag_prepare. */
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(source),
                                               GTK_PHASE_CAPTURE);
    gtk_widget_add_controller(view, GTK_EVENT_CONTROLLER(source));

    GtkDropTarget *target = gtk_drop_target_new(GDK_TYPE_FILE_LIST,
                                                GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect(target, "drop", G_CALLBACK(on_drop), app);
    gtk_widget_add_controller(view, GTK_EVENT_CONTROLLER(target));
}

/* ------------------------------------------------------------------ */
/* The list                                                            */
/* ------------------------------------------------------------------ */

static GtkColumnViewColumn *make_column(const char *title,
                                        GCallback setup, GCallback bind,
                                        gpointer setup_data, int width,
                                        GCompareDataFunc compare)
{
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();

    g_signal_connect(factory, "setup", setup, setup_data);
    g_signal_connect(factory, "bind", bind, NULL);

    GtkColumnViewColumn *column = gtk_column_view_column_new(title, factory);

    if (width > 0)
        gtk_column_view_column_set_fixed_width(column, width);
    else
        gtk_column_view_column_set_expand(column, TRUE);

    /* 정렬기를 붙이면 그 열의 머리글이 눌리는 것이 되고, 두 번 누르면
     * 거꾸로 간다 - 그 왕복은 GtkColumnView 가 알아서 한다. */
    if (compare) {
        GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(compare, NULL, NULL));
        gtk_column_view_column_set_sorter(column, sorter);
        g_object_unref(sorter);
    }
    return column;
}

static GtkWidget *build_list(App *app)
{
    GtkWidget *scroller = gtk_scrolled_window_new();
    GtkWidget *view     = gtk_column_view_new(NULL);
    gtk_column_view_set_enable_rubberband(GTK_COLUMN_VIEW(view), TRUE);

    gtk_column_view_set_model(GTK_COLUMN_VIEW(view), app->selection);
    /* The mockup fixes the order of the three columns; dragging them
     * around would only put the numbers somewhere they were not drawn. */
    gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(view), FALSE);
    gtk_widget_add_css_class(view, "lp-list");

    GtkColumnViewColumn *name = make_column(T("Name", "이름"),
                                            G_CALLBACK(name_setup),
                                            G_CALLBACK(name_bind), NULL, -1,
                                            sort_by_name);
    GtkColumnViewColumn *size = make_column(T("Size", "크기"),
                                            G_CALLBACK(text_setup),
                                            G_CALLBACK(size_bind),
                                            (gpointer)"lp-size",
                                            SIZE_COL_WIDTH,
                                            sort_by_size);
    GtkColumnViewColumn *date = make_column(T("Modified", "수정일"),
                                            G_CALLBACK(text_setup),
                                            G_CALLBACK(date_bind),
                                            (gpointer)"lp-date",
                                            DATE_COL_WIDTH,
                                            sort_by_date);

    gtk_column_view_append_column(GTK_COLUMN_VIEW(view), name);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(view), size);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(view), date);
    g_object_unref(name);
    g_object_unref(size);
    g_object_unref(date);

    g_signal_connect(view, "activate", G_CALLBACK(on_row_activated), app);

    /* 열 머리글을 눌러 정렬하는 것은 이 한 줄이다. 정렬 모델이
     * 뷰의 정렬기를 따라가고, 뷰는 어느 머리글이 눌렸는지를 안다. */
    app->list_view = view;
    gtk_sort_list_model_set_sorter(app->sorted,
                                   gtk_column_view_get_sorter(GTK_COLUMN_VIEW(view)));

    attach_menu(app, view);
    attach_dnd(app, view);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
    return scroller;
}

static GtkWidget *build_grid(App *app)
{
    GtkWidget          *scroller = gtk_scrolled_window_new();
    GtkListItemFactory *factory  = gtk_signal_list_item_factory_new();

    g_signal_connect(factory, "setup", G_CALLBACK(tile_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(tile_bind), NULL);

    /* Same selection model as the list, so switching views keeps what
     * you had picked and the status bar does not flinch. */
    GtkWidget *view = gtk_grid_view_new(g_object_ref(app->selection), factory);
    /* Drag across empty space to select a group, as in every file manager. */
    gtk_grid_view_set_enable_rubberband(GTK_GRID_VIEW(view), TRUE);

    gtk_widget_add_css_class(view, "lp-grid");
    g_signal_connect(view, "activate", G_CALLBACK(on_row_activated), app);

    /* 타일 보기에는 열 머리글이 없으므로 정렬은 목록 쪽이 정한 것을
     * 그대로 따른다 - 두 보기가 같은 모델을 쓰기 때문에 저절로 그렇게
     * 된다. 메뉴와 끌어 놓기는 여기에도 있어야 한다. */
    attach_menu(app, view);
    attach_dnd(app, view);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
    return scroller;
}

/* ------------------------------------------------------------------ */
/* Putting the window together                                         */
/* ------------------------------------------------------------------ */

static void on_window_destroy(GtkWidget *widget, gpointer data)
{
    App *app = data;
    (void)widget;

    /* The debounce timer holds a pointer to a window that is going away;
     * left running it fires into freed memory a third of a second later. */
    if (app->refresh_id != 0) {
        g_source_remove(app->refresh_id);
        app->refresh_id = 0;
    }
    g_clear_object(&app->monitor);

    /* The selection model outlives this signal by however long the views
     * take to let go of it, and it can still emit while they do. */
    g_signal_handlers_disconnect_by_data(app->selection, app);
}

static void free_place(gpointer data)
{
    Place *place = data;

    g_free(place->path);
    g_free(place);
}

/* Hung off the window as data rather than off its destroy signal, so it
 * runs when the window is finally gone - by then the views have let go
 * of the selection model, and unreffing it here takes the whole model
 * chain with it. */
static void free_app(gpointer data)
{
    App *app = data;

    g_ptr_array_free(app->history, TRUE);
    g_ptr_array_free(app->places, TRUE);
    g_clear_pointer(&app->clip, g_ptr_array_unref);
    if (app->menu)
        gtk_widget_unparent(app->menu);
    g_clear_object(&app->selection);
    g_free(app->path);
    g_free(app);
}

static const GActionEntry ACTIONS[] = {
    { "back",    act_back,    NULL, NULL,    NULL, { 0 } },
    { "forward", act_forward, NULL, NULL,    NULL, { 0 } },
    { "up",      act_up,      NULL, NULL,    NULL, { 0 } },
    { "home",    act_home,    NULL, NULL,    NULL, { 0 } },
    { "refresh", act_refresh, NULL, NULL,    NULL, { 0 } },
    { "show-hidden", act_show_hidden, NULL, "false", NULL, { 0 } },

    /* 파일을 건드리는 것들. 위의 여섯은 보기만 바꾸고, 아래는
     * 디스크에 쓴다. */
    { "copy",       act_copy,       NULL, NULL, NULL, { 0 } },
    { "cut",        act_cut,        NULL, NULL, NULL, { 0 } },
    { "paste",      act_paste,      NULL, NULL, NULL, { 0 } },
    { "trash",      act_trash,      NULL, NULL, NULL, { 0 } },
    { "restore",    act_restore,    NULL, NULL, NULL, { 0 } },
    { "delete",     act_delete,     NULL, NULL, NULL, { 0 } },
    { "rename",     act_rename,     NULL, NULL, NULL, { 0 } },
    { "new-folder", act_new_folder, NULL, NULL, NULL, { 0 } },
    { "select-all", act_select_all, NULL, NULL, NULL, { 0 } },
    { "properties", act_properties, NULL, NULL, NULL, { 0 } },
    { "extract",    act_extract,    NULL, NULL, NULL, { 0 } },
    { "open",       act_open,       NULL, NULL, NULL, { 0 } },
    /* A boolean state with its handler as change_state: activating it
     * then toggles the state and hands over the new value. As the
     * activate handler it was called with no value at all, and "Search
     * sub-folders too" never turned on. */
    { "deep-search", NULL, NULL, "false", act_deep_search, { 0 } },
};

static void open_window(GtkApplication *application, const char *start)
{
    App *app = g_new0(App, 1);

    app->history = g_ptr_array_new_with_free_func(g_free);
    app->places  = g_ptr_array_new_with_free_func(free_place);

    app->window = GTK_WINDOW(gtk_application_window_new(application));
    lp_fit_default_size(app->window, WINDOW_WIDTH, WINDOW_HEIGHT);
    gtk_widget_add_css_class(GTK_WIDGET(app->window), "lp-window");

    g_action_map_add_action_entries(G_ACTION_MAP(app->window), ACTIONS,
                                    G_N_ELEMENTS(ACTIONS), app);

    /* store -> filter -> sort -> selection -> the two views. Each link
     * takes the reference of the one below it, so the whole chain is
     * owned by the one reference to the selection model that App keeps;
     * `store`, `filter` and `sorted` are borrowed pointers into it.
     *
     * The sort model starts with no sorter, which means it passes the
     * order through untouched - and the order it passes through is the
     * one fill_list already put the rows in, folders first and then by
     * name. Clicking a column header hands it a sorter and it takes
     * over; there is no third place where the order is decided. */
    app->store  = g_list_store_new(LPF_TYPE_ITEM);
    app->filter = gtk_custom_filter_new(filter_match, app, NULL);

    GtkFilterListModel *filtered =
        gtk_filter_list_model_new(G_LIST_MODEL(app->store),
                                  GTK_FILTER(app->filter));

    app->sorted = gtk_sort_list_model_new(G_LIST_MODEL(filtered), NULL);

    app->selection = GTK_SELECTION_MODEL(
        gtk_multi_selection_new(G_LIST_MODEL(app->sorted)));

    g_signal_connect_swapped(app->selection, "selection-changed",
                             G_CALLBACK(update_status), app);
    g_signal_connect_swapped(app->selection, "items-changed",
                             G_CALLBACK(update_status), app);

    gtk_window_set_titlebar(app->window, build_header(app));

    GtkWidget *root  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *split = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *main_column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_widget_set_vexpand(split, TRUE);
    gtk_widget_set_hexpand(main_column, TRUE);

    /* A path deep enough to overflow the window scrolls sideways rather
     * than squeezing the crumbs into nothing; the last crumb, which is
     * where you are, is the one that must stay readable. */
    GtkWidget *crumb_scroll = gtk_scrolled_window_new();
    app->crumbs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(app->crumbs, "lp-crumbs");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(crumb_scroll),
                                   GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(crumb_scroll),
                                  app->crumbs);
    gtk_widget_add_css_class(crumb_scroll, "lp-crumbbar");
    gtk_box_append(GTK_BOX(main_column), crumb_scroll);

    app->search_entry = gtk_search_entry_new();
    gtk_widget_set_hexpand(app->search_entry, TRUE);
    app->search_bar = gtk_search_bar_new();
    gtk_search_bar_set_child(GTK_SEARCH_BAR(app->search_bar),
                             app->search_entry);
    gtk_search_bar_connect_entry(GTK_SEARCH_BAR(app->search_bar),
                                 GTK_EDITABLE(app->search_entry));
    gtk_search_bar_set_key_capture_widget(GTK_SEARCH_BAR(app->search_bar),
                                          GTK_WIDGET(app->window));
    gtk_widget_add_css_class(app->search_bar, "lp-search");
    g_signal_connect(app->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), app);
    gtk_box_append(GTK_BOX(main_column), app->search_bar);

    GtkWidget *header = gtk_window_get_titlebar(app->window);
    g_object_bind_property(g_object_get_data(G_OBJECT(header),
                                             "search-button"),
                           "active", app->search_bar,
                           "search-mode-enabled",
                           G_BINDING_BIDIRECTIONAL);

    app->stack = gtk_stack_new();
    gtk_widget_set_vexpand(app->stack, TRUE);
    gtk_stack_add_named(GTK_STACK(app->stack), build_list(app), "list");
    gtk_stack_add_named(GTK_STACK(app->stack), build_grid(app), "grid");
    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "list");
    gtk_box_append(GTK_BOX(main_column), app->stack);

    gtk_box_append(GTK_BOX(split), build_sidebar(app));
    gtk_box_append(GTK_BOX(split), main_column);
    gtk_box_append(GTK_BOX(root), split);

    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(status, "lp-status");
    app->status_left  = gtk_label_new(NULL);
    app->status_right = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(app->status_left), 0.0);
    gtk_label_set_xalign(GTK_LABEL(app->status_right), 1.0);
    gtk_widget_set_hexpand(app->status_left, TRUE);
    gtk_widget_add_css_class(app->status_left, "lp-status-left");
    gtk_widget_add_css_class(app->status_right, "lp-status-right");
    gtk_box_append(GTK_BOX(status), app->status_left);
    gtk_box_append(GTK_BOX(status), app->status_right);
    gtk_box_append(GTK_BOX(root), status);

    gtk_window_set_child(app->window, root);

    g_signal_connect(app->window, "destroy",
                     G_CALLBACK(on_window_destroy), app);
    g_object_set_data_full(G_OBJECT(app->window), "lp-app", app, free_app);

    navigate(app, start, TRUE);
    gtk_window_present(app->window);
}

static void on_activate(GtkApplication *application, gpointer data)
{
    (void)data;
    open_window(application, g_get_home_dir());
}

/* lp-files /어떤/폴더, and the directory a compositor hands us when it
 * opens inode/directory with this application. */
static void on_open(GApplication *application, GFile **files, int n,
                    const char *hint, gpointer data)
{
    (void)hint; (void)data;

    for (int i = 0; i < n; i++) {
        char *path = g_file_get_path(files[i]);

        if (!path)
            continue;

        /* Handed a file rather than a folder, open the folder it is in.
         * Refusing would be correct and useless. */
        if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
            char *parent = g_path_get_dirname(path);
            g_free(path);
            path = parent;
        }

        open_window(GTK_APPLICATION(application), path);
        g_free(path);
    }
}

int main(int argc, char **argv)
{
    GtkApplication *application =
        gtk_application_new(APP_ID, G_APPLICATION_HANDLES_OPEN);

    g_signal_connect(application, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(application, "open", G_CALLBACK(on_open), NULL);

    static const struct { const char *action; const char *key; } keys[] = {
        { "win.back",        "<Alt>Left"  },
        { "win.forward",     "<Alt>Right" },
        { "win.up",          "<Alt>Up"    },
        { "win.home",        "<Alt>Home"  },
        { "win.refresh",     "F5"         },
        { "win.show-hidden", "<Control>h" },
        /* 다른 파일 관리자와 같은 자리에 둔다. 여기서 독창적일
         * 이유가 없고, 손이 먼저 기억하고 있는 키다. */
        { "win.copy",        "<Control>c" },
        { "win.cut",         "<Control>x" },
        { "win.paste",       "<Control>v" },
        { "win.select-all",  "<Control>a" },
        { "win.new-folder",  "<Control><Shift>n" },
        { "win.rename",      "F2"         },
        { "win.trash",       "Delete"     },
        { "win.delete",      "<Shift>Delete" },
        { "win.properties",  "<Alt>Return" },
    };

    for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
        const char *accels[] = { keys[i].key, NULL };
        gtk_application_set_accels_for_action(application, keys[i].action,
                                              accels);
    }

    int status = g_application_run(G_APPLICATION(application), argc, argv);

    g_object_unref(application);
    return status;
}
