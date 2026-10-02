/*
 * appearance.c - Appearance: dark or light, the accent colour, the text
 * size (access.c's row, the same setting), the wallpaper, and the dock.
 *
 * ── Where each choice is kept, and who reads it ──
 *
 *   ~/.config/lp/style             "dark" or "light". The same file the top
 *                                  bar's "Dark Style" tile writes
 *                                  (desktop/common/lp-shell.c), which the
 *                                  shell watches - so the two switches are
 *                                  one setting and cannot disagree.
 *   ~/.config/lp/appearance.conf   accent=#rrggbb. This window recolours
 *                                  from it at once.
 *   ~/.config/lp/wallpaper         the picture's path, one line: the file
 *                                  lp-desktop draws the wallpaper from and
 *                                  watches, so a choice shows at once
 *                                  under either compositor.
 *   ~/.config/lp/dock.conf         size=small|medium|large,
 *                                  autohide=yes|no, for lp-dock.
 *
 * Dark and light are also mirrored where applications look for them:
 * org.gnome.desktop.interface color-scheme through gsettings (libadwaita
 * and the portal) and gtk-application-prefer-dark-theme in GTK 3's and
 * GTK 4's settings.ini (plain GTK apps started afterwards). Those mirrors
 * are put back at login by `lp-settings --restore`, because gsettings
 * without a session bus fails and only our file is certain to be there.
 *
 * ── Wallpapers are thumbnails, made off the main thread ──
 *
 * The shipped wallpapers are 3840x2160 PNGs. Decoding one takes about a
 * tenth of a second and 33MB; decoding eight of them in the window's
 * thread would freeze the page for a second as it slides in. So each is
 * decoded straight to a 192x108 thumbnail (gdk-pixbuf's at_scale) in a
 * worker thread, and appears when it is ready.
 */
#include "core.h"

#include <string.h>

static char *appearance_conf(void) { return lp_config_path("appearance.conf"); }

/* ── dark and light ─────────────────────────────────────────────────── */

static void mirror_style(gboolean light)
{
    lp_gsettings_set("org.gnome.desktop.interface", "color-scheme",
                     light ? "default" : "prefer-dark");
    lp_gtk_settings_set("gtk-application-prefer-dark-theme", light ? "0" : "1");
}

/* The page on screen, for its subtitle ("Dark style"), which is the
 * state and was left saying the old one. Cleared when the page goes. */
static GtkWidget *PAGE;

static void set_style(gboolean light)
{
    char *p = lp_config_path("style");
    gboolean ok = lp_write_file(p, light ? "light\n" : "dark\n");
    g_free(p);
    if (!ok) return;
    mirror_style(light);
    lp_apply_palette();
    lp_toast(FALSE, light ? T("Light style", "밝은 스타일") : T("Dark style", "어두운 스타일"));
    if (PAGE)
        page_set_subtitle(PAGE, light ? T("Light style", "밝은 스타일") : T("Dark style", "어두운 스타일"));
}

/* A small window drawn in the style it stands for: a title bar, a
 * sidebar and two lines of text. A picture says "dark" faster than the
 * word does. */
static void preview_draw(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer p)
{
    (void)a;
    gboolean light = GPOINTER_TO_INT(p);
    double bg = light ? 0.965 : 0.137, bar = light ? 0.92 : 0.17, side = light ? 0.92 : 0.118;
    double ink = light ? 0.2 : 0.85;
    cairo_set_source_rgb(cr, 0.23, 0.06, 0.19);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
    double x = 14, y = 12, ww = w - 28, hh = h - 24, r = 6;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + ww - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + ww - r, y + hh - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + hh - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, bg, bg, bg);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, bar, bar, bar);
    cairo_rectangle(cr, x, y + r, ww, 14 - r);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, side, side, side);
    cairo_rectangle(cr, x, y + 14, ww * 0.3, hh - 14 - r);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, ink, ink, ink);
    cairo_rectangle(cr, x + ww * 0.38, y + 26, ww * 0.45, 5);
    cairo_rectangle(cr, x + ww * 0.38, y + 38, ww * 0.3, 5);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.91, 0.33, 0.13);
    cairo_rectangle(cr, x + ww * 0.38, y + 50, ww * 0.2, 8);
    cairo_fill(cr);
}

