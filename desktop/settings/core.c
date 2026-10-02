/*
 * core.c - the Settings window: sidebar, search, the page on the right,
 * the banner that reports what an action did, and the stylesheet.
 *
 * Spec 2-1's layout: a sidebar of at least 158px with the panel names,
 * and on the right the chosen panel as a title, a subtitle carrying the
 * current state, and rows. Search sits at the top of the sidebar because
 * it is how people who do not know which panel holds "night light" find
 * it, and on a touch screen a search behind a button is a search nobody
 * opens.
 *
 * ── Pages are built when chosen, and again every time ──
 *
 * Every panel reads the machine (lp-net, wpctl, /sys) when it is built.
 * Building all sixteen at start-up would spawn a dozen processes before
 * the window appears; building on selection means a panel always shows
 * what is true now, not what was true when the window opened.
 *
 * ── Why the app carries its own stylesheet ──
 *
 * The session runs GTK_THEME=LP, and that theme (desktop/theme) restyles
 * windows, header bars and sidebars to the owner's mockup but not the
 * controls - under it a GtkSwitch is drawn as GTK's bare "I O" fallback
 * and a slider as a line. So this app styles every control it uses, at a
 * priority above the theme's user stylesheet, and sizes them for a finger
 * (48 logical px). The colours follow ~/.config/lp/appearance.conf, which
 * is also why the Appearance panel's dark/light and accent choices can be
 * seen working in this window the moment they are made. Dark or light is
 * read from ~/.config/lp/style, the file the top bar's quick settings
 * write too, so the two switches are one setting and cannot disagree.
 *
 * ── Motion ──
 *
 * Pages slide: a GtkStack whose transition is chosen per change - the page
 * below in the sidebar comes in from the right, the page above from the
 * left - so the direction says where you went (260ms, the stack duration
 * motion.css gives). The sidebar's highlight is not the row's :selected
 * background but a rounded rectangle drawn under the rows and carried
 * from the old row to the new one on the "sheet" spring, so it arrives
 * with the page. It is interruptible: a second tap while it moves bends
 * it towards the new row with the velocity it had.
 *
 * desktop/theme/motion.css (main's, generated from feel.md) is compiled
 * into the binary with .incbin and loaded under this app's own rules, so
 * the switch, press and focus transitions are the same curves the shell
 * uses without this app keeping a second copy of the numbers.
 *
 * With reduced motion (Accessibility), pages crossfade in 90ms instead of
 * sliding and the highlight jumps; GTK's own transitions are off because
 * the same switch turns gtk-enable-animations off.
 */
#include "core.h"
#include "lp-motion.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* motion.css, as a NUL-terminated string in .rodata. The path is relative
 * to desktop/settings, where the Makefile runs the compiler. */
__asm__(".pushsection .rodata\n"
        ".global lp_motion_css\n"
        ".type lp_motion_css, @object\n"
        "lp_motion_css:\n"
        ".incbin \"../theme/motion.css\"\n"
        ".byte 0\n"
        ".popsection\n");
extern const char lp_motion_css[];

#define SIDEBAR_WIDTH 158      /* spec 2-1; the labels may make it wider */

typedef struct {
    GtkApplication *gapp;
    GtkWidget *win;
    GtkWidget *sidebar;        /* list of panels */
    GtkWidget *side_hl;        /* the highlight drawn under the rows */
    GtkWidget *results;        /* list of search results */
    GtkWidget *side_stack;
    GtkWidget *side_scroller;  /* the panel list's scrolled window */
    GtkWidget *search;
    GtkWidget *stack;          /* one scrolled window per page shown */
    GtkWidget *scroller;       /* the current page's scrolled window */
    GtkWidget *toast;
    GtkWidget *toast_label;
    guint      toast_timer;
    guint      serial;
    int        current;
    gboolean   selecting;
    GtkCssProvider *css;
    GtkCssProvider *motion;
    GtkCssProvider *palette;
    LpSpring   hl_y, hl_h;
    LpMotion  *hl_motion;
    gboolean   hl_placed;
    guint      trace_tick;
    gint64     trace_last;
} app_t;

static app_t A;
static char *start_panel;
static char *start_row;

GtkWindow *lp_window(void) { return GTK_WINDOW(A.win); }

/* ── stylesheet ─────────────────────────────────────────────────────── */

