/*
 * appgrid.c - lp-appgrid, every application on one screen.
 *
 *            ┌──────────────── ( 🔍 Type to search ) ────────────────┐
 *            │   [icon]     [icon]     [icon]     [icon]     [icon]  │
 *            │  Calculator   Clocks     Files    Firefox    Settings │
 *            │   ...                                                 │
 *
 * Opened by 현재 활동 (Activities) in the top bar, by the 3x3 button at
 * the foot of the dock, and by a finger swiping up from the bottom edge.
 * It covers the screen between the bar and the dock - both stay visible
 * and usable, the way the owner's mockup leaves them - and lists every
 * application that has a .desktop file meant to be shown, alphabetically,
 * as 144x136 tiles: two fingertips wide.
 *
 * ── the search box and the on-screen keyboard ──
 *
 * The box is a GtkSearchEntry, and GTK 3 speaks text-input-v3, which is
 * what the OSK's input method types into. The box is deliberately NOT
 * focused when the grid opens: a focused text field is what makes the
 * OSK show itself, and a keyboard over the bottom third of the grid every
 * time someone only wants to tap an icon is the wrong default. A tap on
 * the box focuses it and brings the keyboard; on a physical keyboard,
 * typing anywhere starts the search (gtk_search_entry_handle_event).
 *
 * ── motion ──
 *
 * The grid opens on the window spring (340 ms): from scale 0.96 and
 * transparent to full size and opaque, and closes in 0.7x that. It is one
 * spring, so a tap on the empty area while it is still opening turns it
 * round from wherever it is, at the speed it had. The bottom-edge swipe
 * drives the same spring: the dock sends drag-px while the finger moves
 * (the grid follows it, 1:1 over its first 320 px) and release-px with
 * the finger's speed when it lifts; a short flick opens it, a slow drag
 * opens it only past half way (lp_spring_project).
 *
 * The scale and fade are drawn here, with cairo, on a surface that is
 * never resized: the layer surface is mapped once at full size and the
 * content is painted through a transform. Under reduced motion there is
 * no scale, only a short fade.
 *
 * ── pinning ──
 *
 * A long press on a tile offers Pin to dock / Unpin from dock. The dock
 * owns its list (desktop/dock/dock.c); this asks it to change, over the
 * dock's socket, and reads the current list from the runtime copy the
 * dock keeps in $XDG_RUNTIME_DIR/lp-dock-pins.
 *
 * Commands (one process; running it again hands it argv):
 *   lp-appgrid [toggle] | show | hide | daemon
 *   lp-appgrid drag-px PX | release-px VELOCITY_PX_PER_S
 */
#define _GNU_SOURCE 1

#include <stdlib.h>
#include <string.h>

#include "lp-apps.h"
#include "lp-motion.h"
#include "lp-shell.h"

#define TILE_ICON 96
#define DRAG_SPAN 320.0     /* px of finger travel that is "fully open" */

static struct {
    GtkWindow *win;
    GtkWidget *root;         /* app-paintable; draws the transform */
    GtkWidget *content;
    GtkWidget *search;
    GtkWidget *flow;
    GtkWidget *scroll;
    GtkWidget *empty;
    LpSpring   open;         /* 0 closed .. 1 open */
    LpMotion  *motion;
    gboolean   want;
    gboolean   dirty;        /* the application list changed while shut */
} G;

static void grid_hide(void);

/* ── telling the others ──────────────────────────────────────────── */

static void tell(gboolean open)
{
    const char *a[] = { "lp-panel", "open", "grid", open ? "1" : "0", NULL };
    lp_send("panel", a);
    const char *b[] = { "lp-dock", "open", "grid", open ? "1" : "0", NULL };
    lp_send("dock", b);
}

/* ── the pinned list, as the dock last published it ──────────────── */

static gboolean is_pinned(const char *id)
{
    char *p = g_build_filename(g_get_user_runtime_dir(), "lp-dock-pins", NULL);
    char *s = NULL;
    gboolean yes = FALSE;
    if (g_file_get_contents(p, &s, NULL, NULL)) {
        char **lines = g_strsplit(s, "\n", -1);
        for (char **l = lines; *l && !yes; l++)
            yes = strcmp(g_strstrip(*l), id) == 0;
        g_strfreev(lines);
    }
    g_free(s);
    g_free(p);
    return yes;
}

/* ── tiles ───────────────────────────────────────────────────────── */

static void on_tile(GtkButton *b, gpointer d)
{
    (void)d;
    if (lp_hold_consumed(GTK_WIDGET(b)))
        return;
    GAppInfo *info = g_object_get_data(G_OBJECT(b), "app");
    lp_app_launch(info);
    grid_hide();
}

