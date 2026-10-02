/* lp-kit.c - the pieces lp-files, lp-tasks and lp-software share.
 *
 * lp-kit.h says what each piece is for. This file is about how, and
 * about the few places where the obvious way was the wrong one:
 *
 *  - The dialog window must not carry GTK's "background" class. The
 *    theme (~/.config/gtk-4.0/gtk.css) paints every window.background
 *    #232323 at USER priority, above anything an application can say,
 *    so a dialog that only asks for a transparent background still
 *    arrives as a grey rectangle with a card fading in inside it. With
 *    the class gone the theme's rule does not match, and what arrives
 *    is the card and its shadow.
 *
 *  - The sheet's spring starts when the sheet is MAPPED, not when the
 *    dialog is presented. Mapping a new window takes a frame or three on
 *    this machine; a spring started at present() has spent that time
 *    already, and the first frame anybody sees is a third of the way in.
 *
 *  - The password never lives longer than it has to. It is read from
 *    the entry, hex-encoded into one buffer, the entry is emptied, the
 *    buffer is sent and then overwritten. GTK keeps its own copy inside
 *    the entry's buffer until it is emptied; that is as far as a GTK
 *    application can go.
 *
 *  - Settings files are replaced, never rewritten. g_file_set_contents_full
 *    with CONSISTENT|DURABLE writes a temporary file, fsyncs it and
 *    renames it over the old one; the directory is then fsynced here as
 *    well, because the rename itself is only durable once the directory
 *    entry is - a power cut between the two otherwise loses the new file
 *    AND, on some filesystems, the old one.
 */
#define _GNU_SOURCE 1
#include "lp-kit.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gunixsocketaddress.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* motion.css, compiled in. Every application directory sits next to
 * desktop/theme, so the path is the same from each of them; the
 * Makefiles run the compiler in their own directory. */
#ifndef LP_MOTION_CSS
#define LP_MOTION_CSS "../theme/motion.css"
#endif
__asm__(".pushsection .rodata\n"
        ".global lp_kit_motion_css\n"
        ".type lp_kit_motion_css, @object\n"
        "lp_kit_motion_css:\n"
        ".incbin \"" LP_MOTION_CSS "\"\n"
        ".byte 0\n"
        ".popsection\n");
extern const char lp_kit_motion_css[];

/* ═══════════════════════════════════════════════════════════════════
 * Looks
 * ═══════════════════════════════════════════════════════════════════ */

/* The mockup's tokens (design/reference/task-manager-settings-mockup):
 * card #232323, raised #2c2c2c, line #333, text #e8e8e8 / #a8a8a8 /
 * #6a6a6a, amber #f0b350, red #f08585. The accent follows the theme's
 * @accent_bg_color so a changed system accent reaches these too. */
static const char KIT_CSS[] =
    /* Window buttons: the one look every LP app shares (see the theme's
     * gtk.css) - this file's own button rules would otherwise make
     * close a wide padded block. */
    "headerbar windowcontrols > button { min-width: 28px; min-height: 28px; margin: 0 2px;"
    " padding: 0; border: none; border-radius: 8px; box-shadow: none;"
    " background: transparent; color: #9fb3c4; }"
    "headerbar windowcontrols > button:hover { background: alpha(#eaf2f8, 0.12); color: #eaf2f8; }"
    "headerbar windowcontrols > button.close:hover { background: #f28c28; color: #ffffff; }"
    "headerbar windowcontrols.end:not(.empty) { padding-left: 0; background-image: none; }"
    "window.lp-sheet-window { background: none; box-shadow: none; }\n"
    ".lp-sheet-card { background-color: #2c2c2c; border-radius: 14px;"
    "  padding: 26px 28px 20px 28px; margin: 36px;"
    "  box-shadow: 0 0 0 0.5px #3a3a3a, 0 2px 4px rgba(0,0,0,0.3),"
    "              0 14px 40px rgba(0,0,0,0.55); }\n"
    ".lp-sheet-title { font-size: 19px; font-weight: 600; color: #e8e8e8; }\n"
    ".lp-sheet-text { font-size: 14px; color: #a8a8a8; }\n"
    ".lp-sheet-error { font-size: 13px; color: #f08585; }\n"
    ".lp-sheet-buttons button { min-height: 44px; min-width: 112px;"
    "  border-radius: 8px; padding: 0 18px; }\n"
    ".lp-sheet-card passwordentry, .lp-sheet-card entry {"
    "  min-height: 44px; border-radius: 8px; }\n"
    /* The sidebar of all three applications. Not .navigation-sidebar:
     * the theme sizes that for the mockup's desktop density (11px text,
     * rows with no minimum height) at USER priority, where nothing an
     * application says can win, and a finger needs 44px. */
    ".lp-kit-sidebar { background-color: #1e1e1e; border-right: 0.5px solid #333333; }\n"
    "list.lp-kit-side { background: none; padding: 10px 8px; }\n"
    "list.lp-kit-side > row { min-height: 44px; border-radius: 8px; margin: 1px 0;"
    "  padding: 0 10px; color: #a8a8a8; }\n"
    "list.lp-kit-side > row:selected { background-color: #2c2c2c; color: #e8e8e8; }\n"
    "list.lp-kit-side > row image { color: #8a8a8a; -gtk-icon-size: 16px; }\n"
    "list.lp-kit-side > row:selected image { color: #e8e8e8; }\n"
    "list.lp-kit-side > row label { font-size: 14px; }\n"
    "list.lp-kit-side .lp-kit-group { font-size: 12px; color: #6a6a6a; }\n"
    ".lp-kit-badge { background-color: #3584e4; color: #ffffff; border-radius: 999px;"
    "  padding: 1px 8px; font-size: 12px; font-weight: 600; }\n"
    ".lp-kit-btn { min-height: 44px; min-width: 44px; border-radius: 8px;"
    "  padding: 0 12px; }\n"
    ".lp-kit-btn.icon-only { padding: 0; min-width: 48px; }\n"
    "progressbar.lp-kit-progress trough { min-height: 6px; border-radius: 3px;"
    "  background-color: rgba(255,255,255,0.08); }\n"
    "progressbar.lp-kit-progress progress { min-height: 6px; border-radius: 3px;"
    "  background-color: @accent_bg_color; }\n";

