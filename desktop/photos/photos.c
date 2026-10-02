/* localtime_r, strcasecmp and friends hide behind -std=c11 otherwise. */
#define _DEFAULT_SOURCE 1

/*
 * photos.c - LP Photos: the viewer half.
 *
 * Opens a picture, lets you walk through the other pictures in the same
 * folder, zoom, pan, turn it, show it full screen or as a slideshow,
 * and hands it to the editor (edit.c) when you want to draw on it.
 *
 * ── Drawing the picture ──
 *
 * The canvas is a GtkDrawingArea and the picture is painted with cairo.
 * A GtkPicture would be less code, but the editor needs to draw its
 * in-progress stroke on top of the picture in the same coordinates, and
 * one widget that owns the whole transform is the simplest way to make
 * "where the mouse is" and "where the ink goes" agree at every zoom.
 *
 * Scaling a 20 MP photo down to the window on every frame is too slow
 * for this machine, and the cheap filter that is fast enough shimmers.
 * So when the zoom is below 100% the picture is scaled once, well, into
 * app->cache, and frames just copy that. While the cache is being built
 * (a moment after the zoom stops changing) frames use the cheap filter.
 * Above 100% only the visible part is sampled, which is cheap already.
 */

#include "photos.h"
#include "lp-fit.h"
#include <math.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <glib/gstdio.h>

#define SLIDE_MS   3000
#define HIDE_MS    2500
#define ZOOM_STEP  1.25
#define ZOOM_MAX   32.0

static void load_path(app_t *app, const char *path);
static void go_index(app_t *app, int idx);
static void controls_show(app_t *app);
static void update_cursor(app_t *app);
static void st_start(app_t *app);
static void info_toggle(app_t *app);

/* ── pixels ───────────────────────────────────────────────────────── */

/* cairo wants premultiplied native-endian ARGB, gdk-pixbuf gives straight
 * RGB(A) bytes. This is the one conversion everything goes through. */
cairo_surface_t *pixbuf_to_surface(GdkPixbuf *pb, gboolean *has_alpha)
{
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    int n = gdk_pixbuf_get_n_channels(pb), rs = gdk_pixbuf_get_rowstride(pb);
    gboolean alpha = gdk_pixbuf_get_has_alpha(pb);
    const guchar *src = gdk_pixbuf_read_pixels(pb);

    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return NULL;
    }
    cairo_surface_flush(s);
    guchar *dst = cairo_image_surface_get_data(s);
    int ds = cairo_image_surface_get_stride(s);
    for (int y = 0; y < h; y++) {
        const guchar *p = src + (gsize)y * rs;
        guint32 *q = (guint32 *)(dst + (gsize)y * ds);
        for (int x = 0; x < w; x++, p += n) {
            guint r = p[0], g = p[1], b = p[2], a = alpha ? p[3] : 255;
            if (a != 255) {
                r = (r * a + 127) / 255;
                g = (g * a + 127) / 255;
                b = (b * a + 127) / 255;
            }
            q[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(s);
    if (has_alpha) *has_alpha = alpha;
    return s;
}

/* Premultiplied "top over bottom" for one pixel. */
static inline guint32 px_over(guint32 top, guint32 bot)
{
    guint ta = top >> 24;
    if (ta == 255) return top;
    if (ta == 0) return bot;
    guint k = 255 - ta;
    guint32 out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        guint c = ((top >> sh) & 255) + (((bot >> sh) & 255) * k + 127) / 255;
        out |= (guint32)MIN(c, 255u) << sh;
    }
    return out;
}

/* The other way, for saving and copying, with the ink layer (if any)
 * laid over the picture on the way - that costs no second full-size
 * surface. Without alpha (JPEG) the picture is laid on white, because a
 * transparent corner saved as black surprises people. */
GdkPixbuf *surface_to_pixbuf(cairo_surface_t *s, cairo_surface_t *ink, gboolean keep_alpha)
{
    cairo_surface_flush(s);
    if (ink) cairo_surface_flush(ink);
    int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    int ss = cairo_image_surface_get_stride(s);
    const guchar *src = cairo_image_surface_get_data(s);
    const guchar *isrc = ink ? cairo_image_surface_get_data(ink) : NULL;
    int is = ink ? cairo_image_surface_get_stride(ink) : 0;
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, keep_alpha, 8, w, h);
    if (!pb) return NULL;
    int n = keep_alpha ? 4 : 3, rs = gdk_pixbuf_get_rowstride(pb);
    guchar *dst = gdk_pixbuf_get_pixels(pb);
    for (int y = 0; y < h; y++) {
        const guint32 *p = (const guint32 *)(src + (gsize)y * ss);
        const guint32 *ip = isrc ? (const guint32 *)(isrc + (gsize)y * is) : NULL;
        guchar *q = dst + (gsize)y * rs;
        for (int x = 0; x < w; x++, q += n) {
            guint32 v = ip ? px_over(ip[x], p[x]) : p[x];
            guint a = v >> 24, r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
            if (keep_alpha) {
                if (a && a != 255) {
                    r = MIN(255u, (r * 255 + a / 2) / a);
                    g = MIN(255u, (g * 255 + a / 2) / a);
                    b = MIN(255u, (b * 255 + a / 2) / a);
                }
                q[0] = r; q[1] = g; q[2] = b; q[3] = a;
            } else {
                q[0] = r + (255 - a); q[1] = g + (255 - a); q[2] = b + (255 - a);
            }
        }
    }
    return pb;
}

/* ── the clipboard ── */

/*
 * The picture goes to the clipboard as a PNG through wl-copy, the way the
 * rest of LP copies images (the screenshot tool does the same): wl-copy
 * stays behind to serve the paste after this window is closed, which a
 * clipboard owned by GTK does not. Encoding a PNG of a big photo takes a
 * moment, so it happens in a thread. Without wl-copy (not a Wayland
 * session) GTK's own clipboard is the fallback.
 */
static void clip_encode(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    gchar *buf = NULL;
    gsize len = 0;
    GError *e = NULL;
    /* Level 1: a clipboard copy is read once, soon; speed over size. */
    if (gdk_pixbuf_save_to_buffer(data, &buf, &len, "png", &e, "compression", "1", NULL))
        g_task_return_pointer(task, g_bytes_new_take(buf, len), (GDestroyNotify)g_bytes_unref);
    else
        g_task_return_error(task, e);
}

static void clip_texture(app_t *app, GBytes *png)
{
    GdkTexture *tex = gdk_texture_new_from_bytes(png, NULL);
    if (tex) {
        gdk_clipboard_set_texture(gtk_widget_get_clipboard(app->win), tex);
        g_object_unref(tex);
    }
}

static void clip_sent(GObject *o, GAsyncResult *res, gpointer d)
{
    app_t *app = d;
    GError *e = NULL;
    gboolean ok = g_subprocess_communicate_finish(G_SUBPROCESS(o), res, NULL, NULL, &e)
                  && g_subprocess_get_if_exited(G_SUBPROCESS(o))
                  && g_subprocess_get_exit_status(G_SUBPROCESS(o)) == 0;
    GBytes *png = g_object_get_data(o, "png");
    if (!ok && png) clip_texture(app, png);
    g_clear_error(&e);
    toast(app, T("Copied to the clipboard", "클립보드에 복사했습니다"));
    if (app->st_cmds) g_print("selftest: copied (%s)\n", ok ? "wl-copy" : "gtk");
    g_object_unref(o);
}

static void clip_encoded(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)src;
    app_t *app = d;
    GError *e = NULL;
    GBytes *png = g_task_propagate_pointer(G_TASK(res), &e);
    if (!png) {
        char *m = g_strdup_printf(T("Could not copy: %s", "복사하지 못했습니다: %s"), e->message);
        toast(app, m);
        g_free(m);
        g_error_free(e);
        return;
    }
    char *wl = g_find_program_in_path("wl-copy");
    GSubprocess *p = wl && g_getenv("WAYLAND_DISPLAY")
        ? g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                           | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                           wl, "--type", "image/png", NULL)
        : NULL;
    g_free(wl);
    if (p) {
        g_object_set_data_full(G_OBJECT(p), "png", g_bytes_ref(png), (GDestroyNotify)g_bytes_unref);
        g_subprocess_communicate_async(p, png, NULL, clip_sent, app);
    } else {
        clip_texture(app, png);
        toast(app, T("Copied to the clipboard", "클립보드에 복사했습니다"));
        if (app->st_cmds) g_print("selftest: copied (gtk)\n");
    }
    g_bytes_unref(png);
}

void surface_to_clipboard(app_t *app)
{
    if (!app->img) return;
    /* The pixels are taken now, on this thread: the picture may be drawn
     * on again while the PNG is being made. */
    GdkPixbuf *pb = surface_to_pixbuf(app->img, app->ann, TRUE);
    if (!pb) return;
    GTask *task = g_task_new(NULL, NULL, clip_encoded, app);
    g_task_set_task_data(task, pb, g_object_unref);
    g_task_run_in_thread(task, clip_encode);
    g_object_unref(task);
}

/* ── the folder ───────────────────────────────────────────────────── */

/* Which names count as pictures is whatever gdk-pixbuf can read on this
 * machine - if a WebP or HEIF loader is installed, those join in. */
static gboolean is_image_name(const char *name)
{
    static GHashTable *exts;
    if (!exts) {
        exts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        GSList *fmts = gdk_pixbuf_get_formats();
        for (GSList *l = fmts; l; l = l->next) {
            char **e = gdk_pixbuf_format_get_extensions(l->data);
            for (int i = 0; e && e[i]; i++)
                g_hash_table_add(exts, g_ascii_strdown(e[i], -1));
            g_strfreev(e);
        }
        g_slist_free(fmts);
        const char *extra[] = { "jpg", "jpeg", "png", "gif", "webp", "bmp",
                                "tif", "tiff", "ico", "svg", NULL };
        for (int i = 0; extra[i]; i++)
            g_hash_table_add(exts, g_strdup(extra[i]));
    }
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return FALSE;
    char *low = g_ascii_strdown(dot + 1, -1);
    gboolean ok = g_hash_table_contains(exts, low);
    g_free(low);
    return ok;
}

typedef struct { char *key; char *path; } sort_item_t;

static int sort_cmp(const void *a, const void *b)
{
    const sort_item_t *x = a, *y = b;
    return strcmp(x->key, y->key);
}

/* Sorted the way a person reads names: "IMG_2" before "IMG_10". */
static GPtrArray *scan_dir(const char *dir)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return out;
    GArray *items = g_array_new(FALSE, FALSE, sizeof(sort_item_t));
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (name[0] == '.' || !is_image_name(name)) continue;
        char *full = g_build_filename(dir, name, NULL);
        if (!g_file_test(full, G_FILE_TEST_IS_REGULAR)) { g_free(full); continue; }
        sort_item_t it = { g_utf8_collate_key_for_filename(name, -1), full };
        g_array_append_val(items, it);
    }
    g_dir_close(d);
    qsort(items->data, items->len, sizeof(sort_item_t), sort_cmp);
    for (guint i = 0; i < items->len; i++) {
        sort_item_t *it = &g_array_index(items, sort_item_t, i);
        g_ptr_array_add(out, it->path);
        g_free(it->key);
    }
    g_array_free(items, TRUE);
    return out;
}

static int find_in_files(app_t *app, const char *path)
{
    for (guint i = 0; app->files && i < app->files->len; i++)
        if (strcmp(g_ptr_array_index(app->files, i), path) == 0) return (int)i;
    return -1;
}

/* Point the list at `path`, rescanning its folder if it is a new one or
 * `force` says the folder changed under us (delete, save as). */