static void on_style_card(GtkToggleButton *b, gpointer p)
{
    if (!gtk_toggle_button_get_active(b) || lp_quiet) return;
    set_style(GPOINTER_TO_INT(p));
}

static GtkWidget *style_card(const char *label, gboolean light, GtkWidget *group)
{
    GtkWidget *b = gtk_toggle_button_new();
    gtk_widget_add_css_class(b, "lp-style-card");
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, 176, 100);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), preview_draw, GINT_TO_POINTER(light), NULL);
    gtk_box_append(GTK_BOX(v), da);
    gtk_box_append(GTK_BOX(v), gtk_label_new(label));
    gtk_button_set_child(GTK_BUTTON(b), v);
    g_object_set_data_full(G_OBJECT(b), "lp-title", g_strdup(label), g_free);
    if (group) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(group));
    if (light == lp_style_is_light())
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), TRUE);
    g_signal_connect(b, "toggled", G_CALLBACK(on_style_card), GINT_TO_POINTER(light));
    return b;
}

/* ── accent ─────────────────────────────────────────────────────────── */

static const struct { const char *hex, *en, *ko; } ACCENTS[] = {
    { "#e95420", "Orange", "주황" },
    { "#3584e4", "Blue", "파랑" },
    { "#2ec27e", "Green", "초록" },
    { "#9141ac", "Purple", "보라" },
    { "#e01b8a", "Pink", "분홍" },
    { "#e5484d", "Red", "빨강" },
    { "#f5c211", "Yellow", "노랑" },
    { "#1c9fa6", "Teal", "청록" },
    { "#6f8396", "Slate", "회청" },
};

static void on_accent(GtkToggleButton *b, gpointer p)
{
    if (!gtk_toggle_button_get_active(b) || lp_quiet) return;
    int i = GPOINTER_TO_INT(p);
    char *c = appearance_conf();
    gboolean ok = kv_set(c, "accent", ACCENTS[i].hex);
    g_free(c);
    if (!ok) return;
    lp_apply_palette();
    lp_toast(FALSE, T("Accent colour: %s", "강조색: %s"), T(ACCENTS[i].en, ACCENTS[i].ko));
}

static GtkWidget *accent_row(GtkWidget *list)
{
    char *c = appearance_conf();
    char *cur = kv_get(c, "accent");
    g_free(c);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *first = NULL;
    /* One provider for the swatch colours, made the first time the page
     * is built and kept for the life of the window. */
    static gboolean css_done;
    GString *css = g_string_new(NULL);
    for (guint i = 0; i < G_N_ELEMENTS(ACCENTS); i++) {
        GtkWidget *b = gtk_toggle_button_new();
        gtk_widget_add_css_class(b, "lp-swatch");
        char cls[16];
        g_snprintf(cls, sizeof cls, "lp-sw%u", i);
        gtk_widget_add_css_class(b, cls);
        g_string_append_printf(css, ".lp-settings button.%s { background: %s; border-color: %s; }\n",
                               cls, ACCENTS[i].hex, ACCENTS[i].hex);
        gtk_widget_set_tooltip_text(b, T(ACCENTS[i].en, ACCENTS[i].ko));
        g_object_set_data_full(G_OBJECT(b), "lp-title", g_strdup(T(ACCENTS[i].en, ACCENTS[i].ko)), g_free);
        if (first) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(first));
        else first = b;
        if ((cur && !g_ascii_strcasecmp(cur, ACCENTS[i].hex)) || (!cur && i == 0))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), TRUE);
        g_signal_connect(b, "toggled", G_CALLBACK(on_accent), GINT_TO_POINTER(i));
        gtk_box_append(GTK_BOX(box), b);
    }
    if (!css_done) {
        GtkCssProvider *prov = gtk_css_provider_new();
        gtk_css_provider_load_from_data(prov, css->str, -1);
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                                   GTK_STYLE_PROVIDER(prov), 830);
        g_object_unref(prov);
        css_done = TRUE;
    }
    g_string_free(css, TRUE);
    g_free(cur);

    GtkWidget *row = row_shell(T("Accent colour", "강조색"),
                               T("Switches, sliders and selected things", "스위치, 슬라이더, 선택한 것의 색"));
    /* Nine swatches do not fit beside the title on a narrow window; they
     * go on their own line under it. */
    GtkWidget *h = row_box(row);
    gtk_orientable_set_orientation(GTK_ORIENTABLE(h), GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_box_append(GTK_BOX(h), box);
    g_object_set_data(G_OBJECT(row), "lp-control", box);
    row_add(list, row);
    return row;
}

