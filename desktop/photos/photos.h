/*
 * photos.h - what the viewer (photos.c) and the editor (edit.c) share.
 *
 * There is one picture in memory at a time, held as a full-resolution
 * cairo image surface (app->img). The viewer shows it; the editor draws
 * straight into it. The file on disk is only touched by Save, so "works
 * on a copy" means: the copy is the surface, and throwing edits away is
 * reloading the file.
 *
 * Why one surface and not a viewer copy plus an editor copy: a 20 MP
 * photo is 80 MB as ARGB32 and the machine this is written for has
 * 512 MB. Two of them, plus undo, would not fit.
 *
 * The one exception is ink. Pen, highlighter, shapes and text go into a
 * second, transparent surface of the same size (app->ann) that is laid
 * over the picture when it is shown, copied or saved - so the eraser can
 * take ink away and leave the photo under it untouched. It is made on
 * the first stroke, so just looking at pictures never pays for it.
 */

#ifndef LP_PHOTOS_H
#define LP_PHOTOS_H

#include <gtk/gtk.h>
#include "lp-i18n.h"

typedef enum {
    TOOL_MOVE, TOOL_PEN, TOOL_HIGHLIGHT, TOOL_ERASER, TOOL_LINE, TOOL_ARROW,
    TOOL_RECT, TOOL_ELLIPSE, TOOL_TEXT, TOOL_MOSAIC, TOOL_BLUR, TOOL_CROP,
    N_TOOLS
} tool_t;

/* Crop box edges being dragged; a corner is two of them. */
enum { CE_L = 1, CE_R = 2, CE_T = 4, CE_B = 8, CE_MOVE = 16 };

#define N_ASPECT 5

/* Geometric edits that are their own inverse's partner. They cost no
 * memory in the undo stack: undoing a left turn is a right turn. */
typedef enum { XF_ROT_L, XF_ROT_R, XF_FLIP_H, XF_FLIP_V } xform_t;

typedef enum { U_REGION, U_FULL, U_XFORM } ukind_t;

typedef struct {
    ukind_t          kind;
    int              x, y;        /* U_REGION: where surf goes back   */
    cairo_surface_t *surf;        /* U_REGION: old pixels; U_FULL: old image */
    xform_t          op;          /* U_XFORM */
    gboolean         ink;         /* U_REGION: surf belongs to app->ann;
                                   * U_FULL: ann below is swapped too   */
    cairo_surface_t *ann;         /* U_FULL with ink: old ink layer (may be NULL) */
} undo_t;

#define N_SWATCH 8
#define UNDO_MIN_STEPS 20

typedef struct app app_t;
typedef void (*cont_fn)(app_t *app, gpointer data);

struct app {
    GtkApplication *gapp;
    GtkWidget *win, *header, *title, *subtitle;
    GtkWidget *stack;            /* "empty" | "image" */
    GtkWidget *overlay, *canvas;
    GtkWidget *navbar;           /* floating viewer controls */
    GtkWidget *count_label, *zoom_label, *prev_btn, *next_btn;
    GtkWidget *slide_btn, *fs_btn, *info_btn, *edit_btn, *copy_btn;
    GtkWidget *fs_exit;          /* top-right button, fullscreen only */
    GtkWidget *toast;
    guint      toast_timer;
    GtkWidget *spinner;

    /* header bar halves, swapped when the editor opens */
    GtkWidget *hb_view_start, *hb_view_end, *hb_edit_start, *hb_edit_end;
    GtkWidget *undo_btn, *redo_btn, *save_btn;

    /* info panel */
    GtkWidget *info_rev;
    GtkWidget *info_val[6];

    /* editor */
    GtkWidget *edit_bar;
    GtkWidget *tool_btn[N_TOOLS];
    GtkWidget *swatch[N_SWATCH];
    GtkWidget *color_btn;
    GtkWidget *width_btn[3];
    GtkWidget *fill_btn;
    GtkWidget *crop_bar, *crop_apply, *aspect_btn[N_ASPECT];
    GtkWidget *adj_bar, *adj_scale_w[3];
    GtkWidget *text_box, *text_entry;

    /* files */
    GPtrArray *files;            /* char* absolute paths, sorted */
    int        index;
    char      *path;             /* shown now (NULL = empty state) */
    char      *dir;

    /* the picture */
    cairo_surface_t *img;
    cairo_surface_t *ann;        /* ink layer over img, NULL until drawn on */
    int        iw, ih;
    gint64     file_size;
    gboolean   has_alpha;
    char      *fmt_name, *fmt_desc;
    GdkPixbufAnimation     *anim;
    GdkPixbufAnimationIter *iter;
    guint      anim_timer;
    guint      load_gen;
    gboolean   loading;
    gboolean   saving;           /* a Save is being written in a thread */

