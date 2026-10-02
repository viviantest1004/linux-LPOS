/*
 * access.c - Accessibility: less motion, larger text, a larger pointer,
 * and the way to the on-screen keyboard's settings. The text and pointer
 * rows are also shown by Appearance and Touch & mouse (see below).
 *
 * ── Reduce motion ──
 *
 * One switch, four places, because motion on this desktop comes from
 * four places (COMMON.md, Motion):
 *
 *   ~/.config/lp/reduce-motion         exists = on. lp-motion (every
 *                                      spring in the shell, the keyboard,
 *                                      this app) checks it and trades
 *                                      travel for a short crossfade.
 *   gtk-enable-animations=false        in ~/.config/gtk-3.0/settings.ini
 *                                      and gtk-4.0/settings.ini: GTK's own
 *                                      transitions (stacks, revealers,
 *                                      switch knobs) in apps started later,
 *                                      and org.gnome.desktop.interface
 *                                      enable-animations for libadwaita.
 *   wayfire.ini [animate] duration, zoom_duration, fade_duration = 0 and
 *              [vswitch] duration, [expo] duration = 0: the compositor's
 *              window and workspace animations. Wayfire re-reads the file
 *              when it changes, so this is live.
 *
 * Turning it off puts back exactly what was there before - a person who
 * had set a 200ms fade by hand gets 200ms again, not wayfire's default.
 * The values from before are kept in the flag file itself, one
 * "section.key=value" per line ("section.key=" when the key was not set),
 * and the file is written BEFORE anything is changed and removed only
 * AFTER everything is put back: whichever moment the power goes, the
 * flag either is not there and nothing was changed, or is there and
 * holds what to restore.
 *
 * This window follows at once: its own GtkSettings is told, and the page
 * slide becomes a 90ms crossfade (core.c asks lp_motion_reduced()).
 */
#include "core.h"

#include <string.h>

/* The compositor's animation lengths. */
static const struct { const char *sec, *key; } WF_KEYS[] = {
    { "animate", "duration" },
    { "animate", "zoom_duration" },
    { "animate", "fade_duration" },
    { "vswitch", "duration" },
    { "expo",    "duration" },
};

static char *flag_path(void) { return lp_config_path("reduce-motion"); }

static gboolean reduced_on(void)
{
    char *f = flag_path();
    gboolean on = g_file_test(f, G_FILE_TEST_EXISTS);
    g_free(f);
    return on;
}

static gboolean set_reduce(gboolean on)
{
    char *flag = flag_path();
    char *wf = wayfire_ini();
    gboolean have_wf = g_file_test(wf, G_FILE_TEST_EXISTS);
    gboolean done = FALSE;

    if (on && g_file_test(flag, G_FILE_TEST_EXISTS)) {
        /* Already on: the flag holds the values from before it was first
         * turned on, and must not be overwritten with the zeros. */
        done = TRUE;
        goto out;
    }
    if (on) {
        /* 1. What is there now, into the flag (which turns it on for
         *    lp-motion at the same moment). */
        GString *saved = g_string_new("# Reduce motion is on (Settings > Accessibility).\n"
                                      "# What was there before, put back when it is turned off:\n");
        char *prev_gtk = lp_gtk_settings_get("gtk-enable-animations");
        g_string_append_printf(saved, "gtk.gtk-enable-animations=%s\n", prev_gtk ? prev_gtk : "");
        g_free(prev_gtk);
        for (guint i = 0; i < G_N_ELEMENTS(WF_KEYS); i++) {
            char *v = have_wf ? ini_get(wf, WF_KEYS[i].sec, WF_KEYS[i].key) : NULL;
            g_string_append_printf(saved, "%s.%s=%s\n", WF_KEYS[i].sec, WF_KEYS[i].key, v ? v : "");
            g_free(v);
        }
        gboolean ok = lp_write_file(flag, saved->str);
        g_string_free(saved, TRUE);
        if (!ok) goto out;
        /* 2. Then change things. */
        lp_gtk_settings_set("gtk-enable-animations", "false");
        lp_gsettings_set("org.gnome.desktop.interface", "enable-animations", "false");
        if (have_wf)
            for (guint i = 0; i < G_N_ELEMENTS(WF_KEYS); i++)
                ini_set(wf, WF_KEYS[i].sec, WF_KEYS[i].key, "0");
    } else {
        /* 1. Put everything back from the flag. */
        char *saved = lp_slurp(flag);
        char **lines = g_strsplit(saved ? saved : "", "\n", -1);
        gboolean gtk_done = FALSE;
        for (int i = 0; lines[i]; i++) {
            char *l = lines[i];
            if (!*l || *l == '#') continue;
            char *eq = strchr(l, '=');
            char *dot = strchr(l, '.');
            if (!eq || !dot || dot > eq) continue;
            *eq = '\0';
            *dot = '\0';
            const char *sec = l, *key = dot + 1, *val = eq + 1;
            if (!strcmp(sec, "gtk")) {
                lp_gtk_settings_set(key, *val ? val : NULL);
                gtk_done = TRUE;
            } else if (have_wf) {
                if (*val) ini_set(wf, sec, key, val);
                else ini_unset(wf, sec, key);
            }
        }
        g_strfreev(lines);
        g_free(saved);
        if (!gtk_done)
            lp_gtk_settings_set("gtk-enable-animations", NULL);
        lp_gsettings_set("org.gnome.desktop.interface", "enable-animations", "true");
        /* 2. Only then the flag. */
        if (!lp_remove_file(flag)) goto out;
    }
    /* This window, now. */
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", !on, NULL);
    lp_toast(FALSE, on ? T("Reduce motion is on: things fade instead of moving",
                           "움직임 줄이기를 켰습니다: 움직이는 대신 서서히 바뀝니다")
                       : T("Reduce motion is off", "움직임 줄이기를 껐습니다"));
    done = TRUE;
out:
    g_free(wf);
    g_free(flag);
    return done;
}

