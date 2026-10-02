/*
 * desktop.c - lp-desktop, the wallpaper and the icons on it.
 *
 *     ┌────────────────────────────────────────────────┐
 *     │ bar                                            │
 *     ├──┬─────────────────────────────────────────────┤
 *     │  │ [Documents]                                 │
 *     │d │ [Welcome.txt]                               │
 *     │o │ [hello.sh]                                  │
 *     │c │                  (wallpaper)                │
 *     │k │                                             │
 *
 * One surface per output on the background layer, drawing both the
 * wallpaper and ~/Desktop's icons. They are one program and one surface
 * rather than swaybg plus a transparent icon layer because a transparent
 * full-screen surface above the wallpaper is blended by the compositor
 * on every frame anything on the desktop moves, and an opaque one is
 * copied: the wallpaper surface declares itself opaque.
 *
 * ── the wallpaper ──
 *
 * In order: the image named in ~/.config/lp/wallpaper (one line, a path,
 * written by Settings - or by hand); $LP_SHARE/wallpaper.png, which the
 * image installs from desktop/branding/wallpaper.png when the branding
 * track has made one and from desktop/theme/wallpaper.png until then; and
 * if neither loads, the same deep-blue gradient drawn with cairo, so a
 * missing file is never a black screen. The image is scaled to cover the
 * output once and kept at the output's pixel size; a redraw is a copy.
 * The config file is watched (inotify), so a new choice shows at once.
 *
 * ── the icons ──
 *
 * Every entry of ~/Desktop (the XDG desktop directory), with the icon and
 * name GIO gives it; a .desktop file shows as the application it
 * launches. The icons behave the way a desktop's always have:
 *
 *   - a click selects one icon, Ctrl+click adds it to the selection or
 *     takes it out, a click on the wallpaper clears it; a drag that
 *     starts on the wallpaper draws a band and selects what it touches
 *     (with Ctrl, in addition to what was selected). Selected icons are
 *     lit in the accent colour;
 *   - a double click opens an icon with its default application. A
 *     finger's tap opens it at once, as it always has here: touch has no
 *     hover to select with and a double tap is a poor target;
 *   - a right click (or a long press) is a menu for the selection - Open,
 *     Open in Files for one folder, Rename… for one icon, Move to Trash.
 *     On the wallpaper it offers New Folder, Open Desktop in Files, Files,
 *     Open Terminal Here, Change Background…, Appearance… and Display
 *     Settings…; New Folder makes the folder where the menu was asked
 *     for and opens its name for typing straight away;
 *   - keys, once the desktop has been clicked: Delete moves the selection
 *     to the trash, Enter opens it, F2 renames, Ctrl+A selects all,
 *     Escape clears. The surface asks for the keyboard on demand
 *     (layer-shell's ON_DEMAND): the compositor gives it the keyboard
 *     when it is clicked and takes it away when a window is, the way a
 *     window's focus works, and it never takes it on its own.
 *
 * Carrying icons is drag and drop, the real one: a press on an icon that
 * moves 8 px (12 for a finger) picks up the whole selection. Let go on
 * the wallpaper and the icons settle into the nearest free cells, each
 * where it was relative to the one under the pointer; let go on a folder
 * and they are moved into it; on a trash can, trashed; on a Files window,
 * Files takes them (text/uri-list). The same surface is a drop target for
 * files from anywhere else: they are moved into ~/Desktop when they are
 * on the same disk, copied when they are not or when Ctrl is held (as
 * far as the compositor lets us see it - wlroots gives nobody the
 * keyboard during a drag), and land in the cell under the pointer.
 *
 * Icons sit on a 112x120 grid starting at the left edge and below the
 * bar, filling the first column downwards, the way the owner's mockup has
 * Documents, Welcome.txt and hello.sh. Where each icon was put is
 * remembered in ~/.config/lp/desktop-icons (a GKeyFile, name=column,row),
 * written atomically and durably, and restored at the next start. Icons
 * nobody moved flow into the free cells in name order.
 *
 * ~/Desktop is watched, so a file saved there appears without a refresh.
 */
#define _GNU_SOURCE 1

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>

#include "lp-motion.h"
#include "lp-shell.h"

#define CELL_W 112
#define CELL_H 120
#define ORIGIN_X 24            /* the dock is at the bottom now; icons start at the left edge */
#define ORIGIN_Y (40 + 16)     /* below the bar (panel.c: BAR_HEIGHT) */
#define ICON_PX 56
#define DRAG_MOUSE 8           /* px a press moves before it is a drag: GTK's own threshold */
#define DRAG_TOUCH 12          /* a finger wobbles more than a mouse */

typedef struct Desk Desk;

typedef struct {
    Desk      *desk;
    GFile     *file;
    char      *name;           /* file name: the key positions are kept under */
    GtkWidget *button;
    int        col, row;       /* its cell */
    gboolean   placed;         /* the cell came from the saved positions */
    gboolean   is_dir;         /* things dropped on it go into it */
    gboolean   is_trash;       /* things dropped on it go to the trash */
    gboolean   selected;
    gboolean   band_base;      /* selected before the band began */
    /* settling: pixel position of the button */
    LpSpring   sx, sy;
    LpMotion  *motion;
} Icon;

typedef enum {
    PRESS_NONE,                /* nothing, or a double click already done */
    PRESS_CLICK,               /* down, not moved far yet */
    PRESS_BAND,                /* drawing the selection band */
    PRESS_CARRY,               /* handed to drag and drop */
} PressMode;

struct Desk {
    GdkMonitor *mon;
    GtkWindow  *win;
    GtkWidget  *layout;        /* GtkLayout: icons at pixel positions */
    GPtrArray  *icons;
    cairo_surface_t *wall;     /* at the output's size and scale */
    int         wall_w, wall_h;
    int         grid_cols, grid_rows; /* the grid place_all() last filled */
    /* the primary press in progress */
    PressMode   mode;
    char       *press_name;    /* the icon it went down on; NULL: the wallpaper */
    gboolean    press_ctrl, press_touch;
    double      press_x, press_y;
    GdkRectangle band;         /* layout coordinates, while mode == PRESS_BAND */
    /* a drag over this desk */
    Icon       *drop_on;       /* the folder lit under it */
    double      drop_x, drop_y;
    /* kept across fill(): names to show selected, and one to rename */
    GHashTable *want_selected;
    char       *want_rename;
    double      menu_x, menu_y; /* where the wallpaper's menu was asked for */
    /* the rename in progress */
    GtkWidget  *rename_pop, *rename_entry, *rename_msg;
    char       *rename_name;
    gboolean    rename_fresh;  /* a folder just made: "Folder Name", not "Rename" */
    gboolean    rename_stem;   /* the name's stem still to be selected */
};

static GList *desks;
static GFileMonitor *dir_monitor, *wall_monitor;
static GKeyFile *positions;

/* The drag of ours in flight, if any - a seat has one pointer, so one at
 * a time. Names rather than Icons: ~/Desktop can change under a drag
 * (Files, dropped on, moves the file out), and fill() replaces every Icon
 * when it does. */
static struct {
    Desk      *desk;
    GPtrArray *names;          /* NULL: no drag of ours */
    char      *lead;           /* the icon under the pointer */
    double     off_x, off_y;   /* the pointer, inside the lead icon */
    double     pic_x, pic_y;   /* the lead, inside the drag icon */
    gboolean   corner;         /* the drag icon hangs by its corner */
} carry;

static const GtkTargetEntry uri_target[] = { { (char *)"text/uri-list", 0, 0 } };

static void fill(Desk *d);
static void rename_begin(Desk *d, Icon *ic, const char *text, gboolean fresh);

/* ── positions ───────────────────────────────────────────────────── */

static void positions_load(void)
{
    positions = g_key_file_new();
    char *p = lp_config_path("desktop-icons");
    g_key_file_load_from_file(positions, p, G_KEY_FILE_NONE, NULL);
    g_free(p);
}

static void positions_save(void)
{
    gsize len = 0;
    char *data = g_key_file_to_data(positions, &len, NULL);
    lp_config_write("desktop-icons", data);
    g_free(data);
}

static void position_set(const char *name, int col, int row)
{
    char v[32];
    g_snprintf(v, sizeof v, "%d,%d", col, row);
    g_key_file_set_string(positions, "positions", name, v);
}

static void position_forget(const char *name)
{
    g_key_file_remove_key(positions, "positions", name, NULL);
}

static char *desktop_dir(void)
{
    const char *d = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    /* GLib answers $HOME when there is no user-dirs.dirs entry, and the
     * home directory is not the desktop. */
    if (!d || g_strcmp0(d, g_get_home_dir()) == 0)
        return g_build_filename(g_get_home_dir(), "Desktop", NULL);
    return g_strdup(d);
}

static GFile *desktop_file(void)
{
    char *dir = desktop_dir();
    GFile *f = g_file_new_for_path(dir);
    g_free(dir);
    return f;
}

static char *trash_dir(void)
{
    return g_build_filename(g_get_user_data_dir(), "Trash", "files", NULL);
}

/* ── the wallpaper ───────────────────────────────────────────────── */

/* The theme's deep blue: teal light top-left, into near-black navy. The
 * same stops desktop/branding/src/wallpaper.py draws the wallpaper with. */
static void draw_gradient(cairo_t *cr, double W, double H)
{
    double cx = 0.18 * W, cy = 0.12 * H;
    double R = hypot(W - cx, H - cy);
    cairo_pattern_t *p = cairo_pattern_create_radial(cx, cy, 0, cx, cy, R);
    cairo_pattern_add_color_stop_rgb(p, 0.00, 0x23 / 255.0, 0xa6 / 255.0, 0xa0 / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 0.20, 0x17 / 255.0, 0x75 / 255.0, 0x85 / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 0.46, 0x12 / 255.0, 0x47 / 255.0, 0x6e / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 0.74, 0x0d / 255.0, 0x28 / 255.0, 0x46 / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 1.00, 0x07 / 255.0, 0x12 / 255.0, 0x1f / 255.0);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
}

static GdkPixbuf *load_wallpaper(void)
{
    char *chosen = lp_config_read("wallpaper");
    GdkPixbuf *pb = NULL;
    if (chosen && *chosen)
        pb = gdk_pixbuf_new_from_file(chosen, NULL);
    if (!pb && chosen && *chosen)
        g_printerr("lp-desktop: %s: cannot load; using the default\n", chosen);
    g_free(chosen);
    if (!pb) {
        char *def = lp_share_path("wallpaper.png");
        pb = gdk_pixbuf_new_from_file(def, NULL);
        g_free(def);
    }
    return pb;
}