    /* view: widget point = (ox + x*zoom, oy + y*zoom) for image point x,y */
    double     zoom, ox, oy;
    gboolean   fit;
    cairo_surface_t *cache;      /* img scaled to cache_zoom, only when < 1 */
    double     cache_zoom;
    guint      cache_timer;

    /* pointer */
    gboolean   dragging, panning;
    double     press_wx, press_wy, pan_ox0, pan_oy0;

    gboolean   fullscreen, slideshow;
    guint      hide_timer, slide_timer;

    /* editor state */
    gboolean   editing;
    tool_t     tool;
    double     rgba[4];
    int        swatch_sel;       /* -1 = custom colour */
    int        width_idx;
    gboolean   fill;
    GArray    *pts;              /* doubles x,y,x,y in image coords */
    gboolean   stroking;
    double     ax, ay, bx, by;   /* shape / region anchor and end, image coords */
    double     stroke_zoom;      /* zoom when the stroke began: fixes widths */
    gboolean   crop_has;
    double     cx0, cy0, cx1, cy1;   /* kept with cx0 < cx1, cy0 < cy1 */
    int        crop_grab;            /* CE_* being dragged */
    int        crop_aspect;          /* index into the presets, 0 = free */
    gboolean   text_active;
    double     tx, ty, text_zoom;

    undo_t    *u;
    int        un, ucap, upos, usaved;
    gsize      ubytes;

    gboolean   adjusting;
    cairo_surface_t *adj_base, *adj_prev;
    double     adj_scale;
    guint      adj_idle;

    int        jpeg_quality;     /* last one chosen in Save As */
    double     pinch_zoom0;

    /* pending "save / discard / cancel" continuation */
    cont_fn    cont;
    gpointer   cont_data;
    GDestroyNotify cont_free;

    /* self test */
    char     **st_cmds;
    int        st_pos;
};

/* photos.c */
void  view_queue(app_t *app);
void  view_image_changed(app_t *app);          /* size or pixels all changed */
void  view_region_changed(app_t *app, double x, double y, double w, double h);
void  view_fit(app_t *app);
void  view_cursor(app_t *app);
void  viewer_prepare_edit(app_t *app);
void  view_w2i(app_t *app, double wx, double wy, double *ix, double *iy);
void  toast(app_t *app, const char *msg);
void  title_update(app_t *app);
void  reload_current(app_t *app);
void  after_save_as(app_t *app, const char *path);
GtkWidget *dialog_new(app_t *app, const char *title, const char *body);
GtkWidget *dialog_add_button(GtkWidget *dlg, const char *label, const char *css,
                             GCallback cb, gpointer data);
void  surface_to_clipboard(app_t *app);
cairo_surface_t *pixbuf_to_surface(GdkPixbuf *pb, gboolean *has_alpha);
GdkPixbuf *surface_to_pixbuf(cairo_surface_t *s, cairo_surface_t *ink, gboolean keep_alpha);
void  paint_ink(app_t *app, cairo_t *cr, cairo_filter_t f);

/* edit.c */
void  edit_build_ui(app_t *app);
void  edit_enter(app_t *app);
void  edit_leave(app_t *app);                  /* asks if unsaved */
void  edit_leave_now(app_t *app);
gboolean edit_dirty(app_t *app);
void  edit_draw_overlay(app_t *app, cairo_t *cr);
void  edit_press(app_t *app, double wx, double wy);
void  edit_motion(app_t *app, double wx, double wy);
void  edit_release(app_t *app, double wx, double wy);
gboolean edit_key(app_t *app, guint key, GdkModifierType mods);
void  edit_set_tool(app_t *app, tool_t t);
void  edit_set_aspect(app_t *app, int idx);
void  ink_drop(app_t *app);
void  edit_set_color(app_t *app, int swatch);
void  edit_set_width(app_t *app, int idx);
void  edit_toggle_fill(app_t *app);
void  edit_xform(app_t *app, xform_t op);      /* also used by the viewer */
void  edit_undo(app_t *app);
void  edit_redo(app_t *app);
void  undo_clear(app_t *app);
void  edit_update_buttons(app_t *app);
void  edit_crop_apply(app_t *app);
void  edit_crop_cancel(app_t *app);
void  edit_text_commit(app_t *app, const char *text);
void  edit_adjust_begin(app_t *app);
void  edit_adjust_set(app_t *app, double b, double c, double s);
void  edit_adjust_apply(app_t *app);
void  edit_adjust_cancel(app_t *app);
void  edit_resize(app_t *app, int w, int h);
void  edit_resize_dialog(app_t *app);
void  edit_save(app_t *app);                   /* runs app->cont on success */
void  edit_save_as(app_t *app);
gboolean edit_save_to(app_t *app, const char *path, GError **err);
void  ask_unsaved(app_t *app, cont_fn cont, gpointer data, GDestroyNotify fr);
void  cont_run(app_t *app);
void  cont_drop(app_t *app);
void  cont_leave_editor(app_t *app, gpointer data);

#endif /* LP_PHOTOS_H */