static void on_reduce(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    if (!set_reduce(on))
        LP_QUIET(gtk_switch_set_active(GTK_SWITCH(sw), !on));
}

/* ── text and pointer ─────────────────────────────────────────────────
 *
 * One value of each, in ~/.config/lp/accessibility.conf (text_scale,
 * cursor_size), and the rows that change them are made here for every
 * page that shows one - this one, Appearance (text) and Touch & mouse
 * (pointer) - so a size changed on one page is the size on the others.
 *
 * The pointer's theme is named, not "default": Adwaita is the one the
 * image ships, and the session's own files name it with the same size
 * (sway.config, wayfire.ini, settings.ini, XCURSOR_SIZE). 16 is the
 * default there, 32 pixels at scale 2. */

#define CURSOR_THEME "Adwaita"

static const lp_opt_t TEXT[] = {
    { "1.00", "Default", "기본" },
    { "1.15", "Large",   "크게" },
    { "1.30", "Larger",  "더 크게" },
    { "1.50", "Largest", "가장 크게" },
    { NULL, NULL, NULL }
};

static const lp_opt_t CURSOR[] = {
    { "16", "Small (default)", "작게 (기본)" },
    { "24", "Medium",          "보통" },
    { "32", "Large",           "크게" },
    { "48", "Larger",          "더 크게" },
    { "64", "Largest",         "가장 크게" },
    { NULL, NULL, NULL }
};

static char *a11y_conf(void) { return lp_config_path("accessibility.conf"); }

static void apply_text(const char *factor, gboolean live)
{
    double f = g_ascii_strtod(factor, NULL);
    if (f < 0.5 || f > 3) f = 1.0;
    /* GTK's dpi is in 1024ths of a dot per inch, 96 being 100%. */
    int dpi = (int)(96 * 1024 * f + 0.5);
    char d[32];
    g_snprintf(d, sizeof d, "%d", dpi);
    lp_gtk_settings_set("gtk-xft-dpi", f == 1.0 ? NULL : d);
    lp_gsettings_set("org.gnome.desktop.interface", "text-scaling-factor", factor);
    if (live)
        g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", dpi, NULL);
}

/* Only a key that differs is written: every write makes wayfire read its
 * whole config again, and a login that restores the same size should not. */
static void wf_input_set(const char *wf, const char *key, const char *value)
{
    char *old = ini_get(wf, "input", key);
    if (!old || strcmp(old, value))
        ini_set(wf, "input", key, value);
    g_free(old);
}

static void apply_cursor(const char *size)
{
    gint64 n = g_ascii_strtoll(size, NULL, 10);
    if (n < 8 || n > 256) return;       /* a hand-edited file, not ours */
    char s[16];
    g_snprintf(s, sizeof s, "%d", (int)n);
    lp_gtk_settings_set("gtk-cursor-theme-size", s);
    lp_gsettings_set("org.gnome.desktop.interface", "cursor-size", s);
    /* Wayfire reads [input] cursor_theme and cursor_size and changes the
     * pointer the moment the file changes; sway is told with swaymsg. */
    char *wf = wayfire_ini();
    if (g_file_test(wf, G_FILE_TEST_EXISTS)) {
        wf_input_set(wf, "cursor_theme", CURSOR_THEME);
        wf_input_set(wf, "cursor_size", s);
    }
    g_free(wf);
    const char *a[] = { "seat", "*", "xcursor_theme", CURSOR_THEME, s, NULL };
    lp_swaymsg(a);
}

