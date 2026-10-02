/*
 * ui.c - the rows, pages and dialogs every panel is made of.
 *
 * Spec 2-1 says every item is the same row, with one of a few things on
 * the right. Putting each of those things behind its own function is what
 * keeps that true: a panel cannot invent a fifth kind of row without
 * writing it here, where it is visible to everyone.
 *
 * ── Touch ──
 *
 * The screen this runs on is a 15.6" 3840x2160 touch panel at scale 2, so
 * a logical pixel is about 0.18mm and a finger is about 9mm - 48 logical
 * pixels. Every row is at least that tall and every control inside one is
 * at least that big, which is set in core.c's stylesheet rather than
 * here. What is here is the other half: a switch row toggles when the row
 * is tapped, not just the switch, and a chevron row opens when tapped
 * anywhere. A 48px row with a 40px switch at its far right edge is a
 * 40px target with a lot of dead space next to it.
 *
 * Sliders follow the finger 1:1 - the knob and the number beside it move
 * with every event - and apply live, but at most thirty times a second
 * (SCALE_MIN_MS), with the last value always sent once the finger stops.
 * Dragging the volume from 0 to 100 otherwise starts a hundred wpctl
 * processes in a second, and lp-tune gets a hundred backlight glides it
 * has to serialise. Panels send through lp_run_latest(), which keeps at
 * most one command in flight per control.
 *
 * ── Motion ──
 *
 * Rows sit inside a GtkRevealer so a list that changes on screen (Wi-Fi
 * networks, input sources) can open a row as it arrives and close it
 * before it goes - 220ms in, 154ms out, feel.md's "insert" - rather than
 * jumping. Dialogs are sheets: they arrive from 96% size and transparent
 * to full size and opaque on lp-motion's "sheet" spring (260ms, 182ms
 * out), drawn by LpSheet below with a snapshot transform, so nothing is
 * laid out again per frame. With reduced motion the spring settles in
 * about 90ms and only the opacity moves.
 *
 * ── Handlers and lp_quiet ──
 *
 * Every control's handler goes through a trampoline here that returns
 * early while lp_quiet is set. Panels put a control back after a failed
 * action ("could not connect", so the switch goes back off) inside
 * LP_QUIET(), and the trampoline is what stops that correction from
 * running the failed action a second time.
 */
#include "core.h"

#include "lp-motion.h"

#include <math.h>
#include <string.h>

/* 30 calls a second, the brief's ceiling for live sliders. */
#define SCALE_MIN_MS 33

/* ── a width limit for the page ─────────────────────────────────────
 *
 * At scale 2 the window is 1920 logical pixels wide and a row stretched
 * across all of it puts the switch a hand's width from its label. GTK 4.8
 * has no AdwClamp, so this is the part of it that matters: the child is
 * never wider than PAGE_MAX, and is centred when there is room. */
#define PAGE_MAX 760

typedef struct { GtkWidget parent; } LpClamp;
typedef struct { GtkWidgetClass parent_class; } LpClampClass;
G_DEFINE_TYPE(LpClamp, lp_clamp, GTK_TYPE_WIDGET)

static void clamp_measure(GtkWidget *w, GtkOrientation o, int for_size,
                          int *min, int *nat, int *minb, int *natb)
{
    GtkWidget *c = gtk_widget_get_first_child(w);
    *min = *nat = 0;
    *minb = *natb = -1;
    if (!c) return;
    if (o == GTK_ORIENTATION_VERTICAL && for_size > PAGE_MAX)
        for_size = PAGE_MAX;
    gtk_widget_measure(c, o, for_size, min, nat, NULL, NULL);
    if (o == GTK_ORIENTATION_HORIZONTAL && *nat > PAGE_MAX)
        *nat = MAX(*min, PAGE_MAX);
}

static void clamp_allocate(GtkWidget *w, int width, int height, int baseline)
{
    (void)baseline;
    GtkWidget *c = gtk_widget_get_first_child(w);
    if (!c) return;
    int min = 0;
    gtk_widget_measure(c, GTK_ORIENTATION_HORIZONTAL, -1, &min, NULL, NULL, NULL);
    int cw = MAX(MIN(width, PAGE_MAX), min);
    int x = (width - cw) / 2;
    if (x < 0) x = 0;
    GtkAllocation a = { x, 0, cw, height };
    gtk_widget_size_allocate(c, &a, -1);
}

static GtkSizeRequestMode clamp_mode(GtkWidget *w)
{
    (void)w;
    return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

/* A plain GtkWidget does not drop its children when it goes away; this
 * one must, or every panel switch leaks the page it replaced. */
static void clamp_dispose(GObject *o)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(GTK_WIDGET(o))))
        gtk_widget_unparent(c);
    G_OBJECT_CLASS(lp_clamp_parent_class)->dispose(o);
}

static void lp_clamp_class_init(LpClampClass *k)
{
    G_OBJECT_CLASS(k)->dispose = clamp_dispose;
    GTK_WIDGET_CLASS(k)->measure = clamp_measure;
    GTK_WIDGET_CLASS(k)->size_allocate = clamp_allocate;
    GTK_WIDGET_CLASS(k)->get_request_mode = clamp_mode;
}

static void lp_clamp_init(LpClamp *c)
{
    (void)c;
}

/* ── pages ──────────────────────────────────────────────────────────── */