void lp_kit_style(const char *css)
{
    GdkDisplay *dpy = gdk_display_get_default();
    const char *parts[3] = { lp_kit_motion_css, KIT_CSS, css };
    for (int i = 0; i < 3; i++) {
        if (!parts[i] || !*parts[i])
            continue;
        GtkCssProvider *p = gtk_css_provider_new();
        gtk_css_provider_load_from_data(p, parts[i], -1);
        gtk_style_context_add_provider_for_display(dpy, GTK_STYLE_PROVIDER(p),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(p);
    }
}

GtkWidget *lp_kit_scroller(GtkWidget *child, gboolean horizontal)
{
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
        horizontal ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC);
    /* Both are GTK's defaults today. They are set anyway: the brief is
     * "never disabled", and a default is a thing somebody changes. */
    gtk_scrolled_window_set_kinetic_scrolling(GTK_SCROLLED_WINDOW(sw), TRUE);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(sw), TRUE);
    if (child)
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), child);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_widget_set_hexpand(sw, TRUE);
    return sw;
}

GtkWidget *lp_kit_stack(void)
{
    GtkWidget *s = gtk_stack_new();
    g_object_set_data(G_OBJECT(s), "lp-kit-kind", GINT_TO_POINTER(1));
    gtk_stack_set_transition_type(GTK_STACK(s),
        GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration(GTK_STACK(s), 260);
    gtk_stack_set_hhomogeneous(GTK_STACK(s), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(s), FALSE);
    return s;
}

GtkWidget *lp_kit_fade_stack(void)
{
    GtkWidget *s = gtk_stack_new();
    g_object_set_data(G_OBJECT(s), "lp-kit-kind", GINT_TO_POINTER(2));
    gtk_stack_set_transition_type(GTK_STACK(s),
        GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(s), 200);
    return s;
}

/* SLIDE_LEFT_RIGHT already picks the direction from the pages' order,
 * which is the order of the sidebar, so "further down the list" comes
 * in from the right. Reduced motion is looked at on every switch - the
 * person may have changed it while the window was open. */
void lp_kit_stack_show(GtkStack *stack, const char *name)
{
    int kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(stack), "lp-kit-kind"));
    if (lp_motion_reduced()) {
        gtk_stack_set_transition_type(stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
        gtk_stack_set_transition_duration(stack, 90);
    } else if (kind == 1) {
        gtk_stack_set_transition_type(stack, GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
        gtk_stack_set_transition_duration(stack, 260);
    } else {
        gtk_stack_set_transition_type(stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
        gtk_stack_set_transition_duration(stack, 200);
    }
    gtk_stack_set_visible_child_name(stack, name);
}

/* ── rows that arrive and leave ───────────────────────────────────── */

static gboolean reveal_next_frame(GtkWidget *w, GdkFrameClock *c, gpointer d)
{
    (void)c; (void)d;
    gtk_revealer_set_reveal_child(GTK_REVEALER(w), TRUE);
    return G_SOURCE_REMOVE;
}

GtkWidget *lp_kit_reveal_in(GtkWidget *child, gboolean instant)
{
    GtkWidget *r = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(r),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(r),
        lp_spring_ms(LP_SPRING_INSERT, FALSE));
    gtk_revealer_set_child(GTK_REVEALER(r), child);
    if (instant) {
        gtk_revealer_set_transition_duration(GTK_REVEALER(r), 0);
        gtk_revealer_set_reveal_child(GTK_REVEALER(r), TRUE);
        gtk_revealer_set_transition_duration(GTK_REVEALER(r),
            lp_spring_ms(LP_SPRING_INSERT, FALSE));
    } else {
        /* A revealer told to open before it has ever been drawn opens
         * with no animation at all, so it is told on its first frame. */
        gtk_widget_add_tick_callback(r, reveal_next_frame, NULL, NULL);
    }
    return r;
}

typedef struct {
    GCallback gone;
    gpointer  data;
    gulong    handler;
} RevealOut;

static void reveal_out_done(GtkWidget *r, RevealOut *o)
{
    if (o->handler)
        g_signal_handler_disconnect(r, o->handler);
    if (o->gone)
        ((void (*)(GtkWidget *, gpointer))o->gone)(r, o->data);
    g_free(o);
}

static void on_child_revealed(GObject *r, GParamSpec *p, gpointer d)
{
    (void)p;
    if (!gtk_revealer_get_child_revealed(GTK_REVEALER(r)))
        reveal_out_done(GTK_WIDGET(r), d);
}

void lp_kit_reveal_out(GtkWidget *rev, GCallback gone, gpointer data)
{
    RevealOut *o = g_new0(RevealOut, 1);
    o->gone = gone;
    o->data = data;
    if (!gtk_widget_get_mapped(rev) ||
        !gtk_revealer_get_child_revealed(GTK_REVEALER(rev))) {
        reveal_out_done(rev, o);
        return;
    }
    gtk_revealer_set_transition_duration(GTK_REVEALER(rev),
        lp_spring_ms(LP_SPRING_INSERT, TRUE));
    o->handler = g_signal_connect(rev, "notify::child-revealed",
                                  G_CALLBACK(on_child_revealed), o);
    gtk_revealer_set_reveal_child(GTK_REVEALER(rev), FALSE);
}

/* ── long-press and right-click ───────────────────────────────────── */

typedef struct {
    LpKitContextFn cb;
    gpointer       data;
} Ctx;

static void ctx_long(GtkGestureLongPress *g, double x, double y, gpointer d)
{
    Ctx *c = d;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    c->cb(w, x, y, c->data);
}

static void ctx_click(GtkGestureClick *g, int n, double x, double y, gpointer d)
{
    (void)n;
    Ctx *c = d;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    c->cb(w, x, y, c->data);
}

void lp_kit_context(GtkWidget *w, LpKitContextFn cb, gpointer data)
{
    Ctx *c = g_new0(Ctx, 1);
    c->cb = cb;
    c->data = data;
    g_object_set_data_full(G_OBJECT(w), "lp-kit-ctx", c, g_free);

    /* Touch only: a mouse holding the button down is starting a drag
     * or a rubber band, not asking for a menu. */
    GtkGesture *lp = gtk_gesture_long_press_new();
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(lp), TRUE);
    /* Capture: a list's rows take a touch for their own click and
     * drag first, and in the bubble phase the hold never got to see it. */
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(lp),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(lp, "pressed", G_CALLBACK(ctx_long), c);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(lp));

    GtkGesture *rc = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), GDK_BUTTON_SECONDARY);
    g_signal_connect(rc, "pressed", G_CALLBACK(ctx_click), c);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(rc));
}

