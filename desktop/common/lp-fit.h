/* lp-fit.h - a GTK 4 window's first size, made to fit the screen.
 *
 * Header only: the GTK 4 programs here do not all link the same shared
 * code, and this is one small function.
 *
 * The compositor centres a new window in the room the top bar and the
 * dock leave. A window taller than that room was placed with its title
 * bar - and its buttons - under the top bar and its bottom under the
 * dock, and it looked as if the shell covered it: a laptop at 1366x768,
 * or a 1080p panel at 125% (864 tall), has a room of about 700 pixels,
 * and Task Manager asked for 780. So the size asked for is the wanted
 * one or the room, whichever is smaller (settings/core.c does the same).
 *
 * The room is the monitor less the bar (36), the dock's room (about 96)
 * and a margin; Wayland tells a client nothing about either, so these
 * are the shell's own sizes. The first monitor, as a new window opens on
 * the one in use and the two are nearly always the same size. */
#ifndef LP_FIT_H
#define LP_FIT_H

#include <gtk/gtk.h>

#define LP_FIT_TOP    36
#define LP_FIT_BOTTOM 96
#define LP_FIT_MARGIN 24

static inline void lp_fit_default_size(GtkWindow *w, int want_w, int want_h)
{
    int dw = want_w, dh = want_h;
    GListModel *mons = gdk_display_get_monitors(gdk_display_get_default());
    GdkMonitor *mon = mons && g_list_model_get_n_items(mons) > 0
                    ? g_list_model_get_item(mons, 0) : NULL;
    if (mon) {
        GdkRectangle geo;
        gdk_monitor_get_geometry(mon, &geo);
        if (dw > 0)
            dw = MAX(MIN(want_w, 480), MIN(dw, geo.width - 64));
        if (dh > 0)
            dh = MAX(MIN(want_h, 360),
                     MIN(dh, geo.height - LP_FIT_TOP - LP_FIT_BOTTOM - LP_FIT_MARGIN));
        g_object_unref(mon);
    }
    gtk_window_set_default_size(w, dw, dh);
}

#endif