static const char *CSS =
    /* Text sizes are in points, never pixels. Settings > Text size is GTK's
     * gtk-xft-dpi (access.c), and GTK turns pt into pixels through it while
     * a px size stays what it says: with every size here in px, choosing
     * Large changed nothing in this window - the window the choice is made
     * in - nor after a restart, and the setting looked as if it had not
     * been kept. 0.75pt is 1px at 96 dpi, so at Default nothing moves. */
    /* Window buttons: the one look every LP app shares (see the theme's
     * gtk.css) - this file's own button rules would otherwise make
     * close a wide padded block. */
    ".lp-settings headerbar windowcontrols > button { min-width: 28px; min-height: 28px; margin: 0 2px;"
    " padding: 0; border: none; border-radius: 8px; box-shadow: none;"
    " background: transparent; color: #9fb3c4; }"
    ".lp-settings headerbar windowcontrols > button:hover { background: alpha(#eaf2f8, 0.12); color: #eaf2f8; }"
    ".lp-settings headerbar windowcontrols > button.close:hover { background: #f28c28; color: #ffffff; }"
    ".lp-settings headerbar windowcontrols.end:not(.empty) { padding-left: 0; background-image: none; }"
    ".lp-settings { font-family: Pretendard, 'Nanum Gothic', sans-serif; font-size: 10.5pt;"
    "  background-color: @lp_bg; color: @lp_fg; }\n"
    ".lp-settings headerbar { background: @lp_header; color: @lp_fg; min-height: 48px;"
    "  box-shadow: none; border-bottom: 1px solid @lp_border; }\n"
    ".lp-settings headerbar button { min-width: 44px; min-height: 40px; }\n"
    /* The theme colours the title label itself (its light-on-dark text),
     * so the header's colour did not reach it: in the light style the
     * title was white on a light bar. */
    ".lp-settings headerbar .title { color: @lp_fg; }\n"
    ".lp-settings .lp-sidebar { background: @lp_side; border-right: 1px solid @lp_border; }\n"
    ".lp-settings .lp-sidebar list { background: transparent; }\n"
    ".lp-settings .lp-sidebar row { min-height: 48px; border-radius: 8px; margin: 1px 8px; padding: 0 6px;"
    "  color: @lp_dim; }\n"
    ".lp-settings .lp-sidebar row:hover { background: alpha(@lp_fg, 0.06); }\n"
    /* :selected is transparent: the highlight is the spring-driven
     * rectangle under the rows (side_hl), which is what moves. */
    ".lp-settings .lp-sidebar row:selected { background: transparent; color: @lp_fg; }\n"
    ".lp-settings .lp-sidebar row:selected image { color: @lp_accent; }\n"
    ".lp-settings .lp-side-hl { color: @lp_selected; }\n"
    ".lp-settings .lp-sidebar .lp-result-panel { font-size: 9pt; color: @lp_dim; }\n"
    ".lp-settings .lp-search { min-height: 44px; margin: 10px 8px 6px 8px; border-radius: 8px;"
    "  background: @lp_card; color: @lp_fg; border: 1px solid @lp_border; }\n"
    ".lp-settings .lp-search > text { color: @lp_fg; }\n"
    ".lp-settings .lp-search > image { margin-right: 8px; color: @lp_dim; }\n"
    ".lp-settings .lp-title { font-size: 18pt; font-weight: 700; color: @lp_fg; }\n"
    ".lp-settings .lp-subtitle { font-size: 10.5pt; color: @lp_dim; }\n"
    ".lp-settings .lp-heading { font-size: 9.75pt; font-weight: 700; color: @lp_dim; }\n"
    ".lp-settings .lp-note { font-size: 9.75pt; color: @lp_dim; }\n"
    ".lp-settings .lp-warn { color: @lp_amber; }\n"
    ".lp-settings .lp-error { color: @lp_red; }\n"
    ".lp-settings .lp-group { background: @lp_card; border-radius: 10px; border: 1px solid @lp_border; }\n"
    /* The 56px is on the box inside the revealer, not the row, so a row
     * can close to nothing when it leaves a list. */
    ".lp-settings .lp-group > row { min-height: 0; padding: 0; border-bottom: 1px solid @lp_line;"
    "  background: transparent; }\n"
    ".lp-settings .lp-row-box { min-height: 44px; }\n"
    ".lp-settings .lp-group > row:last-child { border-bottom: none; }\n"
    ".lp-settings .lp-group > row:first-child { border-top-left-radius: 10px; border-top-right-radius: 10px; }\n"
    ".lp-settings .lp-group > row:last-child { border-bottom-left-radius: 10px; border-bottom-right-radius: 10px; }\n"
    ".lp-settings .lp-group > row.activatable:hover { background: alpha(@lp_fg, 0.04); }\n"
    ".lp-settings .lp-group > row.activatable:active { background: alpha(@lp_fg, 0.09); }\n"
    ".lp-settings .lp-group > row.lp-flash { background: alpha(@lp_accent, 0.25); }\n"
    ".lp-settings .lp-row-title { color: @lp_fg; }\n"
    ".lp-settings .lp-detail { font-size: 9.375pt; color: @lp_dim; }\n"
    ".lp-settings .lp-value { color: @lp_dim; }\n"
    ".lp-settings .lp-readout { font-feature-settings: 'tnum'; }\n"
    ".lp-settings .lp-chevron, .lp-settings .lp-lock { color: @lp_dim; }\n"
    ".lp-settings button { min-height: 48px; min-width: 48px; padding: 0 18px; border-radius: 8px;"
    "  background: @lp_button; color: @lp_fg; border: 1px solid @lp_border; box-shadow: none;"
    "  background-image: none; text-shadow: none; }\n"
    ".lp-settings button:hover { background: shade(@lp_button, 1.12); }\n"
    ".lp-settings button:active, .lp-settings button:checked { background: @lp_selected; }\n"
    ".lp-settings button:disabled { opacity: 0.45; }\n"
    ".lp-settings button.image-button { padding: 0; }\n"
    ".lp-settings button.suggested-action { background: @lp_accent; color: @lp_on_accent;"
    "  border-color: @lp_accent; font-weight: 700; }\n"
    ".lp-settings button.destructive-action { background: @lp_red; color: #ffffff;"
    "  border-color: @lp_red; font-weight: 700; }\n"
    ".lp-settings .lp-segmented > button { border-radius: 0; margin: 0; }\n"
    ".lp-settings .lp-segmented > button:first-child { border-radius: 8px 0 0 8px; }\n"
    ".lp-settings .lp-segmented > button:last-child { border-radius: 0 8px 8px 0; }\n"
    ".lp-settings .lp-segmented > button:checked { background: @lp_accent; color: @lp_on_accent;"
    "  border-color: @lp_accent; }\n"
    ".lp-settings switch { min-width: 60px; min-height: 34px; border-radius: 17px;"
    "  background: @lp_track; border: 1px solid @lp_border; color: transparent; }\n"
    ".lp-settings switch:checked { background: @lp_accent; border-color: @lp_accent; }\n"
    ".lp-settings switch slider { min-width: 28px; min-height: 28px; margin: 2px;"
    "  border-radius: 14px; background: #ffffff; border: none; box-shadow: 0 1px 2px rgba(0,0,0,0.3); }\n"
    ".lp-settings switch image { color: transparent; }\n"
    ".lp-settings switch:disabled { opacity: 0.45; }\n"
    /* A slider that cannot be moved (no backlight to dim) looked exactly
     * like one that can. */
    ".lp-settings scale:disabled { opacity: 0.45; }\n"
    ".lp-settings scale { min-height: 48px; padding: 0 12px; }\n"
    ".lp-settings scale trough { min-height: 8px; border-radius: 4px; background: @lp_track; border: none; }\n"
    ".lp-settings scale highlight { min-height: 8px; border-radius: 4px; background: @lp_accent; }\n"
    ".lp-settings scale slider { min-width: 28px; min-height: 28px; margin: -10px;"
    "  border-radius: 14px; background: #ffffff; border: none; box-shadow: 0 1px 3px rgba(0,0,0,0.35); }\n"
    ".lp-settings scale marks, .lp-settings scale mark indicator { color: @lp_dim; }\n"
    ".lp-settings dropdown > button { min-width: 160px; }\n"
    ".lp-settings popover > contents { background: @lp_card; color: @lp_fg; border: 1px solid @lp_border;"
    "  border-radius: 10px; padding: 6px; }\n"
    ".lp-settings popover listview > row, .lp-settings popover list > row { min-height: 44px;"
    "  padding: 0 10px; border-radius: 6px; color: @lp_fg; }\n"
    ".lp-settings popover listview > row:selected, .lp-settings popover listview > row:hover"
    "  { background: @lp_selected; }\n"
    ".lp-settings popover listview { background: transparent; }\n"
    ".lp-settings entry, .lp-settings spinbutton, .lp-settings passwordentry {"
    "  min-height: 48px; border-radius: 8px; background: @lp_field; color: @lp_fg;"
    "  border: 1px solid @lp_border; padding: 0 12px; box-shadow: none; }\n"
    ".lp-settings entry:focus-within, .lp-settings passwordentry:focus-within {"
    "  border-color: @lp_accent; }\n"
    ".lp-settings spinbutton > button { min-width: 44px; border-radius: 0; border: none; }\n"
    ".lp-settings checkbutton { min-height: 44px; }\n"
    ".lp-settings check { min-width: 24px; min-height: 24px; border-radius: 6px;"
    "  background: @lp_field; border: 1px solid @lp_border; }\n"
    ".lp-settings check:checked { background: @lp_accent; color: @lp_on_accent; }\n"
    ".lp-settings levelbar trough { min-height: 8px; border-radius: 4px; background: @lp_track; }\n"
    ".lp-settings levelbar block.filled { background: @lp_accent; border-radius: 4px; }\n"
    ".lp-settings levelbar block.high { background: @lp_accent; }\n"
    ".lp-settings levelbar.lp-full block.filled { background: @lp_amber; }\n"
    ".lp-settings levelbar block.empty { background: transparent; }\n"
    ".lp-settings calendar { background: @lp_field; color: @lp_fg; border-radius: 8px;"
    "  border: 1px solid @lp_border; }\n"
    ".lp-settings calendar > grid > label.day-number { min-width: 40px; min-height: 40px; }\n"
    ".lp-settings calendar > grid > label:selected { background: @lp_accent; color: @lp_on_accent;"
    "  border-radius: 20px; }\n"
    ".lp-settings scrollbar slider { min-width: 8px; background: alpha(@lp_fg, 0.3); border-radius: 4px; }\n"
    /* The sidebar's bar is always there (core.c, build_window); only the
     * slider should show, not a trough down the sidebar's edge. */
    ".lp-settings .lp-sidebar scrollbar { background: transparent; border: none; }\n"
    ".lp-settings .lp-toast { background: @lp_card; color: @lp_fg; border: 1px solid @lp_border;"
    "  border-radius: 10px; padding: 12px 18px; margin: 12px; box-shadow: 0 6px 20px rgba(0,0,0,0.35); }\n"
    ".lp-settings .lp-toast.lp-toast-error { border-color: @lp_red; }\n"
    ".lp-settings .lp-toast.lp-toast-error image { color: @lp_red; }\n"
    ".lp-settings .lp-toast image { color: @lp_accent; }\n"
    ".lp-settings .lp-dialog-title { font-size: 13.5pt; font-weight: 700; color: @lp_fg; }\n"
    /* Dialogs: the window is transparent and the card is the sheet
     * (ui.c, LpSheet). The margin is room for the shadow inside the
     * window's own surface. */
    "window.lp-dialog.lp-settings { background: transparent; }\n"
    /* ui.c gives dialogs an empty title bar so GTK tells the compositor
     * they draw their own frame; that also makes them .csd, which the
     * theme answers with a window shadow and outline around the whole
     * transparent window. The card has its own. */
    "window.lp-dialog.lp-settings.csd { box-shadow: none; border-radius: 0; }\n"
    ".lp-settings .lp-sheet { margin: 28px; }\n"
    ".lp-settings .lp-sheet-card { background: @lp_card; color: @lp_fg; border-radius: 14px;"
    "  border: 1px solid @lp_border; padding: 22px 24px 20px 24px;"
    "  box-shadow: 0 1px 2px rgba(0,0,0,0.08), 0 8px 24px rgba(0,0,0,0.35); }\n"
    /* Pressed controls move (feel.md 3-4): scale 0.97 on touch-down with no
     * transition in, and back on the press spring's exit curve (91ms, from
     * motion.css). Rows are wide, so they give less. */
    ".lp-settings button { transition: background-color 91ms cubic-bezier(0.263, 0.487, 0.037, 1.137),"
    "  color 130ms cubic-bezier(0.263, 0.487, 0.037, 1.137),"
    "  transform 91ms cubic-bezier(0.263, 0.487, 0.037, 1.137); }\n"
    ".lp-settings button:active { transform: scale(0.97); transition: none; }\n"
    ".lp-settings .lp-group > row.activatable { transition: background-color 91ms"
    "  cubic-bezier(0.263, 0.487, 0.037, 1.137), transform 91ms cubic-bezier(0.263, 0.487, 0.037, 1.137); }\n"
    ".lp-settings .lp-group > row.activatable:active { transform: scale(0.985); transition: none; }\n"
    ".lp-settings .lp-field-label { color: @lp_dim; font-size: 9.75pt; }\n"
    ".lp-settings .lp-swatch { min-width: 44px; min-height: 44px; padding: 0; border-radius: 22px; }\n"
    ".lp-settings .lp-swatch:checked { border: 3px solid @lp_fg; }\n"
    ".lp-settings .lp-wallpaper { padding: 4px; border-radius: 10px; }\n"
    ".lp-settings .lp-wallpaper:checked { border: 3px solid @lp_accent; background: transparent; }\n"
    ".lp-settings .lp-style-card { padding: 8px; border-radius: 12px; }\n"
    ".lp-settings .lp-style-card:checked { border: 3px solid @lp_accent; background: transparent; }\n"
    ".lp-settings .lp-big { font-size: 15pt; font-weight: 700; color: @lp_fg; }\n"
    ".lp-settings .lp-mono { font-family: 'D2Coding', monospace; }\n"
    ".lp-settings .lp-signal { color: @lp_fg; }\n"
    ".lp-settings .lp-zone { border: 1px solid alpha(@lp_red, 0.6); border-radius: 10px; padding: 16px; }\n"
    ".lp-settings .lp-zone-title { font-weight: 700; color: @lp_red; }\n"
    ".lp-settings .lp-key { background: @lp_button; border: 1px solid @lp_border; border-radius: 6px;"
    "  padding: 2px 8px; font-family: 'D2Coding', monospace; font-size: 9.75pt; }\n"
    ".lp-settings .lp-arrange { background: @lp_field; border-radius: 10px; border: 1px solid @lp_border; }\n";