GtkWidget *lp_kit_button(const char *icon, const char *label, const char *css)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    if (icon)
        gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(icon));
    if (label)
        gtk_box_append(GTK_BOX(box), gtk_label_new(label));
    gtk_button_set_child(GTK_BUTTON(b), box);
    gtk_widget_add_css_class(b, "lp-kit-btn");
    if (!label) {
        gtk_widget_add_css_class(b, "icon-only");
        gtk_widget_add_css_class(b, "flat");
    }
    if (css)
        gtk_widget_add_css_class(b, css);
    return b;
}

/* ═══════════════════════════════════════════════════════════════════
 * The eased progress bar
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    LpSpring  s;
    LpMotion *m;
    guint     pulse;
} Prog;

static void prog_frame(GtkWidget *w, gpointer d)
{
    Prog *p = d;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w), CLAMP(p->s.x, 0.0, 1.0));
}

static gboolean prog_pulse(gpointer d)
{
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(d));
    return G_SOURCE_CONTINUE;
}

static void prog_free(gpointer d)
{
    Prog *p = d;
    if (p->pulse)
        g_source_remove(p->pulse);
    lp_motion_free(p->m);
    g_free(p);
}

GtkWidget *lp_kit_progress_new(void)
{
    GtkWidget *bar = gtk_progress_bar_new();
    gtk_widget_add_css_class(bar, "lp-kit-progress");
    Prog *p = g_new0(Prog, 1);
    /* EXPAND: critically damped. A progress bar that overshoots says
     * "more done than is". */
    lp_spring_init(&p->s, LP_SPRING_EXPAND, 0.0);
    p->m = lp_motion_new(bar, prog_frame, p);
    lp_motion_add(p->m, &p->s);
    gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(bar), 0.08);
    g_object_set_data_full(G_OBJECT(bar), "lp-kit-prog", p, prog_free);
    return bar;
}

void lp_kit_progress_set(GtkWidget *bar, double f)
{
    Prog *p = g_object_get_data(G_OBJECT(bar), "lp-kit-prog");
    if (!p)
        return;
    if (f < 0.0) {
        if (!p->pulse)
            p->pulse = g_timeout_add(120, prog_pulse, bar);
        return;
    }
    if (p->pulse) {
        g_source_remove(p->pulse);
        p->pulse = 0;
        lp_spring_jump(&p->s, 0.0);
    }
    f = CLAMP(f, 0.0, 1.0);
    if (f < p->s.target - 1e-6 && f < 0.01) {
        /* A new job starting over from zero: no glide backwards. */
        lp_spring_jump(&p->s, f);
        prog_frame(bar, p);
        return;
    }
    if (!gtk_widget_get_mapped(bar)) {
        lp_spring_jump(&p->s, f);
        prog_frame(bar, p);
        return;
    }
    lp_spring_set_target(&p->s, f);
    lp_motion_kick(p->m);
}

/* ═══════════════════════════════════════════════════════════════════
 * Settings on disk
 * ═══════════════════════════════════════════════════════════════════ */

static char *state_path(const char *app)
{
    char *name = g_strconcat(app, ".ini", NULL);
    char *p = g_build_filename(g_get_user_config_dir(), "lp", name, NULL);
    g_free(name);
    return p;
}

GKeyFile *lp_kit_state_load(const char *app)
{
    GKeyFile *kf = g_key_file_new();
    char *p = state_path(app);
    /* A missing or damaged file is an empty one: defaults, and the next
     * save replaces it whole. */
    g_key_file_load_from_file(kf, p, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_free(p);
    return kf;
}

gboolean lp_kit_state_save(GKeyFile *kf, const char *app)
{
    char *p = state_path(app);
    char *dir = g_path_get_dirname(p);
    gsize len = 0;
    char *data = g_key_file_to_data(kf, &len, NULL);
    GError *err = NULL;
    gboolean ok = g_mkdir_with_parents(dir, 0700) == 0 &&
        g_file_set_contents_full(p, data, (gssize)len,
            G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
            0600, &err);
    if (ok) {
        int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) {
            fsync(fd);
            close(fd);
        }
    } else if (err) {
        g_warning("lp-kit: could not save %s: %s", p, err->message);
        g_error_free(err);
    }
    g_free(data);
    g_free(dir);
    g_free(p);
    return ok;
}

/* ═══════════════════════════════════════════════════════════════════
 * Sheets
 * ═══════════════════════════════════════════════════════════════════ */

/* The widget that moves: one child, drawn through an opacity and a
 * scale about its centre that follow a spring from 0 (gone) to 1. Only
 * the snapshot changes per frame - the size was measured once - which
 * is COMMON.md's rule for animating on this GPU. */
typedef struct {
    GtkWidget  parent;
    LpSpring   s;
    LpMotion  *m;
    gboolean   closing;
} LpKitSheet;
typedef struct { GtkWidgetClass parent_class; } LpKitSheetClass;
G_DEFINE_TYPE(LpKitSheet, lp_kit_sheet, GTK_TYPE_WIDGET)