static void on_text(GObject *dd, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    const char *v = row_option_value(dd);
    if (!v) return;
    char *c = a11y_conf();
    gboolean ok = kv_set(c, "text_scale", v);
    g_free(c);
    if (!ok) return;
    apply_text(v, TRUE);
    lp_toast(FALSE, T("Text size %d%% - apps opened from now on use it",
                      "글자 크기 %d%% - 지금부터 여는 앱에 적용됩니다"),
             (int)(g_ascii_strtod(v, NULL) * 100 + 0.5));
}

static void on_cursor(GObject *dd, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    const char *v = row_option_value(dd);
    if (!v) return;
    char *c = a11y_conf();
    gboolean ok = kv_set(c, "cursor_size", v);
    g_free(c);
    if (!ok) return;
    apply_cursor(v);
    lp_toast(FALSE, T("Pointer size %s", "포인터 크기 %s"), v);
}

GtkWidget *lp_text_size_row(GtkWidget *list)
{
    char *c = a11y_conf();
    char *ts = kv_get(c, "text_scale");
    g_free(c);
    GtkWidget *row = row_options(list, T("Text size", "글자 크기"), NULL, TEXT, ts, 0,
                                 G_CALLBACK(on_text), NULL);
    g_free(ts);
    return row;
}

/* Nothing stored is the session's own 16, the first choice. */
GtkWidget *lp_pointer_size_row(GtkWidget *list)
{
    char *c = a11y_conf();
    char *cs = kv_get(c, "cursor_size");
    g_free(c);
    GtkWidget *row = row_options(list, T("Pointer size", "포인터 크기"), NULL, CURSOR, cs, 0,
                                 G_CALLBACK(on_cursor), NULL);
    g_free(cs);
    return row;
}

static void on_osk(GtkWidget *row, gpointer p)
{
    (void)row; (void)p;
    lp_show_panel("touch", T("Show the on-screen keyboard", "화상 키보드 보이기"));
}

static GtkWidget *build(void)
{
    GtkWidget *page = page_new(T("Accessibility", "접근성"),
                               reduced_on() ? T("Reduce motion is on", "움직임 줄이기가 켜져 있습니다")
                                            : NULL);
    GtkWidget *g = group_new(page, T("Seeing", "보기"));
    row_switch(g, T("Reduce motion", "움직임 줄이기"),
               T("Windows, panels and switches fade instead of sliding or zooming",
                 "창, 패널, 스위치가 미끄러지거나 커지는 대신 서서히 바뀝니다"),
               reduced_on(), G_CALLBACK(on_reduce), NULL);
    lp_text_size_row(g);
    lp_pointer_size_row(g);

    GtkWidget *t = group_new(page, T("Typing", "입력"));
    row_chevron(t, T("On-screen keyboard", "화상 키보드"),
                T("When it appears, and its layouts", "언제 나타날지, 그리고 자판"),
                NULL, G_CALLBACK(on_osk), NULL);
    return page;
}

/* At login: the stored sizes into GTK's files, gsettings, wayfire.ini
 * and sway. Nothing stored leaves the session's own defaults alone. */
static void restore(void)
{
    char *c = a11y_conf();
    char *ts = kv_get(c, "text_scale");
    char *cs = kv_get(c, "cursor_size");
    g_free(c);
    if (ts) apply_text(ts, FALSE);
    if (cs) apply_cursor(cs);
    g_free(ts); g_free(cs);
    lp_gsettings_set("org.gnome.desktop.interface", "enable-animations",
                     reduced_on() ? "false" : "true");
}

static const char *const KEYS[] = {
    "Reduce motion", "움직임 줄이기",
    "Animations", "애니메이션",
    "Text size", "글자 크기",
    "Pointer size", "포인터 크기",
    "Cursor size", "커서 크기",
    "On-screen keyboard", "화상 키보드",
    NULL
};

const lp_panel_t lp_panel_access = {
    "accessibility", "Accessibility", "접근성", "preferences-desktop-accessibility-symbolic",
    build, KEYS, restore
};