typedef struct {
    const char *bg, *header, *side, *card, *field, *button, *border, *line,
               *fg, *dim, *selected, *track;
} palette_t;

/* The mockup's greys, from desktop/theme/gtk-4.0/gtk.css, for dark; a
 * light set with the same steps between surfaces for light. */
static const palette_t DARK  = { "#232323", "#2c2c2c", "#1e1e1e", "#2c2c2c",
                                 "#1a1a1a", "#363636", "#3a3a3a", "#333333",
                                 "#e8e8e8", "#a8a8a8", "#3d3d3d", "#4a4a4a" };
static const palette_t LIGHT = { "#f6f5f4", "#ebebeb", "#ebebeb", "#ffffff",
                                 "#ffffff", "#f0f0f0", "#d4d4d4", "#e6e6e6",
                                 "#1e1e1e", "#5e5e5e", "#dcdcdc", "#cfcfcf" };

/* Picks black or white text for a filled accent, by luminance. A yellow
 * accent with white text is the classic unreadable toggle. */
static const char *on_colour(const char *hex)
{
    GdkRGBA c;
    if (!gdk_rgba_parse(&c, hex)) return "#ffffff";
    double l = 0.2126 * c.red + 0.7152 * c.green + 0.0722 * c.blue;
    return l > 0.6 ? "#1e1e1e" : "#ffffff";
}