static void sheet_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
    LpKitSheet *sh = (LpKitSheet *)w;
    GtkWidget *c = gtk_widget_get_first_child(w);
    double p = sh->s.x;
    if (!c || p <= 0.001)
        return;
    gtk_snapshot_push_opacity(snap, CLAMP(p, 0.0, 1.0));
    gtk_snapshot_save(snap);
    if (!lp_motion_reduced() && fabs(p - 1.0) > 1e-4) {
        /* Appearing things start at 0.96 (feel.md): a card that grows
         * from nothing reads as a zoom, not an arrival. */
        float sc = (float)(0.96 + 0.04 * p);
        float cx = gtk_widget_get_width(w) / 2.0f;
        float cy = gtk_widget_get_height(w) / 2.0f;
        gtk_snapshot_translate(snap, &GRAPHENE_POINT_INIT(cx, cy));
        gtk_snapshot_scale(snap, sc, sc);
        gtk_snapshot_translate(snap, &GRAPHENE_POINT_INIT(-cx, -cy));
    }
    gtk_widget_snapshot_child(w, c, snap);
    gtk_snapshot_restore(snap);
    gtk_snapshot_pop(snap);
}

static void sheet_dispose(GObject *o)
{
    LpKitSheet *sh = (LpKitSheet *)o;
    g_clear_pointer(&sh->m, lp_motion_free);
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(GTK_WIDGET(o))))
        gtk_widget_unparent(c);
    G_OBJECT_CLASS(lp_kit_sheet_parent_class)->dispose(o);
}

static void sheet_frame(GtkWidget *w, gpointer d)
{
    (void)d;
    LpKitSheet *sh = (LpKitSheet *)w;
    gtk_widget_queue_draw(w);
    if (sh->closing && !sh->s.moving) {
        GtkRoot *win = gtk_widget_get_root(w);
        if (win)
            gtk_window_destroy(GTK_WINDOW(win));
    }
}

static void sheet_map(GtkWidget *w)
{
    GTK_WIDGET_CLASS(lp_kit_sheet_parent_class)->map(w);
    LpKitSheet *sh = (LpKitSheet *)w;
    if (sh->closing)
        return;
    if (!sh->m) {
        sh->m = lp_motion_new(w, sheet_frame, NULL);
        lp_motion_add(sh->m, &sh->s);
    }
    lp_spring_set_target(&sh->s, 1.0);
    lp_motion_kick(sh->m);
}

static void lp_kit_sheet_class_init(LpKitSheetClass *k)
{
    G_OBJECT_CLASS(k)->dispose = sheet_dispose;
    GTK_WIDGET_CLASS(k)->snapshot = sheet_snapshot;
    GTK_WIDGET_CLASS(k)->map = sheet_map;
    gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(k),
                                             GTK_TYPE_BIN_LAYOUT);
    gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(k), "lpkitsheet");
}

static void lp_kit_sheet_init(LpKitSheet *sh)
{
    lp_spring_init(&sh->s, LP_SPRING_SHEET, 0.0);
}

/* Out at 0.7x from wherever it is: a sheet closed while still arriving
 * turns round carrying its velocity. The window goes when it rests. */
static void sheet_close(LpKitSheet *sh)
{
    if (sh->closing)
        return;
    sh->closing = TRUE;
    gtk_widget_set_sensitive(GTK_WIDGET(sh), FALSE);
    GtkRoot *win = gtk_widget_get_root(GTK_WIDGET(sh));
    if (!sh->m || !gtk_widget_get_mapped(GTK_WIDGET(sh))) {
        if (win)
            gtk_window_destroy(GTK_WINDOW(win));
        return;
    }
    lp_spring_set_target_out(&sh->s, 0.0);
    lp_motion_kick(sh->m);
    if (!sh->s.moving && win)
        gtk_window_destroy(GTK_WINDOW(win));
}

struct _LpKitDialog {
    GtkWidget    *win;
    LpKitSheet   *sheet;
    GtkWidget    *body;
    GtkWidget    *error;
    GtkWidget    *buttons;
    GtkWidget    *spinner;
    GtkWidget    *last_button;
    LpKitResponse cb;
    gpointer      data;
    gboolean      closed;
};

static void dialog_respond(LpKitDialog *d, int response)
{
    if (d->closed)
        return;
    if (d->cb)
        d->cb(d, response, d->data);
    else
        lp_kit_dialog_close(d);
}

static void on_dialog_button(GtkButton *b, gpointer data)
{
    LpKitDialog *d = data;
    dialog_respond(d, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "lp-kit-resp")));
}

static gboolean on_dialog_close_request(GtkWindow *w, gpointer data)
{
    (void)w;
    LpKitDialog *d = data;
    if (d->closed)
        return FALSE;
    dialog_respond(d, LP_KIT_CANCEL);
    return TRUE;
}

static gboolean on_dialog_escape(GtkWidget *w, GVariant *args, gpointer data)
{
    (void)w; (void)args; (void)data;
    LpKitDialog *d = g_object_get_data(G_OBJECT(w), "lp-kit-dialog");
    if (d)
        dialog_respond(d, LP_KIT_CANCEL);
    return TRUE;
}