GtkWidget *page_new(const char *title, const char *subtitle)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(page, "lp-page");
    gtk_widget_set_margin_start(page, 28);
    gtk_widget_set_margin_end(page, 28);
    gtk_widget_set_margin_top(page, 24);
    gtk_widget_set_margin_bottom(page, 40);

    GtkWidget *t = gtk_label_new(title);
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    gtk_widget_add_css_class(t, "lp-title");
    gtk_box_append(GTK_BOX(page), t);

    /* Always created, even empty, so an answer that arrives later (a scan,
     * a battery reading) has somewhere to go. */
    GtkWidget *s = gtk_label_new(subtitle ? subtitle : "");
    gtk_widget_set_halign(s, GTK_ALIGN_START);
    gtk_widget_add_css_class(s, "lp-subtitle");
    gtk_label_set_wrap(GTK_LABEL(s), TRUE);
    gtk_label_set_xalign(GTK_LABEL(s), 0.0);
    gtk_widget_set_margin_top(s, 4);
    gtk_widget_set_visible(s, subtitle && *subtitle);
    gtk_box_append(GTK_BOX(page), s);
    g_object_set_data(G_OBJECT(page), "lp-subtitle", s);

    /* The clamp is what goes into the scrolled window; the page is what
     * panels are handed, and it finds its way back to the clamp. */
    GtkWidget *clamp = g_object_new(lp_clamp_get_type(), NULL);
    gtk_widget_set_parent(page, clamp);
    g_object_set_data(G_OBJECT(page), "lp-clamp", clamp);
    g_object_set_data(G_OBJECT(clamp), "lp-page", page);
    return page;
}

void page_set_subtitle(GtkWidget *page, const char *subtitle)
{
    GtkWidget *s = g_object_get_data(G_OBJECT(page), "lp-subtitle");
    if (!s) return;
    gtk_label_set_text(GTK_LABEL(s), subtitle ? subtitle : "");
    gtk_widget_set_visible(s, subtitle && *subtitle);
}

GtkWidget *page_note(GtkWidget *page, const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "lp-note");
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_widget_set_halign(l, GTK_ALIGN_FILL);
    gtk_widget_set_margin_top(l, 8);
    gtk_widget_set_margin_start(l, 4);
    gtk_widget_set_margin_end(l, 4);
    gtk_box_append(GTK_BOX(page), l);
    return l;
}

typedef void (*activate_fn)(GtkWidget *row, gpointer data);

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer d)
{
    (void)box; (void)d;
    activate_fn fn = g_object_get_data(G_OBJECT(row), "lp-activate");
    if (fn)
        fn(GTK_WIDGET(row), g_object_get_data(G_OBJECT(row), "lp-activate-data"));
}

GtkWidget *group_new(GtkWidget *page, const char *heading)
{
    if (heading && *heading) {
        GtkWidget *h = gtk_label_new(heading);
        gtk_widget_set_halign(h, GTK_ALIGN_START);
        gtk_widget_add_css_class(h, "lp-heading");
        gtk_widget_set_margin_top(h, 22);
        gtk_widget_set_margin_bottom(h, 8);
        gtk_widget_set_margin_start(h, 4);
        gtk_box_append(GTK_BOX(page), h);
    }

    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "lp-group");
    if (!heading || !*heading)
        gtk_widget_set_margin_top(list, 18);
    g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated), NULL);
    gtk_box_append(GTK_BOX(page), list);
    return list;
}

/* ── rows ───────────────────────────────────────────────────────────── */

GtkWidget *row_shell(const char *title, const char *detail)
{
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_widget_add_css_class(row, "lp-row");

    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_margin_start(h, 16);
    gtk_widget_set_margin_end(h, 12);
    gtk_widget_set_margin_top(h, 6);
    gtk_widget_set_margin_bottom(h, 6);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);

    GtkWidget *l = gtk_label_new(title);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_widget_add_css_class(l, "lp-row-title");
    gtk_box_append(GTK_BOX(v), l);

    /* Spec 2-1: a second line only where the name does not say what the
     * row does. Created hidden either way so it can be filled in later. */
    GtkWidget *d = gtk_label_new(detail ? detail : "");
    gtk_widget_set_halign(d, GTK_ALIGN_START);
    gtk_widget_add_css_class(d, "lp-detail");
    gtk_label_set_wrap(GTK_LABEL(d), TRUE);
    gtk_label_set_xalign(GTK_LABEL(d), 0.0);
    gtk_widget_set_visible(d, detail && *detail);
    gtk_box_append(GTK_BOX(v), d);

    gtk_box_append(GTK_BOX(h), v);
    gtk_widget_add_css_class(h, "lp-row-box");

    /* Open by default; row_add_animated() closes it first. */
    GtkWidget *rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(rev),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(rev),
                                         lp_spring_ms(LP_SPRING_INSERT, FALSE));
    gtk_revealer_set_reveal_child(GTK_REVEALER(rev), TRUE);
    gtk_revealer_set_child(GTK_REVEALER(rev), h);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), rev);
    g_object_set_data(G_OBJECT(row), "lp-revealer", rev);
    g_object_set_data(G_OBJECT(row), "lp-hbox", h);
    g_object_set_data(G_OBJECT(row), "lp-detail", d);
    g_object_set_data_full(G_OBJECT(row), "lp-title", g_strdup(title), g_free);
    return row;
}

GtkWidget *row_box(GtkWidget *row)
{
    return g_object_get_data(G_OBJECT(row), "lp-hbox");
}

GtkWidget *row_control(GtkWidget *row)
{
    return g_object_get_data(G_OBJECT(row), "lp-control");
}

void row_set_detail(GtkWidget *row, const char *detail)
{
    GtkWidget *d = g_object_get_data(G_OBJECT(row), "lp-detail");
    if (!d) return;
    gtk_label_set_text(GTK_LABEL(d), detail ? detail : "");
    gtk_widget_set_visible(d, detail && *detail);
}

void row_add(GtkWidget *list, GtkWidget *row)
{
    gtk_list_box_append(GTK_LIST_BOX(list), row);
}

/* Opened on the next frame, not now: a revealer told to open before it
 * has been drawn closed has nothing to animate from and simply appears. */