static void files_for(app_t *app, const char *path, gboolean force)
{
    char *dir = g_path_get_dirname(path);
    if (force || !app->files || g_strcmp0(dir, app->dir) != 0) {
        if (app->files) g_ptr_array_unref(app->files);
        app->files = scan_dir(dir);
        g_free(app->dir);
        app->dir = g_strdup(dir);
    }
    int i = find_in_files(app, path);
    if (i < 0) {
        /* Opened by name with an extension we do not recognise: it is
         * still the one the person asked for, so it goes in the list. */
        g_ptr_array_insert(app->files, 0, g_strdup(path));
        i = 0;
    }
    app->index = i;
    g_free(dir);
}

/* ── title, info, toast ───────────────────────────────────────────── */

void title_update(app_t *app)
{
    char *base = app->path ? g_path_get_basename(app->path) : NULL;
    gtk_label_set_text(GTK_LABEL(app->title), base ? base : T("Photos", "사진"));

    char *sub = NULL;
    if (app->editing) {
        sub = g_strdup(edit_dirty(app) ? T("Editing · not saved", "편집 중 · 저장 안 함")
                                       : T("Editing", "편집 중"));
    } else if (app->path && app->files && app->files->len > 0) {
        /* What people check first about a photo - how big it is - stays
         * in sight without opening the details panel. */
        GString *g = g_string_new(NULL);
        g_string_append_printf(g, "%d / %u", app->index + 1, app->files->len);
        if (app->img && !app->loading)
            g_string_append_printf(g, "  ·  %d × %d", app->iw, app->ih);
        if (app->file_size > 0 && !app->loading) {
            char *sz = g_format_size(app->file_size);
            g_string_append_printf(g, "  ·  %s", sz);
            g_free(sz);
        }
        sub = g_string_free(g, FALSE);
    }
    gtk_label_set_text(GTK_LABEL(app->subtitle), sub ? sub : "");
    gtk_widget_set_visible(app->subtitle, sub != NULL);
    if (app->count_label) {
        char *cnt = (app->files && app->path)
                    ? g_strdup_printf("%d / %u", app->index + 1, app->files->len)
                    : g_strdup("");
        gtk_label_set_text(GTK_LABEL(app->count_label), cnt);
        g_free(cnt);
    }

    char *wt = base ? g_strdup_printf("%s — %s", base, T("Photos", "사진"))
                    : g_strdup(T("Photos", "사진"));
    gtk_window_set_title(GTK_WINDOW(app->win), wt);
    g_free(wt); g_free(sub); g_free(base);

    gboolean multi = app->files && app->files->len > 1;
    gtk_widget_set_sensitive(app->prev_btn, multi);
    gtk_widget_set_sensitive(app->next_btn, multi);
    gtk_widget_set_sensitive(app->slide_btn, multi);
}

static void info_update(app_t *app)
{
    for (int i = 0; i < 6; i++) gtk_label_set_text(GTK_LABEL(app->info_val[i]), "—");
    if (!app->path) return;

    char *base = g_path_get_basename(app->path);
    char *dir = g_path_get_dirname(app->path);
    gtk_label_set_text(GTK_LABEL(app->info_val[0]), base);
    gtk_label_set_text(GTK_LABEL(app->info_val[1]), dir);
    g_free(base); g_free(dir);

    if (app->img) {
        char *d = g_strdup_printf(T("%d × %d pixels", "%d × %d 픽셀"), app->iw, app->ih);
        gtk_label_set_text(GTK_LABEL(app->info_val[2]), d);
        g_free(d);
    }
    GFile *f = g_file_new_for_path(app->path);
    GFileInfo *fi = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_SIZE ","
                                      G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                      G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (fi) {
        char *sz = g_format_size(g_file_info_get_size(fi));
        gtk_label_set_text(GTK_LABEL(app->info_val[3]), sz);
        g_free(sz);
        GDateTime *mt = g_file_info_get_modification_date_time(fi);
        if (mt) {
            GDateTime *lt = g_date_time_to_local(mt);
            char *ds = g_date_time_format(lt, T("%b %-d, %Y  %H:%M", "%Y년 %-m월 %-d일 %H:%M"));
            gtk_label_set_text(GTK_LABEL(app->info_val[4]), ds);
            g_free(ds);
            g_date_time_unref(lt);
            g_date_time_unref(mt);
        }
        g_object_unref(fi);
    }
    g_object_unref(f);
    if (app->fmt_desc || app->fmt_name) {
        /* gdk-pixbuf's description is the readable one ("JPEG", "WebP");
         * the short name is only a fallback. */
        char *up = app->fmt_name ? g_ascii_strup(app->fmt_name, -1) : NULL;
        gtk_label_set_text(GTK_LABEL(app->info_val[5]), app->fmt_desc ? app->fmt_desc : up);
        g_free(up);
    }
}

static gboolean toast_hide(gpointer d)
{
    app_t *app = d;
    gtk_widget_set_visible(app->toast, FALSE);
    app->toast_timer = 0;
    return G_SOURCE_REMOVE;
}

void toast(app_t *app, const char *msg)
{
    gtk_label_set_text(GTK_LABEL(app->toast), msg);
    gtk_widget_set_visible(app->toast, TRUE);
    if (app->toast_timer) g_source_remove(app->toast_timer);
    app->toast_timer = g_timeout_add(2600, toast_hide, app);
}

/* ── view geometry ────────────────────────────────────────────────── */

static int cw(app_t *app) { return gtk_widget_get_width(app->canvas); }
static int ch(app_t *app) { return gtk_widget_get_height(app->canvas); }

void view_queue(app_t *app) { gtk_widget_queue_draw(app->canvas); }

void view_w2i(app_t *app, double wx, double wy, double *ix, double *iy)
{
    *ix = (wx - app->ox) / app->zoom;
    *iy = (wy - app->oy) / app->zoom;
}

static double fit_zoom(app_t *app)
{
    int w = cw(app), h = ch(app);
    if (!app->img || w <= 0 || h <= 0) return 1.0;
    double z = MIN((double)w / app->iw, (double)h / app->ih);
    /* A small picture is shown at its real size, not blown up: enlarging
     * an icon to fill the screen shows nothing new, only blur. */
    return MIN(z, 1.0);
}

static void zoom_label_update(app_t *app)
{
    char s[32];
    g_snprintf(s, sizeof s, "%d%%", (int)lround(app->zoom * 100));
    gtk_label_set_text(GTK_LABEL(app->zoom_label), s);
}

/* Keep the picture on screen: centred on an axis where it is smaller than
 * the window, and not dragged past its edge where it is larger. */
static void view_clamp(app_t *app)
{
    int w = cw(app), h = ch(app);
    double dw = app->iw * app->zoom, dh = app->ih * app->zoom;
    app->ox = dw <= w ? (w - dw) / 2 : CLAMP(app->ox, w - dw, 0);
    app->oy = dh <= h ? (h - dh) / 2 : CLAMP(app->oy, h - dh, 0);
}

void view_fit(app_t *app)
{
    app->fit = TRUE;
    if (!app->img) return;
    app->zoom = fit_zoom(app);
    view_clamp(app);
    zoom_label_update(app);
    view_queue(app);
    update_cursor(app);
}

static void zoom_at(app_t *app, double z, double wx, double wy)
{
    if (!app->img) return;
    double zmin = MIN(fit_zoom(app), 1.0) / 4;
    z = CLAMP(z, zmin, ZOOM_MAX);
    if (fabs(z - 1.0) < 0.02) z = 1.0;     /* land on 100% when passing it */
    double ix, iy;
    view_w2i(app, wx, wy, &ix, &iy);
    app->zoom = z;
    app->ox = wx - ix * z;
    app->oy = wy - iy * z;
    app->fit = FALSE;
    view_clamp(app);
    zoom_label_update(app);
    view_queue(app);
    update_cursor(app);
}

static void zoom_step(app_t *app, int dir)
{
    zoom_at(app, dir > 0 ? app->zoom * ZOOM_STEP : app->zoom / ZOOM_STEP,
            cw(app) / 2.0, ch(app) / 2.0);
}

static void zoom_actual(app_t *app, double wx, double wy)
{
    zoom_at(app, 1.0, wx, wy);
}

static void cache_drop(app_t *app)
{
    if (app->cache) cairo_surface_destroy(app->cache);
    app->cache = NULL;
    app->cache_zoom = 0;
}

/* The picture (and its ink) shrunk to zoom `z`. GOOD averages every
 * source pixel under a screen pixel - the expensive, correct filter,
 * paid once per zoom. Touches nothing but its arguments, so the loader
 * thread can call it too. */
static cairo_surface_t *scaled_copy(cairo_surface_t *img, cairo_surface_t *ink, double z)
{
    int w = MAX(1, (int)ceil(cairo_image_surface_get_width(img) * z));
    int h = MAX(1, (int)ceil(cairo_image_surface_get_height(img) * z));
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(s);
    cairo_scale(cr, z, z);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    if (ink) {
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_set_source_surface(cr, ink, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
    }
    cairo_destroy(cr);
    return s;
}

static void cache_build(app_t *app)
{
    if (app->cache_timer) { g_source_remove(app->cache_timer); app->cache_timer = 0; }
    cache_drop(app);
    if (!app->img || app->zoom >= 0.999) return;
    gint64 t0 = g_get_monotonic_time();
    app->cache = scaled_copy(app->img, app->ann, app->zoom);
    app->cache_zoom = app->zoom;
    if (app->st_cmds) g_print("cache %dx%d built in %.0f ms\n", cairo_image_surface_get_width(app->cache),
                              cairo_image_surface_get_height(app->cache), (g_get_monotonic_time() - t0) / 1e3);
}

static gboolean cache_timeout(gpointer d)
{
    app_t *app = d;
    app->cache_timer = 0;
    cache_build(app);
    view_queue(app);
    return G_SOURCE_REMOVE;
}

void view_image_changed(app_t *app)
{
    cache_drop(app);
    if (app->img) {
        app->iw = cairo_image_surface_get_width(app->img);
        app->ih = cairo_image_surface_get_height(app->img);
    }
    if (app->fit) view_fit(app);
    else { view_clamp(app); zoom_label_update(app); }
    view_queue(app);
    info_update(app);
}

/* An edit touched only this rectangle (image coordinates): redo that
 * part of the cache instead of all of it. */
void view_region_changed(app_t *app, double x, double y, double w, double h)
{
    if (app->cache && app->cache_zoom > 0) {
        double z = app->cache_zoom;
        cairo_t *cr = cairo_create(app->cache);
        cairo_rectangle(cr, floor(x * z) - 2, floor(y * z) - 2,
                        ceil(w * z) + 4, ceil(h * z) + 4);
        cairo_clip(cr);
        cairo_scale(cr, z, z);
        cairo_set_source_surface(cr, app->img, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        paint_ink(app, cr, CAIRO_FILTER_GOOD);
        cairo_destroy(cr);
    }
    view_queue(app);
}

/* The ink layer, in picture coordinates, over whatever `cr` has. */
void paint_ink(app_t *app, cairo_t *cr, cairo_filter_t f)
{
    if (!app->ann) return;
    cairo_set_source_surface(cr, app->ann, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), f);
    cairo_paint(cr);
}

/* ── the canvas ───────────────────────────────────────────────────── */

static void draw_checker(cairo_t *cr, double x, double y, double w, double h)
{
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
    cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.62, 0.62, 0.62);
    const int s = 10;
    for (int yy = 0; yy * s < h; yy++)
        for (int xx = (yy & 1); xx * s < w; xx += 2)
            cairo_rectangle(cr, x + xx * s, y + yy * s, s, s);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void draw_fn(GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer d)
{
    (void)da;
    app_t *app = d;
    if (!app->img) {
        if (app->path && !app->loading) {
            PangoLayout *pl = gtk_widget_create_pango_layout(app->canvas,
                T("This file cannot be shown.", "이 파일은 표시할 수 없습니다."));
            int lw, lh;
            pango_layout_get_pixel_size(pl, &lw, &lh);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.7);
            cairo_move_to(cr, (w - lw) / 2.0, (h - lh) / 2.0);
            pango_cairo_show_layout(cr, pl);
            g_object_unref(pl);
        }
        return;
    }

    double z = app->zoom;
    double dw = app->iw * z, dh = app->ih * z;
    if (app->has_alpha) draw_checker(cr, app->ox, app->oy, dw, dh);

    gboolean done = FALSE;
    if (!app->adjusting && z < 0.999) {
        if (!app->cache || app->cache_zoom != z) {
            if ((double)app->iw * app->ih <= 4e6)
                cache_build(app);                 /* small: just do it */
            else if (!app->cache_timer)
                app->cache_timer = g_timeout_add(120, cache_timeout, app);
        }
        if (app->cache && app->cache_zoom == z) {
            double x = round(app->ox), y = round(app->oy);
            cairo_set_source_surface(cr, app->cache, x, y);
            cairo_rectangle(cr, x, y, cairo_image_surface_get_width(app->cache),
                            cairo_image_surface_get_height(app->cache));
            cairo_fill(cr);
            done = TRUE;
        }
    }
    if (!done) {
        cairo_surface_t *src = app->img;
        double k = z;
        if (app->adjusting && app->adj_prev) {
            src = app->adj_prev;
            k = z / app->adj_scale;
        }
        cairo_save(cr);
        cairo_rectangle(cr, app->ox, app->oy, dw, dh);
        cairo_clip(cr);
        cairo_translate(cr, app->ox, app->oy);
        cairo_scale(cr, k, k);
        cairo_set_source_surface(cr, src, 0, 0);
        cairo_filter_t f = k >= 2.0 ? CAIRO_FILTER_NEAREST
                         : (k < 1.0 && src == app->img) ? CAIRO_FILTER_FAST
                         : CAIRO_FILTER_BILINEAR;
        cairo_pattern_set_filter(cairo_get_source(cr), f);
        cairo_paint(cr);
        if (app->ann) {
            /* ink is always full size; undo the preview's scale for it */
            cairo_scale(cr, 1 / k * z, 1 / k * z);
            paint_ink(app, cr, z >= 2.0 ? CAIRO_FILTER_NEAREST
                               : z < 1.0 ? CAIRO_FILTER_FAST : CAIRO_FILTER_BILINEAR);
        }
        cairo_restore(cr);
    }

    if (app->editing) edit_draw_overlay(app, cr);
}