/* Scaled to cover the output once, at its pixel size: every redraw after
 * this - an icon pressed, an icon dragged - is a plain copy of the part
 * that changed, never a resample. */
static void make_wall(Desk *d)
{
    int W = gtk_widget_get_allocated_width(GTK_WIDGET(d->win));
    int H = gtk_widget_get_allocated_height(GTK_WIDGET(d->win));
    if (W < 2 || H < 2)
        return;
    int sf = gtk_widget_get_scale_factor(GTK_WIDGET(d->win));
    if (d->wall && d->wall_w == W && d->wall_h == H)
        return;
    if (d->wall)
        cairo_surface_destroy(d->wall);
    d->wall = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W * sf, H * sf);
    cairo_surface_set_device_scale(d->wall, sf, sf);
    d->wall_w = W;
    d->wall_h = H;
    cairo_t *cr = cairo_create(d->wall);
    GdkPixbuf *pb = load_wallpaper();
    if (pb) {
        double iw = gdk_pixbuf_get_width(pb), ih = gdk_pixbuf_get_height(pb);
        double s = MAX(W / iw, H / ih);
        cairo_translate(cr, (W - iw * s) / 2, (H - ih * s) / 2);
        cairo_scale(cr, s, s);
        gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        g_object_unref(pb);
    } else {
        draw_gradient(cr, W, H);
    }
    cairo_destroy(cr);
}

static gboolean desk_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    Desk *d = data;
    (void)w;
    make_wall(d);
    if (d->wall) {
        cairo_set_source_surface(cr, d->wall, 0, 0);
        cairo_paint(cr);
    }
    return FALSE;      /* and then the icons, drawn by the layout */
}

/* The band, over the icons: connected after the layout has drawn them,
 * and only in the pass for the window the icons live in. */
static gboolean band_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    Desk *d = data;
    if (d->mode != PRESS_BAND ||
        !gtk_cairo_should_draw_window(cr, gtk_layout_get_bin_window(GTK_LAYOUT(w))))
        return FALSE;
    cairo_rectangle(cr, d->band.x + 0.5, d->band.y + 0.5,
                    MAX(d->band.width - 1, 0), MAX(d->band.height - 1, 0));
    cairo_set_source_rgba(cr, 0xf2 / 255.0, 0x8c / 255.0, 0x28 / 255.0, 0.18);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0xff / 255.0, 0xa2 / 255.0, 0x4a / 255.0, 0.9);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    return FALSE;
}

static void on_wall_changed(GFileMonitor *m, GFile *f, GFile *o,
                            GFileMonitorEvent ev, gpointer u)
{
    (void)m; (void)f; (void)o; (void)u;
    if (ev != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
        ev != G_FILE_MONITOR_EVENT_CREATED && ev != G_FILE_MONITOR_EVENT_DELETED)
        return;
    for (GList *l = desks; l; l = l->next) {
        Desk *d = l->data;
        if (d->wall)
            cairo_surface_destroy(d->wall);
        d->wall = NULL;
        gtk_widget_queue_draw(GTK_WIDGET(d->win));
    }
}

/* ── opening things ──────────────────────────────────────────────── */

static void open_file(GFile *f)
{
    char *path = g_file_get_path(f);
    if (path && g_str_has_suffix(path, ".desktop")) {
        GDesktopAppInfo *a = g_desktop_app_info_new_from_filename(path);
        if (a) {
            GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
            g_app_info_launch(G_APP_INFO(a), NULL, G_APP_LAUNCH_CONTEXT(ctx), NULL);
            g_object_unref(ctx);
            g_object_unref(a);
            g_free(path);
            return;
        }
    }
    g_free(path);
    char *uri = g_file_get_uri(f);
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
    GError *err = NULL;
    if (!g_app_info_launch_default_for_uri(uri, G_APP_LAUNCH_CONTEXT(ctx), &err)) {
        g_printerr("lp-desktop: %s: %s\n", uri, err->message);
        g_clear_error(&err);
    }
    g_object_unref(ctx);
    g_free(uri);
}

static void open_in_files(GFile *f)
{
    char *path = g_file_get_path(f);
    const char *a[] = { lp_have("lp-files") ? "lp-files" : "xdg-open", path, NULL };
    lp_spawn(a);
    g_free(path);
}

static void icon_open(Icon *ic)
{
    /* A trash launcher (Type=Link) is not an application GIO can start. */
    if (ic->is_trash && !ic->is_dir) {
        char *t = trash_dir();
        GFile *f = g_file_new_for_path(t);
        open_in_files(f);
        g_object_unref(f);
        g_free(t);
        return;
    }
    open_file(ic->file);
}

/* ── icon placement ──────────────────────────────────────────────── */

/* The grid is measured on the monitor, which the surface covers, and not
 * on the window: the window has no size until the compositor's first
 * configure, and a grid measured before then was one cell - every saved
 * position outside it was forgotten, at every start. */
static int rows_on(Desk *d)
{
    GdkRectangle g;
    gdk_monitor_get_geometry(d->mon, &g);
    int r = (g.height - ORIGIN_Y - 8) / CELL_H;
    return r > 0 ? r : 1;
}

static int cols_on(Desk *d)
{
    GdkRectangle g;
    gdk_monitor_get_geometry(d->mon, &g);
    int c = (g.width - ORIGIN_X - 8) / CELL_W;
    return c > 0 ? c : 1;
}

static gboolean cell_taken(Desk *d, int col, int row, Icon *except, GArray *reserved)
{
    for (guint i = 0; i < d->icons->len; i++) {
        Icon *ic = g_ptr_array_index(d->icons, i);
        if (ic != except && ic->placed && ic->col == col && ic->row == row)
            return TRUE;
    }
    for (guint i = 0; reserved && i + 1 < reserved->len; i += 2)
        if (g_array_index(reserved, int, i) == col && g_array_index(reserved, int, i + 1) == row)
            return TRUE;
    return FALSE;
}

/* The free cell nearest (col, row) - that cell itself when it is free.
 * reserved holds col,row pairs already promised in the same drop. */
static void nearest_free(Desk *d, int *col, int *row, Icon *except, GArray *reserved)
{
    int cols = cols_on(d), rows = rows_on(d);
    int c0 = CLAMP(*col, 0, cols - 1), r0 = CLAMP(*row, 0, rows - 1);
    int best = -1, bc = c0, br = r0;
    for (int c = 0; c < cols; c++)
        for (int r = 0; r < rows; r++)
            if (!cell_taken(d, c, r, except, reserved)) {
                int dist = (c - c0) * (c - c0) + (r - r0) * (r - r0);
                if (best < 0 || dist < best) { best = dist; bc = c; br = r; }
            }
    *col = bc;
    *row = br;
}

static void cell_xy(int col, int row, double *x, double *y)
{
    *x = ORIGIN_X + col * CELL_W;
    *y = ORIGIN_Y + row * CELL_H;
}

/* The cell a point on the desktop falls in. */
static void cell_at(double x, double y, int *col, int *row)
{
    *col = (int)floor((x - ORIGIN_X) / CELL_W);
    *row = (int)floor((y - ORIGIN_Y) / CELL_H);
}

static void icon_move_to(Icon *ic, double x, double y)
{
    gtk_layout_move(GTK_LAYOUT(ic->desk->layout), ic->button, (int)lround(x), (int)lround(y));
}

static void icon_frame(GtkWidget *w, gpointer data)
{
    (void)w;
    Icon *ic = data;
    icon_move_to(ic, ic->sx.x, ic->sy.x);
}

/* Put every icon without a saved cell into the free cells, column by
 * column, top to bottom. */
static void place_all(Desk *d)
{
    int rows = rows_on(d), cols = cols_on(d);
    d->grid_cols = cols;
    d->grid_rows = rows;
    for (guint i = 0; i < d->icons->len; i++) {
        Icon *ic = g_ptr_array_index(d->icons, i);
        if (ic->placed && (ic->col >= cols || ic->row >= rows))
            ic->placed = FALSE;          /* off a smaller screen: re-flow */
        if (ic->placed)
            continue;
        for (int n = 0; n < rows * cols; n++) {
            int c = n / rows, r = n % rows;
            if (!cell_taken(d, c, r, ic, NULL)) {
                ic->col = c;
                ic->row = r;
                ic->placed = TRUE;
                break;
            }
        }
    }
    for (guint i = 0; i < d->icons->len; i++) {
        Icon *ic = g_ptr_array_index(d->icons, i);
        double x, y;
        cell_xy(ic->col, ic->row, &x, &y);
        lp_spring_jump(&ic->sx, x);
        lp_spring_jump(&ic->sy, y);
        icon_move_to(ic, x, y);
    }
}

/* Slide an icon from where it is drawn now into its cell, on the slide
 * spring, and remember the cell. */
static void icon_settle(Icon *ic, int col, int row)
{
    ic->col = col;
    ic->row = row;
    ic->placed = TRUE;
    double x, y;
    cell_xy(col, row, &x, &y);
    lp_spring_set_target(&ic->sx, x);
    lp_spring_set_target(&ic->sy, y);
    lp_motion_kick(ic->motion);
    position_set(ic->name, col, row);
}

/* ── the selection ───────────────────────────────────────────────── */

static Icon *icon_named(Desk *d, const char *name)
{
    for (guint i = 0; name && i < d->icons->len; i++) {
        Icon *ic = g_ptr_array_index(d->icons, i);
        if (strcmp(ic->name, name) == 0)
            return ic;
    }
    return NULL;
}

static void icon_rect(Icon *ic, GdkRectangle *r)
{
    int w = gtk_widget_get_allocated_width(ic->button);
    int h = gtk_widget_get_allocated_height(ic->button);
    r->x = (int)lround(ic->sx.x);
    r->y = (int)lround(ic->sy.x);
    r->width = w > 1 ? w : CELL_W - 8;
    r->height = h > 1 ? h : CELL_H - 8;
}

static Icon *icon_at(Desk *d, double x, double y)
{
    for (guint i = 0; i < d->icons->len; i++) {
        Icon *ic = g_ptr_array_index(d->icons, i);
        GdkRectangle r;
        icon_rect(ic, &r);
        if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height)
            return ic;
    }
    return NULL;
}

static void icon_set_selected(Icon *ic, gboolean on)
{
    if (ic->selected == on)
        return;
    ic->selected = on;
    GtkStyleContext *sc = gtk_widget_get_style_context(ic->button);
    if (on)
        gtk_style_context_add_class(sc, "lp-selected");
    else
        gtk_style_context_remove_class(sc, "lp-selected");
}