/* ── wallpaper ──────────────────────────────────────────────────────── */

/* The picture on the screen is lp-desktop's, and lp-desktop reads it from
 * ~/.config/lp/wallpaper - one line, a path - and watches that file
 * (desktop/desktop-icons/desktop.c). This page used to keep the choice
 * as wallpaper= in appearance.conf and tell sway with `output * bg`:
 * the button lit up and the choice was kept across restarts, but nothing
 * read it, sway's own background is under lp-desktop's opaque surface
 * (and switched off: swaybg_command -), and wayfire has no such command -
 * so the wallpaper never changed, under either compositor. The key in
 * appearance.conf is still read, once, for a choice made before. */
static char *wallpaper_file(void) { return lp_config_path("wallpaper"); }

static char *current_wallpaper(void)
{
    char *p = wallpaper_file();
    char *w = lp_slurp(p);
    g_free(p);
    if (w) g_strstrip(w);
    if (w && *w) return w;
    g_free(w);
    char *c = appearance_conf();
    w = kv_get(c, "wallpaper");
    g_free(c);
    return w;
}

static void apply_wallpaper(const char *path, gboolean report)
{
    char *p = wallpaper_file();
    char *line = g_strconcat(path, "\n", NULL);
    gboolean ok = lp_write_file(p, line);
    g_free(line);
    g_free(p);
    if (!ok) return;
    if (report) {
        char *base = g_path_get_basename(path);
        lp_toast(FALSE, T("Wallpaper: %s", "배경 화면: %s"), base);
        g_free(base);
    }
}

static void on_wall(GtkToggleButton *b, gpointer p)
{
    (void)p;
    if (!gtk_toggle_button_get_active(b) || lp_quiet) return;
    apply_wallpaper(g_object_get_data(G_OBJECT(b), "lp-path"), TRUE);
}

typedef struct { char *path; GdkPixbuf *thumb; } thumb_job_t;

static void thumb_thread(GTask *t, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    thumb_job_t *j = data;
    j->thumb = gdk_pixbuf_new_from_file_at_scale(j->path, 192, 108, FALSE, NULL);
    g_task_return_boolean(t, j->thumb != NULL);
}

static void thumb_free(gpointer p)
{
    thumb_job_t *j = p;
    g_free(j->path);
    if (j->thumb) g_object_unref(j->thumb);
    g_free(j);
}

static void thumb_done(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    GTask *t = G_TASK(res);
    thumb_job_t *j = g_task_get_task_data(t);
    GtkWidget *pic = GTK_WIDGET(src);
    if (j->thumb && gtk_widget_get_root(pic)) {
        GdkTexture *tex = gdk_texture_new_for_pixbuf(j->thumb);
        gtk_picture_set_paintable(GTK_PICTURE(pic), GDK_PAINTABLE(tex));
        g_object_unref(tex);
    }
}

static GtkWidget *wall_button(const char *path, GtkWidget *group, const char *cur)
{
    GtkWidget *b = gtk_toggle_button_new();
    gtk_widget_add_css_class(b, "lp-wallpaper");
    GtkWidget *pic = gtk_picture_new();
    gtk_widget_set_size_request(pic, 192, 108);
    gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
    gtk_button_set_child(GTK_BUTTON(b), pic);
    char *base = g_path_get_basename(path);
    gtk_widget_set_tooltip_text(b, base);
    g_object_set_data_full(G_OBJECT(b), "lp-title", base, g_free);
    g_object_set_data_full(G_OBJECT(b), "lp-path", g_strdup(path), g_free);
    if (group) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(group));
    if (cur && !strcmp(cur, path))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), TRUE);
    g_signal_connect(b, "toggled", G_CALLBACK(on_wall), NULL);

    thumb_job_t *j = g_new0(thumb_job_t, 1);
    j->path = g_strdup(path);
    GTask *t = g_task_new(pic, NULL, thumb_done, NULL);
    g_task_set_task_data(t, j, thumb_free);
    g_task_run_in_thread(t, thumb_thread);
    g_object_unref(t);
    return b;
}

