/*
 * driver.c - a script that works the app's controls from inside it.
 *
 * LP_SETTINGS_SCRIPT=<file> makes lp-settings read one step per line and
 * perform it against its own widgets, a few hundred milliseconds apart:
 *
 *     panel display              open a panel (optionally: a row title)
 *     set "Scale" "150%"         switch on/off, drop-down item, slider value,
 *                                segmented label, or entry text - found by
 *                                the row's title (or the entry's label)
 *     click "Connect"            a button, by its label or tooltip
 *     tap "CAFE_FREE_WIFI"       a row, as a finger on it would
 *     search "night"             type into the search field
 *     wait 1500                  let an asynchronous answer land
 *     run grim /media/x.png      run a command and wait for it
 *     shots /media/p 60,120,200  grim at those offsets from now, without
 *                                waiting - for frames in the middle of an
 *                                animation, which a blocking `run` would
 *                                freeze (the main loop draws the frames)
 *     echo text                  a marker in the output
 *     dump                       print the widget tree's labels
 *     quit
 *
 * Why this exists: the proof that a control changes the system is the
 * command it ran and the file it wrote, and the only honest way to get
 * that for sixty controls on a machine without the hardware is to drive
 * the real handlers. This does not fake clicks at the pixel level - it
 * sets the widget's state the way GTK does when a finger does (a switch's
 * "active", a drop-down's "selected", a button's "clicked"), so the same
 * signal handler runs and the same backend command is spawned. Pixel-level
 * input is checked separately with the compositor's own pointer and wtype.
 *
 * It is compiled in, not a test build, so the binary that was tested is
 * the binary that ships. It does nothing unless the variable is set, and
 * anyone who can set this process's environment can already run anything
 * as this user, so it opens no door that was not open.
 */
#include "core.h"

#include <string.h>
#include <stdlib.h>

typedef struct {
    char   **lines;
    int      at;
    int      failures;
} drv_t;

static drv_t D;

/* ── finding widgets ────────────────────────────────────────────────── */

static GList *windows_dialogs_first(void)
{
    GListModel *m = gtk_window_get_toplevels();
    GList *dialogs = NULL, *rest = NULL;
    for (guint i = 0; i < g_list_model_get_n_items(m); i++) {
        GtkWidget *w = g_list_model_get_item(m, i);
        g_object_unref(w);
        if (!gtk_widget_get_visible(w)) continue;
        /* A dialog on its way out (the sheet's exit) is not a target. */
        if (g_object_get_data(G_OBJECT(w), "lp-closing")) continue;
        if (g_object_get_data(G_OBJECT(w), "lp-dialog"))
            dialogs = g_list_prepend(dialogs, w);
        else
            rest = g_list_append(rest, w);
    }
    return g_list_concat(dialogs, rest);
}

typedef gboolean (*pred_fn)(GtkWidget *w, const char *want);

static GtkWidget *walk(GtkWidget *w, pred_fn pred, const char *want)
{
    if (!gtk_widget_get_visible(w)) return NULL;
    if (pred(w, want)) return w;
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *r = walk(c, pred, want);
        if (r) return r;
    }
    return NULL;
}

static GtkWidget *find_any(pred_fn pred, const char *want)
{
    GList *wins = windows_dialogs_first();
    GtkWidget *hit = NULL;
    for (GList *l = wins; l && !hit; l = l->next)
        hit = walk(l->data, pred, want);
    g_list_free(wins);
    return hit;
}

static gboolean is_titled(GtkWidget *w, const char *want)
{
    const char *t = g_object_get_data(G_OBJECT(w), "lp-title");
    return t && !strcmp(t, want) && (GTK_IS_LIST_BOX_ROW(w) || GTK_IS_EDITABLE(w));
}

static const char *button_text(GtkWidget *w)
{
    const char *l = gtk_button_get_label(GTK_BUTTON(w));
    if (l) return l;
    GtkWidget *c = gtk_button_get_child(GTK_BUTTON(w));
    if (c && GTK_IS_LABEL(c)) return gtk_label_get_text(GTK_LABEL(c));
    /* A button whose child is a box with a label in it (cards, swatches). */
    const char *n = g_object_get_data(G_OBJECT(w), "lp-title");
    if (n) return n;
    return gtk_widget_get_tooltip_text(w);
}