static gboolean reveal_now(gpointer p)
{
    GtkWidget *rev = p;
    if (gtk_widget_get_parent(rev))
        gtk_revealer_set_reveal_child(GTK_REVEALER(rev), TRUE);
    g_object_unref(rev);
    return G_SOURCE_REMOVE;
}

void row_insert_animated(GtkWidget *list, GtkWidget *row, int pos)
{
    GtkWidget *rev = g_object_get_data(G_OBJECT(row), "lp-revealer");
    if (rev) {
        gtk_revealer_set_transition_duration(GTK_REVEALER(rev),
                                             lp_spring_ms(LP_SPRING_INSERT, FALSE));
        gtk_revealer_set_reveal_child(GTK_REVEALER(rev), FALSE);
    }
    gtk_list_box_insert(GTK_LIST_BOX(list), row, pos);
    if (rev)
        g_timeout_add(16, reveal_now, g_object_ref(rev));
}

void row_add_animated(GtkWidget *list, GtkWidget *row)
{
    row_insert_animated(list, row, -1);
}

static void row_closed(GObject *rev, GParamSpec *ps, gpointer p)
{
    (void)ps;
    GtkWidget *row = p;
    if (gtk_revealer_get_child_revealed(GTK_REVEALER(rev)))
        return;
    GtkWidget *list = gtk_widget_get_parent(row);
    if (list && GTK_IS_LIST_BOX(list))
        gtk_list_box_remove(GTK_LIST_BOX(list), row);
}

void row_remove_animated(GtkWidget *row)
{
    GtkWidget *rev = g_object_get_data(G_OBJECT(row), "lp-revealer");
    GtkWidget *list = gtk_widget_get_parent(row);
    if (!list) return;
    if (!rev || !gtk_widget_get_mapped(row) ||
        !gtk_revealer_get_child_revealed(GTK_REVEALER(rev))) {
        gtk_list_box_remove(GTK_LIST_BOX(list), row);
        return;
    }
    /* No taps on a row that is on its way out. */
    gtk_widget_set_sensitive(row, FALSE);
    gtk_revealer_set_transition_duration(GTK_REVEALER(rev),
                                         lp_spring_ms(LP_SPRING_INSERT, TRUE));
    g_signal_connect(rev, "notify::child-revealed", G_CALLBACK(row_closed), row);
    gtk_revealer_set_reveal_child(GTK_REVEALER(rev), FALSE);
}

static void set_control(GtkWidget *row, GtkWidget *c)
{
    gtk_widget_set_valign(c, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row_box(row)), c);
    g_object_set_data(G_OBJECT(row), "lp-control", c);
    g_object_set_data(G_OBJECT(c), "lp-row", row);
}

GtkWidget *row_value(GtkWidget *list, const char *title, const char *detail,
                     const char *value)
{
    GtkWidget *row = row_shell(title, detail);
    GtkWidget *v = gtk_label_new(value ? value : "");
    gtk_widget_add_css_class(v, "lp-value");
    gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(v), 40);
    /* Values are things people copy: an IP address, a MAC, a kernel
     * version. Selectable costs nothing and saves retyping them. */
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_widget_set_can_focus(v, FALSE);
    set_control(row, v);
    if (list) row_add(list, row);
    return row;
}

void row_set_value(GtkWidget *row, const char *value)
{
    GtkWidget *c = row_control(row);
    if (c && GTK_IS_LABEL(c))
        gtk_label_set_text(GTK_LABEL(c), value ? value : "");
}

GtkWidget *row_widget(GtkWidget *list, const char *title, const char *detail,
                      GtkWidget *widget)
{
    GtkWidget *row = row_shell(title, detail);
    set_control(row, widget);
    if (list) row_add(list, row);
    return row;
}

/* switch */

typedef void (*switch_fn)(GObject *sw, GParamSpec *ps, gpointer data);

static void switch_tramp(GObject *sw, GParamSpec *ps, gpointer d)
{
    (void)d;
    if (lp_quiet) return;
    switch_fn fn = g_object_get_data(sw, "lp-cb");
    if (fn) fn(sw, ps, g_object_get_data(sw, "lp-cb-data"));
}

static void switch_row_tapped(GtkWidget *row, gpointer d)
{
    (void)d;
    GtkWidget *sw = row_control(row);
    if (sw && gtk_widget_is_sensitive(sw))
        gtk_switch_set_active(GTK_SWITCH(sw), !gtk_switch_get_active(GTK_SWITCH(sw)));
}

/* GtkSwitch (4.8) has two gestures: a click that toggles it and a pan
 * that drags the knob. A press on the knob leaves the decision to the
 * pan, and once the switch has been put back from code - an
 * administrator password cancelled, a radio that did not come on - a tap
 * on the knob stopped doing anything at all: the switch looked alive and
 * ignored the finger until it was tapped beside the knob. Nobody drags a
 * 60px switch, so the pan goes and every tap on it is a click. */
static void switch_drop_drag(GtkWidget *sw)
{
    GListModel *cs = gtk_widget_observe_controllers(sw);
    for (guint i = g_list_model_get_n_items(cs); i-- > 0;) {
        GtkEventController *c = g_list_model_get_item(cs, i);
        if (GTK_IS_GESTURE_PAN(c)) {
            /* Kept alive, though off the switch: GtkSwitch holds the pan
             * by a bare pointer and denies it on every press beside the
             * knob (gtkswitch.c, click_gesture_pressed). Removed and
             * freed, every tap there wrote into freed memory - which
             * could close Settings at any later moment. Kept, the call
             * finds a gesture with no touches, and does nothing. */
            g_object_set_data_full(G_OBJECT(sw), "lp-pan-kept",
                                   g_object_ref(c), g_object_unref);
            gtk_widget_remove_controller(sw, c);
        }
        g_object_unref(c);
    }
    g_object_unref(cs);
}