static void m_open(GtkMenuItem *m, gpointer d)
{
    (void)m;
    lp_app_launch(G_APP_INFO(d));
    grid_hide();
}

static void m_pin(GtkMenuItem *m, gpointer d)
{
    (void)m;
    const char *id = g_app_info_get_id(G_APP_INFO(d));
    const char *a[] = { "lp-dock", is_pinned(id) ? "unpin" : "pin", id, NULL };
    if (!lp_send("dock", a))
        g_printerr("lp-appgrid: the dock is not running; nothing pinned\n");
}

static void on_hold(GtkWidget *w, double x, double y, gpointer d)
{
    (void)d;
    GAppInfo *info = g_object_get_data(G_OBJECT(w), "app");
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *head = gtk_menu_item_new_with_label(lp_app_name(info));
    gtk_widget_set_sensitive(head, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), head);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget *o = gtk_menu_item_new_with_label(T("Open", "열기"));
    g_signal_connect(o, "activate", G_CALLBACK(m_open), info);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), o);
    GtkWidget *p = gtk_menu_item_new_with_label(
        is_pinned(g_app_info_get_id(info)) ? T("Unpin from dock", "독에서 고정 해제")
                                           : T("Pin to dock", "독에 고정"));
    g_signal_connect(p, "activate", G_CALLBACK(m_pin), info);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), p);
    gtk_widget_show_all(menu);
    lp_menu_destroy_when_closed(menu);
    lp_menu_popup_at(menu, w, x, y);
}

static GtkWidget *tile_new(GAppInfo *info)
{
    GtkWidget *b = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "lp-tile");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GIcon *gi = g_app_info_get_icon(info);
    GtkWidget *img = gi ? gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_DIALOG)
                        : gtk_image_new_from_icon_name("application-x-executable",
                                                       GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(img), TILE_ICON);
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    GtkWidget *l = gtk_label_new(lp_app_name(info));
    gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(l), 2);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 14);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(b), box);
    g_object_set_data_full(G_OBJECT(b), "app", g_object_ref(info), g_object_unref);
    const char *desc = g_app_info_get_description(info);
    if (desc && *desc)
        gtk_widget_set_tooltip_text(b, desc);
    g_signal_connect(b, "clicked", G_CALLBACK(on_tile), NULL);
    lp_on_hold(b, on_hold, NULL);
    return b;
}

static gint by_name(gconstpointer a, gconstpointer b)
{
    return g_utf8_collate(lp_app_name((GAppInfo *)a), lp_app_name((GAppInfo *)b));
}

static void fill(void)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(G.flow));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);

    GList *all = g_app_info_get_all();
    GList *shown = NULL;
    for (GList *l = all; l; l = l->next)
        if (g_app_info_should_show(l->data))
            shown = g_list_prepend(shown, l->data);
    shown = g_list_sort(shown, by_name);
    for (GList *l = shown; l; l = l->next)
        gtk_flow_box_insert(GTK_FLOW_BOX(G.flow), tile_new(l->data), -1);
    g_list_free(shown);
    g_list_free_full(all, g_object_unref);
    gtk_widget_show_all(G.flow);
    G.dirty = FALSE;
}

/* ── search ──────────────────────────────────────────────────────── */

static gboolean matches(GAppInfo *info, const char *q)
{
    if (!*q)
        return TRUE;
    /* Name, generic name, keywords and the command, case-folded - so
     * "term", "터미널" and "foot" all find the terminal. */
    const char *fields[4] = { lp_app_name(info), NULL, NULL,
                              g_app_info_get_executable(info) };
    if (G_IS_DESKTOP_APP_INFO(info)) {
        fields[1] = g_desktop_app_info_get_generic_name(G_DESKTOP_APP_INFO(info));
        const char *const *kw = g_desktop_app_info_get_keywords(G_DESKTOP_APP_INFO(info));
        for (; kw && *kw; kw++) {
            char *k = g_utf8_casefold(*kw, -1);
            gboolean hit = strstr(k, q) != NULL;
            g_free(k);
            if (hit)
                return TRUE;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (!fields[i])
            continue;
        char *f = g_utf8_casefold(fields[i], -1);
        gboolean hit = strstr(f, q) != NULL;
        g_free(f);
        if (hit)
            return TRUE;
    }
    return FALSE;
}

static char *query;

static gboolean filter(GtkFlowBoxChild *child, gpointer d)
{
    (void)d;
    GtkWidget *b = gtk_bin_get_child(GTK_BIN(child));
    return matches(g_object_get_data(G_OBJECT(b), "app"), query ? query : "");
}

static void on_search(GtkSearchEntry *e, gpointer d)
{
    (void)d;
    g_free(query);
    query = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(e)), -1);
    gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(G.flow));
    int n = 0;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(G.flow));
    for (GList *l = kids; l; l = l->next)
        if (filter(l->data, NULL))
            n++;
    g_list_free(kids);
    gtk_widget_set_visible(G.empty, n == 0);
}