static gboolean is_image(const char *n)
{
    return g_str_has_suffix(n, ".png") || g_str_has_suffix(n, ".jpg") ||
           g_str_has_suffix(n, ".jpeg") || g_str_has_suffix(n, ".webp");
}

/* Where wallpapers are: the branding track's, Debian's, the person's
 * own. One level of subdirectories, because Debian keeps each theme's
 * pictures in a folder of its own. At most twelve, in name order, and
 * not the 1920x1080 copies of the 4K ones - the same picture twice. */
static void find_walls(const char *dir, GPtrArray *out, int depth)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const char *n;
    while ((n = g_dir_read_name(d)))
        g_ptr_array_add(names, g_strdup(n));
    g_dir_close(d);
    g_ptr_array_sort(names, (GCompareFunc)g_strcmp0);
    for (guint i = 0; i < names->len && out->len < 12; i++) {
        const char *name = *(char **)&g_ptr_array_index(names, i);
        char *p = g_build_filename(dir, name, NULL);
        if (depth > 0 && g_file_test(p, G_FILE_TEST_IS_DIR))
            find_walls(p, out, depth - 1);
        else if (is_image(name) && !strstr(name, "1920x1080"))
            g_ptr_array_add(out, g_strdup(p));
        g_free(p);
    }
    g_ptr_array_free(names, TRUE);
}

static void on_chooser(GtkNativeDialog *d, int resp, gpointer p)
{
    (void)p;
    if (resp == GTK_RESPONSE_ACCEPT) {
        GFile *f = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(d));
        char *path = f ? g_file_get_path(f) : NULL;
        if (path) {
            apply_wallpaper(path, TRUE);
            lp_refresh();
        }
        g_free(path);
        if (f) g_object_unref(f);
    }
    g_object_unref(d);
}

static void on_pick_picture(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    GtkFileChooserNative *n = gtk_file_chooser_native_new(
        T("Choose a wallpaper", "배경 화면 고르기"), lp_window(),
        GTK_FILE_CHOOSER_ACTION_OPEN, T("Use", "사용"), T("Cancel", "취소"));
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, T("Pictures", "그림"));
    gtk_file_filter_add_mime_type(f, "image/png");
    gtk_file_filter_add_mime_type(f, "image/jpeg");
    gtk_file_filter_add_mime_type(f, "image/webp");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(n), f);
    g_object_unref(f);
    char *pics = g_build_filename(g_get_home_dir(), "Pictures", NULL);
    GFile *dir = g_file_new_for_path(pics);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(n), dir, NULL);
    g_object_unref(dir);
    g_free(pics);
    g_signal_connect(n, "response", G_CALLBACK(on_chooser), NULL);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(n));
}

/* ── the dock ───────────────────────────────────────────────────────── */

static const lp_opt_t DOCK_SIZE[] = {
    { "small",  "Small",  "작게" },
    { "medium", "Medium", "보통" },
    { "large",  "Large",  "크게" },
    { NULL, NULL, NULL }
};

static void on_dock_size(GObject *dd, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    const char *v = row_option_value(dd);
    if (!v) return;
    char *c = lp_config_path("dock.conf");
    if (kv_set(c, "size", v))
        lp_toast(FALSE, T("Dock size saved", "독 크기를 저장했습니다"));
    g_free(c);
}

G_GNUC_UNUSED static void on_dock_hide(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    char *c = lp_config_path("dock.conf");
    if (kv_set(c, "autohide", on ? "yes" : "no"))
        lp_toast(FALSE, on ? T("The dock hides when a window needs the room",
                               "창이 자리를 쓰면 독을 숨깁니다")
                           : T("The dock always shows", "독을 항상 보입니다"));
    g_free(c);
}

/* ── window animation ───────────────────────────────────────────────── */
/*
 * How windows open, close, snap and switch workspace: wayfire's animation
 * durations in wayfire.ini, which wayfire re-reads by itself. Off, fast,
 * normal (what the image ships) or relaxed. Under sway there are no window
 * animations to set; the choice is kept for the next wayfire session.
 * Reduce motion (Accessibility) still wins: it zeroes these on its own.
 */