static void on_resize(GtkDrawingArea *da, int w, int h, gpointer d)
{
    (void)da; (void)w; (void)h;
    app_t *app = d;
    if (!app->img) return;
    if (app->fit) view_fit(app);
    else view_clamp(app);
}

static gboolean pannable(app_t *app)
{
    return app->img && (app->iw * app->zoom > cw(app) + 0.5 ||
                        app->ih * app->zoom > ch(app) + 0.5);
}

static void update_cursor(app_t *app)
{
    const char *name = "default";
    if (app->fullscreen && !gtk_revealer_get_reveal_child(GTK_REVEALER(app->navbar)))
        name = "none";
    else if (app->editing && app->tool != TOOL_MOVE && !app->adjusting)
        name = app->tool == TOOL_TEXT ? "text" : "crosshair";
    else if (app->panning)
        name = "grabbing";
    else if (pannable(app))
        name = "grab";
    gtk_widget_set_cursor_from_name(app->canvas, name);
}

static void drag_begin(GtkGestureDrag *g, double x, double y, gpointer d)
{
    app_t *app = d;
    guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    app->press_wx = x; app->press_wy = y;
    app->dragging = TRUE;
    if (!app->img || btn == GDK_BUTTON_SECONDARY) { app->dragging = FALSE; return; }
    if (app->editing && btn == GDK_BUTTON_PRIMARY && app->tool != TOOL_MOVE
        && !app->adjusting) {
        app->panning = FALSE;
        edit_press(app, x, y);
        return;
    }
    app->panning = TRUE;
    app->pan_ox0 = app->ox; app->pan_oy0 = app->oy;
    update_cursor(app);
}

static void drag_update(GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    (void)g;
    app_t *app = d;
    if (!app->dragging) return;
    if (app->panning) {
        app->ox = app->pan_ox0 + dx;
        app->oy = app->pan_oy0 + dy;
        app->fit = FALSE;
        view_clamp(app);
        view_queue(app);
    } else {
        edit_motion(app, app->press_wx + dx, app->press_wy + dy);
    }
}

static void drag_end(GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    (void)g;
    app_t *app = d;
    if (!app->dragging) return;
    app->dragging = FALSE;
    if (app->panning) {
        app->panning = FALSE;
        update_cursor(app);
    } else {
        edit_release(app, app->press_wx + dx, app->press_wy + dy);
    }
}

static void on_click(GtkGestureClick *g, int n, double x, double y, gpointer d)
{
    (void)g;
    app_t *app = d;
    if (n != 2 || !app->img) return;
    if (app->editing && app->tool != TOOL_MOVE) return;
    /* Double-click: between "all of it" and "every pixel of it". */
    if (fabs(app->zoom - fit_zoom(app)) < 1e-6 && fit_zoom(app) < 1.0)
        zoom_actual(app, x, y);
    else
        view_fit(app);
}

static double ptr_x, ptr_y;

/* Two fingers on a touchpad or screen: the picture follows the pinch,
 * centred between the fingers. */
static void pinch_begin(GtkGesture *g, GdkEventSequence *seq, gpointer d)
{
    (void)g; (void)seq;
    app_t *app = d;
    app->pinch_zoom0 = app->zoom;
}

static void pinch_scale(GtkGestureZoom *g, double scale, gpointer d)
{
    app_t *app = d;
    double cx, cy;
    if (!app->img || app->pinch_zoom0 <= 0) return;
    if (!gtk_gesture_get_bounding_box_center(GTK_GESTURE(g), &cx, &cy)) {
        cx = cw(app) / 2.0; cy = ch(app) / 2.0;
    }
    zoom_at(app, app->pinch_zoom0 * scale, cx, cy);
}

/* The self test's pinch: the same arithmetic, with no fingers to ask
 * where the middle is. */
static void pinch_fake(app_t *app, double scale)
{
    app->pinch_zoom0 = app->zoom;
    zoom_at(app, app->pinch_zoom0 * scale, cw(app) / 2.0, ch(app) / 2.0);
}

static gboolean on_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer d)
{
    app_t *app = d;
    if (!app->img) return FALSE;
    GdkModifierType m = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(c));
    gboolean wheel = gtk_event_controller_scroll_get_unit(c) == GDK_SCROLL_UNIT_WHEEL;
    /* A mouse wheel zooms, at the pointer - in a picture viewer that is
     * what a wheel is for. Two fingers on a touchpad pan (they have the
     * pinch to zoom), unless Ctrl is held. Shift+wheel pans sideways. */
    if ((m & GDK_CONTROL_MASK) || (wheel && !(m & GDK_SHIFT_MASK) && dy != 0)) {
        double f = wheel ? pow(ZOOM_STEP, -dy) : pow(1.01, -dy);
        zoom_at(app, app->zoom * f, ptr_x, ptr_y);
        return TRUE;
    }
    if (!pannable(app)) return FALSE;
    double k = wheel ? 60 : 1;
    if (m & GDK_SHIFT_MASK) { dx = dy; dy = 0; }
    app->ox -= dx * k;
    app->oy -= dy * k;
    app->fit = FALSE;
    view_clamp(app);
    view_queue(app);
    return TRUE;
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer d)
{
    (void)c;
    app_t *app = d;
    /* Wayland repeats motion with the same position when nothing moved
     * (e.g. after a redraw); only real movement wakes the controls. */
    if (fabs(x - ptr_x) < 0.5 && fabs(y - ptr_y) < 0.5) return;
    ptr_x = x; ptr_y = y;
    if (app->fullscreen) controls_show(app);
}

/* ── loading ──────────────────────────────────────────────────────── */

typedef struct {
    app_t *app;
    guint gen;
    char *path;
    GdkPixbufAnimation *anim;
    cairo_surface_t *surf;
    gboolean alpha;
    char *fmt_name, *fmt_desc, *err;
    gint64 size;
    double ms;
    int cw, ch;                  /* canvas size when the load began */
    cairo_surface_t *cache;      /* the fit-to-window cache, made here too */
    double cache_zoom;
} load_t;

static void load_free(gpointer p)
{
    load_t *L = p;
    g_free(L->path); g_free(L->fmt_name); g_free(L->fmt_desc); g_free(L->err);
    if (L->anim) g_object_unref(L->anim);
    if (L->surf) cairo_surface_destroy(L->surf);
    if (L->cache) cairo_surface_destroy(L->cache);
    g_free(L);
}

/* Decoding a 20 MP JPEG takes seconds on this machine; in a thread the
 * window keeps answering while it happens. */
static void load_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    load_t *L = data;
    gint64 t0 = g_get_monotonic_time();
    GStatBuf st;
    if (g_stat(L->path, &st) == 0) L->size = st.st_size;
    GdkPixbufFormat *f = gdk_pixbuf_get_file_info(L->path, NULL, NULL);
    if (f) {
        L->fmt_name = gdk_pixbuf_format_get_name(f);
        L->fmt_desc = gdk_pixbuf_format_get_description(f);
    }
    GError *e = NULL;
    if (L->fmt_name && (!strcmp(L->fmt_name, "gif") || !strcmp(L->fmt_name, "webp"))) {
        GdkPixbufAnimation *an = gdk_pixbuf_animation_new_from_file(L->path, &e);
        if (an && !gdk_pixbuf_animation_is_static_image(an)) {
            L->anim = an;
            g_task_return_boolean(task, TRUE);
            return;
        }
        if (an) {
            L->surf = pixbuf_to_surface(gdk_pixbuf_animation_get_static_image(an), &L->alpha);
            g_object_unref(an);
        }
        g_clear_error(&e);
    }
    if (!L->surf) {
        GdkPixbuf *pb = gdk_pixbuf_new_from_file(L->path, &e);
        if (pb) {
            /* Phones store portraits sideways plus a note saying so. */
            GdkPixbuf *o = gdk_pixbuf_apply_embedded_orientation(pb);
            g_object_unref(pb);
            L->surf = pixbuf_to_surface(o, &L->alpha);
            g_object_unref(o);
        }
    }
    if (!L->surf && !L->anim)
        L->err = g_strdup(e ? e->message : "?");
    g_clear_error(&e);
    /* A big picture shown whole needs its scaled-down copy before the
     * first frame; making it here, with the same numbers fit_zoom will
     * come to, keeps that off the main thread as well. */
    if (L->surf && L->cw > 0 && L->ch > 0) {
        int w = cairo_image_surface_get_width(L->surf), h = cairo_image_surface_get_height(L->surf);
        double z = MIN(MIN((double)L->cw / w, (double)L->ch / h), 1.0);
        if (z < 0.999 && (double)w * h > 4e6) {
            L->cache = scaled_copy(L->surf, NULL, z);
            L->cache_zoom = z;
        }
    }
    L->ms = (g_get_monotonic_time() - t0) / 1e3;
    g_task_return_boolean(task, TRUE);
}

static void anim_stop(app_t *app)
{
    if (app->anim_timer) g_source_remove(app->anim_timer);
    app->anim_timer = 0;
    g_clear_object(&app->iter);
    g_clear_object(&app->anim);
}

static void anim_schedule(app_t *app);

static gboolean anim_tick(gpointer d)
{
    app_t *app = d;
    app->anim_timer = 0;
    if (!app->iter) return G_SOURCE_REMOVE;
    gdk_pixbuf_animation_iter_advance(app->iter, NULL);
    GdkPixbuf *pb = gdk_pixbuf_animation_iter_get_pixbuf(app->iter);
    cairo_surface_t *s = pb ? pixbuf_to_surface(pb, NULL) : NULL;
    if (s) {
        if (app->img) cairo_surface_destroy(app->img);
        app->img = s;
        cache_drop(app);
        view_queue(app);
    }
    anim_schedule(app);
    return G_SOURCE_REMOVE;
}

