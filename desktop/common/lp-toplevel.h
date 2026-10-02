/*
 * lp-toplevel.h - the list of open windows, from the compositor.
 *
 * The top bar names the focused application and the dock puts a dot
 * beside every application that has a window. Neither can find that out
 * from inside its own process: on Wayland a client sees its own windows
 * and nobody else's, which is the point of Wayland. The compositor tells
 * a shell through wlr-foreign-toplevel-management, which wayfire 0.7
 * and sway both implement in their core, and this is the client side of
 * it, sharing the GDK connection so that its events arrive on the GTK
 * main loop with everything else.
 */
#ifndef LP_TOPLEVEL_H
#define LP_TOPLEVEL_H

#include <glib.h>

typedef struct LpToplevel {
    char *app_id;
    char *title;
    gboolean activated;
    gboolean minimized;
    gboolean maximized;
    gboolean fullscreen;
    void *handle;           /* the protocol object */
    gboolean done;          /* has seen its first `done` */
} LpToplevel;

typedef void (*LpToplevelsFn)(gpointer data);

/* FALSE when the compositor does not offer the protocol; the list then
 * stays empty and the callers carry on without running indicators. */
gboolean lp_toplevels_init(void);
void lp_toplevels_watch(LpToplevelsFn fn, gpointer data);

/* The current windows, oldest first. Not owned by the caller. */
GList *lp_toplevels(void);

void lp_toplevel_activate(LpToplevel *t);
void lp_toplevel_close(LpToplevel *t);
void lp_toplevel_set_minimized(LpToplevel *t, gboolean min);

#endif /* LP_TOPLEVEL_H */