static const lp_opt_t ANIM[] = {
    { "off",     "Off",     "끄기" },
    { "fast",    "Fast",    "빠르게" },
    { "normal",  "Normal",  "보통" },
    { "relaxed", "Relaxed", "느긋하게" },
    { NULL, NULL, NULL }
};

static const char *anim_ms(const char *v, gboolean window)
{
    if (!g_strcmp0(v, "off"))     return "0";
    if (!g_strcmp0(v, "fast"))    return window ? "150" : "200";
    if (!g_strcmp0(v, "relaxed")) return window ? "450" : "500";
    return window ? "300" : "340";
}

static void on_anim(GObject *dd, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    const char *v = row_option_value(dd);
    if (!v) return;
    char *wf = wayfire_ini();
    gboolean ok = ini_set(wf, "animate", "duration", anim_ms(v, TRUE)) &&
                  ini_set(wf, "grid", "duration", anim_ms(v, TRUE)) &&
                  ini_set(wf, "vswitch", "duration", anim_ms(v, FALSE)) &&
                  ini_set(wf, "vswipe", "duration", anim_ms(v, FALSE)) &&
                  ini_set(wf, "scale", "duration", anim_ms(v, FALSE)) &&
                  ini_set(wf, "expo", "duration", anim_ms(v, FALSE));
    g_free(wf);
    char *c = lp_config_path("appearance.conf");
    kv_set(c, "window-animation", v);
    g_free(c);
    if (ok)
        lp_toast(FALSE, T("Window animation: %s", "창 애니메이션: %s"),
                 !g_strcmp0(v, "off") ? T("Off", "끄기") : !g_strcmp0(v, "fast") ? T("Fast", "빠르게")
                 : !g_strcmp0(v, "relaxed") ? T("Relaxed", "느긋하게") : T("Normal", "보통"));
}

/* ── building ───────────────────────────────────────────────────────── */