/* keep: the one icon left selected, or NULL for none. */
static void select_only(Desk *d, Icon *keep)
{
    for (guint i = 0; i < d->icons->len; i++) {
        Icon *ic = g_ptr_array_index(d->icons, i);
        icon_set_selected(ic, ic == keep);
    }
}

/* The selected icons, in grid order. Not owned: good until the next
 * fill(), which is to say for the length of one handler. */
static GPtrArray *selection(Desk *d)
{
    GPtrArray *a = g_ptr_array_new();
    for (guint i = 0; i < d->icons->len; i++) {
        Icon *ic = g_ptr_array_index(d->icons, i);
        if (ic->selected)
            g_ptr_array_add(a, ic);
    }
    return a;
}

static void drop_light(Desk *d, Icon *on)
{
    if (d->drop_on == on)
        return;
    if (d->drop_on)
        gtk_style_context_remove_class(gtk_widget_get_style_context(d->drop_on->button), "lp-drop");
    d->drop_on = on;
    if (on)
        gtk_style_context_add_class(gtk_widget_get_style_context(on->button), "lp-drop");
}

/* ── moving files ────────────────────────────────────────────────── */

/* dir/name when that is free, else "name (2)", "name (3)"… with a file's
 * extension kept at the end - what lp-files calls a second copy. taken:
 * names already given out in the same drop. */
static GFile *free_child(GFile *dir, const char *name, gboolean is_dir, GPtrArray *taken)
{
    const char *ext = is_dir ? NULL : strrchr(name, '.');
    if (!ext || ext == name)
        ext = name + strlen(name);
    int stem = (int)(ext - name);
    for (int n = 1; ; n++) {
        char *c = n == 1 ? g_strdup(name) : g_strdup_printf("%.*s (%d)%s", stem, name, n, ext);
        gboolean busy = FALSE;
        for (guint i = 0; taken && i < taken->len; i++)
            if (g_strcmp0(g_ptr_array_index(taken, i), c) == 0)
                busy = TRUE;
        GFile *f = g_file_get_child(dir, c);
        g_free(c);
        /* At 1000 give up looking: the move then fails on the existing
         * file instead of overwriting it. */
        if (!busy && (n >= 1000 || !g_file_query_exists(f, NULL)))
            return f;
        g_object_unref(f);
    }
}

static char *fs_id(GFile *f, GFileQueryInfoFlags flags)
{
    GFileInfo *i = g_file_query_info(f, G_FILE_ATTRIBUTE_ID_FILESYSTEM, flags, NULL, NULL);
    char *id = i ? g_strdup(g_file_info_get_attribute_string(i, G_FILE_ATTRIBUTE_ID_FILESYSTEM)) : NULL;
    g_clear_object(&i);
    return id;
}

/* Whether src can be renamed into dir: the same filesystem. src itself
 * (a link is moved as a link), dir followed (a link to a folder on a
 * stick is the stick). */
static gboolean same_fs(GFile *src, GFile *dir)
{
    char *a = fs_id(src, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS);
    char *b = fs_id(dir, G_FILE_QUERY_INFO_NONE);
    gboolean same = a && b && strcmp(a, b) == 0;
    g_free(a);
    g_free(b);
    return same;
}

typedef struct {
    GFile   *src, *dst;
    gboolean move;
} Xfer;

static void xfer_free(gpointer p)
{
    Xfer *x = p;
    g_object_unref(x->src);
    g_object_unref(x->dst);
    g_free(x);
}

/* g_file_copy, and for a folder everything in it: GIO copies one file. */
static gboolean copy_tree(GFile *src, GFile *dst, GCancellable *c, GError **err)
{
    if (g_file_equal(src, dst) || g_file_has_prefix(dst, src)) {
        g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "cannot copy a folder into itself");
        return FALSE;
    }
    if (g_file_query_file_type(src, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, c) != G_FILE_TYPE_DIRECTORY)
        return g_file_copy(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA,
                           c, NULL, NULL, err);
    if (!g_file_make_directory(dst, c, err))
        return FALSE;
    GFileEnumerator *en = g_file_enumerate_children(src, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, c, err);
    if (!en)
        return FALSE;
    gboolean ok = TRUE;
    GFileInfo *info;
    while (ok && (info = g_file_enumerator_next_file(en, c, err))) {
        GFile *s = g_file_get_child(src, g_file_info_get_name(info));
        GFile *t = g_file_get_child(dst, g_file_info_get_name(info));
        ok = copy_tree(s, t, c, err);
        g_object_unref(s);
        g_object_unref(t);
        g_object_unref(info);
    }
    if (ok && err && *err)
        ok = FALSE;                      /* next_file failed */
    g_object_unref(en);
    return ok;
}

/* In a thread: a copy from a stick can take minutes, and the desktop
 * must not stop drawing for it. What appears is the monitor's business. */
static void xfer_thread(GTask *t, gpointer src_obj, gpointer data, GCancellable *c)
{
    (void)src_obj;
    GPtrArray *xs = data;
    GString *errs = g_string_new(NULL);
    for (guint i = 0; i < xs->len; i++) {
        Xfer *x = g_ptr_array_index(xs, i);
        GError *err = NULL;
        gboolean ok;
        if (x->move) {
            ok = g_file_move(x->src, x->dst, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA,
                             c, NULL, NULL, &err);
            /* A folder that cannot be renamed there after all: copied,
             * and the original left where it was rather than half gone. */
            if (!ok && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)) {
                g_clear_error(&err);
                ok = copy_tree(x->src, x->dst, c, &err);
            }
        } else {
            ok = copy_tree(x->src, x->dst, c, &err);
        }
        if (!ok) {
            char *n = g_file_get_parse_name(x->src);
            g_string_append_printf(errs, "%s: %s\n", n, err ? err->message : "failed");
            g_free(n);
        }
        g_clear_error(&err);
    }
    g_task_return_pointer(t, g_string_free(errs, FALSE), g_free);
}

static void xfer_done(GObject *o, GAsyncResult *r, gpointer data)
{
    (void)o; (void)data;
    char *errs = g_task_propagate_pointer(G_TASK(r), NULL);
    if (errs && *errs)
        g_printerr("lp-desktop: %s", errs);
    g_free(errs);
}

/* Everything in srcs into dir: moved when it is on the same disk and copy
 * is FALSE, copied otherwise - what every file manager does, and what
 * lp-files does. Returns the name each will have in dir, in the order of
 * srcs: its own name for what is already there, NULL for what is skipped
 * (dir itself, or a folder dropped into itself). */
static GPtrArray *transfer(GPtrArray *srcs, GFile *dir, gboolean copy)
{
    GPtrArray *xs = g_ptr_array_new_with_free_func(xfer_free);
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < srcs->len; i++) {
        GFile *s = g_ptr_array_index(srcs, i);
        GFile *parent = g_file_get_parent(s);
        gboolean here = parent && g_file_equal(parent, dir);
        g_clear_object(&parent);
        char *base = g_file_get_basename(s);
        if (here || g_file_equal(s, dir) || g_file_has_prefix(dir, s)) {
            g_ptr_array_add(names, here ? base : NULL);
            if (!here)
                g_free(base);
            continue;
        }
        gboolean is_dir = g_file_query_file_type(s, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL)
                          == G_FILE_TYPE_DIRECTORY;
        Xfer *x = g_new0(Xfer, 1);
        x->src = g_object_ref(s);
        x->dst = free_child(dir, base, is_dir, names);
        x->move = !copy && same_fs(s, dir);
        g_ptr_array_add(names, g_file_get_basename(x->dst));
        g_ptr_array_add(xs, x);
        g_free(base);
    }
    if (xs->len) {
        GTask *t = g_task_new(NULL, NULL, xfer_done, NULL);
        g_task_set_task_data(t, xs, (GDestroyNotify)g_ptr_array_unref);
        g_task_run_in_thread(t, xfer_thread);
        g_object_unref(t);
    } else {
        g_ptr_array_unref(xs);
    }
    return names;
}

/* g_file_trash is a rename into ~/.local/share/Trash plus the .trashinfo
 * that lets lp-files put it back; it does not wait on anything. */
static void trash_files(GPtrArray *files)
{
    GFile *desk = desktop_file();
    gboolean forgot = FALSE;
    for (guint i = 0; i < files->len; i++) {
        GFile *f = g_ptr_array_index(files, i);
        GError *err = NULL;
        if (g_file_trash(f, NULL, &err)) {
            GFile *parent = g_file_get_parent(f);
            if (parent && g_file_equal(parent, desk)) {
                char *base = g_file_get_basename(f);
                position_forget(base);
                g_free(base);
                forgot = TRUE;
            }
            g_clear_object(&parent);
        } else {
            char *n = g_file_get_parse_name(f);
            g_printerr("lp-desktop: trash: %s: %s\n", n, err->message);
            g_free(n);
            g_clear_error(&err);
        }
    }
    if (forgot)
        positions_save();
    g_object_unref(desk);
}

/* ── acting on the selection ─────────────────────────────────────── */

static void act_open(Desk *d)
{
    GPtrArray *sel = selection(d);
    for (guint i = 0; i < sel->len; i++)
        icon_open(g_ptr_array_index(sel, i));
    g_ptr_array_unref(sel);
}

static void act_trash(Desk *d)
{
    GPtrArray *sel = selection(d);
    GPtrArray *files = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < sel->len; i++)
        g_ptr_array_add(files, g_object_ref(((Icon *)g_ptr_array_index(sel, i))->file));
    g_ptr_array_unref(sel);
    trash_files(files);
    g_ptr_array_unref(files);
}

static void act_rename(Desk *d)
{
    GPtrArray *sel = selection(d);
    if (sel->len == 1)
        rename_begin(d, g_ptr_array_index(sel, 0), NULL, FALSE);
    g_ptr_array_unref(sel);
}

/* ── renaming ────────────────────────────────────────────────────── */

static gboolean destroy_later(gpointer w)
{
    gtk_widget_destroy(w);
    return G_SOURCE_REMOVE;
}

static void rename_close(Desk *d)
{
    if (!d->rename_pop)
        return;
    GtkWidget *p = d->rename_pop;
    d->rename_pop = d->rename_entry = d->rename_msg = NULL;
    g_clear_pointer(&d->rename_name, g_free);
    gtk_widget_destroy(p);
}

/* Escape, or a press outside it: the name stays as it was. The popover
 * is destroyed from idle - "closed" comes from inside its own handling. */