static gboolean is_button(GtkWidget *w, const char *want)
{
    if (!GTK_IS_BUTTON(w) || !gtk_widget_is_sensitive(w)) return FALSE;
    const char *t = button_text(w);
    return t && !strcmp(t, want);
}

static gboolean is_search(GtkWidget *w, const char *x)
{
    (void)x;
    return GTK_IS_SEARCH_ENTRY(w);
}

/* ── doing things ───────────────────────────────────────────────────── */

static gboolean set_control(GtkWidget *c, const char *value)
{
    if (GTK_IS_SWITCH(c)) {
        gboolean on = !strcmp(value, "on") || !strcmp(value, "1") || !strcmp(value, "true");
        gtk_switch_set_active(GTK_SWITCH(c), on);
        return TRUE;
    }
    if (GTK_IS_DROP_DOWN(c)) {
        GListModel *m = gtk_drop_down_get_model(GTK_DROP_DOWN(c));
        for (guint i = 0; m && i < g_list_model_get_n_items(m); i++) {
            GtkStringObject *s = g_list_model_get_item(m, i);
            gboolean hit = !strcmp(gtk_string_object_get_string(s), value);
            g_object_unref(s);
            if (hit) {
                gtk_drop_down_set_selected(GTK_DROP_DOWN(c), i);
                return TRUE;
            }
        }
        return FALSE;
    }
    if (GTK_IS_RANGE(c)) {
        gtk_range_set_value(GTK_RANGE(c), g_ascii_strtod(value, NULL));
        return TRUE;
    }
    if (GTK_IS_EDITABLE(c)) {
        gtk_editable_set_text(GTK_EDITABLE(c), value);
        return TRUE;
    }
    if (GTK_IS_BOX(c)) {      /* segmented */
        for (GtkWidget *b = gtk_widget_get_first_child(c); b; b = gtk_widget_get_next_sibling(b))
            if (GTK_IS_TOGGLE_BUTTON(b) && button_text(b) && !strcmp(button_text(b), value)) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), TRUE);
                return TRUE;
            }
    }
    if (GTK_IS_SPIN_BUTTON(c)) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(c), g_ascii_strtod(value, NULL));
        return TRUE;
    }
    return FALSE;
}

static void dump(GtkWidget *w, int depth)
{
    if (!gtk_widget_get_visible(w)) return;
    const char *t = g_object_get_data(G_OBJECT(w), "lp-title");
    const char *label = GTK_IS_LABEL(w) ? gtk_label_get_text(GTK_LABEL(w)) : NULL;
    if (t || (label && *label))
        g_print("%*s%s %s%s%s\n", depth * 2, "", G_OBJECT_TYPE_NAME(w),
                t ? "[" : "", t ? t : label, t ? "]" : "");
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        dump(c, depth + 1);
}

static gboolean step(gpointer p);

static gboolean shot_at(gpointer p)
{
    char *file = p;
    const char *v[] = { "grim", file, NULL };
    GError *e = NULL;
    if (!g_spawn_async(NULL, (char **)v, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &e)) {
        g_print("driver: grim: %s\n", e->message);
        g_error_free(e);
    } else {
        g_print("driver: shot %s at %" G_GINT64_FORMAT " ms\n", file,
                g_get_monotonic_time() / 1000);
    }
    g_free(file);
    return G_SOURCE_REMOVE;
}

static void fail(const char *why, const char *line)
{
    D.failures++;
    g_print("driver: FAIL %s: %s\n", why, line);
}