/* Enter launches the first match: search, then Enter, is the fastest way
 * to start anything from a physical keyboard. */
static void on_search_go(GtkEntry *e, gpointer d)
{
    (void)e; (void)d;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(G.flow));
    for (GList *l = kids; l; l = l->next)
        if (filter(l->data, NULL)) {
            gtk_button_clicked(GTK_BUTTON(gtk_bin_get_child(GTK_BIN(l->data))));
            break;
        }
    g_list_free(kids);
}

/* ── drawing: fade and scale ─────────────────────────────────────── */

/* While the grid moves, what moves is a picture of it.
 *
 * The first version redrew every tile through a cairo group each frame
 * and measured 40 ms of CPU per frame at 3840x2160 - every label laid
 * out and every icon blitted, then the whole surface resampled. Nothing
 * inside the grid changes while it opens, so the tiles are drawn once,
 * into an image the size of the part of the screen they cover, and each
 * frame only fills the backdrop and paints that image through the scale.
 * The picture is dropped the moment the grid comes to rest, so a grid
 * that is open is always the live widgets and never a stale copy. */
static cairo_surface_t *snap;
static GdkRectangle snap_box;       /* in root coordinates */

static void drop_snapshot(void)
{
    if (snap)
        cairo_surface_destroy(snap);
    snap = NULL;
}

static void take_snapshot(GtkWidget *root)
{
    /* The search box and the visible part of the tiles - not the empty
     * backdrop under them, which is a flat fill and costs nothing. */
    GtkAllocation sa, fa, ca;
    gtk_widget_get_allocation(G.search, &sa);
    gtk_widget_get_allocation(G.scroll, &ca);
    int fx = 0, fy = 0;
    gtk_widget_translate_coordinates(G.flow, root, 0, 0, &fx, &fy);
    gtk_widget_get_allocation(G.flow, &fa);
    GdkRectangle flow = { fx, fy, fa.width, fa.height }, vis;
    GdkRectangle scr = { ca.x, ca.y, ca.width, ca.height };
    if (!gdk_rectangle_intersect(&flow, &scr, &vis))
        vis = scr;
    GdkRectangle box = { sa.x, sa.y, sa.width, sa.height };
    gdk_rectangle_union(&box, &vis, &box);
    /* A few pixels of margin for the tiles' rounded press highlights. */
    box.x -= 8; box.y -= 8; box.width += 16; box.height += 16;

    int sf = gtk_widget_get_scale_factor(root);
    snap = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                      box.width * sf, box.height * sf);
    cairo_surface_set_device_scale(snap, sf, sf);
    cairo_t *c = cairo_create(snap);
    cairo_translate(c, -box.x, -box.y);
    gtk_widget_draw(G.content, c);
    cairo_destroy(c);
    snap_box = box;
}

static gboolean root_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    gint64 t0 = lp_trace_now();
    double x = G.open.x;
    double a = CLAMP(x, 0.0, 1.0);
    double W = gtk_widget_get_allocated_width(w);
    double H = gtk_widget_get_allocated_height(w);

    /* The backdrop fades; it is a flat fill, which costs a memset. */
    GdkRGBA bg = { 0.055, 0.055, 0.055, 1 };
    gtk_style_context_lookup_color(gtk_widget_get_style_context(w), "lp_canvas", &bg);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, bg.red, bg.green, bg.blue, 0.94 * a);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (a >= 0.999 && !G.open.moving) {
        drop_snapshot();
        gtk_container_propagate_draw(GTK_CONTAINER(w), G.content, cr);
        lp_trace_draw("grid-live", t0);
        return TRUE;
    }
    if (a <= 0.001)
        return TRUE;
    if (!snap)
        take_snapshot(w);
    /* The content scales about the middle of the screen from 0.96, and
     * fades with the backdrop. The scale follows the spring past 1 when a
     * fling overshoots; the opacity cannot. */
    double s = lp_motion_reduced() ? 1.0 : 0.96 + 0.04 * x;
    cairo_translate(cr, W / 2, H / 2);
    cairo_scale(cr, s, s);
    cairo_translate(cr, -W / 2, -H / 2);
    cairo_set_source_surface(cr, snap, snap_box.x, snap_box.y);
    /* Nearest-neighbour while it moves: a 4% scale that changes every
     * 16 ms shows no difference, and bilinear costs several times more
     * on the CPU. At rest the live widgets are drawn with no resampling
     * at all (above). */
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
    cairo_rectangle(cr, snap_box.x, snap_box.y, snap_box.width, snap_box.height);
    cairo_clip(cr);
    cairo_paint_with_alpha(cr, a);
    lp_trace_draw("grid", t0);
    return TRUE;
}