static void anim_schedule(app_t *app)
{
    int ms = gdk_pixbuf_animation_iter_get_delay_time(app->iter);
    if (ms < 0) return;                     /* last frame stays */
    app->anim_timer = g_timeout_add(MAX(ms, 20), anim_tick, app);
}

static void set_image(app_t *app, cairo_surface_t *s, gboolean alpha)
{
    /* A new picture ends any editing of the old one; every path here
     * has already asked about unsaved edits. */
    edit_leave_now(app);
    if (app->img) cairo_surface_destroy(app->img);
    app->img = s;
    app->has_alpha = alpha;
    ink_drop(app);
    undo_clear(app);
    app->fit = TRUE;
    view_image_changed(app);
}

static void load_done(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)src; (void)d;
    load_t *L = g_task_get_task_data(G_TASK(res));
    app_t *app = L->app;
    if (L->gen != app->load_gen) return;          /* superseded */
    app->loading = FALSE;
    gtk_widget_set_visible(app->spinner, FALSE);
    gtk_spinner_stop(GTK_SPINNER(app->spinner));

    g_free(app->fmt_name); app->fmt_name = g_steal_pointer(&L->fmt_name);
    g_free(app->fmt_desc); app->fmt_desc = g_steal_pointer(&L->fmt_desc);
    app->file_size = L->size;
    if (app->st_cmds) g_print("selftest: decoded %s in %.0f ms (off the main thread)\n", L->path, L->ms);

    if (L->anim) {
        app->anim = g_steal_pointer(&L->anim);
        app->iter = gdk_pixbuf_animation_get_iter(app->anim, NULL);
        GdkPixbuf *pb = gdk_pixbuf_animation_iter_get_pixbuf(app->iter);
        gboolean alpha = FALSE;
        set_image(app, pb ? pixbuf_to_surface(pb, &alpha) : NULL, alpha);
        anim_schedule(app);
    } else if (L->surf) {
        set_image(app, g_steal_pointer(&L->surf), L->alpha);
        if (L->cache && app->zoom == L->cache_zoom && !app->cache) {
            app->cache = g_steal_pointer(&L->cache);
            app->cache_zoom = L->cache_zoom;
            view_queue(app);
        }
    } else {
        set_image(app, NULL, FALSE);
        char *base = g_path_get_basename(L->path);
        char *m = g_strdup_printf(T("Could not open %s", "%s 을(를) 열 수 없습니다"), base);
        toast(app, m);
        g_free(m); g_free(base);
    }
    gtk_widget_set_sensitive(app->edit_btn, app->img != NULL);
    gtk_widget_set_sensitive(app->copy_btn, app->img != NULL);
    title_update(app);
    info_update(app);
    update_cursor(app);
    if (app->st_cmds && app->st_pos == 0) st_start(app);
}

static void load_path(app_t *app, const char *path)
{
    anim_stop(app);
    char *keep = g_strdup(path);          /* path may be app->path itself */
    g_free(app->path);
    app->path = keep;
    path = keep;
    files_for(app, path, FALSE);
    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "image");
    gtk_widget_set_visible(app->hb_view_end, !app->editing);

    load_t *L = g_new0(load_t, 1);
    L->app = app;
    L->gen = ++app->load_gen;
    L->path = g_strdup(path);
    L->cw = cw(app);
    L->ch = ch(app);
    app->loading = TRUE;
    /* The old picture stays up until the new one is decoded; a spinner
     * says something is happening if that takes a while. */
    gtk_widget_set_visible(app->spinner, TRUE);
    gtk_spinner_start(GTK_SPINNER(app->spinner));
    title_update(app);

    GTask *task = g_task_new(NULL, NULL, load_done, NULL);
    g_task_set_task_data(task, L, load_free);
    g_task_run_in_thread(task, load_thread);
    g_object_unref(task);
}

void reload_current(app_t *app)
{
    if (app->path) {
        char *p = g_strdup(app->path);
        load_path(app, p);
        g_free(p);
    }
}

static void show_empty(app_t *app)
{
    anim_stop(app);
    if (app->img) cairo_surface_destroy(app->img);
    app->img = NULL;
    ink_drop(app);
    cache_drop(app);
    undo_clear(app);
    g_free(app->path); app->path = NULL;
    app->file_size = 0;
    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "empty");
    gtk_widget_set_sensitive(app->edit_btn, FALSE);
    gtk_widget_set_sensitive(app->copy_btn, FALSE);
    /* Nothing to copy, edit or show full screen: say so by absence. */
    gtk_widget_set_visible(app->hb_view_end, FALSE);
    if (gtk_revealer_get_reveal_child(GTK_REVEALER(app->info_rev))) info_toggle(app);
    title_update(app);
    info_update(app);
}

/* After Save As the picture on screen *is* the new file. */
void after_save_as(app_t *app, const char *path)
{
    char *keep = g_strdup(path);          /* path may be app->path itself */
    g_free(app->path);
    app->path = keep;
    path = keep;
    files_for(app, path, TRUE);
    GStatBuf st;
    if (g_stat(path, &st) == 0) app->file_size = st.st_size;
    GdkPixbufFormat *f = gdk_pixbuf_get_file_info(path, NULL, NULL);
    if (f) {
        g_free(app->fmt_name); app->fmt_name = gdk_pixbuf_format_get_name(f);
        g_free(app->fmt_desc); app->fmt_desc = gdk_pixbuf_format_get_description(f);
    }
    title_update(app);
    info_update(app);
}

/* ── going places (with the unsaved-edits question in between) ────── */

static void cont_go(app_t *app, gpointer data)
{
    int idx = GPOINTER_TO_INT(data);
    edit_leave_now(app);
    if (app->files && idx >= 0 && idx < (int)app->files->len)
        load_path(app, g_ptr_array_index(app->files, idx));
}

static void go_index(app_t *app, int idx)
{
    if (!app->files || app->files->len == 0) return;
    int n = app->files->len;
    idx = ((idx % n) + n) % n;
    /* Turning a picture in the viewer is an edit too; it is not thrown
     * away without asking, any more than a drawing would be. */
    if (edit_dirty(app) && !app->slideshow) {
        ask_unsaved(app, cont_go, GINT_TO_POINTER(idx), NULL);
        return;
    }
    cont_go(app, GINT_TO_POINTER(idx));
}

static void cont_open(app_t *app, gpointer data)
{
    edit_leave_now(app);
    load_path(app, data);
}

static void open_path(app_t *app, const char *path)
{
    if (edit_dirty(app)) {
        ask_unsaved(app, cont_open, g_strdup(path), g_free);
        return;
    }
    cont_open(app, (gpointer)path);
}

/* Several files at once (from the file manager or a drop): those are
 * the list to walk through, not the whole folder. */
static void open_many(app_t *app, GFile **files, int n)
{
    if (n <= 0) return;
    if (n == 1) {
        char *p = g_file_get_path(files[0]);
        if (p) open_path(app, p);
        g_free(p);
        return;
    }
    if (edit_dirty(app)) {
        char *p = g_file_get_path(files[0]);
        if (p) open_path(app, p);            /* asks; then the folder */
        g_free(p);
        return;
    }
    GPtrArray *list = g_ptr_array_new_with_free_func(g_free);
    for (int i = 0; i < n; i++) {
        char *p = g_file_get_path(files[i]);
        if (p) g_ptr_array_add(list, p);
    }
    if (list->len == 0) { g_ptr_array_unref(list); return; }
    edit_leave_now(app);
    if (app->files) g_ptr_array_unref(app->files);
    app->files = list;
    g_free(app->dir);
    app->dir = g_path_get_dirname(g_ptr_array_index(list, 0));
    load_path(app, g_ptr_array_index(list, 0));
}

static void on_chooser_response(GtkNativeDialog *nd, int resp, gpointer d)
{
    app_t *app = d;
    if (resp == GTK_RESPONSE_ACCEPT) {
        GFile *f = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(nd));
        if (f) {
            char *p = g_file_get_path(f);
            if (p) open_path(app, p);
            g_free(p);
            g_object_unref(f);
        }
    }
    g_object_unref(nd);
}

static void open_dialog(app_t *app)
{
    GtkFileChooserNative *nd = gtk_file_chooser_native_new(
        T("Open a Picture", "사진 열기"), GTK_WINDOW(app->win),
        GTK_FILE_CHOOSER_ACTION_OPEN, T("Open", "열기"), T("Cancel", "취소"));
    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, T("Pictures", "사진"));
    gtk_file_filter_add_pixbuf_formats(ff);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(nd), ff);
    g_object_unref(ff);
    const char *start = app->dir ? app->dir : g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    if (start) {
        GFile *df = g_file_new_for_path(start);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(nd), df, NULL);
        g_object_unref(df);
    }
    g_signal_connect(nd, "response", G_CALLBACK(on_chooser_response), app);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(nd));
}

static gboolean on_drop(GtkDropTarget *t, const GValue *v, double x, double y, gpointer d)
{
    (void)t; (void)x; (void)y;
    app_t *app = d;
    if (!G_VALUE_HOLDS(v, GDK_TYPE_FILE_LIST)) return FALSE;
    GSList *l = gdk_file_list_get_files(g_value_get_boxed(v));
    int n = g_slist_length(l);
    GFile **arr = g_new0(GFile *, n + 1);
    int i = 0;
    for (GSList *it = l; it; it = it->next) arr[i++] = it->data;
    open_many(app, arr, n);
    g_free(arr);
    g_slist_free(l);
    return TRUE;
}

/* ── small dialogs ────────────────────────────────────────────────── */

/* GtkMessageDialog is on its way out and GtkAlertDialog is 4.10; a
 * plain window with a label and a row of buttons is all this needs. */
static gboolean dialog_key(GtkEventControllerKey *k, guint kv, guint code,
                           GdkModifierType m, gpointer d)
{
    (void)k; (void)code; (void)m;
    if (kv == GDK_KEY_Escape) { gtk_window_destroy(GTK_WINDOW(d)); return TRUE; }
    return FALSE;
}

GtkWidget *dialog_new(app_t *app, const char *title, const char *body)
{
    GtkWidget *w = gtk_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(w), GTK_WINDOW(app->win));
    gtk_window_set_modal(GTK_WINDOW(w), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(w), FALSE);
    gtk_window_set_title(GTK_WINDOW(w), title);
    gtk_window_set_default_size(GTK_WINDOW(w), 400, -1);
    gtk_widget_add_css_class(w, "lp-photos-dialog");
    /* An empty title bar: the title is already the first line of the
     * dialog, and a compositor-drawn bar would say it a second time. */
    GtkWidget *nobar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_visible(nobar, FALSE);
    gtk_window_set_titlebar(GTK_WINDOW(w), nobar);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 20);
    gtk_widget_set_margin_bottom(box, 18);

    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "lp-photos-h2");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_label_set_wrap(GTK_LABEL(t), TRUE);
    gtk_box_append(GTK_BOX(box), t);
    if (body) {
        GtkWidget *b = gtk_label_new(body);
        gtk_label_set_xalign(GTK_LABEL(b), 0);
        gtk_label_set_wrap(GTK_LABEL(b), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(b), 48);
        gtk_widget_add_css_class(b, "dim-label");
        gtk_box_append(GTK_BOX(box), b);
    }
    GtkWidget *extra = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(box), extra);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    gtk_widget_set_margin_top(row, 10);
    gtk_box_append(GTK_BOX(box), row);
    g_object_set_data(G_OBJECT(w), "row", row);
    g_object_set_data(G_OBJECT(w), "extra", extra);

    GtkEventController *k = gtk_event_controller_key_new();
    g_signal_connect(k, "key-pressed", G_CALLBACK(dialog_key), w);
    gtk_widget_add_controller(w, k);

    gtk_window_set_child(GTK_WINDOW(w), box);
    return w;
}