/* ~/.config/lp/style is "dark" or "light", shared with the shell
 * (desktop/common/lp-shell.c reads and writes the same file). */
gboolean lp_style_is_light(void)
{
    char *p = lp_config_path("style");
    char *s = lp_slurp(p);
    g_free(p);
    gboolean light = s && g_str_has_prefix(g_strstrip(s), "light");
    g_free(s);
    return light;
}

void lp_apply_palette(void)
{
    char *conf = lp_config_path("appearance.conf");
    char *style = g_strdup(lp_style_is_light() ? "light" : "dark");
    char *accent = kv_get(conf, "accent");
    g_free(conf);

    GdkRGBA probe;
    if (!accent || !gdk_rgba_parse(&probe, accent)) {
        g_free(accent);
        accent = g_strdup("#e95420");
    }
    const palette_t *p = (style && !strcmp(style, "light")) ? &LIGHT : &DARK;
    char *css = g_strdup_printf(
        "@define-color lp_bg %s; @define-color lp_header %s; @define-color lp_side %s;"
        "@define-color lp_card %s; @define-color lp_field %s; @define-color lp_button %s;"
        "@define-color lp_border %s; @define-color lp_line %s; @define-color lp_fg %s;"
        "@define-color lp_dim %s; @define-color lp_selected %s; @define-color lp_track %s;"
        "@define-color lp_accent %s; @define-color lp_on_accent %s;"
        "@define-color lp_amber #f0a020; @define-color lp_red #e5484d;",
        p->bg, p->header, p->side, p->card, p->field, p->button, p->border,
        p->line, p->fg, p->dim, p->selected, p->track, accent, on_colour(accent));
    gtk_css_provider_load_from_data(A.palette, css, -1);
    g_free(css);

    /* Built-in GTK widgets we did not restyle (the file chooser) follow the
     * base theme's variant. Under GTK_THEME=LP this has no effect - the
     * environment variable wins - which is the theme's to change. */
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme",
                 p == &DARK, NULL);
    g_free(style);
    g_free(accent);
}

static void css_error(GtkCssProvider *p, GtkCssSection *sec, const GError *e, gpointer d)
{
    (void)p; (void)sec;
    g_printerr("lp-settings: stylesheet (%s): %s\n", (const char *)d, e->message);
}

static void load_css(void)
{
    GdkDisplay *dpy = gdk_display_get_default();
    A.palette = gtk_css_provider_new();
    A.motion = gtk_css_provider_new();
    A.css = gtk_css_provider_new();
    g_signal_connect(A.motion, "parsing-error", G_CALLBACK(css_error), "motion.css");
    g_signal_connect(A.css, "parsing-error", G_CALLBACK(css_error), "core.c");
    lp_apply_palette();
    gtk_css_provider_load_from_data(A.motion, lp_motion_css, -1);
    gtk_css_provider_load_from_data(A.css, CSS, -1);
    /* Above GTK_STYLE_PROVIDER_PRIORITY_USER (800), where the theme's
     * ~/.config/gtk-4.0/gtk.css sits: at equal priority the theme's
     * 36px row height would win over the 48px a finger needs. The palette
     * must come first so the rules can resolve its names; motion.css sits
     * between, so this file's own rules win where both say something. */
    gtk_style_context_add_provider_for_display(dpy, GTK_STYLE_PROVIDER(A.palette), 810);
    gtk_style_context_add_provider_for_display(dpy, GTK_STYLE_PROVIDER(A.motion), 815);
    gtk_style_context_add_provider_for_display(dpy, GTK_STYLE_PROVIDER(A.css), 820);
}

/* ── the banner ─────────────────────────────────────────────────────── */

static gboolean toast_hide(gpointer p)
{
    (void)p;
    A.toast_timer = 0;
    gtk_revealer_set_reveal_child(GTK_REVEALER(A.toast), FALSE);
    return G_SOURCE_REMOVE;
}