GtkWidget *row_switch(GtkWidget *list, const char *title, const char *detail,
                      gboolean on, GCallback cb, gpointer data)
{
    GtkWidget *row = row_shell(title, detail);
    GtkWidget *sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), on);
    switch_drop_drag(sw);
    set_control(row, sw);
    if (cb) {
        g_object_set_data(G_OBJECT(sw), "lp-cb", (gpointer)cb);
        g_object_set_data(G_OBJECT(sw), "lp-cb-data", data);
        g_signal_connect(sw, "notify::active", G_CALLBACK(switch_tramp), NULL);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        g_object_set_data(G_OBJECT(row), "lp-activate", (gpointer)switch_row_tapped);
    } else {
        gtk_widget_set_sensitive(sw, FALSE);
    }
    if (list) row_add(list, row);
    return row;
}

/* button */

typedef void (*button_fn)(GtkButton *b, gpointer data);

static void button_tramp(GtkButton *b, gpointer d)
{
    (void)d;
    if (lp_quiet) return;
    button_fn fn = g_object_get_data(G_OBJECT(b), "lp-cb");
    if (fn) fn(b, g_object_get_data(G_OBJECT(b), "lp-cb-data"));
}

GtkWidget *row_button(GtkWidget *list, const char *title, const char *detail,
                      const char *label, GCallback cb, gpointer data)
{
    GtkWidget *row = row_shell(title, detail);
    GtkWidget *b = gtk_button_new_with_label(label);
    set_control(row, b);
    if (cb) {
        g_object_set_data(G_OBJECT(b), "lp-cb", (gpointer)cb);
        g_object_set_data(G_OBJECT(b), "lp-cb-data", data);
        g_signal_connect(b, "clicked", G_CALLBACK(button_tramp), NULL);
    } else {
        gtk_widget_set_sensitive(b, FALSE);
    }
    if (list) row_add(list, row);
    return row;
}

/* chevron */

GtkWidget *row_chevron(GtkWidget *list, const char *title, const char *detail,
                       const char *value, GCallback cb, gpointer data)
{
    GtkWidget *row = row_shell(title, detail);
    GtkWidget *h = row_box(row);

    GtkWidget *v = gtk_label_new(value ? value : "");
    gtk_widget_add_css_class(v, "lp-value");
    gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(v), 32);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(h), v);
    g_object_set_data(G_OBJECT(row), "lp-control", v);

    GtkWidget *arrow = gtk_image_new_from_icon_name("go-next-symbolic");
    gtk_widget_add_css_class(arrow, "lp-chevron");
    gtk_widget_set_valign(arrow, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(h), arrow);

    if (cb) {
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        g_object_set_data(G_OBJECT(row), "lp-activate", (gpointer)cb);
        g_object_set_data(G_OBJECT(row), "lp-activate-data", data);
    } else {
        gtk_widget_set_opacity(arrow, 0.3);
    }
    if (list) row_add(list, row);
    return row;
}

/* choice */

typedef void (*choice_fn)(GObject *dd, GParamSpec *ps, gpointer data);

static void choice_tramp(GObject *dd, GParamSpec *ps, gpointer d)
{
    (void)d;
    if (lp_quiet) return;
    choice_fn fn = g_object_get_data(dd, "lp-cb");
    if (fn) fn(dd, ps, g_object_get_data(dd, "lp-cb-data"));
}

GtkWidget *row_choice(GtkWidget *list, const char *title, const char *detail,
                      const char *const *items, guint selected,
                      GCallback cb, gpointer data)
{
    GtkWidget *row = row_shell(title, detail);
    GtkWidget *dd = gtk_drop_down_new_from_strings(items);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), selected);
    set_control(row, dd);
    if (cb) {
        g_object_set_data(G_OBJECT(dd), "lp-cb", (gpointer)cb);
        g_object_set_data(G_OBJECT(dd), "lp-cb-data", data);
        g_signal_connect(dd, "notify::selected", G_CALLBACK(choice_tramp), NULL);
    } else {
        gtk_widget_set_sensitive(dd, FALSE);
    }
    if (list) row_add(list, row);
    return row;
}

GtkWidget *row_options(GtkWidget *list, const char *title, const char *detail,
                       const lp_opt_t *opts, const char *current, guint dflt,
                       GCallback cb, gpointer data)
{
    GPtrArray *labels = g_ptr_array_new();
    guint sel = dflt;
    for (guint i = 0; opts[i].value; i++) {
        g_ptr_array_add(labels, (gpointer)T(opts[i].en, opts[i].ko));
        if (current && !strcmp(current, opts[i].value))
            sel = i;
    }
    g_ptr_array_add(labels, NULL);
    GtkWidget *row = row_choice(list, title, detail, (const char *const *)labels->pdata,
                                sel, cb, data);
    g_ptr_array_free(labels, TRUE);
    g_object_set_data(G_OBJECT(row_control(row)), "lp-opts", (gpointer)opts);
    return row;
}

const char *row_option_value(GObject *dd)
{
    const lp_opt_t *opts = g_object_get_data(dd, "lp-opts");
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (!opts) return NULL;
    for (guint k = 0; opts[k].value; k++)
        if (k == i) return opts[k].value;
    return NULL;
}

/* scale */

typedef void (*scale_fn)(GtkRange *r, gpointer data);

/* The throttle: when the last call was at least SCALE_MIN_MS ago the value
 * goes out at once; otherwise one call is scheduled for when that much
 * time has passed, and it sends whatever the value is by then. So the
 * handler sees at most thirty calls a second and always the final value. */