GtkWidget *dialog_add_button(GtkWidget *dlg, const char *label, const char *css,
                             GCallback cb, gpointer data)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    if (css) gtk_widget_add_css_class(b, css);
    if (cb) g_signal_connect(b, "clicked", cb, data);
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(gtk_window_destroy), dlg);
    gtk_box_append(GTK_BOX(g_object_get_data(G_OBJECT(dlg), "row")), b);
    return b;
}

/* ── delete ───────────────────────────────────────────────────────── */

static void do_trash(GtkButton *b, gpointer d)
{
    (void)b;
    app_t *app = d;
    if (!app->path) return;
    GFile *f = g_file_new_for_path(app->path);
    GError *e = NULL;
    if (!g_file_trash(f, NULL, &e)) {
        char *m = g_strdup_printf(T("Could not move it to the trash: %s",
                                    "휴지통으로 옮기지 못했습니다: %s"), e->message);
        toast(app, m);
        g_free(m);
        g_error_free(e);
        g_object_unref(f);
        return;
    }
    g_object_unref(f);
    toast(app, T("Moved to the trash", "휴지통으로 옮겼습니다"));
    int i = app->index;
    g_ptr_array_remove_index(app->files, i);
    undo_clear(app);
    if (app->files->len == 0) { show_empty(app); return; }
    if (i >= (int)app->files->len) i = app->files->len - 1;
    load_path(app, g_ptr_array_index(app->files, i));
}

static void ask_trash(app_t *app)
{
    if (!app->path || app->editing) return;
    char *base = g_path_get_basename(app->path);
    char *body = g_strdup_printf(T("“%s” will be moved to the trash. You can restore it from there.",
                                   "“%s” 을(를) 휴지통으로 옮깁니다. 휴지통에서 되살릴 수 있습니다."), base);
    GtkWidget *dlg = dialog_new(app, T("Move to the trash?", "휴지통으로 옮길까요?"), body);
    dialog_add_button(dlg, T("Cancel", "취소"), NULL, NULL, NULL);
    GtkWidget *ok = dialog_add_button(dlg, T("Move to Trash", "휴지통으로 옮기기"),
                                      "destructive-action", G_CALLBACK(do_trash), app);
    gtk_window_set_default_widget(GTK_WINDOW(dlg), ok);
    gtk_window_present(GTK_WINDOW(dlg));
    gtk_widget_grab_focus(ok);
    g_free(body); g_free(base);
}

/* ── fullscreen, slideshow, controls ──────────────────────────────── */

static gboolean hide_controls(gpointer d)
{
    app_t *app = d;
    app->hide_timer = 0;
    if (app->fullscreen) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(app->navbar), FALSE);
        gtk_widget_set_visible(app->fs_exit, FALSE);
        update_cursor(app);
    }
    return G_SOURCE_REMOVE;
}

static void controls_show(app_t *app)
{
    gboolean was = gtk_revealer_get_reveal_child(GTK_REVEALER(app->navbar));
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->navbar), !app->editing);
    gtk_widget_set_visible(app->fs_exit, app->fullscreen);
    if (app->hide_timer) g_source_remove(app->hide_timer);
    app->hide_timer = app->fullscreen ? g_timeout_add(HIDE_MS, hide_controls, app) : 0;
    if (!was) update_cursor(app);
}

static void slideshow_set(app_t *app, gboolean on);

static void on_fullscreened(GObject *o, GParamSpec *p, gpointer d)
{
    (void)p;
    app_t *app = d;
    app->fullscreen = gtk_window_is_fullscreen(GTK_WINDOW(o));
    gtk_widget_set_visible(app->header, !app->fullscreen);
    if (app->fullscreen) gtk_widget_add_css_class(app->win, "lp-photos-fs");
    else gtk_widget_remove_css_class(app->win, "lp-photos-fs");
    gtk_button_set_icon_name(GTK_BUTTON(app->fs_btn),
                             app->fullscreen ? "view-restore-symbolic" : "view-fullscreen-symbolic");
    if (!app->fullscreen && app->slideshow) slideshow_set(app, FALSE);
    controls_show(app);
    update_cursor(app);
}

static void fullscreen_set(app_t *app, gboolean on)
{
    if (on) gtk_window_fullscreen(GTK_WINDOW(app->win));
    else gtk_window_unfullscreen(GTK_WINDOW(app->win));
}

static gboolean slide_tick(gpointer d)
{
    app_t *app = d;
    if (!app->slideshow) { app->slide_timer = 0; return G_SOURCE_REMOVE; }
    go_index(app, app->index + 1);
    return G_SOURCE_CONTINUE;
}

static void slideshow_set(app_t *app, gboolean on)
{
    if (on && (app->editing || !app->files || app->files->len < 2)) return;
    app->slideshow = on;
    if (app->slide_timer) g_source_remove(app->slide_timer);
    app->slide_timer = on ? g_timeout_add(SLIDE_MS, slide_tick, app) : 0;
    gtk_button_set_icon_name(GTK_BUTTON(app->slide_btn),
                             on ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
    gtk_widget_set_tooltip_text(app->slide_btn, on ? T("Stop the slideshow (F5)", "슬라이드 쇼 멈추기 (F5)")
                                                   : T("Slideshow (F5)", "슬라이드 쇼 (F5)"));
    if (on && !app->fullscreen) fullscreen_set(app, TRUE);
    if (on) toast(app, T("Slideshow — Esc to stop", "슬라이드 쇼 — Esc 를 누르면 멈춥니다"));
}

static void info_toggle(app_t *app)
{
    gboolean on = !gtk_revealer_get_reveal_child(GTK_REVEALER(app->info_rev));
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->info_rev), on);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->info_btn), on);
}

void view_cursor(app_t *app) { update_cursor(app); }

/* The editor wants a still picture and the header bar (for Save). */
void viewer_prepare_edit(app_t *app)
{
    anim_stop(app);
    if (app->slideshow) slideshow_set(app, FALSE);
    if (app->fullscreen) fullscreen_set(app, FALSE);
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->navbar), FALSE);
}

static void view_xform(app_t *app, xform_t op)
{
    if (!app->img || app->adjusting) return;
    /* An animation cannot be turned frame by frame here; freeze it. */
    if (app->anim) anim_stop(app);
    edit_xform(app, op);
}

/* ── buttons ──────────────────────────────────────────────────────── */

static void b_open(GtkButton *b, gpointer d) { (void)b; open_dialog(d); }
static void b_prev(GtkButton *b, gpointer d) { (void)b; app_t *a = d; go_index(a, a->index - 1); }
static void b_next(GtkButton *b, gpointer d) { (void)b; app_t *a = d; go_index(a, a->index + 1); }
static void b_zin(GtkButton *b, gpointer d) { (void)b; zoom_step(d, 1); }
static void b_zout(GtkButton *b, gpointer d) { (void)b; zoom_step(d, -1); }
static void b_fit(GtkButton *b, gpointer d) { (void)b; view_fit(d); }
static void b_100(GtkButton *b, gpointer d) { (void)b; app_t *a = d; zoom_actual(a, cw(a) / 2.0, ch(a) / 2.0); }
static void b_rotl(GtkButton *b, gpointer d) { (void)b; view_xform(d, XF_ROT_L); }
static void b_rotr(GtkButton *b, gpointer d) { (void)b; view_xform(d, XF_ROT_R); }
static void b_fliph(GtkButton *b, gpointer d) { (void)b; view_xform(d, XF_FLIP_H); }
static void b_slide(GtkButton *b, gpointer d) { (void)b; app_t *a = d; slideshow_set(a, !a->slideshow); }
static void b_trash(GtkButton *b, gpointer d) { (void)b; ask_trash(d); }
static void b_fs(GtkButton *b, gpointer d) { (void)b; app_t *a = d; fullscreen_set(a, !a->fullscreen); }
static void b_copy(GtkButton *b, gpointer d) { (void)b; surface_to_clipboard(d); }
static void b_edit(GtkButton *b, gpointer d) { (void)b; edit_enter(d); }

static void b_info(GtkToggleButton *b, gpointer d)
{
    app_t *app = d;
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->info_rev), gtk_toggle_button_get_active(b));
}

/* ── keys ─────────────────────────────────────────────────────────── */

static gboolean on_key(GtkEventControllerKey *k, guint kv, guint code,
                       GdkModifierType mods, gpointer d)
{
    (void)k; (void)code;
    app_t *app = d;
    if (app->saving) return TRUE;         /* the window waits for the file */
    /* Typing into the text tool's entry or a spin button is typing, not
     * shortcuts - except Escape, which the entry does not use. */
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(app->win));
    if (focus && (GTK_IS_TEXT(focus) || GTK_IS_EDITABLE(focus)) && kv != GDK_KEY_Escape)
        return FALSE;

    mods &= gtk_accelerator_get_default_mod_mask();
    gboolean ctrl = (mods & GDK_CONTROL_MASK) != 0;
    gboolean shift = (mods & GDK_SHIFT_MASK) != 0;
    guint low = gdk_keyval_to_lower(kv);

    if (ctrl) {
        switch (low) {
        case GDK_KEY_o: open_dialog(app); return TRUE;
        case GDK_KEY_c: surface_to_clipboard(app); return TRUE;
        case GDK_KEY_w: case GDK_KEY_q: gtk_window_close(GTK_WINDOW(app->win)); return TRUE;
        case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add: zoom_step(app, 1); return TRUE;
        case GDK_KEY_minus: case GDK_KEY_KP_Subtract: zoom_step(app, -1); return TRUE;
        case GDK_KEY_0: case GDK_KEY_KP_0: view_fit(app); return TRUE;
        default: break;
        }
    }
    if (app->editing && edit_key(app, kv, mods)) return TRUE;
    if (ctrl) return FALSE;

    switch (kv) {
    case GDK_KEY_Left: case GDK_KEY_Page_Up: case GDK_KEY_BackSpace: case GDK_KEY_KP_Left:
        if (!app->editing) go_index(app, app->index - 1);
        return !app->editing;
    case GDK_KEY_Right: case GDK_KEY_Page_Down: case GDK_KEY_space: case GDK_KEY_KP_Right:
        if (!app->editing) go_index(app, app->index + 1);
        return !app->editing;
    case GDK_KEY_Home: if (!app->editing) go_index(app, 0); return TRUE;
    case GDK_KEY_End: if (!app->editing && app->files) go_index(app, app->files->len - 1); return TRUE;
    case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add: zoom_step(app, 1); return TRUE;
    case GDK_KEY_minus: case GDK_KEY_underscore: case GDK_KEY_KP_Subtract: zoom_step(app, -1); return TRUE;
    case GDK_KEY_0: case GDK_KEY_KP_0: case GDK_KEY_1: b_100(NULL, app); return TRUE;
    case GDK_KEY_F11: fullscreen_set(app, !app->fullscreen); return TRUE;
    case GDK_KEY_F5: slideshow_set(app, !app->slideshow); return TRUE;
    case GDK_KEY_Delete: case GDK_KEY_KP_Delete: ask_trash(app); return TRUE;
    case GDK_KEY_Escape:
        if (app->slideshow) slideshow_set(app, FALSE);
        if (app->fullscreen) fullscreen_set(app, FALSE);
        return TRUE;
    default: break;
    }
    switch (low) {
    case GDK_KEY_f: view_fit(app); return TRUE;
    case GDK_KEY_r: view_xform(app, shift ? XF_ROT_L : XF_ROT_R); return TRUE;
    case GDK_KEY_h: view_xform(app, XF_FLIP_H); return TRUE;
    case GDK_KEY_v: view_xform(app, XF_FLIP_V); return TRUE;
    case GDK_KEY_i: info_toggle(app); return TRUE;
    case GDK_KEY_e: if (!app->editing) edit_enter(app); return TRUE;
    default: break;
    }
    return FALSE;
}