void lp_toast(gboolean err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    /* Also on stderr: the test driver's log and a person running this from
     * a terminal see the same sentence the banner showed. */
    g_printerr("lp-settings: %s%s\n", err ? "error: " : "", msg);
    if (!A.toast) { g_free(msg); return; }

    gtk_label_set_text(GTK_LABEL(A.toast_label), msg);
    GtkWidget *box = gtk_revealer_get_child(GTK_REVEALER(A.toast));
    GtkWidget *icon = gtk_widget_get_first_child(box);
    gtk_image_set_from_icon_name(GTK_IMAGE(icon), err ? "dialog-warning-symbolic"
                                                      : "emblem-ok-symbolic");
    if (err) gtk_widget_add_css_class(box, "lp-toast-error");
    else     gtk_widget_remove_css_class(box, "lp-toast-error");
    gtk_revealer_set_reveal_child(GTK_REVEALER(A.toast), TRUE);
    if (A.toast_timer) g_source_remove(A.toast_timer);
    /* Failures stay longer: they are read, successes are glanced at. */
    A.toast_timer = g_timeout_add(err ? 9000 : 4000, toast_hide, NULL);
    g_free(msg);
}

/* ── the sidebar highlight ──────────────────────────────────────────── */

static void hl_frame(GtkWidget *w, gpointer d)
{
    (void)d;
    gtk_widget_queue_draw(w);
}

/* Where the selected row is, in the sidebar list's coordinates. */
static gboolean hl_target(double *x, double *y, double *w, double *h)
{
    GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(A.sidebar));
    graphene_rect_t b;
    if (!r || !gtk_widget_compute_bounds(GTK_WIDGET(r), A.sidebar, &b))
        return FALSE;
    *x = b.origin.x; *y = b.origin.y;
    *w = b.size.width; *h = b.size.height;
    return TRUE;
}

static void hl_draw(GtkDrawingArea *a, cairo_t *cr, int W, int H, gpointer d)
{
    (void)W; (void)H; (void)d;
    double x, y, w, h;
    if (!hl_target(&x, &y, &w, &h))
        return;
    if (!A.hl_placed) {
        lp_spring_jump(&A.hl_y, y);
        lp_spring_jump(&A.hl_h, h);
        A.hl_placed = TRUE;
    } else if (!A.hl_y.moving && !A.hl_h.moving) {
        /* At rest it is wherever the row is now - after a resize, or a
         * row growing a second line. */
        lp_spring_jump(&A.hl_y, y);
        lp_spring_jump(&A.hl_h, h);
    }
    GdkRGBA c;
    gtk_style_context_get_color(gtk_widget_get_style_context(GTK_WIDGET(a)), &c);
    double yy = A.hl_y.x, hh = A.hl_h.x, r = 8;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, yy + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, yy + hh - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, yy + hh - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, yy + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
    gdk_cairo_set_source_rgba(cr, &c);
    cairo_fill(cr);
}

static void hl_move(void)
{
    double x, y, w, h;
    if (!A.side_hl || !hl_target(&x, &y, &w, &h))
        return;
    if (!A.hl_placed || lp_motion_reduced()) {
        lp_spring_jump(&A.hl_y, y);
        lp_spring_jump(&A.hl_h, h);
        A.hl_placed = TRUE;
        gtk_widget_queue_draw(A.side_hl);
        return;
    }
    lp_spring_set_target(&A.hl_y, y);
    lp_spring_set_target(&A.hl_h, h);
    lp_motion_kick(A.hl_motion);
}

/* ── frame times, for the report ─────────────────────────────────────────
 *
 * GtkStack's slide is GTK's own animation and prints nothing, so while one
 * runs and LP_MOTION_TRACE is set, a tick callback on the stack prints the
 * gap between frames in the same format lp-motion uses. */

static gboolean trace_on(void)
{
    const char *e = g_getenv("LP_MOTION_TRACE");
    return e && *e && strcmp(e, "0") != 0;
}

static gboolean trace_tick(GtkWidget *w, GdkFrameClock *clock, gpointer d)
{
    (void)w; (void)d;
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    if (A.trace_last)
        fprintf(stderr, "lp-settings: stack frame dt_ms=%.1f\n", (now - A.trace_last) / 1000.0);
    A.trace_last = now;
    if (!gtk_stack_get_transition_running(GTK_STACK(A.stack))) {
        A.trace_tick = 0;
        A.trace_last = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* ── showing panels ─────────────────────────────────────────────────── */

static int panel_count(void)
{
    int n = 0;
    while (lp_panels[n]) n++;
    return n;
}

static GtkWidget *find_row(GtkWidget *w, const char *title)
{
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
        const char *t = g_object_get_data(G_OBJECT(c), "lp-title");
        if (GTK_IS_LIST_BOX_ROW(c) && t && !g_ascii_strcasecmp(t, title))
            return c;
        GtkWidget *r = find_row(c, title);
        if (r) return r;
    }
    return NULL;
}

static gboolean unflash(gpointer p)
{
    gtk_widget_remove_css_class(GTK_WIDGET(p), "lp-flash");
    g_object_unref(p);
    return G_SOURCE_REMOVE;
}

static gboolean scroll_to_row(gpointer p)
{
    GtkWidget *row = p;
    GtkWidget *page = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(A.scroller));
    graphene_point_t in = { 0, 0 }, out;
    if (page && gtk_widget_compute_point(row, page, &in, &out)) {
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(A.scroller));
        gtk_adjustment_set_value(adj, MAX(0, out.y - 80));
    }
    gtk_widget_add_css_class(row, "lp-flash");
    g_timeout_add(1600, unflash, row);   /* takes the ref */
    return G_SOURCE_REMOVE;
}

/* Pages that slid out are dropped once the slide is over, so the stack
 * holds one page at rest and two only while moving. */
static void on_transition(GObject *o, GParamSpec *ps, gpointer d)
{
    (void)ps; (void)d;
    if (gtk_stack_get_transition_running(GTK_STACK(o))) {
        if (trace_on() && !A.trace_tick)
            A.trace_tick = gtk_widget_add_tick_callback(A.stack, trace_tick, NULL, NULL);
        return;
    }
    GtkWidget *keep = gtk_stack_get_visible_child(GTK_STACK(o));
    GtkWidget *c = gtk_widget_get_first_child(GTK_WIDGET(o));
    while (c) {
        GtkWidget *next = gtk_widget_get_next_sibling(c);
        if (c != keep)
            gtk_stack_remove(GTK_STACK(o), c);
        c = next;
    }
}

typedef struct { double y; } restore_t;

/* A new page's adjustment has no range until it is laid out; the old
 * scroll position is put back the first time the range can hold it. */