static void on_rename_closed(GtkPopover *p, gpointer data)
{
    Desk *d = data;
    if (d->rename_pop != GTK_WIDGET(p))
        return;
    d->rename_pop = d->rename_entry = d->rename_msg = NULL;
    g_clear_pointer(&d->rename_name, g_free);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, destroy_later, g_object_ref(p), g_object_unref);
}

static void rename_commit(Desk *d)
{
    if (!d->rename_pop)
        return;
    char *want = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(d->rename_entry))));
    char *why = NULL;
    if (!*want)
        why = g_strdup(T("The name cannot be empty.", "이름이 비어 있습니다."));
    else if (strchr(want, '/'))
        why = g_strdup(T("A name cannot contain “/”.", "이름에 “/”는 쓸 수 없습니다."));
    else if (!strcmp(want, ".") || !strcmp(want, ".."))
        why = g_strdup(T("That name cannot be used.", "그 이름은 쓸 수 없습니다."));
    else if (strcmp(want, d->rename_name) != 0) {
        GFile *desk = desktop_file();
        GFile *f = g_file_get_child(desk, d->rename_name);
        GError *err = NULL;
        GFile *nf = g_file_set_display_name(f, want, NULL, &err);
        if (nf) {
            /* The icon keeps its place and its selection under the new name. */
            char *v = g_key_file_get_string(positions, "positions", d->rename_name, NULL);
            if (v) {
                g_key_file_set_string(positions, "positions", want, v);
                position_forget(d->rename_name);
                positions_save();
                g_free(v);
            }
            g_hash_table_add(d->want_selected, g_strdup(want));
            g_object_unref(nf);
        } else if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
            why = g_strdup(T("Something here already has that name.", "같은 이름이 이미 있습니다."));
        } else {
            why = g_strdup(err->message);
        }
        g_clear_error(&err);
        g_object_unref(f);
        g_object_unref(desk);
    }
    g_free(want);
    if (why) {
        gtk_label_set_text(GTK_LABEL(d->rename_msg), why);
        gtk_widget_show(d->rename_msg);
        g_free(why);
        return;
    }
    rename_close(d);
}

static void on_rename_activate(GtkWidget *w, gpointer data) { (void)w; rename_commit(data); }

/* The name without its extension selected, ready to be typed over, the
 * way every file manager hands over a name. After each of GtkEntry's own
 * grab_focus, which selects all of it (gtk-entry-select-on-focus) - the
 * popover's map grabs the focus more than once - until the owner first
 * clicks or types in the field. */
static void on_rename_grab(GtkWidget *e, gpointer data)
{
    Desk *d = data;
    if (!d->rename_stem)
        return;
    const char *t = gtk_entry_get_text(GTK_ENTRY(e));
    const char *dot = strrchr(t, '.');
    Icon *ic = icon_named(d, d->rename_name);
    int end = (!ic || ic->is_dir || !dot || dot == t) ? -1 : (int)g_utf8_pointer_to_offset(t, dot);
    gtk_editable_select_region(GTK_EDITABLE(e), 0, end);
}

static gboolean on_rename_touched(GtkWidget *e, GdkEvent *ev, gpointer data)
{
    (void)e; (void)ev;
    ((Desk *)data)->rename_stem = FALSE;
    return FALSE;
}

static void on_rename_map(GtkWidget *pop, gpointer data)
{
    (void)pop;
    Desk *d = data;
    if (d->rename_entry)
        gtk_widget_grab_focus(d->rename_entry);
}

/* A popover under the icon, inside the desktop's own surface: the surface
 * has the keyboard, because it was just clicked (ON_DEMAND). text: what
 * was typed before a fill() replaced the icon, or NULL for its name. */
static void rename_begin(Desk *d, Icon *ic, const char *text, gboolean fresh)
{
    rename_close(d);
    d->rename_name = g_strdup(ic->name);
    d->rename_fresh = fresh;
    d->rename_stem = text == NULL;

    GtkWidget *pop = gtk_popover_new(d->layout);
    gtk_style_context_add_class(gtk_widget_get_style_context(pop), "lp-note");
    gtk_style_context_add_class(gtk_widget_get_style_context(pop), "lp-rename");
    double x, y;
    cell_xy(ic->col, ic->row, &x, &y);
    GdkRectangle r = { (int)x, (int)y, CELL_W - 8, CELL_H - 8 };
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *t = gtk_label_new(fresh ? T("Folder Name", "폴더 이름") : T("Rename", "이름 바꾸기"));
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "lp-note-title");
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), text ? text : ic->name);
    gtk_entry_set_width_chars(GTK_ENTRY(e), 24);
    g_signal_connect(e, "activate", G_CALLBACK(on_rename_activate), d);
    g_signal_connect_after(e, "grab-focus", G_CALLBACK(on_rename_grab), d);
    g_signal_connect(e, "button-press-event", G_CALLBACK(on_rename_touched), d);
    g_signal_connect(e, "key-press-event", G_CALLBACK(on_rename_touched), d);
    gtk_box_pack_start(GTK_BOX(row), e, TRUE, TRUE, 0);
    GtkWidget *b = gtk_button_new_with_label(fresh ? T("Done", "완료") : T("Rename", "바꾸기"));
    g_signal_connect(b, "clicked", G_CALLBACK(on_rename_activate), d);
    gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    GtkWidget *msg = gtk_label_new(NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(msg), "lp-rename-msg");
    gtk_widget_set_halign(msg, GTK_ALIGN_START);
    gtk_widget_set_no_show_all(msg, TRUE);
    gtk_box_pack_start(GTK_BOX(box), msg, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(pop), box);
    gtk_widget_show_all(box);

    d->rename_pop = pop;
    d->rename_entry = e;
    d->rename_msg = msg;
    g_signal_connect(pop, "closed", G_CALLBACK(on_rename_closed), d);
    g_signal_connect_after(pop, "map", G_CALLBACK(on_rename_map), d);
    gtk_popover_popup(GTK_POPOVER(pop));
}

/* ── menus ───────────────────────────────────────────────────────── */

static void m_open(GtkMenuItem *m, gpointer d) { (void)m; act_open(d); }
static void m_rename(GtkMenuItem *m, gpointer d) { (void)m; act_rename(d); }
static void m_trash(GtkMenuItem *m, gpointer d) { (void)m; act_trash(d); }
static void m_files(GtkMenuItem *m, gpointer d)
{
    (void)m;
    GPtrArray *sel = selection(d);
    if (sel->len == 1)
        open_in_files(((Icon *)g_ptr_array_index(sel, 0))->file);
    g_ptr_array_unref(sel);
}

static void menu_item(GtkWidget *menu, const char *label, GCallback cb, gpointer d,
                      gboolean danger)
{
    GtkWidget *mi = gtk_menu_item_new_with_label(label);
    if (danger)
        gtk_style_context_add_class(gtk_widget_get_style_context(mi), "lp-danger");
    g_signal_connect(mi, "activate", cb, d);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

static void icon_menu(Desk *d, GtkWidget *w, double x, double y)
{
    GPtrArray *sel = selection(d);
    if (!sel->len) {
        g_ptr_array_unref(sel);
        return;
    }
    Icon *one = sel->len == 1 ? g_ptr_array_index(sel, 0) : NULL;
    GtkWidget *menu = gtk_menu_new();
    char *title = one ? g_strdup(one->name)
                      : g_strdup_printf(T("%u items", "항목 %u개"), sel->len);
    GtkWidget *head = gtk_menu_item_new_with_label(title);
    g_free(title);
    gtk_widget_set_sensitive(head, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), head);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, T("Open", "열기"), G_CALLBACK(m_open), d, FALSE);
    if (one && one->is_dir)
        menu_item(menu, T("Open in Files", "파일에서 열기"), G_CALLBACK(m_files), d, FALSE);
    if (one)
        menu_item(menu, T("Rename…", "이름 바꾸기…"), G_CALLBACK(m_rename), d, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, T("Move to Trash", "휴지통으로 이동"), G_CALLBACK(m_trash), d, TRUE);
    g_ptr_array_unref(sel);
    gtk_widget_show_all(menu);
    lp_menu_destroy_when_closed(menu);
    lp_menu_popup_at(menu, w, x, y);
}

/* New Folder: made in the cell the menu was asked for, and its name
 * opened for typing as soon as the folder shows (fill() does that). */
static void m_new_folder(GtkMenuItem *m, gpointer data)
{
    (void)m;
    Desk *d = data;
    char *dir = desktop_dir();
    char *made = NULL;
    for (int n = 1; n < 100 && !made; n++) {
        char *name = n == 1 ? g_strdup(T("New Folder", "새 폴더"))
                            : g_strdup_printf("%s %d", T("New Folder", "새 폴더"), n);
        char *p = g_build_filename(dir, name, NULL);
        if (g_mkdir(p, 0755) == 0)
            made = g_strdup(name);
        else if (errno != EEXIST) {
            g_printerr("lp-desktop: %s: %s\n", p, g_strerror(errno));
            n = 100;
        }
        g_free(p);
        g_free(name);
    }
    g_free(dir);
    if (!made)
        return;
    int col, row;
    cell_at(d->menu_x, d->menu_y, &col, &row);
    nearest_free(d, &col, &row, NULL, NULL);
    position_set(made, col, row);
    positions_save();
    select_only(d, NULL);
    g_hash_table_add(d->want_selected, g_strdup(made));
    g_free(d->want_rename);
    d->want_rename = made;
    if (!dir_monitor)
        fill(d);
}

static void m_open_desktop(GtkMenuItem *m, gpointer d)
{
    (void)m; (void)d;
    GFile *f = desktop_file();
    open_in_files(f);
    g_object_unref(f);
}

static void m_background(GtkMenuItem *m, gpointer d)
{
    (void)m; (void)d;
    const char *a[] = { "lp-settings", "appearance", NULL };
    lp_spawn(a);
}

static void m_files_home(GtkMenuItem *m, gpointer d)
{
    (void)m; (void)d;
    const char *a[] = { "lp-files", NULL };
    lp_spawn(a);
}

/* A terminal that opens in ~/Desktop, where the finger was. */
static void m_terminal(GtkMenuItem *m, gpointer d)
{
    (void)m; (void)d;
    char *dir = desktop_dir();
    char *kgx = g_find_program_in_path("kgx");
    char *wd = g_strconcat("--working-directory=", dir, NULL);
    const char *a_kgx[] = { "kgx", wd, NULL };
    const char *a_foot[] = { "foot", "--working-directory", dir, NULL };
    lp_spawn(kgx ? a_kgx : a_foot);
    g_free(kgx);
    g_free(wd);
    g_free(dir);
}