static void on_frame(GtkWidget *w, gpointer d)
{
    (void)d;
    gtk_widget_queue_draw(w);
    if (!G.open.moving && G.open.x <= 0.001 &&
        gtk_widget_get_visible(GTK_WIDGET(G.win))) {
        gtk_widget_hide(GTK_WIDGET(G.win));
        gtk_entry_set_text(GTK_ENTRY(G.search), "");
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(G.scroll));
        gtk_adjustment_set_value(adj, 0);
        tell(FALSE);
    }
}

/* ── showing and hiding ──────────────────────────────────────────── */

/* The tiles stop above the dock, which floats over the grid's bottom
 * edge: as much room as the dock reserves for windows (it says how much
 * in $XDG_RUNTIME_DIR/lp-dock-zone), or a dock's worth without it. */
static void keep_clear_of_dock(void)
{
    int zone = 96;
    char *p = g_build_filename(g_get_user_runtime_dir(), "lp-dock-zone", NULL);
    char *s = NULL;
    if (g_file_get_contents(p, &s, NULL, NULL)) {
        int z = atoi(s);
        if (z > 0 && z < 400)
            zone = z;
        g_free(s);
    }
    g_free(p);
    gtk_widget_set_margin_bottom(G.scroll, zone);
}

static void map_now(void)
{
    if (gtk_widget_get_visible(GTK_WIDGET(G.win)))
        return;
    if (G.dirty)
        fill();
    keep_clear_of_dock();
    gtk_widget_show_all(GTK_WIDGET(G.win));
    gtk_widget_set_visible(G.empty, FALSE);
    tell(TRUE);
}

static void grid_show(void)
{
    G.want = TRUE;
    drop_snapshot();       /* the content may have changed since */
    map_now();
    lp_spring_set_target(&G.open, 1.0);
    lp_motion_kick(G.motion);
}

static void grid_hide(void)
{
    G.want = FALSE;
    lp_spring_set_target_out(&G.open, 0.0);
    lp_motion_kick(G.motion);
}

/* A press on the backdrop - not on a tile, not on the search box - is
 * "never mind". It works at any point of the opening, which is what the
 * spring is for. */
static gboolean on_root_press(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    (void)d;
    GtkWidget *target = gtk_get_event_widget((GdkEvent *)ev);
    if (target == w || target == G.content || target == G.flow ||
        target == G.scroll || GTK_IS_VIEWPORT(target)) {
        grid_hide();
        return TRUE;
    }
    return FALSE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w; (void)d;
    if (ev->keyval == GDK_KEY_Escape) {
        if (*gtk_entry_get_text(GTK_ENTRY(G.search)))
            gtk_entry_set_text(GTK_ENTRY(G.search), "");
        else
            grid_hide();
        return TRUE;
    }
    if (!gtk_widget_has_focus(G.search))
        return gtk_search_entry_handle_event(GTK_SEARCH_ENTRY(G.search), (GdkEvent *)ev);
    return FALSE;
}

static void on_command(int argc, char **argv, gpointer d)
{
    (void)d;
    const char *cmd = argc >= 2 ? argv[1] : "toggle";
    if (!strcmp(cmd, "toggle")) {
        if (G.want) grid_hide(); else grid_show();
    } else if (!strcmp(cmd, "show")) {
        grid_show();
    } else if (!strcmp(cmd, "hide")) {
        grid_hide();
    } else if (!strcmp(cmd, "drag-px") && argc >= 3) {
        double f = g_ascii_strtod(argv[2], NULL) / DRAG_SPAN;
        G.want = f > 0;
        map_now();
        lp_spring_jump(&G.open, CLAMP(f, 0.0, 1.0));
        lp_motion_kick(G.motion);
    } else if (!strcmp(cmd, "release-px") && argc >= 3) {
        double v = g_ascii_strtod(argv[2], NULL) / DRAG_SPAN;
        double target = lp_spring_project(G.open.x, v) >= 0.5 ? 1.0 : 0.0;
        G.want = target > 0.5;
        lp_spring_fling(&G.open, target, v);
        lp_motion_kick(G.motion);
    }
}