static void cont_close(app_t *app, gpointer data)
{
    (void)data;
    app->editing = FALSE;
    undo_clear(app);
    gtk_window_destroy(GTK_WINDOW(app->win));
}

static gboolean on_close_request(GtkWindow *w, gpointer d)
{
    (void)w;
    app_t *app = d;
    if (app->saving) {
        toast(app, T("Saving…", "저장하는 중…"));
        return TRUE;
    }
    if (edit_dirty(app)) {
        ask_unsaved(app, cont_close, NULL, NULL);
        return TRUE;
    }
    return FALSE;
}

/* ── building the window ──────────────────────────────────────────── */

static GtkWidget *icon_button(const char *icon, const char *tip, GCallback cb, gpointer d)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_focus_on_click(b, FALSE);
    if (cb) g_signal_connect(b, "clicked", cb, d);
    return b;
}

/* LP's own colours: the navy of the window theme around the picture
 * (a neutral that does not tint the photo), and the one warm accent -
 * LP orange, #f28c28, as in theme/shell.css - for the thing to press. */
static const char *CSS =
    "window.lp-photos, window.lp-photos-dialog {"
    "  font-family: \"Pretendard Variable\", \"Pretendard\", \"Noto Sans CJK KR\", sans-serif; }\n"
    ".lp-photos-canvas { background-color: #0e161f; }\n"
    "window.lp-photos-fs .lp-photos-canvas { background-color: #000; }\n"
    ".lp-photos-empty { background-color: #0e161f; color: #eaf2f8; }\n"
    ".lp-photos-empty .dim-label { color: #9fb3c4; }\n"
    ".lp-photos-title { font-weight: bold; }\n"
    ".lp-photos-subtitle { font-size: 9pt; color: rgba(234,242,248,0.62); }\n"
    ".lp-photos-h1 { font-size: 17pt; font-weight: bold; }\n"
    ".lp-photos-h2 { font-size: 12.5pt; font-weight: bold; }\n"
    ".lp-photos-empty button { background-color: #f28c28; background-image: none; color: #1a1206;"
    "  font-weight: 600; padding: 8px 26px; border-radius: 999px; }\n"
    ".lp-photos-empty button:hover { background-color: #ffa24a; }\n"
    ".lp-photos-osd { background-color: rgba(14,18,24,0.88); color: #f1f5f9;"
    "  border-radius: 14px; padding: 4px; border: 1px solid rgba(255,255,255,0.08); }\n"
    ".lp-photos-osd button { color: #f1f5f9; background: none; border: none; box-shadow: none;"
    "  min-width: 34px; min-height: 34px; border-radius: 10px; padding: 0 6px; }\n"
    ".lp-photos-osd button:hover { background-color: rgba(255,255,255,0.12); }\n"
    ".lp-photos-osd button:checked { background-color: rgba(242,140,40,0.30); color: #ffb36b; }\n"
    ".lp-photos-osd button:disabled { color: rgba(241,245,249,0.35); }\n"
    ".lp-photos-osd label { color: #f1f5f9; }\n"
    ".lp-photos-osd separator { background-color: rgba(255,255,255,0.14); margin: 6px 4px; min-width: 1px; }\n"
    ".lp-photos-osd button.text-button { background-color: rgba(255,255,255,0.10); padding: 0 14px; }\n"
    ".lp-photos-osd button.text-button:checked { background-color: rgba(242,140,40,0.30); color: #ffb36b; }\n"
    ".lp-photos-osd button.suggested-action, headerbar button.suggested-action,"
    " .lp-photos-textbox button.suggested-action {"
    "  background-color: #f28c28; background-image: none; color: #1a1206; font-weight: 600; }\n"
    ".lp-photos-osd button.suggested-action:hover, headerbar button.suggested-action:hover,"
    " .lp-photos-textbox button.suggested-action:hover { background-color: #ffa24a; }\n"
    "headerbar button.suggested-action { padding: 2px 16px; }\n"
    ".lp-photos-textbox button { padding: 2px 14px; min-height: 30px; }\n"
    ".lp-photos-textbox entry { min-height: 30px; }\n"
    ".lp-photos-osd button.suggested-action:disabled { background-color: rgba(242,140,40,0.30); color: rgba(255,255,255,0.45); }\n"
    ".lp-photos-osd scale { min-height: 26px; }\n"
    ".lp-photos-osd scale trough { min-height: 4px; border-radius: 2px; background-color: rgba(255,255,255,0.22); }\n"
    ".lp-photos-osd scale highlight { min-height: 4px; border-radius: 2px; background-color: #f28c28; }\n"
    ".lp-photos-osd scale slider { min-width: 16px; min-height: 16px; margin: -6px; border-radius: 8px;"
    "  background-color: #f4f4f6; border: none; box-shadow: 0 1px 2px rgba(0,0,0,0.5); }\n"
    ".lp-photos-osd scale value { color: #f1f5f9; min-width: 34px; }\n"
    ".lp-photos-dialog button { padding: 6px 16px; background-color: rgba(127,127,127,0.18); }\n"
    ".lp-photos-dialog button.suggested-action { background-color: #f28c28; background-image: none;"
    "  color: #1a1206; font-weight: 600; }\n"
    ".lp-photos-dialog checkbutton check { min-width: 16px; min-height: 16px; border-radius: 4px;"
    "  border: 1px solid rgba(255,255,255,0.45); background-color: rgba(255,255,255,0.06); }\n"
    ".lp-photos-dialog checkbutton check:checked { background-color: #f28c28; border-color: #f28c28; color: #1a1206; }\n"
    ".lp-photos-dialog button.destructive-action { background-color: #c01c28; color: #fff; }\n"
    ".lp-photos-toast { background-color: rgba(14,18,24,0.92); color: #fff; border-radius: 10px;"
    "  padding: 8px 16px; margin-top: 14px; }\n"
    ".lp-photos-info { padding: 18px 18px; }\n"
    ".lp-photos-info .lp-photos-key { font-size: smaller; opacity: 0.65; margin-top: 10px; }\n"
    ".lp-photos-editbar { padding: 3px 8px; }\n"
    ".lp-photos-editbar button { min-width: 28px; min-height: 28px; padding: 2px; margin: 0; }\n"
    ".lp-photos-editbar button:checked { background-color: rgba(242,140,40,0.28); color: #ffb36b; }\n"
    ".lp-photos-editbar button.lp-photos-swatch { min-width: 24px; min-height: 24px; padding: 1px; }\n"
    ".lp-photos-editbar button.lp-photos-swatch:checked { background-color: rgba(242,140,40,0.55); }\n"
    ".lp-photos-editbar separator { margin: 5px 5px; }\n"
    ".lp-photos-textbox { background-color: rgba(14,18,24,0.92); border-radius: 10px; padding: 6px; }\n";

static GtkWidget *info_panel(app_t *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(box, "lp-photos-info");
    gtk_widget_set_size_request(box, 250, -1);
    GtkWidget *h = gtk_label_new(T("Details", "정보"));
    gtk_widget_add_css_class(h, "lp-photos-h2");
    gtk_label_set_xalign(GTK_LABEL(h), 0);
    gtk_box_append(GTK_BOX(box), h);
    const char *keys[6] = {
        T("Name", "이름"), T("Folder", "폴더"), T("Dimensions", "해상도"),
        T("File size", "파일 크기"), T("Modified", "수정한 날짜"), T("Format", "형식"),
    };
    for (int i = 0; i < 6; i++) {
        GtkWidget *k = gtk_label_new(keys[i]);
        gtk_widget_add_css_class(k, "lp-photos-key");
        gtk_label_set_xalign(GTK_LABEL(k), 0);
        gtk_box_append(GTK_BOX(box), k);
        GtkWidget *v = gtk_label_new("—");
        gtk_label_set_xalign(GTK_LABEL(v), 0);
        gtk_label_set_wrap(GTK_LABEL(v), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_selectable(GTK_LABEL(v), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(v), 28);
        gtk_box_append(GTK_BOX(box), v);
        app->info_val[i] = v;
    }
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), box);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(outer), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(outer), sw);
    gtk_widget_set_vexpand(sw, TRUE);

    app->info_rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(app->info_rev),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_child(GTK_REVEALER(app->info_rev), outer);
    return app->info_rev;
}