static void m_display(GtkMenuItem *m, gpointer d)
{
    (void)m; (void)d;
    const char *a[] = { "lp-settings", "display", NULL };
    lp_spawn(a);
}

static void desk_menu(Desk *d, GtkWidget *w, double x, double y)
{
    d->menu_x = x;
    d->menu_y = y;
    GtkWidget *menu = gtk_menu_new();
    menu_item(menu, T("New Folder", "새 폴더"), G_CALLBACK(m_new_folder), d, FALSE);
    menu_item(menu, T("Open Desktop in Files", "바탕 화면을 파일에서 열기"),
              G_CALLBACK(m_open_desktop), NULL, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, T("Files", "파일"), G_CALLBACK(m_files_home), NULL, FALSE);
    menu_item(menu, T("Open Terminal Here", "여기서 터미널 열기"), G_CALLBACK(m_terminal), NULL, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, T("Change Background…", "배경 바꾸기…"), G_CALLBACK(m_background), NULL, FALSE);
    menu_item(menu, T("Appearance…", "모양…"), G_CALLBACK(m_background), NULL, FALSE);
    menu_item(menu, T("Display Settings…", "디스플레이 설정…"), G_CALLBACK(m_display), NULL, FALSE);
    gtk_widget_show_all(menu);
    lp_menu_destroy_when_closed(menu);
    lp_menu_popup_at(menu, w, x, y);
}

/* One hold handler for the whole desktop, which tells an icon from the
 * wallpaper by where the press was: a capture-phase gesture on the
 * layout sees presses on the icons too, before they do. An icon outside
 * the selection becomes the selection; one inside keeps it, so a right
 * click on five selected icons is a menu for the five. */
static void on_hold(GtkWidget *w, double x, double y, gpointer data)
{
    Desk *d = data;
    if (d->mode == PRESS_BAND || d->mode == PRESS_CARRY)
        return;
    Icon *ic = icon_at(d, x, y);
    if (ic) {
        if (!ic->selected)
            select_only(d, ic);
        icon_menu(d, w, x, y);
    } else {
        select_only(d, NULL);
        desk_menu(d, w, x, y);
    }
}

/* ── press: click, band, carry ───────────────────────────────────── */

static gboolean gesture_ctrl(GtkGesture *g)
{
    GdkEventSequence *seq = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(g));
    const GdkEvent *ev = gtk_gesture_get_last_event(g, seq);
    GdkModifierType st = 0;
    return ev && gdk_event_get_state(ev, &st) && (st & GDK_CONTROL_MASK);
}

/* The press, from the drag gesture (which begins on it). What is decided
 * here is what must hold for a drag that may follow: an icon outside the
 * selection is the selection now, so that it is what gets carried. The
 * rest - Ctrl's toggle, a click on one of several selected icons leaving
 * just that one - waits for the release, because a drag carries them all. */
static void on_press(GtkGestureDrag *g, double x, double y, gpointer data)
{
    Desk *d = data;
    d->mode = PRESS_CLICK;
    d->press_x = x;
    d->press_y = y;
    d->press_touch = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(g)) != NULL;
    d->press_ctrl = gesture_ctrl(GTK_GESTURE(g));
    g_clear_pointer(&d->press_name, g_free);
    Icon *ic = icon_at(d, x, y);
    if (!ic) {
        if (!d->press_ctrl)
            select_only(d, NULL);
        return;
    }
    d->press_name = g_strdup(ic->name);
    if (!d->press_ctrl && !ic->selected)
        select_only(d, ic);
}

static void on_multi_press(GtkGestureMultiPress *g, int n, double x, double y, gpointer data)
{
    Desk *d = data;
    if (n < 2 || gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(g)))
        return;
    Icon *ic = icon_at(d, x, y);
    if (!ic)
        return;
    d->mode = PRESS_NONE;
    select_only(d, ic);
    icon_open(ic);
}

static void on_release(GtkGestureMultiPress *g, int n, double x, double y, gpointer data)
{
    (void)g; (void)n; (void)x; (void)y;
    Desk *d = data;
    if (d->mode != PRESS_CLICK)
        return;
    d->mode = PRESS_NONE;
    Icon *ic = icon_named(d, d->press_name);
    if (!ic)
        return;
    if (d->press_touch) {
        select_only(d, ic);
        icon_open(ic);
    } else if (d->press_ctrl) {
        icon_set_selected(ic, !ic->selected);
    } else {
        select_only(d, ic);
    }
}

/* Only what changed is redrawn: the band's fill is the same inside both
 * the old rectangle and the new one, so the part they share (short of
 * the border) stays as it is. Icons that change state redraw themselves. */
static void band_update(Desk *d, double x0, double y0, double x1, double y1)
{
    GdkRectangle old = d->band;
    d->band.x = (int)floor(MIN(x0, x1));
    d->band.y = (int)floor(MIN(y0, y1));
    d->band.width = (int)ceil(fabs(x1 - x0)) + 1;
    d->band.height = (int)ceil(fabs(y1 - y0)) + 1;
    for (guint i = 0; i < d->icons->len; i++) {
        Icon *ic = g_ptr_array_index(d->icons, i);
        GdkRectangle r;
        icon_rect(ic, &r);
        gboolean in = gdk_rectangle_intersect(&d->band, &r, NULL);
        icon_set_selected(ic, in || (d->press_ctrl && ic->band_base));
    }
    GdkRectangle a = { old.x - 2, old.y - 2, old.width + 4, old.height + 4 };
    GdkRectangle b = { d->band.x - 2, d->band.y - 2, d->band.width + 4, d->band.height + 4 };
    cairo_region_t *reg = cairo_region_create_rectangle(&b);
    if (old.width > 0) {
        cairo_region_union_rectangle(reg, &a);
        GdkRectangle same;
        if (gdk_rectangle_intersect(&old, &d->band, &same) && same.width > 6 && same.height > 6) {
            same.x += 3;
            same.y += 3;
            same.width -= 6;
            same.height -= 6;
            cairo_region_subtract_rectangle(reg, &same);
        }
    }
    gtk_widget_queue_draw_region(d->layout, reg);
    cairo_region_destroy(reg);
}

static void band_end(Desk *d)
{
    gtk_widget_queue_draw_area(d->layout, d->band.x - 2, d->band.y - 2,
                               d->band.width + 4, d->band.height + 4);
    d->band = (GdkRectangle){ 0, 0, 0, 0 };
}

static void carry_clear(void)
{
    if (carry.desk && carry.names)
        for (guint i = 0; i < carry.names->len; i++) {
            Icon *ic = icon_named(carry.desk, g_ptr_array_index(carry.names, i));
            if (ic)
                gtk_widget_set_opacity(ic->button, 1.0);
        }
    g_clear_pointer(&carry.names, g_ptr_array_unref);
    g_clear_pointer(&carry.lead, g_free);
    carry.desk = NULL;
}

static gboolean carried(const char *name)
{
    for (guint i = 0; carry.names && i < carry.names->len; i++)
        if (strcmp(g_ptr_array_index(carry.names, i), name) == 0)
            return TRUE;
    return FALSE;
}

/* Under Wayland the button's release goes to the drag now, never to the
 * surface: the gestures that saw the press would still be waiting for it
 * at the next click, and take that click as its end. Reset once the
 * handler that began the drag has returned. */
static gboolean reset_gestures(gpointer layout)
{
    static const char *const kinds[] = { "GtkGestureDrag", "GtkGestureMultiPress" };
    for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) {
        GtkEventController *c = g_object_get_data(G_OBJECT(layout), kinds[i]);
        if (c)
            gtk_event_controller_reset(c);
    }
    return G_SOURCE_REMOVE;
}

/* Hand the press to drag and drop, with the selection. The real protocol
 * and not a picture moved about by hand: it is what lets the icons leave
 * the desktop - into a Files window, onto anything that takes a file -
 * and the drag icon is the compositor's to move, not a redraw of ours. */
static void carry_start(Desk *d, GtkGesture *g, Icon *lead)
{
    if (!lead->selected)
        icon_set_selected(lead, TRUE);   /* Ctrl held on an unselected icon */
    carry_clear();
    carry.desk = d;
    carry.names = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *sel = selection(d);
    for (guint i = 0; i < sel->len; i++)
        g_ptr_array_add(carry.names, g_strdup(((Icon *)g_ptr_array_index(sel, i))->name));
    g_ptr_array_unref(sel);
    carry.lead = g_strdup(lead->name);
    carry.off_x = d->press_x - lead->sx.x;
    carry.off_y = d->press_y - lead->sy.x;
    carry.pic_x = carry.pic_y = 0;
    /* sway 1.7 draws a drag icon with its top-left corner at the pointer,
     * whatever hotspot it was given (1.8 reads it). There the drop puts
     * the icons where their picture was, not where the grab would. */
    carry.corner = g_getenv("SWAYSOCK") != NULL;

    GdkEventSequence *seq = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(g));
    const GdkEvent *ev = gtk_gesture_get_last_event(g, seq);
    GtkTargetList *tl = gtk_target_list_new(uri_target, G_N_ELEMENTS(uri_target));
    GdkDragContext *ctx = gtk_drag_begin_with_coordinates(d->layout, tl,
                                                          GDK_ACTION_MOVE | GDK_ACTION_COPY,
                                                          GDK_BUTTON_PRIMARY, (GdkEvent *)ev,
                                                          -1, -1);
    gtk_target_list_unref(tl);
    if (!ctx) {
        carry_clear();
        return;
    }
    g_idle_add_full(G_PRIORITY_DEFAULT, reset_gestures, g_object_ref(d->layout), g_object_unref);
}

static void on_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer data)
{
    Desk *d = data;
    if (d->mode == PRESS_CLICK) {
        if (hypot(dx, dy) < (d->press_touch ? DRAG_TOUCH : DRAG_MOUSE))
            return;
        /* Claimed: the button under the press does not click, and a
         * long press no longer opens the menu. */
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
        Icon *ic = icon_named(d, d->press_name);
        if (ic) {
            d->mode = PRESS_CARRY;
            carry_start(d, GTK_GESTURE(g), ic);
            return;
        }
        d->mode = PRESS_BAND;
        d->band = (GdkRectangle){ 0, 0, 0, 0 };
        for (guint i = 0; i < d->icons->len; i++) {
            Icon *k = g_ptr_array_index(d->icons, i);
            k->band_base = k->selected;
        }
    }
    if (d->mode == PRESS_BAND)
        band_update(d, d->press_x, d->press_y, d->press_x + dx, d->press_y + dy);
}