static void scale_fire(GtkRange *r)
{
    g_object_set_data(G_OBJECT(r), "lp-last", GINT_TO_POINTER(
        (int)(g_get_monotonic_time() / 1000 & 0x3fffffff)));
    scale_fn fn = g_object_get_data(G_OBJECT(r), "lp-cb");
    if (fn) fn(r, g_object_get_data(G_OBJECT(r), "lp-cb-data"));
}

static gboolean scale_trailing(gpointer p)
{
    GtkRange *r = p;
    g_object_set_data(G_OBJECT(r), "lp-timer", NULL);
    scale_fire(r);
    return G_SOURCE_REMOVE;
}

static void scale_tramp(GtkRange *r, gpointer d)
{
    (void)d;
    GtkWidget *val = g_object_get_data(G_OBJECT(r), "lp-readout");
    if (val) {
        char *(*fmt)(double) = g_object_get_data(G_OBJECT(r), "lp-fmt");
        char *t = fmt ? fmt(gtk_range_get_value(r))
                      : g_strdup_printf("%.0f", gtk_range_get_value(r));
        gtk_label_set_text(GTK_LABEL(val), t);
        g_free(t);
    }
    if (lp_quiet) return;
    if (g_object_get_data(G_OBJECT(r), "lp-timer"))
        return;                         /* a call is already scheduled */
    int now = (int)(g_get_monotonic_time() / 1000 & 0x3fffffff);
    int last = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "lp-last"));
    int since = (now - last) & 0x3fffffff;
    if (!g_object_get_data(G_OBJECT(r), "lp-fired") || since >= SCALE_MIN_MS) {
        g_object_set_data(G_OBJECT(r), "lp-fired", GINT_TO_POINTER(1));
        scale_fire(r);
        return;
    }
    guint id = g_timeout_add(SCALE_MIN_MS - since, scale_trailing, r);
    g_object_set_data(G_OBJECT(r), "lp-timer", GUINT_TO_POINTER(id));
}

/* A panel sets its lp-fmt after row_scale has written the first readout,
 * and setting the same value again changes nothing - so the first
 * number used to be the bare default ("100" beside a volume that then
 * read "33%"). The readout is written once more when the panel's code
 * has returned; a slider already gone from its page is left alone. */
static gboolean scale_first_readout(gpointer p)
{
    GtkWidget *sc = p;
    if (gtk_widget_get_root(sc))
        LP_QUIET(scale_tramp(GTK_RANGE(sc), NULL));
    return G_SOURCE_REMOVE;
}

static void scale_gone(GtkWidget *w, gpointer d)
{
    (void)d;
    guint id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "lp-timer"));
    if (id) g_source_remove(id);
    g_object_set_data(G_OBJECT(w), "lp-timer", NULL);
}

GtkWidget *row_scale(GtkWidget *list, const char *title, const char *detail,
                     double min, double max, double step, double value,
                     GCallback cb, gpointer data)
{
    GtkWidget *row = row_shell(title, detail);
    GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                             min, max, step);
    gtk_range_set_value(GTK_RANGE(sc), value);
    gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
    gtk_widget_set_size_request(sc, 260, -1);
    set_control(row, sc);

    /* The number beside the slider, because "somewhere past the middle"
     * is not an answer to "how loud is it". */
    GtkWidget *val = gtk_label_new(NULL);
    gtk_widget_add_css_class(val, "lp-value");
    gtk_widget_add_css_class(val, "lp-readout");
    gtk_label_set_width_chars(GTK_LABEL(val), 5);
    gtk_label_set_xalign(GTK_LABEL(val), 1.0);
    gtk_box_append(GTK_BOX(row_box(row)), val);
    g_object_set_data(G_OBJECT(sc), "lp-readout", val);

    g_object_set_data(G_OBJECT(sc), "lp-cb", (gpointer)cb);
    g_object_set_data(G_OBJECT(sc), "lp-cb-data", data);
    g_signal_connect(sc, "value-changed", G_CALLBACK(scale_tramp), NULL);
    g_signal_connect(sc, "destroy", G_CALLBACK(scale_gone), NULL);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, scale_first_readout, g_object_ref(sc),
                    g_object_unref);
    LP_QUIET(scale_tramp(GTK_RANGE(sc), NULL));
    if (!cb) gtk_widget_set_sensitive(sc, FALSE);
    if (list) row_add(list, row);
    return row;
}

/* segmented */

typedef void (*seg_fn)(GtkWidget *seg, int index, gpointer data);

static void seg_toggled(GtkToggleButton *b, gpointer d)
{
    (void)d;
    if (!gtk_toggle_button_get_active(b)) return;
    GtkWidget *seg = gtk_widget_get_parent(GTK_WIDGET(b));
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "lp-index"));
    g_object_set_data(G_OBJECT(seg), "lp-active", GINT_TO_POINTER(idx + 1));
    if (lp_quiet) return;
    seg_fn fn = g_object_get_data(G_OBJECT(seg), "lp-cb");
    if (fn) fn(seg, idx, g_object_get_data(G_OBJECT(seg), "lp-cb-data"));
}

GtkWidget *row_segmented(GtkWidget *list, const char *title, const char *detail,
                         const char *const *items, int active,
                         GCallback cb, gpointer data)
{
    GtkWidget *row = row_shell(title, detail);
    GtkWidget *seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(seg, "linked");
    gtk_widget_add_css_class(seg, "lp-segmented");
    g_object_set_data(G_OBJECT(seg), "lp-cb", (gpointer)cb);
    g_object_set_data(G_OBJECT(seg), "lp-cb-data", data);

    GtkWidget *first = NULL;
    for (int i = 0; items[i]; i++) {
        GtkWidget *b = gtk_toggle_button_new_with_label(items[i]);
        g_object_set_data(G_OBJECT(b), "lp-index", GINT_TO_POINTER(i));
        if (first)
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(first));
        else
            first = b;
        g_signal_connect(b, "toggled", G_CALLBACK(seg_toggled), NULL);
        gtk_box_append(GTK_BOX(seg), b);
        if (i == active)
            LP_QUIET(gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), TRUE));
    }
    set_control(row, seg);
    if (!cb) gtk_widget_set_sensitive(seg, FALSE);
    if (list) row_add(list, row);
    return row;
}