static gboolean step(gpointer p)
{
    (void)p;
    for (;;) {
        if (!D.lines[D.at]) {
            g_print("driver: end, %d failure(s)\n", D.failures);
            g_application_quit(g_application_get_default());
            return G_SOURCE_REMOVE;
        }
        char *line = g_strstrip(g_strdup(D.lines[D.at++]));
        if (!*line || *line == '#') { g_free(line); continue; }

        int argc = 0;
        char **argv = NULL;
        if (!g_shell_parse_argv(line, &argc, &argv, NULL)) {
            fail("cannot parse", line);
            g_free(line);
            continue;
        }
        g_print("driver: %s\n", line);
        guint delay = 400;
        const char *cmd = argv[0];

        if (!strcmp(cmd, "panel") && argc >= 2) {
            lp_show_panel(argv[1], argc >= 3 ? argv[2] : NULL);
            delay = 700;
        } else if (!strcmp(cmd, "set") && argc >= 3) {
            GtkWidget *w = find_any(is_titled, argv[1]);
            GtkWidget *c = w ? (GTK_IS_LIST_BOX_ROW(w) ? row_control(w) : w) : NULL;
            if (!c) fail("no control titled", argv[1]);
            else if (!gtk_widget_is_sensitive(c)) fail("control is locked", argv[1]);
            else if (!set_control(c, argv[2])) fail("value not accepted", line);
        } else if (!strcmp(cmd, "click") && argc >= 2) {
            GtkWidget *b = find_any(is_button, argv[1]);
            if (!b) fail("no button", argv[1]);
            else g_signal_emit_by_name(b, "clicked");
        } else if (!strcmp(cmd, "tap") && argc >= 2) {
            GtkWidget *r = find_any(is_titled, argv[1]);
            if (!r || !GTK_IS_LIST_BOX_ROW(r)) fail("no row", argv[1]);
            else if (!gtk_list_box_row_get_activatable(GTK_LIST_BOX_ROW(r)))
                fail("row is not tappable", argv[1]);
            else g_signal_emit_by_name(gtk_widget_get_parent(r), "row-activated", r);
        } else if (!strcmp(cmd, "search") && argc >= 2) {
            GtkWidget *e = find_any(is_search, NULL);
            if (!e) fail("no search field", line);
            else gtk_editable_set_text(GTK_EDITABLE(e), argv[1]);
            delay = 600;
        } else if (!strcmp(cmd, "wait") && argc >= 2) {
            delay = (guint)atoi(argv[1]);
        } else if (!strcmp(cmd, "run") && argc >= 2) {
            char *out = NULL, *err = NULL;
            /* A test step may run something slow on purpose (a scan,
             * a sync); two minutes before it counts as hung. */
            int st = lp_run_full_timeout((const char *const *)(argv + 1), NULL, &out, &err,
                                         120 * 1000);
            g_print("driver: run exit %d%s%s%s%s\n", st, out && *out ? " out: " : "",
                    out ? out : "", err && *err ? " err: " : "", err ? err : "");
            g_free(out); g_free(err);
        } else if (!strcmp(cmd, "shots") && argc >= 3) {
            char **ms = g_strsplit(argv[2], ",", -1);
            for (int k = 0; ms[k]; k++) {
                guint at = (guint)atoi(ms[k]);
                char *f = g_strdup_printf("%s-%03ums.png", argv[1], at);
                if (at) g_timeout_add(at, shot_at, f);
                else shot_at(f);
            }
            g_strfreev(ms);
            delay = 0;
        } else if (!strcmp(cmd, "echo")) {
            char *t = g_strjoinv(" ", argv + 1);
            g_print("MARK %s\n", t);
            g_free(t);
            delay = 0;
        } else if (!strcmp(cmd, "dump")) {
            GList *wins = windows_dialogs_first();
            for (GList *l = wins; l; l = l->next) dump(l->data, 0);
            g_list_free(wins);
            delay = 0;
        } else if (!strcmp(cmd, "quit")) {
            D.lines[D.at] = NULL;
        } else {
            fail("unknown step", line);
        }
        g_strfreev(argv);
        g_free(line);
        g_timeout_add(delay ? delay : 1, step, NULL);
        return G_SOURCE_REMOVE;
    }
}

gboolean lp_driver_start(void)
{
    static gboolean started;
    const char *path = g_getenv("LP_SETTINGS_SCRIPT");
    if (!path || started) return FALSE;
    started = TRUE;
    char *body = NULL;
    if (!g_file_get_contents(path, &body, NULL, NULL)) {
        g_printerr("driver: cannot read %s\n", path);
        return FALSE;
    }
    D.lines = g_strsplit(body, "\n", -1);
    g_free(body);
    g_timeout_add(1200, step, NULL);
    return TRUE;
}