static void restore_scroll(GtkAdjustment *adj, gpointer p)
{
    restore_t *r = p;
    if (gtk_adjustment_get_upper(adj) <= 0)
        return;
    g_signal_handlers_disconnect_by_func(adj, restore_scroll, p);
    gtk_adjustment_set_value(adj, r->y);
    g_free(r);
}

/* The chosen panel's row in view: `lp-settings users` and a search hit
 * choose a panel that may be below the fold of a short window, and a
 * selection nobody can see looks like no selection. */
static gboolean side_reveal(gpointer p)
{
    (void)p;
    GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(A.sidebar));
    graphene_rect_t b;
    if (!r || !A.side_scroller ||
        !gtk_widget_compute_bounds(GTK_WIDGET(r), A.sidebar, &b))
        return G_SOURCE_REMOVE;
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(A.side_scroller));
    double top = gtk_adjustment_get_value(adj), page = gtk_adjustment_get_page_size(adj);
    if (page <= 0)
        return G_SOURCE_REMOVE;             /* not laid out yet */
    if (b.origin.y < top)
        gtk_adjustment_set_value(adj, b.origin.y);
    else if (b.origin.y + b.size.height > top + page)
        gtk_adjustment_set_value(adj, b.origin.y + b.size.height - page);
    return G_SOURCE_REMOVE;
}

typedef enum { MOVE_AUTO, MOVE_NONE } move_t;

static void show_index_full(int i, const char *row, move_t how, double keep_y)
{
    if (i < 0 || i >= panel_count()) return;
    int from = A.current;
    gboolean first = gtk_stack_get_visible_child(GTK_STACK(A.stack)) == NULL;
    A.current = i;

    GtkWidget *page = lp_panels[i]->build();
    GtkWidget *clamp = g_object_get_data(G_OBJECT(page), "lp-clamp");
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), clamp ? clamp : page);
    char name[24];
    g_snprintf(name, sizeof name, "page%u", ++A.serial);
    gtk_stack_add_named(GTK_STACK(A.stack), sw, name);
    A.scroller = sw;

    GtkStackTransitionType t;
    guint ms = lp_spring_ms(LP_SPRING_SHEET, FALSE);        /* 260ms */
    if (first || how == MOVE_NONE || i == from)
        t = GTK_STACK_TRANSITION_TYPE_NONE;
    else if (lp_motion_reduced())
        t = GTK_STACK_TRANSITION_TYPE_CROSSFADE, ms = lp_spring_ms(LP_SPRING_SHEET, FALSE);
    else
        /* Further down the list is forward: it comes in from the right. */
        t = i > from ? GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT
                     : GTK_STACK_TRANSITION_TYPE_SLIDE_RIGHT;
    gtk_stack_set_transition_duration(GTK_STACK(A.stack), ms);
    gtk_stack_set_visible_child_full(GTK_STACK(A.stack), name, t);
    if (t == GTK_STACK_TRANSITION_TYPE_NONE)
        on_transition(G_OBJECT(A.stack), NULL, NULL);

    if (keep_y > 0) {
        restore_t *r = g_new0(restore_t, 1);
        r->y = keep_y;
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
        g_signal_connect(adj, "changed", G_CALLBACK(restore_scroll), r);
    }

    A.selecting = TRUE;
    GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(A.sidebar), i);
    gtk_list_box_select_row(GTK_LIST_BOX(A.sidebar), r);
    A.selecting = FALSE;
    hl_move();
    g_idle_add(side_reveal, NULL);

    if (row && *row) {
        GtkWidget *hit = find_row(page, row);
        if (hit)
            /* After the slide and the first layout, or compute_point has
             * nothing to measure and the flash is lost in the motion. */
            g_timeout_add(ms + 60, scroll_to_row, g_object_ref(hit));
    }
}

static void show_index(int i, const char *row)
{
    show_index_full(i, row, MOVE_AUTO, 0);
}

void lp_show_panel(const char *id, const char *row)
{
    for (int i = 0; lp_panels[i]; i++)
        if (!g_strcmp0(lp_panels[i]->id, id)) {
            show_index(i, row);
            return;
        }
}

void lp_refresh(void)
{
    GtkAdjustment *adj = A.scroller
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(A.scroller)) : NULL;
    double y = adj ? gtk_adjustment_get_value(adj) : 0;
    show_index_full(A.current, NULL, MOVE_NONE, y);
}

static void on_sidebar_row(GtkListBox *box, GtkListBoxRow *row, gpointer d)
{
    (void)box; (void)d;
    if (!row || A.selecting) return;
    show_index(gtk_list_box_row_get_index(row), NULL);
}

/* ── search ─────────────────────────────────────────────────────────── */

static const char *pname(const lp_panel_t *p) { return lp_korean() ? p->ko : p->en; }

static void add_result(const lp_panel_t *p, const char *label, const char *row)
{
    GtkWidget *r = gtk_list_box_row_new();
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(h, 4);
    gtk_widget_set_margin_bottom(h, 4);
    gtk_box_append(GTK_BOX(h), gtk_image_new_from_icon_name(p->icon));
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(v), l);
    if (row) {
        GtkWidget *s = gtk_label_new(pname(p));
        gtk_widget_set_halign(s, GTK_ALIGN_START);
        gtk_widget_add_css_class(s, "lp-result-panel");
        gtk_box_append(GTK_BOX(v), s);
    }
    gtk_box_append(GTK_BOX(h), v);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(r), h);
    g_object_set_data(G_OBJECT(r), "lp-panel", (gpointer)p->id);
    g_object_set_data_full(G_OBJECT(r), "lp-row", g_strdup(row), g_free);
    g_object_set_data_full(G_OBJECT(r), "lp-title", g_strdup(label), g_free);
    gtk_list_box_append(GTK_LIST_BOX(A.results), r);
}

static gboolean matches(const char *hay, const char *needle)
{
    char *a = g_utf8_casefold(hay, -1), *b = g_utf8_casefold(needle, -1);
    gboolean m = strstr(a, b) != NULL;
    g_free(a); g_free(b);
    return m;
}