static GtkWidget *nav_bar(app_t *app)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(bar, "lp-photos-osd");

    app->prev_btn = icon_button("go-previous-symbolic", T("Previous (←)", "이전 사진 (←)"), G_CALLBACK(b_prev), app);
    gtk_box_append(GTK_BOX(bar), app->prev_btn);
    app->count_label = gtk_label_new("");
    gtk_widget_set_size_request(app->count_label, 64, -1);
    gtk_widget_add_css_class(app->count_label, "numeric");
    gtk_box_append(GTK_BOX(bar), app->count_label);
    app->next_btn = icon_button("go-next-symbolic", T("Next (→)", "다음 사진 (→)"), G_CALLBACK(b_next), app);
    gtk_box_append(GTK_BOX(bar), app->next_btn);
    gtk_box_append(GTK_BOX(bar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    gtk_box_append(GTK_BOX(bar), icon_button("zoom-out-symbolic", T("Zoom out (−)", "축소 (−)"), G_CALLBACK(b_zout), app));
    GtkWidget *zb = gtk_button_new();
    app->zoom_label = gtk_label_new("100%");
    gtk_widget_add_css_class(app->zoom_label, "numeric");
    gtk_widget_set_size_request(app->zoom_label, 50, -1);
    gtk_button_set_child(GTK_BUTTON(zb), app->zoom_label);
    gtk_widget_set_tooltip_text(zb, T("Actual size (0)", "실제 크기 (0)"));
    gtk_widget_set_focus_on_click(zb, FALSE);
    g_signal_connect(zb, "clicked", G_CALLBACK(b_100), app);
    gtk_box_append(GTK_BOX(bar), zb);
    gtk_box_append(GTK_BOX(bar), icon_button("zoom-in-symbolic", T("Zoom in (+)", "확대 (+)"), G_CALLBACK(b_zin), app));
    gtk_box_append(GTK_BOX(bar), icon_button("zoom-fit-best-symbolic", T("Fit to window (F)", "창에 맞추기 (F)"), G_CALLBACK(b_fit), app));
    gtk_box_append(GTK_BOX(bar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    gtk_box_append(GTK_BOX(bar), icon_button("object-rotate-left-symbolic", T("Rotate left (Shift+R)", "왼쪽으로 돌리기 (Shift+R)"), G_CALLBACK(b_rotl), app));
    gtk_box_append(GTK_BOX(bar), icon_button("object-rotate-right-symbolic", T("Rotate right (R)", "오른쪽으로 돌리기 (R)"), G_CALLBACK(b_rotr), app));
    gtk_box_append(GTK_BOX(bar), icon_button("object-flip-horizontal-symbolic", T("Flip horizontally (H)", "좌우 뒤집기 (H)"), G_CALLBACK(b_fliph), app));
    gtk_box_append(GTK_BOX(bar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    app->slide_btn = icon_button("media-playback-start-symbolic", T("Slideshow (F5)", "슬라이드 쇼 (F5)"), G_CALLBACK(b_slide), app);
    gtk_box_append(GTK_BOX(bar), app->slide_btn);
    gtk_box_append(GTK_BOX(bar), icon_button("user-trash-symbolic", T("Move to the trash (Delete)", "휴지통으로 옮기기 (Delete)"), G_CALLBACK(b_trash), app));

    GtkWidget *rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(rev), GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_revealer_set_child(GTK_REVEALER(rev), bar);
    gtk_revealer_set_reveal_child(GTK_REVEALER(rev), TRUE);
    gtk_widget_set_halign(rev, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(rev, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(rev, 18);
    return rev;
}

static GtkWidget *empty_page(app_t *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    GtkWidget *im = gtk_image_new_from_icon_name("image-x-generic-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(im), 96);
    gtk_widget_add_css_class(im, "dim-label");
    gtk_box_append(GTK_BOX(box), im);
    GtkWidget *t = gtk_label_new(T("No picture open", "열린 사진이 없습니다"));
    gtk_widget_add_css_class(t, "lp-photos-h1");
    gtk_box_append(GTK_BOX(box), t);
    GtkWidget *s = gtk_label_new(T("Open a picture, or drag one here.",
                                   "사진을 열거나 이곳으로 끌어다 놓으십시오."));
    gtk_widget_add_css_class(s, "dim-label");
    gtk_box_append(GTK_BOX(box), s);
    GtkWidget *b = gtk_button_new_with_label(T("Open…", "열기…"));
    gtk_widget_add_css_class(b, "suggested-action");
    gtk_widget_add_css_class(b, "pill");
    gtk_widget_set_halign(b, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(b, 12);
    g_signal_connect(b, "clicked", G_CALLBACK(b_open), app);
    gtk_box_append(GTK_BOX(box), b);

    GtkWidget *bg = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(bg, "lp-photos-empty");
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_set_vexpand(box, TRUE);
    gtk_box_append(GTK_BOX(bg), box);
    return bg;
}

static void build_window(app_t *app)
{
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, CSS, -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    app->win = gtk_application_window_new(app->gapp);
    lp_fit_default_size(GTK_WINDOW(app->win), 1100, 740);
    gtk_widget_add_css_class(app->win, "lp-photos");

    /* header */
    app->header = gtk_header_bar_new();
    GtkWidget *tb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(tb, GTK_ALIGN_CENTER);
    app->title = gtk_label_new(T("Photos", "사진"));
    gtk_widget_add_css_class(app->title, "lp-photos-title");
    gtk_label_set_ellipsize(GTK_LABEL(app->title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(app->title), 40);
    app->subtitle = gtk_label_new("");
    gtk_widget_add_css_class(app->subtitle, "lp-photos-subtitle");
    gtk_widget_add_css_class(app->subtitle, "numeric");
    gtk_box_append(GTK_BOX(tb), app->title);
    gtk_box_append(GTK_BOX(tb), app->subtitle);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(app->header), tb);

    app->hb_view_start = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *ob = gtk_button_new_with_label(T("Open", "열기"));
    gtk_widget_set_tooltip_text(ob, T("Open a picture (Ctrl+O)", "사진 열기 (Ctrl+O)"));
    g_signal_connect(ob, "clicked", G_CALLBACK(b_open), app);
    gtk_box_append(GTK_BOX(app->hb_view_start), ob);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(app->header), app->hb_view_start);

    app->hb_view_end = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->edit_btn = gtk_button_new();
    {
        GtkWidget *bx = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append(GTK_BOX(bx), gtk_image_new_from_icon_name("document-edit-symbolic"));
        gtk_box_append(GTK_BOX(bx), gtk_label_new(T("Edit", "편집")));
        gtk_button_set_child(GTK_BUTTON(app->edit_btn), bx);
    }
    gtk_widget_set_tooltip_text(app->edit_btn, T("Draw on the picture, crop, adjust (E)",
                                                 "그리기, 자르기, 보정 (E)"));
    g_signal_connect(app->edit_btn, "clicked", G_CALLBACK(b_edit), app);
    app->copy_btn = icon_button("edit-copy-symbolic", T("Copy the picture (Ctrl+C)", "사진 복사 (Ctrl+C)"), G_CALLBACK(b_copy), app);
    app->info_btn = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(app->info_btn), "dialog-information-symbolic");
    gtk_widget_set_tooltip_text(app->info_btn, T("Details (I)", "정보 (I)"));
    g_signal_connect(app->info_btn, "toggled", G_CALLBACK(b_info), app);
    app->fs_btn = icon_button("view-fullscreen-symbolic", T("Full screen (F11)", "전체 화면 (F11)"), G_CALLBACK(b_fs), app);
    gtk_box_append(GTK_BOX(app->hb_view_end), app->copy_btn);
    gtk_box_append(GTK_BOX(app->hb_view_end), app->info_btn);
    gtk_box_append(GTK_BOX(app->hb_view_end), app->fs_btn);
    gtk_box_append(GTK_BOX(app->hb_view_end), app->edit_btn);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(app->header), app->hb_view_end);
    gtk_window_set_titlebar(GTK_WINDOW(app->win), app->header);

    /* canvas and what floats over it */
    app->canvas = gtk_drawing_area_new();
    gtk_widget_add_css_class(app->canvas, "lp-photos-canvas");
    gtk_widget_set_hexpand(app->canvas, TRUE);
    gtk_widget_set_vexpand(app->canvas, TRUE);
    gtk_widget_set_focusable(app->canvas, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app->canvas), draw_fn, app, NULL);
    g_signal_connect(app->canvas, "resize", G_CALLBACK(on_resize), app);

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 0);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin), app);
    g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), app);
    g_signal_connect(drag, "drag-end", G_CALLBACK(drag_end), app);
    gtk_widget_add_controller(app->canvas, GTK_EVENT_CONTROLLER(drag));
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_click), app);
    gtk_widget_add_controller(app->canvas, GTK_EVENT_CONTROLLER(click));
    GtkGesture *pinch = gtk_gesture_zoom_new();
    g_signal_connect(pinch, "begin", G_CALLBACK(pinch_begin), app);
    g_signal_connect(pinch, "scale-changed", G_CALLBACK(pinch_scale), app);
    gtk_widget_add_controller(app->canvas, GTK_EVENT_CONTROLLER(pinch));
    GtkEventController *sc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(sc, "scroll", G_CALLBACK(on_scroll), app);
    gtk_widget_add_controller(app->canvas, sc);

    app->overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(app->overlay), app->canvas);
    GtkEventController *mo = gtk_event_controller_motion_new();
    g_signal_connect(mo, "motion", G_CALLBACK(on_motion), app);
    gtk_widget_add_controller(app->overlay, mo);

    app->navbar = nav_bar(app);
    gtk_overlay_add_overlay(GTK_OVERLAY(app->overlay), app->navbar);

    app->fs_exit = icon_button("view-restore-symbolic", T("Leave full screen (Esc)", "전체 화면 끝내기 (Esc)"), G_CALLBACK(b_fs), app);
    GtkWidget *fsbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(fsbox, "lp-photos-osd");
    gtk_box_append(GTK_BOX(fsbox), app->fs_exit);
    gtk_widget_set_halign(fsbox, GTK_ALIGN_END);
    gtk_widget_set_valign(fsbox, GTK_ALIGN_START);
    gtk_widget_set_margin_top(fsbox, 14);
    gtk_widget_set_margin_end(fsbox, 14);
    gtk_overlay_add_overlay(GTK_OVERLAY(app->overlay), fsbox);
    /* the box is what shows; the button is what we toggle - keep both */
    g_object_set_data(G_OBJECT(app->fs_exit), "box", fsbox);
    app->fs_exit = fsbox;
    gtk_widget_set_visible(app->fs_exit, FALSE);

    app->toast = gtk_label_new("");
    gtk_widget_add_css_class(app->toast, "lp-photos-toast");
    gtk_widget_set_halign(app->toast, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(app->toast, GTK_ALIGN_START);
    gtk_widget_set_can_target(app->toast, FALSE);
    gtk_widget_set_visible(app->toast, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(app->overlay), app->toast);

    app->spinner = gtk_spinner_new();
    gtk_widget_set_size_request(app->spinner, 32, 32);
    gtk_widget_set_halign(app->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(app->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_can_target(app->spinner, FALSE);
    gtk_widget_set_visible(app->spinner, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(app->overlay), app->spinner);

    edit_build_ui(app);   /* edit bar, crop/adjust bars, text box */

    GtkWidget *imgcol = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(imgcol), app->edit_bar);
    gtk_box_append(GTK_BOX(imgcol), app->overlay);

    app->stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(app->stack), empty_page(app), "empty");
    gtk_stack_add_named(GTK_STACK(app->stack), imgcol, "image");
    gtk_widget_set_hexpand(app->stack, TRUE);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(row), app->stack);
    gtk_box_append(GTK_BOX(row), info_panel(app));
    gtk_window_set_child(GTK_WINDOW(app->win), row);

    GtkEventController *key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), app);
    gtk_widget_add_controller(app->win, key);

    GtkDropTarget *dt = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(dt, "drop", G_CALLBACK(on_drop), app);
    gtk_widget_add_controller(app->win, GTK_EVENT_CONTROLLER(dt));

    g_signal_connect(app->win, "close-request", G_CALLBACK(on_close_request), app);
    g_signal_connect(app->win, "notify::fullscreened", G_CALLBACK(on_fullscreened), app);

    show_empty(app);
}

/* ── self test ────────────────────────────────────────────────────── */

/*
 * LP_PHOTOS_SELFTEST="cmd;cmd;..." drives the app from inside, so a
 * headless run can prove the editor maps coordinates and writes files
 * correctly. Drags go through the same handlers as the mouse, in widget
 * coordinates computed from image fractions, so zoom and pan are tested
 * too. Nobody sets this by accident; it does nothing otherwise.
 */
static void st_drag(app_t *app, double fx0, double fy0, double fx1, double fy1, gboolean wave)
{
    double x0 = app->ox + fx0 * app->iw * app->zoom, y0 = app->oy + fy0 * app->ih * app->zoom;
    double x1 = app->ox + fx1 * app->iw * app->zoom, y1 = app->oy + fy1 * app->ih * app->zoom;
    app->press_wx = x0; app->press_wy = y0;
    edit_press(app, x0, y0);
    for (int i = 1; i <= 24; i++) {
        double t = i / 24.0;
        double x = x0 + (x1 - x0) * t, y = y0 + (y1 - y0) * t;
        if (wave) y += sin(t * G_PI * 4) * 30;
        edit_motion(app, x, y);
    }
    edit_release(app, x1, y1 + (wave ? sin(G_PI * 4) * 30 : 0));
}

static GtkWidget *st_find_button(GtkWidget *w, const char *label)
{
    if (GTK_IS_BUTTON(w) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(w)), label) == 0) return w;
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *r = st_find_button(c, label);
        if (r) return r;
    }
    return NULL;
}

/* Click a button in whichever dialog is open, by its label. */
static void st_press(app_t *app, const char *label)
{
    GListModel *tl = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(tl); i++) {
        GtkWidget *w = g_list_model_get_item(tl, i);
        g_object_unref(w);
        if (w == app->win) continue;
        GtkWidget *b = st_find_button(w, label);
        if (b) { g_signal_emit_by_name(b, "clicked"); return; }
    }
    g_print("selftest: no button %s\n", label);
}

static const char *ST_TOOLS[N_TOOLS] = {
    "move", "pen", "highlight", "eraser", "line", "arrow",
    "rect", "ellipse", "text", "mosaic", "blur", "crop",
};

/* A key press as the window's key handler would see it:
 * "key:z,ctrl,shift" is Ctrl+Shift+Z. */
static void st_key(app_t *app, char *arg)
{
    char **p = g_strsplit(arg, ",", -1);
    GdkModifierType m = 0;
    for (int i = 1; p[i]; i++) {
        if (!strcmp(p[i], "ctrl")) m |= GDK_CONTROL_MASK;
        if (!strcmp(p[i], "shift")) m |= GDK_SHIFT_MASK;
    }
    guint kv = gdk_keyval_from_name(p[0]);
    if (m & GDK_SHIFT_MASK) kv = gdk_keyval_to_upper(kv);
    gtk_widget_grab_focus(app->canvas);
    on_key(NULL, kv, 0, m, app);
    g_strfreev(p);
}

/* The first two spin buttons in `w` (the resize dialog's width, height). */
static void st_spins(GtkWidget *w, GtkWidget **sb, int *n)
{
    if (GTK_IS_SPIN_BUTTON(w) && *n < 2) sb[(*n)++] = w;
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        st_spins(c, sb, n);
}