LpKitDialog *lp_kit_dialog_new(GtkWindow *parent, const char *title,
                               const char *text)
{
    LpKitDialog *d = g_new0(LpKitDialog, 1);
    d->win = gtk_window_new();
    gtk_widget_remove_css_class(d->win, "background");
    gtk_widget_add_css_class(d->win, "lp-sheet-window");
    gtk_window_set_decorated(GTK_WINDOW(d->win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(d->win), FALSE);
    gtk_window_set_modal(GTK_WINDOW(d->win), TRUE);
    if (parent)
        gtk_window_set_transient_for(GTK_WINDOW(d->win), parent);
    gtk_window_set_title(GTK_WINDOW(d->win), title);
    g_object_set_data_full(G_OBJECT(d->win), "lp-kit-dialog", d, g_free);
    g_signal_connect(d->win, "close-request",
                     G_CALLBACK(on_dialog_close_request), d);

    GtkEventController *sc = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0),
                         gtk_callback_action_new(on_dialog_escape, NULL, NULL)));
    gtk_widget_add_controller(d->win, sc);

    d->sheet = g_object_new(lp_kit_sheet_get_type(), NULL);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(card, "lp-sheet-card");
    gtk_widget_set_size_request(card, 440, -1);

    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "lp-sheet-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_label_set_wrap(GTK_LABEL(t), TRUE);
    gtk_box_append(GTK_BOX(card), t);

    if (text && *text) {
        GtkWidget *l = gtk_label_new(text);
        gtk_widget_add_css_class(l, "lp-sheet-text");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 46);
        gtk_box_append(GTK_BOX(card), l);
    }

    d->body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_append(GTK_BOX(card), d->body);

    d->error = gtk_label_new("");
    gtk_widget_add_css_class(d->error, "lp-sheet-error");
    gtk_label_set_xalign(GTK_LABEL(d->error), 0);
    gtk_label_set_wrap(GTK_LABEL(d->error), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(d->error), 46);
    gtk_widget_set_visible(d->error, FALSE);
    gtk_box_append(GTK_BOX(card), d->error);

    d->buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(d->buttons, "lp-sheet-buttons");
    gtk_widget_set_halign(d->buttons, GTK_ALIGN_END);
    gtk_widget_set_margin_top(d->buttons, 10);
    d->spinner = gtk_spinner_new();
    gtk_widget_set_visible(d->spinner, FALSE);
    gtk_box_append(GTK_BOX(d->buttons), d->spinner);
    gtk_box_append(GTK_BOX(card), d->buttons);

    gtk_widget_set_parent(card, GTK_WIDGET(d->sheet));
    gtk_window_set_child(GTK_WINDOW(d->win), GTK_WIDGET(d->sheet));
    return d;
}

GtkWidget *lp_kit_dialog_body(LpKitDialog *d) { return d->body; }
GtkWindow *lp_kit_dialog_window(LpKitDialog *d) { return GTK_WINDOW(d->win); }

GtkWidget *lp_kit_dialog_button(LpKitDialog *d, const char *label,
                                int response, const char *style)
{
    GtkWidget *b = gtk_button_new_with_mnemonic(label);
    g_object_set_data(G_OBJECT(b), "lp-kit-resp", GINT_TO_POINTER(response));
    char *nm = g_strdup_printf("dialog-%d", response);   /* for lp_kit_drive */
    gtk_widget_set_name(b, nm);
    g_free(nm);
    if (style)
        gtk_widget_add_css_class(b, style);
    g_signal_connect(b, "clicked", G_CALLBACK(on_dialog_button), d);
    gtk_box_append(GTK_BOX(d->buttons), b);
    gtk_window_set_default_widget(GTK_WINDOW(d->win), b);
    d->last_button = b;
    return b;
}

void lp_kit_dialog_on_response(LpKitDialog *d, LpKitResponse cb, gpointer data)
{
    d->cb = cb;
    d->data = data;
}

void lp_kit_dialog_error(LpKitDialog *d, const char *text)
{
    gtk_label_set_text(GTK_LABEL(d->error), text ? text : "");
    gtk_widget_set_visible(d->error, text && *text);
}

void lp_kit_dialog_busy(LpKitDialog *d, gboolean busy)
{
    gtk_widget_set_visible(d->spinner, busy);
    if (busy)
        gtk_spinner_start(GTK_SPINNER(d->spinner));
    else
        gtk_spinner_stop(GTK_SPINNER(d->spinner));
    for (GtkWidget *c = gtk_widget_get_first_child(d->buttons); c;
         c = gtk_widget_get_next_sibling(c))
        if (c != d->spinner)
            gtk_widget_set_sensitive(c, !busy);
    gtk_widget_set_sensitive(d->body, !busy);
}

void lp_kit_dialog_present(LpKitDialog *d)
{
    gtk_window_present(GTK_WINDOW(d->win));
}

void lp_kit_dialog_close(LpKitDialog *d)
{
    if (d->closed)
        return;
    d->closed = TRUE;
    sheet_close(d->sheet);
}

typedef struct {
    LpKitConfirmFn cb;
    gpointer       data;
} Confirm;

static void on_confirm(LpKitDialog *d, int response, gpointer data)
{
    Confirm *c = data;
    lp_kit_dialog_close(d);
    if (c->cb)
        c->cb(response == LP_KIT_OK, c->data);
    g_free(c);
}

void lp_kit_confirm(GtkWindow *parent, const char *title, const char *text,
                    const char *ok_label, gboolean destructive,
                    LpKitConfirmFn cb, gpointer data)
{
    Confirm *c = g_new0(Confirm, 1);
    c->cb = cb;
    c->data = data;
    LpKitDialog *d = lp_kit_dialog_new(parent, title, text);
    lp_kit_dialog_button(d, T("Cancel", "취소"), LP_KIT_CANCEL, NULL);
    lp_kit_dialog_button(d, ok_label, LP_KIT_OK,
                         destructive ? "destructive-action" : "suggested-action");
    lp_kit_dialog_on_response(d, on_confirm, c);
    lp_kit_dialog_present(d);
}

/* ═══════════════════════════════════════════════════════════════════
 * lp-privd
 * ═══════════════════════════════════════════════════════════════════
 *
 * One request is one connection: connect, write the line, read lines
 * until "done" or "fail". The protocol is lp-privd's (its header says
 * it all); nothing here interprets more of it than the first word.
 */

const char *lp_priv_socket(void)
{
    const char *e = g_getenv("LP_PRIVD_SOCK");
    return (e && *e) ? e : "/run/lp-privd.sock";
}

typedef void (*TalkLine)(const char *line, gpointer data);
typedef void (*TalkEnd)(const char *last, gpointer data); /* NULL: no daemon */