static void on_search(GtkSearchEntry *e, gpointer d)
{
    (void)d;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(e));
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(A.results)))
        gtk_list_box_remove(GTK_LIST_BOX(A.results), c);

    if (!q || !*q) {
        gtk_stack_set_visible_child_name(GTK_STACK(A.side_stack), "panels");
        return;
    }
    int hits = 0;
    for (int i = 0; lp_panels[i]; i++) {
        const lp_panel_t *p = lp_panels[i];
        if (matches(p->en, q) || matches(p->ko, q) || matches(p->id, q)) {
            add_result(p, pname(p), NULL);
            hits++;
        }
        for (int k = 0; p->keys && p->keys[k] && p->keys[k + 1]; k += 2) {
            /* A search in either language finds the row; the result is
             * shown in the language the screen is in, because that is the
             * title that will be flashed when it opens. */
            if (matches(p->keys[k], q) || matches(p->keys[k + 1], q)) {
                const char *shown = lp_korean() ? p->keys[k + 1] : p->keys[k];
                add_result(p, shown, shown);
                hits++;
            }
        }
    }
    if (!hits) {
        GtkWidget *r = gtk_list_box_row_new();
        GtkWidget *l = gtk_label_new(T("Nothing matches", "찾는 항목이 없습니다"));
        gtk_widget_add_css_class(l, "lp-result-panel");
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(r), l);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r), FALSE);
        gtk_list_box_append(GTK_LIST_BOX(A.results), r);
    }
    gtk_stack_set_visible_child_name(GTK_STACK(A.side_stack), "results");
}

static void on_result(GtkListBox *box, GtkListBoxRow *row, gpointer d)
{
    (void)box; (void)d;
    const char *id = g_object_get_data(G_OBJECT(row), "lp-panel");
    if (id)
        lp_show_panel(id, g_object_get_data(G_OBJECT(row), "lp-row"));
}

static void on_search_activate(GtkSearchEntry *e, gpointer d)
{
    (void)e; (void)d;
    GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(A.results), 0);
    if (r) on_result(GTK_LIST_BOX(A.results), r, NULL);
}

/* ── the window ─────────────────────────────────────────────────────── */

static void build_window(GtkApplication *gapp)
{
    A.gapp = gapp;
    A.win = gtk_application_window_new(gapp);
    gtk_widget_add_css_class(A.win, "lp-settings");
    gtk_window_set_title(GTK_WINDOW(A.win), T("Settings", "설정"));
    /* 1100x760, or less on a small screen: what is left of the monitor
     * under the top bar (36) and above the dock's room (about 96), with a
     * margin. A window taller than that was placed with its title bar -
     * and its buttons - under the top bar, and its bottom under the dock:
     * a VM window on a laptop, or a scaled 4K panel, is often that small. */
    int dw = 1100, dh = 760;
    GListModel *mons = gdk_display_get_monitors(gdk_display_get_default());
    GdkMonitor *mon = mons && g_list_model_get_n_items(mons) > 0
                    ? g_list_model_get_item(mons, 0) : NULL;
    if (mon) {
        GdkRectangle geo;
        gdk_monitor_get_geometry(mon, &geo);
        dw = MAX(560, MIN(dw, geo.width - 64));
        dh = MAX(400, MIN(dh, geo.height - 36 - 96 - 24));
        g_object_unref(mon);
    }
    gtk_window_set_default_size(GTK_WINDOW(A.win), dw, dh);
    load_css();

    GtkWidget *head = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(head), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(A.win), head);

    GtkWidget *split = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(side, "lp-sidebar");
    gtk_widget_set_size_request(side, SIDEBAR_WIDTH, -1);

    A.search = gtk_search_entry_new();
    gtk_widget_add_css_class(A.search, "lp-search");
    g_object_set(A.search, "placeholder-text", T("Search settings", "설정 검색"), NULL);
    g_signal_connect(A.search, "search-changed", G_CALLBACK(on_search), NULL);
    g_signal_connect(A.search, "activate", G_CALLBACK(on_search_activate), NULL);
    gtk_box_append(GTK_BOX(side), A.search);

    A.sidebar = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(A.sidebar), GTK_SELECTION_SINGLE);
    gtk_widget_set_valign(A.sidebar, GTK_ALIGN_START);
    for (int i = 0; lp_panels[i]; i++) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_start(h, 4);
        gtk_box_append(GTK_BOX(h), gtk_image_new_from_icon_name(lp_panels[i]->icon));
        GtkWidget *l = gtk_label_new(pname(lp_panels[i]));
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(h), l);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), h);
        g_object_set_data_full(G_OBJECT(row), "lp-title", g_strdup(pname(lp_panels[i])), g_free);
        gtk_list_box_append(GTK_LIST_BOX(A.sidebar), row);
    }
    g_signal_connect(A.sidebar, "row-selected", G_CALLBACK(on_sidebar_row), NULL);

    A.results = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(A.results), GTK_SELECTION_NONE);
    g_signal_connect(A.results, "row-activated", G_CALLBACK(on_result), NULL);

    A.side_stack = gtk_stack_new();
    /* The highlight is drawn by a drawing area under the list: an
     * overlay whose main child is the drawing area and whose overlay is
     * the list, measured by the list. */
    A.side_hl = gtk_drawing_area_new();
    gtk_widget_add_css_class(A.side_hl, "lp-side-hl");
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(A.side_hl), hl_draw, NULL, NULL);
    lp_spring_init(&A.hl_y, LP_SPRING_SHEET, 0);
    lp_spring_init(&A.hl_h, LP_SPRING_SHEET, 48);
    A.hl_motion = lp_motion_new(A.side_hl, hl_frame, NULL);
    lp_motion_add(A.hl_motion, &A.hl_y);
    lp_motion_add(A.hl_motion, &A.hl_h);
    GtkWidget *side_ov = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(side_ov), A.side_hl);
    gtk_overlay_add_overlay(GTK_OVERLAY(side_ov), A.sidebar);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(side_ov), A.sidebar, TRUE);

    GtkWidget *s1 = gtk_scrolled_window_new();
    A.side_scroller = s1;
    /* A scrollbar that is there, not one that appears on hover: in a
     * short window (1280x800, or a VM window) the list ends mid-way with
     * nothing to say that Date & time, Users and the rest are below, and
     * a touch screen never hovers. */
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(s1), FALSE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s1), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s1), side_ov);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(s1), TRUE);
    GtkWidget *s2 = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s2), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s2), A.results);
    gtk_stack_add_named(GTK_STACK(A.side_stack), s1, "panels");
    gtk_stack_add_named(GTK_STACK(A.side_stack), s2, "results");
    gtk_widget_set_vexpand(A.side_stack, TRUE);
    gtk_box_append(GTK_BOX(side), A.side_stack);
    gtk_box_append(GTK_BOX(split), side);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(overlay, TRUE);
    A.stack = gtk_stack_new();
    gtk_widget_set_vexpand(A.stack, TRUE);
    gtk_widget_set_hexpand(A.stack, TRUE);
    /* Two pages are side by side only during a slide; neither may make
     * the other's size jump. */
    gtk_stack_set_hhomogeneous(GTK_STACK(A.stack), TRUE);
    gtk_stack_set_vhomogeneous(GTK_STACK(A.stack), TRUE);
    g_signal_connect(A.stack, "notify::transition-running", G_CALLBACK(on_transition), NULL);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), A.stack);

    A.toast = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(A.toast),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_widget_set_halign(A.toast, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(A.toast, GTK_ALIGN_START);
    GtkWidget *tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(tb, "lp-toast");
    gtk_box_append(GTK_BOX(tb), gtk_image_new_from_icon_name("emblem-ok-symbolic"));
    A.toast_label = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(A.toast_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(A.toast_label), 70);
    gtk_box_append(GTK_BOX(tb), A.toast_label);
    gtk_revealer_set_child(GTK_REVEALER(A.toast), tb);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), A.toast);
    gtk_box_append(GTK_BOX(split), overlay);

    gtk_window_set_child(GTK_WINDOW(A.win), split);

    /* Ctrl+F and typing anywhere go to search, as in every GNOME app. */
    gtk_search_entry_set_key_capture_widget(GTK_SEARCH_ENTRY(A.search), A.win);
}