int segmented_active(GtkWidget *seg)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(seg), "lp-active")) - 1;
}

void row_lock(GtkWidget *row)
{
    GtkWidget *c = row_control(row);
    if (c) gtk_widget_set_sensitive(c, FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    g_object_set_data(G_OBJECT(row), "lp-activate", NULL);

    GtkWidget *lock = gtk_image_new_from_icon_name("system-lock-screen-symbolic");
    gtk_widget_add_css_class(lock, "lp-lock");
    gtk_widget_set_valign(lock, GTK_ALIGN_CENTER);
    gtk_box_insert_child_after(GTK_BOX(row_box(row)), lock,
                               gtk_widget_get_first_child(row_box(row)));

    /* Spec (account-permissions): a locked row says who can, in the same
     * words everywhere, instead of leaving a grey control to be guessed at. */
    row_set_detail(row, T("Only an administrator can change this",
                          "관리자만 바꿀 수 있습니다"));
}

/* ── the sheet ──────────────────────────────────────────────────────
 *
 * The card a dialog is drawn on, and the only thing about a dialog that
 * moves. One child; its snapshot is pushed through an opacity and a scale
 * about the centre that follow a spring from 0 (gone) to 1 (here). The
 * window around it is transparent and undecorated, so what arrives is the
 * card and its shadow, not a grey rectangle that is there at once with a
 * card fading in inside it. Scale and opacity are the two things a frame
 * can change without a relayout (COMMON.md: animate only those), so the
 * whole animation is snapshots of a size that was measured once. */

typedef struct {
    GtkWidget  parent;
    LpSpring   s;
    LpMotion  *m;
    gboolean   closing;
} LpSheet;
typedef struct { GtkWidgetClass parent_class; } LpSheetClass;
G_DEFINE_TYPE(LpSheet, lp_sheet, GTK_TYPE_WIDGET)

static void sheet_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
    LpSheet *sh = (LpSheet *)w;
    GtkWidget *c = gtk_widget_get_first_child(w);
    if (!c) return;
    double p = sh->s.x;
    if (p <= 0.0) return;
    double a = CLAMP(p, 0.0, 1.0);
    gtk_snapshot_push_opacity(snap, a);
    gtk_snapshot_save(snap);
    if (!lp_motion_reduced() && fabs(p - 1.0) > 1e-4) {
        /* Appearing things start at 0.96, never at 0 (feel.md 2-5). */
        float sc = (float)(0.96 + 0.04 * p);
        float cx = gtk_widget_get_width(w) / 2.0f, cy = gtk_widget_get_height(w) / 2.0f;
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
    LpSheet *sh = (LpSheet *)o;
    g_clear_pointer(&sh->m, lp_motion_free);
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(GTK_WIDGET(o))))
        gtk_widget_unparent(c);
    G_OBJECT_CLASS(lp_sheet_parent_class)->dispose(o);
}

static void lp_sheet_class_init(LpSheetClass *k)
{
    G_OBJECT_CLASS(k)->dispose = sheet_dispose;
    GTK_WIDGET_CLASS(k)->snapshot = sheet_snapshot;
    gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(k), GTK_TYPE_BIN_LAYOUT);
    gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(k), "lpsheet");
}

static void lp_sheet_init(LpSheet *sh)
{
    lp_spring_init(&sh->s, LP_SPRING_SHEET, 0.0);
}

/* Once only: the spring coming to rest and the deadline below can both
 * get there. */
static void sheet_destroy_window(GtkRoot *win)
{
    if (!win || g_object_get_data(G_OBJECT(win), "lp-sheet-gone")) return;
    g_object_set_data(G_OBJECT(win), "lp-sheet-gone", GINT_TO_POINTER(1));
    gtk_window_destroy(GTK_WINDOW(win));
}

static void sheet_frame(GtkWidget *w, gpointer d)
{
    (void)d;
    LpSheet *sh = (LpSheet *)w;
    gtk_widget_queue_draw(w);
    if (sh->closing && !sh->s.moving)
        sheet_destroy_window(gtk_widget_get_root(w));
}

static void sheet_open(LpSheet *sh)
{
    if (!sh->m) {
        sh->m = lp_motion_new(GTK_WIDGET(sh), sheet_frame, NULL);
        lp_motion_add(sh->m, &sh->s);
    }
    sh->closing = FALSE;
    lp_spring_set_target(&sh->s, 1.0);
    lp_motion_kick(sh->m);
}

/* The frames that finish a close come from the compositor, and it sends
 * none to a window that is on no screen. Display's "keep these settings?"
 * is centred on the mode being tried; a 5120x2160 tried from 1440x900 and
 * put back left it wholly off the screen, where the closing spring never
 * ticked again, and the modal dialog nobody could see stayed up and took
 * every click Settings got. So a close also has a deadline: well after the
 * spring's 182ms, the window goes whether or not it was ever drawn. */
#define SHEET_CLOSE_DEADLINE_MS 600

static gboolean sheet_close_late(gpointer p)
{
    GtkWidget *win = g_weak_ref_get(p);
    if (win) {
        sheet_destroy_window(GTK_ROOT(win));
        g_object_unref(win);
    }
    return G_SOURCE_REMOVE;
}

static void sheet_weak_free(gpointer p)
{
    g_weak_ref_clear(p);
    g_free(p);
}

/* Out at 0.7x, from wherever it is - a sheet closed while still arriving
 * turns round with the velocity it had (lp-motion's springs). The window
 * is destroyed when the spring comes to rest at 0, or at the deadline
 * above if no frame comes to finish it. */