static void on_drag_end(GtkGestureDrag *g, double dx, double dy, gpointer data)
{
    (void)g; (void)dx; (void)dy;
    Desk *d = data;
    if (d->mode == PRESS_BAND) {
        d->mode = PRESS_NONE;
        band_end(d);
    } else if (d->mode == PRESS_CARRY) {
        d->mode = PRESS_NONE;          /* drag and drop has it now */
    }
}

/* ── drag and drop: the icons as a source ────────────────────────── */

/* What follows the pointer: the carried icons as they are drawn, where
 * they are relative to the one under it. At most so far around the
 * pointer - Ctrl+A on a full desktop would otherwise be a screen-sized
 * drag icon, blended by the compositor on every motion. */
static cairo_surface_t *carry_picture(Desk *d)
{
    Icon *lead = icon_named(d, carry.lead);
    if (!lead)
        return NULL;
    double hx = lead->sx.x + carry.off_x, hy = lead->sy.x + carry.off_y;
    double x0 = hx - 360, y0 = hy - 280, x1 = hx + 360, y1 = hy + 280;
    double bx0 = G_MAXDOUBLE, by0 = G_MAXDOUBLE, bx1 = -G_MAXDOUBLE, by1 = -G_MAXDOUBLE;
    for (guint i = 0; i < carry.names->len; i++) {
        Icon *ic = icon_named(d, g_ptr_array_index(carry.names, i));
        if (!ic)
            continue;
        GdkRectangle r;
        icon_rect(ic, &r);
        bx0 = MIN(bx0, r.x);
        by0 = MIN(by0, r.y);
        bx1 = MAX(bx1, r.x + r.width);
        by1 = MAX(by1, r.y + r.height);
    }
    bx0 = MAX(bx0, x0);
    by0 = MAX(by0, y0);
    bx1 = MIN(bx1, x1);
    by1 = MIN(by1, y1);
    if (bx1 - bx0 < 2 || by1 - by0 < 2)
        return NULL;
    int sf = gtk_widget_get_scale_factor(d->layout);
    int W = (int)ceil(bx1 - bx0), H = (int)ceil(by1 - by0);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W * sf, H * sf);
    cairo_surface_set_device_scale(s, sf, sf);
    cairo_t *cr = cairo_create(s);
    for (guint i = 0; i < carry.names->len; i++) {
        Icon *ic = icon_named(d, g_ptr_array_index(carry.names, i));
        cairo_surface_t *pic = ic ? lp_widget_snapshot(ic->button) : NULL;
        if (!pic)
            continue;
        cairo_set_source_surface(cr, pic, ic->sx.x - bx0, ic->sy.x - by0);
        cairo_paint(cr);
        cairo_surface_destroy(pic);
    }
    cairo_destroy(cr);
    /* The hotspot, as GTK reads it: the device offset of the pointer's
     * pixel, negated (gtktreeview.c does the same). */
    cairo_surface_set_device_offset(s, -(hx - bx0) * sf, -(hy - by0) * sf);
    carry.pic_x = lead->sx.x - bx0;
    carry.pic_y = lead->sy.x - by0;
    return s;
}

static void on_carry_begin(GtkWidget *w, GdkDragContext *ctx, gpointer data)
{
    (void)w;
    Desk *d = data;
    if (!carry.names || carry.desk != d)
        return;
    cairo_surface_t *s = carry_picture(d);
    if (s) {
        gtk_drag_set_icon_surface(ctx, s);
        cairo_surface_destroy(s);
    }
    /* The originals stay, dimmed, until the drop says where they go. */
    for (guint i = 0; i < carry.names->len; i++) {
        Icon *ic = icon_named(d, g_ptr_array_index(carry.names, i));
        if (ic)
            gtk_widget_set_opacity(ic->button, 0.4);
    }
}

static void on_carry_get(GtkWidget *w, GdkDragContext *ctx, GtkSelectionData *sel,
                         guint info, guint time, gpointer data)
{
    (void)w; (void)ctx; (void)info; (void)time; (void)data;
    if (!carry.names)
        return;
    char *dir = desktop_dir();
    char **uris = g_new0(char *, carry.names->len + 1);
    guint n = 0;
    for (guint i = 0; i < carry.names->len; i++) {
        char *p = g_build_filename(dir, g_ptr_array_index(carry.names, i), NULL);
        char *u = g_filename_to_uri(p, NULL, NULL);
        if (u)
            uris[n++] = u;
        g_free(p);
    }
    gtk_selection_data_set_uris(sel, uris);
    g_strfreev(uris);
    g_free(dir);
}

static void on_carry_end(GtkWidget *w, GdkDragContext *ctx, gpointer data)
{
    (void)w; (void)ctx; (void)data;
    carry_clear();
}

/* No flying back: under Wayland the compositor owns the drag icon and
 * GTK's animation would only move a window nobody sees. */
static gboolean on_carry_failed(GtkWidget *w, GdkDragContext *ctx, GtkDragResult r, gpointer data)
{
    (void)w; (void)ctx; (void)r; (void)data;
    return TRUE;
}

/* ── drag and drop: the desktop as a target ──────────────────────── */

/* Our own drag let go on a desktop. */
static void carry_drop(Desk *d, double x, double y)
{
    Icon *target = icon_at(d, x, y);
    if (target && (target->is_dir || target->is_trash) && !carried(target->name)) {
        GFile *desk = desktop_file();
        GPtrArray *files = g_ptr_array_new_with_free_func(g_object_unref);
        for (guint i = 0; i < carry.names->len; i++)
            g_ptr_array_add(files, g_file_get_child(desk, g_ptr_array_index(carry.names, i)));
        if (target->is_trash) {
            trash_files(files);
        } else {
            g_ptr_array_unref(transfer(files, target->file, FALSE));
            for (guint i = 0; i < carry.names->len; i++)
                position_forget(g_ptr_array_index(carry.names, i));
            positions_save();
        }
        g_ptr_array_unref(files);
        g_object_unref(desk);
        return;
    }

    /* On the wallpaper, or on an icon that takes nothing: the lead where
     * the pointer let go of it, the others where they were relative to
     * it, each settling from there into the nearest free cell. */
    Icon *lead = icon_named(d, carry.lead);
    if (!lead)
        return;
    double lx = carry.corner ? x + carry.pic_x : x - carry.off_x;
    double ly = carry.corner ? y + carry.pic_y : y - carry.off_y;
    GPtrArray *moving = g_ptr_array_new();
    GArray *at = g_array_new(FALSE, FALSE, sizeof(double));
    for (guint i = 0; i < carry.names->len; i++) {
        Icon *ic = icon_named(d, g_ptr_array_index(carry.names, i));
        if (!ic)
            continue;
        /* The lead first: it is the one the pointer put somewhere. */
        double px = lx + (ic->sx.x - lead->sx.x), py = ly + (ic->sy.x - lead->sy.x);
        if (ic == lead) {
            g_ptr_array_insert(moving, 0, ic);
            g_array_prepend_val(at, py);
            g_array_prepend_val(at, px);
        } else {
            g_ptr_array_add(moving, ic);
            g_array_append_val(at, px);
            g_array_append_val(at, py);
        }
    }
    for (guint i = 0; i < moving->len; i++)
        ((Icon *)g_ptr_array_index(moving, i))->placed = FALSE;
    for (guint i = 0; i < moving->len; i++) {
        Icon *ic = g_ptr_array_index(moving, i);
        double px = g_array_index(at, double, 2 * i), py = g_array_index(at, double, 2 * i + 1);
        int col = (int)lround((px - ORIGIN_X) / CELL_W), row = (int)lround((py - ORIGIN_Y) / CELL_H);
        nearest_free(d, &col, &row, ic, NULL);
        lp_spring_jump(&ic->sx, px);
        lp_spring_jump(&ic->sy, py);
        icon_move_to(ic, px, py);
        icon_settle(ic, col, row);
    }
    positions_save();
    g_ptr_array_unref(moving);
    g_array_unref(at);
}

/* Files from elsewhere let go on a desktop. */
static void drop_uris(Desk *d, char **uris, double x, double y, gboolean copy)
{
    GPtrArray *files = g_ptr_array_new_with_free_func(g_object_unref);
    for (char **u = uris; *u; u++) {
        GFile *f = g_file_new_for_uri(*u);
        if (g_file_is_native(f))
            g_ptr_array_add(files, f);
        else {
            g_printerr("lp-desktop: %s: not a local file; not dropped\n", *u);
            g_object_unref(f);
        }
    }
    Icon *on = icon_at(d, x, y);
    if (on && on->is_trash) {
        trash_files(files);
    } else if (on && on->is_dir) {
        g_ptr_array_unref(transfer(files, on->file, copy));
    } else {
        GFile *desk = desktop_file();
        GPtrArray *names = transfer(files, desk, copy);
        /* The first in the cell under the pointer, the rest in the free
         * cells nearest it; fill() puts them there when they appear. */
        GArray *reserved = g_array_new(FALSE, FALSE, sizeof(int));
        int c0, r0;
        cell_at(x, y, &c0, &r0);
        select_only(d, NULL);
        for (guint i = 0; i < names->len; i++) {
            const char *name = g_ptr_array_index(names, i);
            if (!name)
                continue;
            Icon *here = icon_named(d, name);     /* already on the desktop */
            int col = c0, row = r0;
            if (here)
                here->placed = FALSE;
            nearest_free(d, &col, &row, here, reserved);
            g_array_append_val(reserved, col);
            g_array_append_val(reserved, row);
            if (here) {
                icon_settle(here, col, row);
                icon_set_selected(here, TRUE);
            } else {
                position_set(name, col, row);
            }
        }
        positions_save();
        g_array_unref(reserved);
        g_ptr_array_unref(names);
        g_object_unref(desk);
    }
    g_ptr_array_unref(files);
}

/* Ctrl copies - when we can see it. During a drag wlroots gives no client
 * the keyboard, so this is the state from before the drag began, and on
 * sway only if the desktop had the keyboard then. */
static gboolean ctrl_held(void)
{
    GdkKeymap *km = gdk_keymap_get_for_display(gdk_display_get_default());
    return (gdk_keymap_get_modifier_state(km) & GDK_CONTROL_MASK) != 0;
}