typedef struct {
    char             *req;
    TalkLine          on_line;
    TalkEnd           on_end;
    gpointer          data;
    GSocketConnection *conn;
    GDataInputStream *in;
} Talk;

static void talk_free(Talk *t)
{
    if (t->req) {
        memset(t->req, 0, strlen(t->req));   /* may hold a password */
        g_free(t->req);
    }
    g_clear_object(&t->in);
    if (t->conn)
        g_io_stream_close(G_IO_STREAM(t->conn), NULL, NULL);
    g_clear_object(&t->conn);
    g_free(t);
}

static void talk_read(Talk *t);

static void on_talk_line(GObject *src, GAsyncResult *res, gpointer data)
{
    Talk *t = data;
    gsize len = 0;
    char *line = g_data_input_stream_read_line_finish_utf8(
        G_DATA_INPUT_STREAM(src), res, &len, NULL);
    if (!line) {
        /* The daemon went away without a verdict. */
        t->on_end("fail failed the connection to lp-privd was lost", t->data);
        talk_free(t);
        return;
    }
    if (g_str_has_prefix(line, "done") || g_str_has_prefix(line, "fail")) {
        t->on_end(line, t->data);
        g_free(line);
        talk_free(t);
        return;
    }
    if (t->on_line)
        t->on_line(line, t->data);
    g_free(line);
    talk_read(t);
}

static void talk_read(Talk *t)
{
    g_data_input_stream_read_line_async(t->in, G_PRIORITY_DEFAULT, NULL,
                                        on_talk_line, t);
}

static void on_talk_connected(GObject *src, GAsyncResult *res, gpointer data)
{
    Talk *t = data;
    GError *err = NULL;
    t->conn = g_socket_client_connect_finish(G_SOCKET_CLIENT(src), res, &err);
    g_object_unref(src);
    if (!t->conn) {
        g_clear_error(&err);
        t->on_end(NULL, t->data);
        talk_free(t);
        return;
    }
    /* One short line on a local socket: written in one go. */
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(t->conn));
    if (!g_output_stream_write_all(out, t->req, strlen(t->req), NULL, NULL, &err)) {
        g_clear_error(&err);
        t->on_end(NULL, t->data);
        talk_free(t);
        return;
    }
    memset(t->req, 0, strlen(t->req));
    t->in = g_data_input_stream_new(
        g_io_stream_get_input_stream(G_IO_STREAM(t->conn)));
    g_data_input_stream_set_newline_type(t->in, G_DATA_STREAM_NEWLINE_TYPE_LF);
    talk_read(t);
}

/* Takes ownership of req (which must end in "\n"). */
static void talk(char *req, TalkLine on_line, TalkEnd on_end, gpointer data)
{
    Talk *t = g_new0(Talk, 1);
    t->req = req;
    t->on_line = on_line;
    t->on_end = on_end;
    t->data = data;
    GSocketClient *c = g_socket_client_new();
    GSocketAddress *a = g_unix_socket_address_new(lp_priv_socket());
    g_socket_client_connect_async(c, G_SOCKET_CONNECTABLE(a), NULL,
                                  on_talk_connected, t);
    g_object_unref(a);
}

/* ── one job, and the password it may need ─────────────────────────── */

typedef struct {
    GtkWindow   *parent;
    char        *req;
    char        *why_text;
    LpPrivLine   on_line;
    LpPrivDone   on_done;
    gpointer     data;
    LpKitDialog *pw;
    GtkWidget   *entry;
    GtkWidget   *ok;
    guint        wait_timer;
    int          wait_left;
    int          asked;
    gboolean     authing;    /* a password is with lp-privd, auth_end to come */
    gboolean     cancelled;  /* ... and the sheet was closed meanwhile */
} Job;

static void job_free(Job *j)
{
    if (j->wait_timer)
        g_source_remove(j->wait_timer);
    if (j->parent)
        g_object_remove_weak_pointer(G_OBJECT(j->parent), (gpointer *)&j->parent);
    g_free(j->req);
    g_free(j->why_text);
    g_free(j);
}

static void job_send(Job *j);

/* "fail <why> <text>" -> why, text */
static void split_fail(const char *line, char **why, char **text)
{
    const char *p = line + 4;
    while (*p == ' ')
        p++;
    const char *sp = strchr(p, ' ');
    *why = sp ? g_strndup(p, (gsize)(sp - p)) : g_strdup(p);
    *text = g_strdup(sp ? sp + 1 : "");
}

static void job_finish(Job *j, gboolean ok, const char *why, const char *text)
{
    if (j->pw) {
        lp_kit_dialog_close(j->pw);
        j->pw = NULL;
    }
    if (j->on_done)
        j->on_done(ok, why, text, j->data);
    job_free(j);
}

static void job_line(const char *line, gpointer data)
{
    Job *j = data;
    if (!j->on_line)
        return;
    const char *sp = strchr(line, ' ');
    char *kind = sp ? g_strndup(line, (gsize)(sp - line)) : g_strdup(line);
    const char *rest = sp ? sp + 1 : "";
    int pct = -1;
    if (strcmp(kind, "progress") == 0) {
        char *end = NULL;
        long v = strtol(rest, &end, 10);
        if (end != rest) {
            pct = (int)CLAMP(v, 0, 100);
            rest = end;
            while (*rest == ' ')
                rest++;
        }
    }
    j->on_line(kind, pct, rest, j->data);
    g_free(kind);
}

static void ask_password(Job *j);

static void job_end(const char *last, gpointer data)
{
    Job *j = data;
    if (!last) {
        job_finish(j, FALSE, "unreachable",
                   T("The system service lp-privd is not running.",
                     "시스템 서비스 lp-privd 가 실행 중이 아닙니다."));
        return;
    }
    if (g_str_has_prefix(last, "done")) {
        job_finish(j, TRUE, NULL, last[4] ? last + 5 : "");
        return;
    }
    char *why, *text;
    split_fail(last, &why, &text);
    if (strcmp(why, "auth") == 0 && j->asked < 4) {
        g_free(why);
        g_free(text);
        ask_password(j);
        return;
    }
    job_finish(j, FALSE, why, text);
    g_free(why);
    g_free(text);
}