static void sheet_close(LpSheet *sh)
{
    if (sh->closing) return;
    sh->closing = TRUE;
    GtkRoot *win = gtk_widget_get_root(GTK_WIDGET(sh));
    if (win) {
        g_object_set_data(G_OBJECT(win), "lp-closing", GINT_TO_POINTER(1));
        /* Nothing on a sheet that is leaving can be tapped. */
        gtk_widget_set_sensitive(GTK_WIDGET(sh), FALSE);
    }
    if (!sh->m || !gtk_widget_get_mapped(GTK_WIDGET(sh))) {
        sheet_destroy_window(win);
        return;
    }
    lp_spring_set_target_out(&sh->s, 0.0);
    lp_motion_kick(sh->m);
    if (!sh->s.moving && win) {
        sheet_destroy_window(win);
        return;
    }
    if (win) {
        GWeakRef *ref = g_new0(GWeakRef, 1);
        g_weak_ref_init(ref, win);
        g_timeout_add_full(G_PRIORITY_DEFAULT, SHEET_CLOSE_DEADLINE_MS,
                           sheet_close_late, ref, sheet_weak_free);
    }
}

/* ── dialogs ────────────────────────────────────────────────────────── */

struct lp_dialog {
    GtkWidget   *win;
    GtkWidget   *sheet;
    GtkWidget   *body;
    GtkWidget   *error;
    GtkWidget   *ok;
    GtkWidget   *cancel;
    GtkWidget   *spinner;
    lp_dialog_fn on_ok;
    gpointer     data;
    gboolean     busy;
};

static void dialog_ok_clicked(GtkButton *b, gpointer p)
{
    (void)b;
    lp_dialog_t *d = p;
    if (d->busy) return;
    gtk_widget_set_visible(d->error, FALSE);
    if (d->on_ok)
        d->on_ok(d, d->data);
    else
        lp_dialog_close(d);
}

static void dialog_cancel_clicked(GtkButton *b, gpointer p)
{
    (void)b;
    lp_dialog_close(p);
}

static void dialog_free(gpointer p)
{
    g_free(p);
}

/* Escape, and the compositor asking the window to close, go through the
 * sheet's exit rather than destroying the window under it. */
static gboolean dialog_close_request(GtkWindow *w, gpointer p)
{
    (void)w;
    lp_dialog_t *d = p;
    sheet_close((LpSheet *)d->sheet);
    return TRUE;
}

static void dialog_mapped(GtkWidget *w, gpointer p)
{
    (void)w;
    lp_dialog_t *d = p;
    sheet_open((LpSheet *)d->sheet);
}

lp_dialog_t *lp_dialog_new(const char *title, const char *ok, gboolean danger,
                           lp_dialog_fn on_ok, gpointer data)
{
    lp_dialog_t *d = g_new0(lp_dialog_t, 1);
    d->on_ok = on_ok;
    d->data = data;

    d->win = gtk_window_new();
    gtk_widget_add_css_class(d->win, "lp-settings");
    gtk_widget_add_css_class(d->win, "lp-dialog");
    gtk_window_set_title(GTK_WINDOW(d->win), title);
    gtk_window_set_modal(GTK_WINDOW(d->win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(d->win), lp_window());
    gtk_window_set_default_size(GTK_WINDOW(d->win), 560, -1);
    gtk_window_set_hide_on_close(GTK_WINDOW(d->win), FALSE);
    /* The card draws the frame; see LpSheet. An undecorated window also
     * has to say so to the compositor: GTK tells it "client-side" only
     * for a window with a title bar of its own, and wayfire, whose
     * preferred_decoration_mode is server, took the silence as a request
     * and drew its own bar - a second title and three buttons above the
     * card. An empty title bar, never shown (the window is undecorated),
     * makes GTK say it. */
    GtkWidget *nobar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_visible(nobar, FALSE);
    gtk_window_set_titlebar(GTK_WINDOW(d->win), nobar);
    gtk_window_set_decorated(GTK_WINDOW(d->win), FALSE);
    g_object_set_data_full(G_OBJECT(d->win), "lp-dialog", d, dialog_free);
    g_signal_connect(d->win, "close-request", G_CALLBACK(dialog_close_request), d);

    d->sheet = g_object_new(lp_sheet_get_type(), NULL);
    gtk_widget_add_css_class(d->sheet, "lp-sheet");
    g_signal_connect(d->sheet, "map", G_CALLBACK(dialog_mapped), d);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(outer, "lp-sheet-card");

    GtkWidget *tl = gtk_label_new(title);
    gtk_widget_add_css_class(tl, "lp-dialog-title");
    gtk_label_set_wrap(GTK_LABEL(tl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(tl), 0.0);
    gtk_box_append(GTK_BOX(outer), tl);

    d->body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_append(GTK_BOX(outer), d->body);

    d->error = gtk_label_new(NULL);
    gtk_widget_add_css_class(d->error, "lp-error");
    gtk_label_set_wrap(GTK_LABEL(d->error), TRUE);
    gtk_label_set_xalign(GTK_LABEL(d->error), 0.0);
    gtk_widget_set_visible(d->error, FALSE);
    gtk_box_append(GTK_BOX(outer), d->error);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    gtk_widget_set_margin_top(buttons, 6);

    d->spinner = gtk_spinner_new();
    gtk_widget_set_visible(d->spinner, FALSE);
    gtk_box_append(GTK_BOX(buttons), d->spinner);

    d->cancel = gtk_button_new_with_label(ok ? T("Cancel", "취소") : T("Close", "닫기"));
    g_signal_connect(d->cancel, "clicked", G_CALLBACK(dialog_cancel_clicked), d);
    gtk_box_append(GTK_BOX(buttons), d->cancel);

    if (ok) {
        d->ok = gtk_button_new_with_label(ok);
        gtk_widget_add_css_class(d->ok, danger ? "destructive-action" : "suggested-action");
        g_signal_connect(d->ok, "clicked", G_CALLBACK(dialog_ok_clicked), d);
        gtk_box_append(GTK_BOX(buttons), d->ok);
        gtk_window_set_default_widget(GTK_WINDOW(d->win), d->ok);
    }
    gtk_box_append(GTK_BOX(outer), buttons);
    gtk_widget_set_parent(outer, d->sheet);
    gtk_window_set_child(GTK_WINDOW(d->win), d->sheet);

    /* Escape leaves every dialog. Spec (reset): a confirmation that cannot
     * be left without answering is a trap, not a confirmation. */
    GtkEventController *sc = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0),
                         gtk_named_action_new("window.close")));
    gtk_widget_add_controller(d->win, sc);
    return d;
}