static gboolean on_dnd_motion(GtkWidget *w, GdkDragContext *ctx, int x, int y,
                              guint time, gpointer data)
{
    Desk *d = data;
    gboolean ours = carry.names != NULL;
    if (!ours && gtk_drag_dest_find_target(w, ctx, NULL) == GDK_NONE) {
        drop_light(d, NULL);
        gdk_drag_status(ctx, 0, time);
        return FALSE;
    }
    Icon *on = icon_at(d, x, y);
    if (on && (!(on->is_dir || on->is_trash) || (ours && carried(on->name))))
        on = NULL;
    drop_light(d, on);
    GdkDragAction offered = gdk_drag_context_get_actions(ctx);
    GdkDragAction act = GDK_ACTION_COPY;
    if ((offered & GDK_ACTION_MOVE) && (ours || !ctrl_held()))
        act = GDK_ACTION_MOVE;
    if (!(offered & act))
        act = offered & GDK_ACTION_MOVE ? GDK_ACTION_MOVE : offered & GDK_ACTION_COPY;
    gdk_drag_status(ctx, act, time);
    return TRUE;
}

static void on_dnd_leave(GtkWidget *w, GdkDragContext *ctx, guint time, gpointer data)
{
    (void)w; (void)ctx; (void)time;
    drop_light(data, NULL);
}

static gboolean on_dnd_drop(GtkWidget *w, GdkDragContext *ctx, int x, int y,
                            guint time, gpointer data)
{
    Desk *d = data;
    drop_light(d, NULL);
    if (carry.names) {
        carry_drop(d, x, y);
        gtk_drag_finish(ctx, TRUE, FALSE, time);
        return TRUE;
    }
    GdkAtom t = gtk_drag_dest_find_target(w, ctx, NULL);
    if (t == GDK_NONE) {
        gtk_drag_finish(ctx, FALSE, FALSE, time);
        return TRUE;
    }
    d->drop_x = x;
    d->drop_y = y;
    gtk_drag_get_data(w, ctx, t, time);
    return TRUE;
}

static void on_dnd_received(GtkWidget *w, GdkDragContext *ctx, int x, int y,
                            GtkSelectionData *sel, guint info, guint time, gpointer data)
{
    (void)w; (void)x; (void)y; (void)info;
    Desk *d = data;
    char **uris = gtk_selection_data_get_uris(sel);
    gboolean ok = uris && uris[0];
    if (ok) {
        gboolean copy = gdk_drag_context_get_selected_action(ctx) == GDK_ACTION_COPY || ctrl_held();
        drop_uris(d, uris, d->drop_x, d->drop_y, copy);
    }
    g_strfreev(uris);
    /* Never del: the move, when it is one, is ours and is done. */
    gtk_drag_finish(ctx, ok, FALSE, time);
}

/* ── keys ────────────────────────────────────────────────────────── */

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer data)
{
    (void)w;
    Desk *d = data;
    if (d->rename_pop)
        return FALSE;                    /* the name being typed has them */
    guint mods = e->state & gtk_accelerator_get_default_mod_mask();
    switch (e->keyval) {
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
        if (mods)
            return FALSE;
        act_trash(d);
        return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter:
        act_open(d);
        return TRUE;
    case GDK_KEY_F2:
        act_rename(d);
        return TRUE;
    case GDK_KEY_Escape:
        select_only(d, NULL);
        return TRUE;
    case GDK_KEY_a:
    case GDK_KEY_A:
        if (mods != GDK_CONTROL_MASK)
            return FALSE;
        for (guint i = 0; i < d->icons->len; i++)
            icon_set_selected(g_ptr_array_index(d->icons, i), TRUE);
        return TRUE;
    default:
        return FALSE;
    }
}

/* ── building ────────────────────────────────────────────────────── */

/* A trash can on the desktop: a link to the trash folder itself, or a
 * launcher for it (Type=Link to trash:///, an Exec that opens it, or
 * anything wearing the user-trash icon). What is dropped on it is
 * trashed, not moved in: the folder is only half of the trash, and the
 * other half - the .trashinfo that says where a thing came from, so it
 * can go back - only g_file_trash writes. */
static gboolean trash_folder(GFile *f)
{
    char *p = g_file_get_path(f), *t = trash_dir();
    char *rp = p ? realpath(p, NULL) : NULL, *rt = realpath(t, NULL);
    gboolean yes = rp && rt && strcmp(rp, rt) == 0;
    free(rp);
    free(rt);
    g_free(p);
    g_free(t);
    return yes;
}

static gboolean trash_entry(const char *url, const char *icon, const char *exec)
{
    return (url && g_str_has_prefix(url, "trash:")) ||
           (icon && g_str_has_prefix(icon, "user-trash")) ||
           (exec && (strstr(exec, "trash:") || strstr(exec, "Trash/files")));
}

static void icon_free(gpointer p)
{
    Icon *ic = p;
    lp_motion_free(ic->motion);
    g_object_unref(ic->file);
    g_free(ic->name);
    g_free(ic);
}

static Icon *icon_new(Desk *d, GFile *f, GFileInfo *info)
{
    Icon *ic = g_new0(Icon, 1);
    ic->desk = d;
    ic->file = g_object_ref(f);
    ic->name = g_strdup(g_file_info_get_name(info));
    ic->is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;

    const char *label = g_file_info_get_display_name(info);
    char *label_own = NULL;
    GIcon *gi = g_file_info_get_icon(info);
    GIcon *gi_own = NULL;
    GDesktopAppInfo *app = NULL;
    if (ic->is_dir) {
        ic->is_trash = trash_folder(f);
    } else if (g_str_has_suffix(ic->name, ".desktop")) {
        char *path = g_file_get_path(f);
        app = g_desktop_app_info_new_from_filename(path);
        if (app) {
            label = g_app_info_get_display_name(G_APP_INFO(app));
            if (g_app_info_get_icon(G_APP_INFO(app)))
                gi = g_app_info_get_icon(G_APP_INFO(app));
            char *icon = g_desktop_app_info_get_string(app, "Icon");
            ic->is_trash = trash_entry(NULL, icon, g_app_info_get_commandline(G_APP_INFO(app)));
            g_free(icon);
        } else {
            /* Not an application - a Type=Link, which GIO does not start.
             * The one such link the desktop knows is the trash can. */
            GKeyFile *kf = g_key_file_new();
            if (path && g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
                char *url = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP, "URL", NULL);
                char *icon = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP, "Icon", NULL);
                ic->is_trash = trash_entry(url, icon, NULL);
                if (ic->is_trash) {
                    label_own = g_key_file_get_locale_string(kf, G_KEY_FILE_DESKTOP_GROUP, "Name", NULL, NULL);
                    if (label_own)
                        label = label_own;
                    gi = gi_own = g_themed_icon_new(icon ? icon : "user-trash");
                }
                g_free(url);
                g_free(icon);
            }
            g_key_file_free(kf);
        }
        g_free(path);
    }
    ic->button = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(ic->button), "lp-desk-item");
    /* Pressed through, never focused: the desktop's gestures and keys
     * act on the selection, and a button with the focus would take Enter. */
    gtk_widget_set_can_focus(ic->button, FALSE);
    gtk_widget_set_size_request(ic->button, CELL_W - 8, CELL_H - 8);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *img = gi ? gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_DIALOG)
                        : gtk_image_new_from_icon_name("text-x-generic", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(img), ICON_PX);
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(l), 2);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 12);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(ic->button), box);
    if (app)
        g_object_unref(app);
    g_clear_object(&gi_own);
    g_free(label_own);

    char *v = g_key_file_get_string(positions, "positions", ic->name, NULL);
    int c, r;
    if (v && sscanf(v, "%d,%d", &c, &r) == 2 && c >= 0 && r >= 0) {
        ic->col = c;
        ic->row = r;
        ic->placed = TRUE;
    }
    g_free(v);
    lp_spring_init(&ic->sx, LP_SPRING_SLIDE, 0);
    lp_spring_init(&ic->sy, LP_SPRING_SLIDE, 0);
    ic->motion = lp_motion_new(ic->button, icon_frame, ic);
    lp_motion_add(ic->motion, &ic->sx);
    lp_motion_add(ic->motion, &ic->sy);
    gtk_layout_put(GTK_LAYOUT(d->layout), ic->button, 0, 0);
    return ic;
}

static gint by_name(gconstpointer a, gconstpointer b)
{
    GFileInfo *x = *(GFileInfo **)a, *y = *(GFileInfo **)b;
    return g_utf8_collate(g_file_info_get_display_name(x), g_file_info_get_display_name(y));
}

/* Every icon made again from ~/Desktop. What was selected stays selected
 * and a rename being typed stays open (with what was typed), by name,
 * because the Icons themselves are all replaced. */
static void fill(Desk *d)
{
    for (guint i = 0; i < d->icons->len; i++) {
        Icon *ic = g_ptr_array_index(d->icons, i);
        if (ic->selected)
            g_hash_table_add(d->want_selected, g_strdup(ic->name));
    }
    char *renaming = NULL, *typed = NULL;
    gboolean fresh = d->rename_fresh;
    if (d->rename_pop) {
        renaming = g_strdup(d->rename_name);
        typed = g_strdup(gtk_entry_get_text(GTK_ENTRY(d->rename_entry)));
        rename_close(d);
    }
    d->drop_on = NULL;

    g_ptr_array_set_size(d->icons, 0);
    GList *kids = gtk_container_get_children(GTK_CONTAINER(d->layout));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);

    GFile *df = desktop_file();
    GFileEnumerator *en = g_file_enumerate_children(df,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_ICON "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","
        G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    GPtrArray *infos = g_ptr_array_new_with_free_func(g_object_unref);
    if (en) {
        GFileInfo *info;
        while ((info = g_file_enumerator_next_file(en, NULL, NULL))) {
            if (g_file_info_get_is_hidden(info))
                g_object_unref(info);
            else
                g_ptr_array_add(infos, info);
        }
        g_object_unref(en);
    }
    g_ptr_array_sort(infos, by_name);
    for (guint i = 0; i < infos->len; i++) {
        GFileInfo *info = g_ptr_array_index(infos, i);
        GFile *f = g_file_get_child(df, g_file_info_get_name(info));
        Icon *ic = icon_new(d, f, info);
        g_ptr_array_add(d->icons, ic);
        if (g_hash_table_contains(d->want_selected, ic->name))
            icon_set_selected(ic, TRUE);
        g_object_unref(f);
    }
    g_hash_table_remove_all(d->want_selected);
    g_ptr_array_unref(infos);
    g_object_unref(df);
    gtk_widget_show_all(d->layout);
    place_all(d);

    /* A folder just made from the menu waits here for its name; it may
     * take more than one change of ~/Desktop to appear. */
    if (!renaming && d->want_rename && icon_named(d, d->want_rename)) {
        renaming = d->want_rename;
        d->want_rename = NULL;
        fresh = TRUE;
    }
    Icon *ic = icon_named(d, renaming);
    if (ic)
        rename_begin(d, ic, typed, fresh);
    g_free(renaming);
    g_free(typed);
}