static void job_send(Job *j)
{
    talk(g_strdup(j->req), job_line, job_end, j);
}

static gboolean pw_tick(gpointer data)
{
    Job *j = data;
    if (!j->pw) {
        j->wait_timer = 0;
        return G_SOURCE_REMOVE;
    }
    if (--j->wait_left <= 0) {
        j->wait_timer = 0;
        lp_kit_dialog_error(j->pw, T("Try again now.", "이제 다시 입력하세요."));
        gtk_widget_set_sensitive(j->ok, TRUE);
        gtk_widget_set_sensitive(j->entry, TRUE);
        gtk_widget_grab_focus(j->entry);
        return G_SOURCE_REMOVE;
    }
    char *m = g_strdup_printf(T("Wrong password. Wait %d s before trying again.",
                                "암호가 틀렸습니다. %d초 뒤에 다시 입력하세요."),
                              j->wait_left);
    lp_kit_dialog_error(j->pw, m);
    g_free(m);
    return G_SOURCE_CONTINUE;
}

static void pw_wait(Job *j, int secs)
{
    j->wait_left = secs + 1;
    gtk_widget_set_sensitive(j->ok, FALSE);
    gtk_widget_set_sensitive(j->entry, FALSE);
    if (j->wait_timer)
        g_source_remove(j->wait_timer);
    j->wait_timer = g_timeout_add_seconds(1, pw_tick, j);
    pw_tick(j);
}

static void auth_end(const char *last, gpointer data)
{
    Job *j = data;
    j->authing = FALSE;
    if (j->cancelled) {   /* the dialog was cancelled meanwhile */
        job_free(j);
        return;
    }
    if (!j->pw)
        return;
    lp_kit_dialog_busy(j->pw, FALSE);
    if (!last) {
        lp_kit_dialog_error(j->pw, T("The system service lp-privd is not running.",
                                     "시스템 서비스 lp-privd 가 실행 중이 아닙니다."));
        return;
    }
    if (g_str_has_prefix(last, "done")) {
        /* Accepted: the dialog leaves and the job goes again. */
        lp_kit_dialog_close(j->pw);
        j->pw = NULL;
        job_send(j);
        return;
    }
    char *why, *text;
    split_fail(last, &why, &text);
    if (strcmp(why, "auth") == 0 && g_str_has_prefix(text, "wait ")) {
        int s = atoi(text + 5);
        pw_wait(j, s > 0 ? s : 2);
    } else if (strcmp(why, "auth") == 0) {
        /* "wrong password": the daemon also starts a wait now, and says
         * how long on the next try - so the wait is shown right away. */
        lp_kit_dialog_error(j->pw, T("Wrong password. Try again.",
                                     "암호가 틀렸습니다. 다시 입력하세요."));
        gtk_widget_grab_focus(j->entry);
    } else if (strcmp(why, "denied") == 0) {
        lp_kit_dialog_error(j->pw,
            T("This account is not an administrator. Ask an administrator "
              "to do this.",
              "이 계정은 관리자가 아닙니다. 관리자에게 부탁하세요."));
        gtk_widget_set_sensitive(j->ok, FALSE);
        gtk_widget_set_sensitive(j->entry, FALSE);
    } else {
        lp_kit_dialog_error(j->pw, text);
    }
    g_free(why);
    g_free(text);
}

static void pw_response(LpKitDialog *d, int response, gpointer data)
{
    Job *j = data;
    if (response != LP_KIT_OK) {
        j->pw = NULL;
        lp_kit_dialog_close(d);
        if (j->authing) {
            /* Escape while the password is being checked: the caller
             * hears now, but the Job stays until lp-privd answers -
             * auth_end has it as its data, and freed here it read freed
             * memory when the answer came. */
            j->cancelled = TRUE;
            if (j->on_done)
                j->on_done(FALSE, "cancelled", T("Cancelled", "취소했습니다"), j->data);
            j->on_done = NULL;
            j->on_line = NULL;
            return;
        }
        job_finish(j, FALSE, "cancelled", T("Cancelled", "취소했습니다"));
        return;
    }
    const char *pw = gtk_editable_get_text(GTK_EDITABLE(j->entry));
    size_t n = strlen(pw);
    if (n == 0 || n > 255) {
        lp_kit_dialog_error(d, T("Type your password.", "암호를 입력하세요."));
        return;
    }
    /* "auth\t" + hex + "\n", built in one buffer that talk() wipes. */
    static const char hx[] = "0123456789abcdef";
    char *req = g_malloc(5 + n * 2 + 2);
    memcpy(req, "auth\t", 5);
    for (size_t i = 0; i < n; i++) {
        req[5 + 2 * i] = hx[((unsigned char)pw[i]) >> 4];
        req[5 + 2 * i + 1] = hx[((unsigned char)pw[i]) & 15];
    }
    req[5 + 2 * n] = '\n';
    req[5 + 2 * n + 1] = '\0';
    gtk_editable_set_text(GTK_EDITABLE(j->entry), "");
    lp_kit_dialog_error(d, NULL);
    lp_kit_dialog_busy(d, TRUE);
    j->authing = TRUE;
    talk(req, NULL, auth_end, j);
}