static void activate(GtkApplication *gapp, gpointer d)
{
    (void)d;
    if (!A.win)
        build_window(gapp);
    /* Network first: it is the most common reason to open Settings. */
    if (start_panel)
        lp_show_panel(start_panel, start_row);
    else if (!gtk_stack_get_visible_child(GTK_STACK(A.stack)))
        show_index(0, NULL);
    g_clear_pointer(&start_panel, g_free);
    g_clear_pointer(&start_row, g_free);
    gtk_window_present(GTK_WINDOW(A.win));
    lp_driver_start();
}

/* `lp-settings wifi`, `lp-settings --panel display`: the top bar and the
 * quick menu open a panel directly. A second invocation while the window
 * is open is forwarded to it over D-Bus by GApplication and switches the
 * panel there instead of opening a second window. */
static int command_line(GApplication *gapp, GApplicationCommandLine *cl, gpointer d)
{
    (void)d;
    int argc = 0;
    char **argv = g_application_command_line_get_arguments(cl, &argc);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--panel") && i + 1 < argc) {
            g_free(start_panel);
            start_panel = g_strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--row") && i + 1 < argc) {
            g_free(start_row);
            start_row = g_strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            g_application_command_line_print(cl,
                "usage: lp-settings [PANEL | --panel PANEL] [--row ROW]\n\npanels:");
            for (int k = 0; lp_panels[k]; k++)
                g_application_command_line_print(cl, " %s", lp_panels[k]->id);
            g_application_command_line_print(cl, "\n");
            g_strfreev(argv);
            return 0;
        } else if (argv[i][0] != '-') {
            g_free(start_panel);
            /* gnome-control-center's names, so muscle memory works. */
            const char *id = argv[i];
            if (!strcmp(id, "wifi") || !strcmp(id, "wlan")) id = "network";
            else if (!strcmp(id, "info") || !strcmp(id, "about")) id = "system";
            else if (!strcmp(id, "universal-access") || !strcmp(id, "a11y")) id = "accessibility";
            else if (!strcmp(id, "datetime") || !strcmp(id, "date")) id = "datetime";
            else if (!strcmp(id, "user-accounts") || !strcmp(id, "users")) id = "users";
            else if (!strcmp(id, "power")) id = "power";
            else if (!strcmp(id, "mouse") || !strcmp(id, "touchpad")) id = "touch";
            else if (!strcmp(id, "background")) id = "appearance";
            else if (!strcmp(id, "region")) id = "region";
            else if (!strcmp(id, "default-apps")) id = "apps";
            start_panel = g_strdup(id);
        }
    }
    g_strfreev(argv);
    g_application_activate(gapp);
    return 0;
}

/* `lp-settings --restore`, run by the session at login: every panel that
 * keeps a setting somewhere a running program has to be told about
 * (gsettings keys mirrored from our files, the night-light daemon) puts
 * it back. The files are the record; this makes the machine match them.
 * `--apply-night-light` does only that one, for the same reason. */
char *lp_keyboard_sway_keys(void);     /* panels/keyboard.c */

static gboolean wake(gpointer p) { (void)p; return G_SOURCE_CONTINUE; }

static int restore_all(void)
{
    int n = 0;
    for (int i = 0; lp_panels[i]; i++)
        if (lp_panels[i]->restore) {
            lp_panels[i]->restore();
            n++;
        }
    /* Wait for what the restores started (gsettings, makoctl) - at most
     * ten seconds, so a service that never answers cannot hold up the
     * login. */
    gint64 until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    guint tick = g_timeout_add(200, wake, NULL);
    while (lp_jobs_pending() > 0 && g_get_monotonic_time() < until)
        g_main_context_iteration(NULL, TRUE);
    g_source_remove(tick);
    g_printerr("lp-settings: restored %d panels' settings\n", n);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--restore"))
        return restore_all();
    if (argc >= 2 && !strcmp(argv[1], "--apply-night-light"))
        return lp_night_light_exec();
    /* The shortcuts as sway bindings, from $WAYFIRE_CONFIG_FILE (or the
     * person's wayfire.ini): how the image's default file is made. */
    if (argc >= 2 && !strcmp(argv[1], "--sway-keys")) {
        char *t = lp_keyboard_sway_keys();
        fputs(t, stdout);
        g_free(t);
        return 0;
    }

    GtkApplication *app = gtk_application_new("org.lpzero.Settings",
                                              G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(app, "command-line", G_CALLBACK(command_line), NULL);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return r;
}