static void on_dir_changed(GFileMonitor *m, GFile *f, GFile *o,
                           GFileMonitorEvent ev, gpointer u)
{
    (void)m; (void)f; (void)o; (void)u;
    if (ev == G_FILE_MONITOR_EVENT_CREATED || ev == G_FILE_MONITOR_EVENT_DELETED ||
        ev == G_FILE_MONITOR_EVENT_RENAMED || ev == G_FILE_MONITOR_EVENT_MOVED_IN ||
        ev == G_FILE_MONITOR_EVENT_MOVED_OUT)
        for (GList *l = desks; l; l = l->next)
            fill(l->data);
}

/* ── one per output ──────────────────────────────────────────────── */

/* The selection, the folder a drag is over and the rename field. Here
 * rather than in shell.css so the desktop's behaviour and its look
 * change together; one step above shell.css (lp-shell.c: USER + 100) so
 * that a selected icon under the pointer stays selected-looking. */
static const char desk_css[] =
    ".lp-desktop button.lp-desk-item.lp-selected {"
    "  background-color: rgba(242, 140, 40, 0.30);"
    "  box-shadow: inset 0 0 0 1px rgba(255, 162, 74, 0.75);"
    "}"
    ".lp-desktop button.lp-desk-item.lp-selected:hover {"
    "  background-color: rgba(242, 140, 40, 0.38);"
    "}"
    ".lp-desktop button.lp-desk-item.lp-drop {"
    "  background-color: rgba(255, 255, 255, 0.20);"
    "  box-shadow: inset 0 0 0 2px rgba(255, 162, 74, 0.95);"
    "}"
    "popover.lp-rename entry {"
    "  min-height: 36px;"
    "  border-radius: 8px;"
    "  padding: 0 10px;"
    "}"
    "popover.lp-rename .lp-rename-msg { color: #f08585; font-size: 13px; }";

static void load_desk_css(void)
{
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, desk_css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
                                              GTK_STYLE_PROVIDER_PRIORITY_USER + 101);
    g_object_unref(p);
}

static void on_realize(GtkWidget *w, gpointer d)
{
    (void)d;
    /* Opaque: the compositor copies it instead of blending it with
     * nothing, every frame. */
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    cairo_rectangle_int_t r = { 0, 0, 16384, 16384 };
    cairo_region_t *reg = cairo_region_create_rectangle(&r);
    gdk_window_set_opaque_region(gtk_widget_get_window(w), reg);
    cairo_region_destroy(reg);

    /* The input method, ready before the first name field needs it. GTK 3
     * loads its Wayland input method module - and creates its text input -
     * on the first focus of a text field, and that first focus-in finds
     * the text input not yet entered and never enables it: Korean typed
     * into the first new folder's name came out as latin letters, and only
     * the second field worked. A context reset once here loads the module
     * now, so the text input exists before the desktop is clicked and is
     * entered with the keyboard focus like any other. */
    GtkIMContext *im = gtk_im_multicontext_new();
    gtk_im_context_set_client_window(im, gtk_widget_get_window(w));
    gtk_im_context_reset(im);
    g_object_set_data_full(G_OBJECT(w), "lp-im-early", im, g_object_unref);
}

/* A new mode, or a new scale: the grid may have changed. Only then -
 * placing moves every icon, which queues a new allocation, which would
 * place them again, every frame. */
static void regrid(Desk *d)
{
    if (cols_on(d) == d->grid_cols && rows_on(d) == d->grid_rows)
        return;
    place_all(d);
}

static void on_size(GtkWidget *w, GdkRectangle *a, gpointer data)
{
    (void)w; (void)a;
    regrid(data);
}

static void on_geometry(GObject *o, GParamSpec *ps, gpointer data)
{
    (void)o; (void)ps;
    regrid(data);
}

static GtkGesture *desk_gesture(Desk *d, GtkGesture *g)
{
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(g), FALSE);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_PRIMARY);
    /* Capture: seen before the icon buttons see it, so one set of
     * gestures on the layout serves the icons and the wallpaper both. */
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(g), GTK_PHASE_CAPTURE);
    g_object_set_data_full(G_OBJECT(d->layout), G_OBJECT_TYPE_NAME(g), g, g_object_unref);
    return g;
}

static Desk *desk_new(GdkMonitor *mon)
{
    Desk *d = g_new0(Desk, 1);
    d->mon = mon;
    d->icons = g_ptr_array_new_with_free_func(icon_free);
    d->want_selected = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    d->win = lp_layer_window("lp-desktop", GTK_LAYER_SHELL_LAYER_BACKGROUND,
                             LP_EDGE_TOP | LP_EDGE_BOTTOM | LP_EDGE_LEFT | LP_EDGE_RIGHT);
    gtk_layer_set_monitor(d->win, mon);
    g_signal_connect(mon, "notify::geometry", G_CALLBACK(on_geometry), d);
    /* -1: under the bar and the dock too; they are translucent. */
    gtk_layer_set_exclusive_zone(d->win, -1);
    /* The keyboard when clicked, for Delete, Enter and typing a name. */
    gtk_layer_set_keyboard_mode(d->win, GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    gtk_widget_set_visual(GTK_WIDGET(d->win),
                          gdk_screen_get_system_visual(gdk_screen_get_default()));
    g_signal_connect(d->win, "realize", G_CALLBACK(on_realize), NULL);
    g_signal_connect(d->win, "key-press-event", G_CALLBACK(on_key), d);
    g_signal_connect_after(d->win, "size-allocate", G_CALLBACK(on_size), d);

    d->layout = gtk_layout_new(NULL, NULL);
    gtk_widget_set_app_paintable(d->layout, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(d->layout), "lp-desktop");
    g_signal_connect(d->layout, "draw", G_CALLBACK(desk_draw), d);
    g_signal_connect_after(d->layout, "draw", G_CALLBACK(band_draw), d);
    gtk_container_add(GTK_CONTAINER(d->win), d->layout);

    /* The gestures below need these on the layout's own windows. */
    gtk_widget_add_events(d->layout, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                     GDK_BUTTON_MOTION_MASK | GDK_TOUCH_MASK);
    lp_on_hold(d->layout, on_hold, d);
    GtkGesture *mp = desk_gesture(d, gtk_gesture_multi_press_new(d->layout));
    g_signal_connect(mp, "pressed", G_CALLBACK(on_multi_press), d);
    g_signal_connect(mp, "released", G_CALLBACK(on_release), d);
    GtkGesture *drag = desk_gesture(d, gtk_gesture_drag_new(d->layout));
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_press), d);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), d);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), d);

    /* Source (gtk_drag_begin in carry_start) and target both. */
    g_signal_connect(d->layout, "drag-begin", G_CALLBACK(on_carry_begin), d);
    g_signal_connect(d->layout, "drag-data-get", G_CALLBACK(on_carry_get), d);
    g_signal_connect(d->layout, "drag-end", G_CALLBACK(on_carry_end), d);
    g_signal_connect(d->layout, "drag-failed", G_CALLBACK(on_carry_failed), d);
    gtk_drag_dest_set(d->layout, 0, uri_target, G_N_ELEMENTS(uri_target),
                      GDK_ACTION_MOVE | GDK_ACTION_COPY);
    g_signal_connect(d->layout, "drag-motion", G_CALLBACK(on_dnd_motion), d);
    g_signal_connect(d->layout, "drag-leave", G_CALLBACK(on_dnd_leave), d);
    g_signal_connect(d->layout, "drag-drop", G_CALLBACK(on_dnd_drop), d);
    g_signal_connect(d->layout, "drag-data-received", G_CALLBACK(on_dnd_received), d);

    fill(d);
    gtk_widget_show_all(GTK_WIDGET(d->win));
    desks = g_list_append(desks, d);
    return d;
}

static void desk_free(Desk *d)
{
    desks = g_list_remove(desks, d);
    g_signal_handlers_disconnect_by_data(d->mon, d);
    if (carry.desk == d)
        carry_clear();
    rename_close(d);
    g_ptr_array_unref(d->icons);
    g_hash_table_unref(d->want_selected);
    g_free(d->want_rename);
    g_free(d->press_name);
    if (d->wall)
        cairo_surface_destroy(d->wall);
    gtk_widget_destroy(GTK_WIDGET(d->win));
    g_free(d);
}

static void on_monitor_added(GdkDisplay *dpy, GdkMonitor *m, gpointer u)
{
    (void)dpy; (void)u;
    desk_new(m);
}

static void on_monitor_removed(GdkDisplay *dpy, GdkMonitor *m, gpointer u)
{
    (void)dpy; (void)u;
    for (GList *l = desks; l; l = l->next)
        if (((Desk *)l->data)->mon == m) {
            desk_free(l->data);
            break;
        }
}

static void on_command(int argc, char **argv, gpointer d)
{
    (void)d;
    if (argc >= 2 && !strcmp(argv[1], "refresh"))
        for (GList *l = desks; l; l = l->next) {
            Desk *k = l->data;
            if (k->wall)
                cairo_surface_destroy(k->wall);
            k->wall = NULL;
            fill(k);
            gtk_widget_queue_draw(GTK_WIDGET(k->win));
        }
}

int main(int argc, char **argv)
{
    if (!lp_single_instance("desktop", argc, argv, on_command, NULL))
        return 0;
    lp_shell_init(&argc, &argv);
    load_desk_css();
    positions_load();

    char *dir = desktop_dir();
    g_mkdir_with_parents(dir, 0755);
    GFile *df = g_file_new_for_path(dir);
    dir_monitor = g_file_monitor_directory(df, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (dir_monitor)
        g_signal_connect(dir_monitor, "changed", G_CALLBACK(on_dir_changed), NULL);
    g_object_unref(df);
    g_free(dir);

    char *wp = lp_config_path("wallpaper");
    GFile *wf = g_file_new_for_path(wp);
    wall_monitor = g_file_monitor_file(wf, G_FILE_MONITOR_NONE, NULL, NULL);
    if (wall_monitor)
        g_signal_connect(wall_monitor, "changed", G_CALLBACK(on_wall_changed), NULL);
    g_object_unref(wf);
    g_free(wp);

    GdkDisplay *dpy = gdk_display_get_default();
    for (int i = 0; i < gdk_display_get_n_monitors(dpy); i++)
        desk_new(gdk_display_get_monitor(dpy, i));
    g_signal_connect(dpy, "monitor-added", G_CALLBACK(on_monitor_added), NULL);
    g_signal_connect(dpy, "monitor-removed", G_CALLBACK(on_monitor_removed), NULL);
    gtk_main();
    return 0;
}