/* Type a width into the open resize dialog and report the height it
 * answered with. */
static void st_spin(app_t *app, double v)
{
    GListModel *tl = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(tl); i++) {
        GtkWidget *w = g_list_model_get_item(tl, i);
        g_object_unref(w);
        if (w == app->win) continue;
        GtkWidget *sb[2] = { NULL, NULL };
        int n = 0;
        st_spins(w, sb, &n);
        if (n < 2) continue;
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(sb[0]), v);
        g_print("selftest: resize dialog width=%.0f height=%.0f\n",
                gtk_spin_button_get_value(GTK_SPIN_BUTTON(sb[0])),
                gtk_spin_button_get_value(GTK_SPIN_BUTTON(sb[1])));
        return;
    }
}

/* Pick, in every drop-down under `w`, the item whose text has `needle`. */
static void st_pick(GtkWidget *w, const char *needle)
{
    if (GTK_IS_DROP_DOWN(w)) {
        GListModel *m = gtk_drop_down_get_model(GTK_DROP_DOWN(w));
        for (guint i = 0; m && i < g_list_model_get_n_items(m); i++) {
            GObject *o = g_list_model_get_item(m, i);
            if (GTK_IS_STRING_OBJECT(o) && strstr(gtk_string_object_get_string(GTK_STRING_OBJECT(o)), needle))
                gtk_drop_down_set_selected(GTK_DROP_DOWN(w), i);
            g_object_unref(o);
        }
    }
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        st_pick(c, needle);
}

/* Accept whatever file chooser is open, as if Save had been pressed:
 * "chooser:name,JPEG,(70)" also picks those items in its drop-downs,
 * the way a person would. An Open dialog is given a whole path to
 * select instead: "chooser:/dir/picture.jpg". */
static void st_chooser(app_t *app, char *arg)
{
    char **p = g_strsplit(arg, ",", -1);
    GListModel *tl = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(tl); i++) {
        GtkWidget *w = g_list_model_get_item(tl, i);
        g_object_unref(w);
        if (w == app->win || !GTK_IS_FILE_CHOOSER(w)) continue;
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        if (gtk_file_chooser_get_action(GTK_FILE_CHOOSER(w)) == GTK_FILE_CHOOSER_ACTION_OPEN) {
            GFile *f = g_file_new_for_path(p[0]);
            gtk_file_chooser_set_file(GTK_FILE_CHOOSER(w), f, NULL);
            g_object_unref(f);
            g_print("selftest: open dialog selects %s\n", p[0]);
            gtk_dialog_response(GTK_DIALOG(w), GTK_RESPONSE_ACCEPT);
            G_GNUC_END_IGNORE_DEPRECATIONS
            break;
        }
        for (int k = 1; p[k]; k++) st_pick(w, p[k]);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(w), p[0]);
        g_print("selftest: dialog choice now type=%s quality=%s\n",
                gtk_file_chooser_get_choice(GTK_FILE_CHOOSER(w), "type"),
                gtk_file_chooser_get_choice(GTK_FILE_CHOOSER(w), "quality"));
        gtk_dialog_response(GTK_DIALOG(w), GTK_RESPONSE_ACCEPT);
        G_GNUC_END_IGNORE_DEPRECATIONS
        break;
    }
    g_strfreev(p);
}

static gboolean st_step(gpointer d)
{
    app_t *app = d;
    if (!app->st_cmds[app->st_pos]) return G_SOURCE_REMOVE;
    if (app->loading || app->saving) { g_timeout_add(100, st_step, app); return G_SOURCE_REMOVE; }
    char *c = app->st_cmds[app->st_pos++];
    g_print("selftest %.2f: %s\n", g_get_monotonic_time() / 1e6, c);
    double a[4] = {0};
    char *arg = strchr(c, ':');
    if (arg) *arg++ = 0;
    if (arg) sscanf(arg, "%lf,%lf,%lf,%lf", &a[0], &a[1], &a[2], &a[3]);
    int delay = 250;

    if (!strcmp(c, "edit")) edit_enter(app);
    else if (!strcmp(c, "leave")) edit_leave(app);
    else if (!strcmp(c, "info")) info_toggle(app);
    else if (!strcmp(c, "fs")) fullscreen_set(app, !app->fullscreen);
    else if (!strcmp(c, "slideshow")) slideshow_set(app, !app->slideshow);
    else if (!strcmp(c, "next")) go_index(app, app->index + 1);
    else if (!strcmp(c, "prev")) go_index(app, app->index - 1);
    else if (!strcmp(c, "zoom")) zoom_at(app, a[0], cw(app) / 2.0, ch(app) / 2.0);
    else if (!strcmp(c, "fit")) view_fit(app);
    else if (!strcmp(c, "pan")) { app->ox += a[0]; app->oy += a[1]; app->fit = FALSE; view_clamp(app); view_queue(app); }
    else if (!strcmp(c, "tool")) {
        for (int i = 0; arg && i < N_TOOLS; i++)
            if (!strcmp(arg, ST_TOOLS[i])) edit_set_tool(app, i);
    }
    else if (!strcmp(c, "aspect")) edit_set_aspect(app, (int)a[0]);
    else if (!strcmp(c, "spin")) st_spin(app, a[0]);
    else if (!strcmp(c, "pinch")) pinch_fake(app, a[0]);
    else if (!strcmp(c, "key") && arg) st_key(app, arg);
    else if (!strcmp(c, "copy")) surface_to_clipboard(app);
    else if (!strcmp(c, "quality")) app->jpeg_quality = (int)a[0];
    else if (!strcmp(c, "saveasdlg")) { cont_drop(app); edit_save_as(app); }
    else if (!strcmp(c, "chooser") && arg) st_chooser(app, arg);
    else if (!strcmp(c, "open") && arg) open_path(app, arg);
    else if (!strcmp(c, "opendlg")) open_dialog(app);
    else if (!strcmp(c, "rgb")) {
        GdkRGBA rgba = { a[0], a[1], a[2], 1 };
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(app->color_btn), &rgba);
        G_GNUC_END_IGNORE_DEPRECATIONS
        g_signal_emit_by_name(app->color_btn, "color-set");
    }
    else if (!strcmp(c, "close")) gtk_window_close(GTK_WINDOW(app->win));
    else if (!strcmp(c, "wheel")) zoom_at(app, app->zoom * pow(ZOOM_STEP, -a[0]),
                                          app->ox + a[1] * app->iw * app->zoom,
                                          app->oy + a[2] * app->ih * app->zoom);
    else if (!strcmp(c, "color")) edit_set_color(app, (int)a[0]);
    else if (!strcmp(c, "width")) edit_set_width(app, (int)a[0]);
    else if (!strcmp(c, "fill")) edit_toggle_fill(app);
    else if (!strcmp(c, "drag")) st_drag(app, a[0], a[1], a[2], a[3], FALSE);
    else if (!strcmp(c, "wave")) st_drag(app, a[0], a[1], a[2], a[3], TRUE);
    else if (!strcmp(c, "click")) {
        double x = app->ox + a[0] * app->iw * app->zoom, y = app->oy + a[1] * app->ih * app->zoom;
        app->press_wx = x; app->press_wy = y;
        edit_press(app, x, y); edit_release(app, x, y);
    }
    else if (!strcmp(c, "type")) gtk_editable_set_text(GTK_EDITABLE(app->text_entry), arg ? arg : "");
    else if (!strcmp(c, "commit")) edit_text_commit(app, gtk_editable_get_text(GTK_EDITABLE(app->text_entry)));
    else if (!strcmp(c, "apply")) edit_crop_apply(app);
    else if (!strcmp(c, "rotl")) edit_xform(app, XF_ROT_L);
    else if (!strcmp(c, "rotr")) edit_xform(app, XF_ROT_R);
    else if (!strcmp(c, "fliph")) edit_xform(app, XF_FLIP_H);
    else if (!strcmp(c, "flipv")) edit_xform(app, XF_FLIP_V);
    else if (!strcmp(c, "adjust")) { edit_adjust_begin(app); edit_adjust_set(app, a[0], a[1], a[2]); }
    else if (!strcmp(c, "adjapply")) edit_adjust_apply(app);
    else if (!strcmp(c, "resize")) edit_resize(app, (int)a[0], (int)a[1]);
    else if (!strcmp(c, "resizedlg")) edit_resize_dialog(app);
    else if (!strcmp(c, "undo")) edit_undo(app);
    else if (!strcmp(c, "redo")) edit_redo(app);
    else if (!strcmp(c, "saveas")) {
        GError *e = NULL;
        if (edit_save_to(app, arg, &e)) { app->usaved = app->upos; after_save_as(app, arg); title_update(app); edit_update_buttons(app); }
        else { g_print("selftest: save failed: %s\n", e ? e->message : "?"); g_clear_error(&e); }
    }
    else if (!strcmp(c, "save")) edit_save(app);
    else if (!strcmp(c, "ask")) ask_unsaved(app, NULL, NULL, NULL);
    else if (!strcmp(c, "trash")) ask_trash(app);
    else if (!strcmp(c, "wait")) delay = (int)a[0];
    else if (!strcmp(c, "press") && arg) st_press(app, arg);
    else if (!strcmp(c, "shot") && arg) {
        const char *argv[] = { "grim", arg, NULL };
        g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    }
    else if (!strcmp(c, "print"))
        g_print("selftest: state img=%dx%d zoom=%.4f ox=%.1f oy=%.1f undo=%d/%d dirty=%d editing=%d index=%d/%u fs=%d ink=%d crop=%.0f,%.0f,%.0f,%.0f path=%s sub=\"%s\"\n",
                app->iw, app->ih, app->zoom, app->ox, app->oy, app->upos, app->un,
                edit_dirty(app), app->editing, app->index, app->files ? app->files->len : 0,
                app->fullscreen, app->ann != NULL, app->cx0, app->cy0, app->cx1, app->cy1, app->path,
                gtk_label_get_text(GTK_LABEL(app->subtitle)));
    else if (!strcmp(c, "quit")) { app->editing = FALSE; gtk_window_destroy(GTK_WINDOW(app->win)); return G_SOURCE_REMOVE; }
    else g_print("selftest: unknown command %s\n", c);

    g_timeout_add(delay, st_step, app);
    return G_SOURCE_REMOVE;
}

static void st_start(app_t *app)
{
    static gboolean started;
    if (started) return;
    started = TRUE;
    app->st_pos = 0;
    g_timeout_add(600, st_step, app);
}

/* ── application ──────────────────────────────────────────────────── */

static app_t *the_app;

static void ensure_window(GtkApplication *gapp)
{
    if (the_app) return;
    app_t *app = g_new0(app_t, 1);
    app->gapp = gapp;
    app->zoom = 1;
    app->fit = TRUE;
    app->index = -1;
    app->tool = TOOL_PEN;
    app->width_idx = 1;
    app->rgba[0] = 0.93; app->rgba[1] = 0.16; app->rgba[2] = 0.16; app->rgba[3] = 1;
    app->swatch_sel = 2;
    app->usaved = 0;
    const char *st = g_getenv("LP_PHOTOS_SELFTEST");
    if (st && *st) app->st_cmds = g_strsplit(st, ";", -1);
    the_app = app;
    /* Pictures look right on dark grey; ask for the dark variant of the
     * theme if it has one. */
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    build_window(app);
}

static void on_activate(GtkApplication *gapp, gpointer d)
{
    (void)d;
    ensure_window(gapp);
    gtk_window_present(GTK_WINDOW(the_app->win));
    if (the_app->st_cmds && !the_app->path) st_start(the_app);
}

static void on_open(GApplication *gapp, GFile **files, int n, const char *hint, gpointer d)
{
    (void)hint; (void)d;
    ensure_window(GTK_APPLICATION(gapp));
    gtk_window_present(GTK_WINDOW(the_app->win));
    open_many(the_app, files, n);
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("org.lpzero.Photos", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return r;
}