static GtkWidget *build(void)
{
    GtkWidget *page = page_new(T("Appearance", "모양"),
                               lp_style_is_light() ? T("Light style", "밝은 스타일")
                                                   : T("Dark style", "어두운 스타일"));
    if (PAGE) g_object_remove_weak_pointer(G_OBJECT(PAGE), (gpointer *)&PAGE);
    PAGE = page;
    g_object_add_weak_pointer(G_OBJECT(PAGE), (gpointer *)&PAGE);

    GtkWidget *g = group_new(page, T("Style", "스타일"));
    GtkWidget *cards = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_set_halign(cards, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(cards, 12);
    gtk_widget_set_margin_bottom(cards, 12);
    LP_QUIET({
        GtkWidget *dark = style_card(T("Dark", "어둡게"), FALSE, NULL);
        gtk_box_append(GTK_BOX(cards), dark);
        gtk_box_append(GTK_BOX(cards), style_card(T("Light", "밝게"), TRUE, dark));
    });
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), cards);
    g_object_set_data_full(G_OBJECT(row), "lp-title", g_strdup(T("Dark style", "어두운 스타일")), g_free);
    gtk_list_box_append(GTK_LIST_BOX(g), row);
    LP_QUIET(accent_row(g));
    /* The same row as Accessibility's: one text size. */
    lp_text_size_row(g);

    GtkWidget *mg = group_new(page, T("Windows", "창"));
    char *ac = lp_config_path("appearance.conf");
    char *anim = kv_get(ac, "window-animation");
    row_options(mg, T("Window animation", "창 애니메이션"),
                lp_sway() ? T("Takes effect with 3D graphics (wayfire)", "3D 그래픽(wayfire)에서 적용됩니다")
                          : T("Opening, closing, snapping and switching workspaces",
                              "열기, 닫기, 스냅, 작업 공간 전환"),
                ANIM, anim ? anim : "normal", 2, G_CALLBACK(on_anim), NULL);
    g_free(anim); g_free(ac);

    GtkWidget *wg = group_new(page, T("Wallpaper", "배경 화면"));
    GtkWidget *flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 3);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 8);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 8);
    gtk_widget_set_margin_top(flow, 10);
    gtk_widget_set_margin_bottom(flow, 10);
    gtk_widget_set_margin_start(flow, 10);
    gtk_widget_set_margin_end(flow, 10);

    GPtrArray *walls = g_ptr_array_new_with_free_func(g_free);
    const char *dirs[] = { "/usr/local/share/lp/wallpaper", "/usr/local/share/backgrounds",
                           "/usr/share/backgrounds/lp", "/usr/share/backgrounds", NULL };
    for (int i = 0; dirs[i]; i++)
        find_walls(dirs[i], walls, i == 3 ? 1 : 0);
    char *shipped = g_build_filename("/usr/local/share/lp", "wallpaper.png", NULL);
    if (g_file_test(shipped, G_FILE_TEST_EXISTS)) g_ptr_array_insert(walls, 0, shipped);
    else g_free(shipped);
    char *cur = current_wallpaper();
    /* The person's own picture stays in the list after it was chosen. */
    gboolean listed = FALSE;
    for (guint i = 0; cur && i < walls->len; i++)
        listed |= !strcmp(g_ptr_array_index(walls, i), cur);
    if (cur && !listed && g_file_test(cur, G_FILE_TEST_EXISTS))
        g_ptr_array_insert(walls, 0, g_strdup(cur));

    GtkWidget *first = NULL;
    LP_QUIET({
        for (guint i = 0; i < walls->len; i++) {
            GtkWidget *b = wall_button(g_ptr_array_index(walls, i), first, cur);
            if (!first) first = b;
            gtk_flow_box_append(GTK_FLOW_BOX(flow), b);
        }
    });
    if (walls->len) {
        GtkWidget *wr = gtk_list_box_row_new();
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(wr), FALSE);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(wr), flow);
        g_object_set_data_full(G_OBJECT(wr), "lp-title", g_strdup(T("Wallpaper", "배경 화면")), g_free);
        gtk_list_box_append(GTK_LIST_BOX(wg), wr);
    } else {
        g_object_ref_sink(flow);
        g_object_unref(flow);
    }
    row_button(wg, T("Your own picture", "내 그림"),
               walls->len ? NULL : T("No wallpapers are installed", "설치된 배경 화면이 없습니다"),
               T("Choose…", "고르기…"), G_CALLBACK(on_pick_picture), NULL);
    g_ptr_array_free(walls, TRUE);
    g_free(cur);

    GtkWidget *dg = group_new(page, T("Dock", "독"));
    char *dc = lp_config_path("dock.conf");
    char *size = kv_get(dc, "size");
    char *hide = kv_get(dc, "autohide");
    row_options(dg, T("Dock size", "독 크기"), NULL, DOCK_SIZE, size, 1,
                G_CALLBACK(on_dock_size), NULL);
    g_free(size); g_free(hide); g_free(dc);
    return page;
}

/* At login: the mirrors of dark/light, and a wallpaper chosen before
 * lp-desktop's file was the one written (see apply_wallpaper), moved
 * there - lp-desktop is watching it and changes the picture at once. */
static void restore(void)
{
    char *p = lp_config_path("style");
    char *s = lp_slurp(p);
    g_free(p);
    if (s) mirror_style(g_str_has_prefix(g_strstrip(s), "light"));
    g_free(s);
    char *wf = wallpaper_file();
    if (!g_file_test(wf, G_FILE_TEST_EXISTS)) {
        char *c = appearance_conf();
        char *w = kv_get(c, "wallpaper");
        if (w && *w && g_file_test(w, G_FILE_TEST_EXISTS))
            apply_wallpaper(w, FALSE);
        g_free(w); g_free(c);
    }
    g_free(wf);
}

static const char *const KEYS[] = {
    "Dark style", "어두운 스타일",
    "Dark mode", "다크 모드",
    "Accent colour", "강조색",
    "Text size", "글자 크기",
    "Font size", "글꼴 크기",
    "Wallpaper", "배경 화면",
    "Background", "배경",
    "Dock size", "독 크기",
    "Hide the dock automatically", "독 자동 숨기기",
    NULL
};

const lp_panel_t lp_panel_appearance = {
    "appearance", "Appearance", "모양", "preferences-desktop-appearance-symbolic",
    build, KEYS, restore
};