int lp_dialog_fit(int want, int chrome)
{
    GdkDisplay *dpy = gdk_display_get_default();
    GtkNative *nat = lp_window() ? GTK_NATIVE(lp_window()) : NULL;
    GdkSurface *surf = nat ? gtk_native_get_surface(nat) : NULL;
    GdkMonitor *mon = surf ? gdk_display_get_monitor_at_surface(dpy, surf) : NULL;
    if (!mon) {
        GListModel *ms = gdk_display_get_monitors(dpy);
        mon = ms && g_list_model_get_n_items(ms) ? g_list_model_get_item(ms, 0) : NULL;
        if (mon) g_object_unref(mon);          /* the list keeps it */
    }
    if (!mon) return want;
    GdkRectangle geo;
    gdk_monitor_get_geometry(mon, &geo);
    /* The top bar (36) and the dock's room (96) cover the screen's edges
     * above every window, dialogs included; a dialog taller than what is
     * between them had its buttons under the dock. */
    int room = geo.height - 36 - 96 - 16 - chrome;
    return CLAMP(room, MIN(160, want), want);
}

GtkWidget *lp_dialog_body(lp_dialog_t *d)      { return d->body; }
GtkWidget *lp_dialog_window(lp_dialog_t *d)    { return d->win; }
GtkWidget *lp_dialog_ok_button(lp_dialog_t *d) { return d->ok; }

void lp_dialog_text(lp_dialog_t *d, const char *text, const char *css)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 52);
    if (css) gtk_widget_add_css_class(l, css);
    gtk_box_append(GTK_BOX(d->body), l);
}

static void osk_show(GtkButton *b, gpointer entry)
{
    (void)b;
    /* Focus first, so the keyboard types into this field and not into
     * whatever had focus before the button was tapped. */
    gtk_widget_grab_focus(entry);
    static const char *const v[] = { "lp-osk", "show", NULL };
    lp_spawn_bg(v);
}

GtkWidget *lp_dialog_entry(lp_dialog_t *d, const char *label,
                           const char *initial, gboolean password)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_widget_add_css_class(l, "lp-field-label");
    gtk_box_append(GTK_BOX(d->body), l);

    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *e;
    if (password) {
        e = gtk_password_entry_new();
        gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(e), TRUE);
        g_object_set(e, "activates-default", TRUE, NULL);
    } else {
        e = gtk_entry_new();
        gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    }
    if (initial)
        gtk_editable_set_text(GTK_EDITABLE(e), initial);
    gtk_widget_set_hexpand(e, TRUE);
    g_object_set_data_full(G_OBJECT(e), "lp-title", g_strdup(label), g_free);
    gtk_box_append(GTK_BOX(h), e);

    GtkWidget *kb = gtk_button_new_from_icon_name("input-keyboard-symbolic");
    gtk_widget_set_tooltip_text(kb, T("Show the on-screen keyboard",
                                      "화상 키보드 보이기"));
    gtk_widget_add_css_class(kb, "lp-osk-button");
    g_signal_connect(kb, "clicked", G_CALLBACK(osk_show), e);
    gtk_box_append(GTK_BOX(h), kb);

    gtk_box_append(GTK_BOX(d->body), h);
    if (!g_object_get_data(G_OBJECT(d->win), "lp-first-entry")) {
        g_object_set_data(G_OBJECT(d->win), "lp-first-entry", e);
    }
    return e;
}

void lp_dialog_error(lp_dialog_t *d, const char *msg)
{
    gtk_label_set_text(GTK_LABEL(d->error), msg);
    gtk_widget_set_visible(d->error, msg && *msg);
}

void lp_dialog_busy(lp_dialog_t *d, gboolean busy)
{
    if (!d) return;
    d->busy = busy;
    gtk_widget_set_visible(d->spinner, busy);
    gtk_spinner_set_spinning(GTK_SPINNER(d->spinner), busy);
    if (d->ok) gtk_widget_set_sensitive(d->ok, !busy);
}

void lp_dialog_present(lp_dialog_t *d)
{
    gtk_window_present(GTK_WINDOW(d->win));
    GtkWidget *e = g_object_get_data(G_OBJECT(d->win), "lp-first-entry");
    if (e) gtk_widget_grab_focus(e);
}

void lp_dialog_close(lp_dialog_t *d)
{
    if (d && d->win)
        sheet_close((LpSheet *)d->sheet);
}

void lp_dialog_set_data(lp_dialog_t *d, const char *key, gpointer v,
                        GDestroyNotify free_fn)
{
    if (!v)
        g_object_steal_data(G_OBJECT(d->win), key);
    else
        g_object_set_data_full(G_OBJECT(d->win), key, v, free_fn);
}

gpointer lp_dialog_get_data(lp_dialog_t *d, const char *key)
{
    return g_object_get_data(G_OBJECT(d->win), key);
}
