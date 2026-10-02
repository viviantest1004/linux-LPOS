/*
 * lp-wfshell.h - wayfire's own protocol for shells (wayfire-shell-v2).
 *
 * wayfire 0.7 does not tell other clients when a window is full screen:
 * its wlr-foreign-toplevel code sends the state only along with other
 * changes, and F11 is not one of them - the dock never learnt that the
 * screen had been taken, and could not offer itself at the bottom edge.
 * wayfire's own shell protocol, the one wf-panel hides itself with, says
 * it outright per output, and it also offers hotspots: an edge of the
 * output where the pointer or a finger has rested, reported over a
 * full-screen window as well. sway does not have it; there the
 * foreign-toplevel state is right, and lp_wfshell_init() says FALSE.
 */
#ifndef LP_WFSHELL_H
#define LP_WFSHELL_H

#include <gtk/gtk.h>

typedef void (*LpFullscreenFn)(GdkMonitor *mon, gboolean fullscreen, gpointer data);
typedef void (*LpHotspotFn)(GdkMonitor *mon, gboolean inside, gpointer data);

/* Edges, as the protocol numbers them. */
enum { LP_WF_EDGE_TOP = 1, LP_WF_EDGE_BOTTOM = 2, LP_WF_EDGE_LEFT = 4, LP_WF_EDGE_RIGHT = 8 };

/* TRUE when the compositor offers the protocol (wayfire). */
gboolean lp_wfshell_init(void);

/* fn(mon, TRUE) when a full-screen window starts covering mon, FALSE when
 * it stops. */
void lp_wfshell_watch_fullscreen(GdkMonitor *mon, LpFullscreenFn fn, gpointer data);

/* fn(mon, TRUE) once the pointer or a touch has stayed within threshold
 * pixels of the edges for timeout_ms, fn(mon, FALSE) when it leaves. */
void lp_wfshell_hotspot(GdkMonitor *mon, guint edges, guint threshold, guint timeout_ms,
                        LpHotspotFn fn, gpointer data);

#endif /* LP_WFSHELL_H */