static void on_apps_changed(GAppInfoMonitor *m, gpointer d)
{
    (void)m; (void)d;
    /* Rebuilt now if it is open, else the next time it opens: an apt
     * install that adds forty .desktop files should not cost forty
     * rebuilds of a grid nobody is looking at. */
    if (gtk_widget_get_visible(GTK_WIDGET(G.win)))
        fill();
    else
        G.dirty = TRUE;
}

int main(int argc, char **argv)
{
    if (!lp_single_instance("appgrid", argc, argv, on_command, NULL))
        return 0;
    lp_shell_init(&argc, &argv);

    G.win = lp_layer_window("lp-appgrid", GTK_LAYER_SHELL_LAYER_TOP,
                            LP_EDGE_TOP | LP_EDGE_BOTTOM | LP_EDGE_LEFT | LP_EDGE_RIGHT);
    /* Exclusive zone -1: the whole screen, as Launchpad has it, the top
     * bar under it too. With 0 it filled only what the bar and the dock
     * leave, and stopped short of the bottom edge: a band of wallpaper
     * around the dock under the grid's backdrop. The dock goes over the
     * grid while it is open (it is told, below - tell()), and the tiles
     * keep clear of it (keep_clear_of_dock). */
    gtk_layer_set_exclusive_zone(G.win, -1);
    gtk_layer_set_keyboard_mode(G.win, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    g_signal_connect(G.win, "key-press-event", G_CALLBACK(on_key), NULL);

    G.root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_app_paintable(G.root, TRUE);
    g_signal_connect(G.root, "draw", G_CALLBACK(root_draw), NULL);
    gtk_container_add(GTK_CONTAINER(G.win), G.root);
    gtk_widget_add_events(GTK_WIDGET(G.win), GDK_BUTTON_PRESS_MASK);
    g_signal_connect(G.win, "button-press-event", G_CALLBACK(on_root_press), NULL);

    G.content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(G.content), "lp-grid");
    gtk_box_pack_start(GTK_BOX(G.root), G.content, TRUE, TRUE, 0);

    G.search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(G.search), T("Type to search", "검색하려면 입력하세요"));
    gtk_style_context_add_class(gtk_widget_get_style_context(G.search), "lp-search");
    gtk_widget_set_halign(G.search, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(G.search, 56);
    gtk_widget_set_margin_bottom(G.search, 20);
    g_signal_connect(G.search, "search-changed", G_CALLBACK(on_search), NULL);
    g_signal_connect(G.search, "activate", G_CALLBACK(on_search_go), NULL);
    gtk_box_pack_start(GTK_BOX(G.content), G.search, FALSE, FALSE, 0);

    G.empty = gtk_label_new(T("No applications match.", "맞는 앱이 없습니다."));
    gtk_style_context_add_class(gtk_widget_get_style_context(G.empty), "lp-empty");
    gtk_widget_set_no_show_all(G.empty, TRUE);
    gtk_box_pack_start(GTK_BOX(G.content), G.empty, FALSE, FALSE, 0);

    /* Kinetic under a finger, with GTK's overshoot at the ends. */
    G.scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(G.scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_kinetic_scrolling(GTK_SCROLLED_WINDOW(G.scroll), TRUE);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(G.scroll), TRUE);
    gtk_widget_set_vexpand(G.scroll, TRUE);
    G.flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(G.flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(G.flow), TRUE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(G.flow), 8);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(G.flow), 3);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(G.flow), 12);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(G.flow), 12);
    gtk_flow_box_set_filter_func(GTK_FLOW_BOX(G.flow), filter, NULL, NULL);
    gtk_widget_set_valign(G.flow, GTK_ALIGN_START);
    gtk_widget_set_halign(G.flow, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(G.flow, 48);
    gtk_widget_set_margin_end(G.flow, 48);
    gtk_widget_set_margin_bottom(G.flow, 32);
    gtk_container_add(GTK_CONTAINER(G.scroll), G.flow);
    gtk_box_pack_start(GTK_BOX(G.content), G.scroll, TRUE, TRUE, 0);

    lp_spring_init(&G.open, LP_SPRING_WINDOW, 0.0);
    G.motion = lp_motion_new(G.root, on_frame, NULL);
    lp_motion_add(G.motion, &G.open);

    g_signal_connect(g_app_info_monitor_get(), "changed", G_CALLBACK(on_apps_changed), NULL);
    fill();
    on_command(argc, argv, NULL);
    gtk_main();
    return 0;
}