static void ask_password(Job *j)
{
    j->asked++;
    const char *user = g_get_user_name();
    char *who = g_strdup_printf(T("Enter the password for %s.",
                                  "%s 사용자의 암호를 입력하세요."), user);
    char *text = g_strdup_printf("%s %s", j->why_text ? j->why_text : "", who);
    LpKitDialog *d = lp_kit_dialog_new(j->parent,
        T("Administrator password", "관리자 암호"), text);
    g_free(who);
    g_free(text);

    j->entry = gtk_password_entry_new();
    gtk_widget_set_name(j->entry, "password");
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(j->entry), TRUE);
    g_object_set(j->entry, "activates-default", TRUE, NULL);
    gtk_box_append(GTK_BOX(lp_kit_dialog_body(d)), j->entry);

    GtkWidget *note = gtk_label_new(
        T("It is kept for 5 minutes, so the next change does not ask again.",
          "5분 동안 기억하므로 그 사이의 다음 변경은 다시 묻지 않습니다."));
    gtk_widget_add_css_class(note, "lp-sheet-text");
    gtk_widget_add_css_class(note, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(note), 46);
    gtk_box_append(GTK_BOX(lp_kit_dialog_body(d)), note);

    lp_kit_dialog_button(d, T("Cancel", "취소"), LP_KIT_CANCEL, NULL);
    j->ok = lp_kit_dialog_button(d, T("Authenticate", "인증"), LP_KIT_OK,
                                 "suggested-action");
    lp_kit_dialog_on_response(d, pw_response, j);
    j->pw = d;
    lp_kit_dialog_present(d);
    gtk_widget_grab_focus(j->entry);
}

void lp_priv_run(GtkWindow *parent, const char *const *fields,
                 const char *why_text, LpPrivLine on_line,
                 LpPrivDone on_done, gpointer data)
{
    Job *j = g_new0(Job, 1);
    j->parent = parent;
    if (parent)
        g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&j->parent);
    j->why_text = g_strdup(why_text);
    j->on_line = on_line;
    j->on_done = on_done;
    j->data = data;

    GString *s = g_string_new(NULL);
    for (int i = 0; fields[i]; i++) {
        /* A TAB or newline inside a field would split it into two; the
         * daemon would refuse the result, but it is refused here first
         * so the message says what actually went wrong. */
        if (strpbrk(fields[i], "\t\n\r")) {
            g_string_free(s, TRUE);
            job_finish(j, FALSE, "invalid", T("Not a valid name", "올바른 이름이 아닙니다"));
            return;
        }
        if (i)
            g_string_append_c(s, '\t');
        g_string_append(s, fields[i]);
    }
    g_string_append_c(s, '\n');
    j->req = g_string_free(s, FALSE);
    job_send(j);
}

/* ═══════════════════════════════════════════════════════════════════
 * Test driving (see lp-kit.h)
 * ═══════════════════════════════════════════════════════════════════ */

static GtkWidget *find_in(GtkWidget *w, const char *name)
{
    if (g_strcmp0(gtk_widget_get_name(w), name) == 0)
        return w;
    for (GtkWidget *c = gtk_widget_get_first_child(w); c;
         c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *r = find_in(c, name);
        if (r)
            return r;
    }
    return NULL;
}

GtkWidget *lp_kit_find(const char *name)
{
    GListModel *tops = gtk_window_get_toplevels();
    GtkWidget *hit = NULL;
    /* Newest window first: a dialog's "dialog-1" before anything else. */
    for (guint i = g_list_model_get_n_items(tops); i-- > 0 && !hit;) {
        GtkWidget *w = g_list_model_get_item(tops, i);
        if (gtk_widget_get_visible(w))
            hit = find_in(w, name);
        g_object_unref(w);
    }
    return hit;
}

typedef struct {
    LpKitDriveFn other;
    gpointer     data;
} Drive;

static void drive_line(Drive *d, char *line)
{
    g_strstrip(line);
    if (!*line)
        return;
    char *sp = strchr(line, ' ');
    char *arg = sp ? sp + 1 : (char *)"";
    if (sp)
        *sp = '\0';
    fprintf(stderr, "lp-kit drive: %s %s\n", line, arg);
    if (strcmp(line, "click") == 0 || strcmp(line, "activate") == 0) {
        GtkWidget *w = lp_kit_find(arg);
        if (!w)
            fprintf(stderr, "lp-kit drive: no widget %s\n", arg);
        else if (strcmp(line, "click") == 0 && GTK_IS_BUTTON(w))
            g_signal_emit_by_name(w, "clicked");
        else if (GTK_IS_LIST_BOX_ROW(w)) {
            GtkWidget *lb = gtk_widget_get_parent(w);
            gtk_list_box_select_row(GTK_LIST_BOX(lb), GTK_LIST_BOX_ROW(w));
            g_signal_emit_by_name(lb, "row-activated", w);
        } else
            gtk_widget_activate(w);
    } else if (strcmp(line, "text") == 0) {
        char *sp2 = strchr(arg, ' ');
        if (sp2)
            *sp2 = '\0';
        GtkWidget *w = lp_kit_find(arg);
        if (w && GTK_IS_EDITABLE(w))
            gtk_editable_set_text(GTK_EDITABLE(w), sp2 ? sp2 + 1 : "");
        else
            fprintf(stderr, "lp-kit drive: no entry %s\n", arg);
    } else if (d->other) {
        d->other(line, arg, d->data);
    }
}

static gboolean drive_in(GIOChannel *ch, GIOCondition c, gpointer data)
{
    (void)c;
    char *line = NULL;
    gsize len;
    while (g_io_channel_read_line(ch, &line, &len, NULL, NULL) == G_IO_STATUS_NORMAL
           && line) {
        drive_line(data, line);
        g_free(line);
        line = NULL;
    }
    return G_SOURCE_CONTINUE;
}

void lp_kit_drive(LpKitDriveFn other, gpointer data)
{
    const char *path = g_getenv("LP_KIT_DRIVE");
    if (!path || !*path)
        return;
    /* O_RDWR: the FIFO then always has a writer (us), so it never reads
     * as end-of-file between two test commands. */
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;
    Drive *d = g_new0(Drive, 1);
    d->other = other;
    d->data = data;
    GIOChannel *ch = g_io_channel_unix_new(fd);
    g_io_channel_set_flags(ch, G_IO_FLAG_NONBLOCK, NULL);
    g_io_add_watch(ch, G_IO_IN, drive_in, d);
}
