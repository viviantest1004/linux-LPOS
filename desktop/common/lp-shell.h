/*
 * lp-shell.h - what every piece of the desktop shell shares.
 *
 * The shell is six small GTK 3 programs rather than one: the top bar,
 * the dock, the app grid, the desktop icons, the quick settings panel
 * and the lock screen. Each is a wlr-layer-shell surface, which is the
 * only kind of window a compositor lets sit at a screen edge, reserve
 * space there and stay out of the window list. They are separate
 * processes so that a crash in one leaves the others standing - a dock
 * that dies must not take the lock screen with it - and so that each
 * one's CPU and memory can be read off `ps` on its own.
 *
 * GTK 3 and not GTK 4 because gtk-layer-shell 0.8, the version this
 * Debian base carries, only speaks GTK 3. The applications are GTK 4
 * and the shell around them is GTK 3; nobody can tell from the screen.
 *
 * What lives here is what all six would otherwise write six times:
 *
 *   - the stylesheet: one shell.css for every component, found in one
 *     place, reloaded when the owner switches between dark and light;
 *   - single instance: `lp-quick` run a second time toggles the panel
 *     that is already running instead of opening a second one. A unix
 *     socket in $XDG_RUNTIME_DIR does it, not D-Bus, so that it still
 *     works on the day the session bus did not come up;
 *   - spawning, so that every launch is detached and reaped the same way;
 *   - touch: a long press is a right click, on everything, always.
 */
#ifndef LP_SHELL_H
#define LP_SHELL_H

#include <gtk/gtk.h>
#include <gtk-layer-shell.h>

#include "lp-i18n.h"

/* ── start-up ─────────────────────────────────────────────────────── */

/* gtk_init, the stylesheet, the dark/light watch. Call first. */
void lp_shell_init(int *argc, char ***argv);

/* Where shell.css and the other shared files are. LP_SHARE overrides
 * the installed /usr/local/share/lp, which is how the headless tests
 * point a component at the checkout. */
const char *lp_share_dir(void);
char *lp_share_path(const char *name);

/* ~/.config/lp/<name>, created on demand for writers. */
char *lp_config_path(const char *name);

/* Replace a file so that a power cut leaves either the old contents or
 * the new, never a truncated mix: temp file in the same directory,
 * fsync, rename over, fsync the directory (GLib's CONSISTENT|DURABLE).
 * The parent directory is created if missing. COMMON.md's Persistence
 * rule; every setting the shell remembers goes through here. */
gboolean lp_write_atomic(const char *path, const char *data, gssize len);
/* The same, for ~/.config/lp/<name>. */
gboolean lp_config_write(const char *name, const char *data);
/* ~/.config/lp/<name> with trailing whitespace removed, or NULL. */
char *lp_config_read(const char *name);

/* TRUE when the owner has turned 어두운 스타일 off. */
gboolean lp_style_light(void);
void lp_style_set_light(gboolean light);

/* ── one process per component ───────────────────────────────────── */

typedef void (*LpCommandFn)(int argc, char **argv, gpointer data);

/* Returns TRUE when this process is the one that stays. FALSE means an
 * instance was already running and has been handed argv; the caller
 * exits 0. fn is called for every later invocation, with its argv, on
 * the main loop. */
gboolean lp_single_instance(const char *name, int argc, char **argv,
                            LpCommandFn fn, gpointer data);

/* Hand argv (NULL-terminated, argv[0] first) to the running instance of
 * another component. FALSE when it is not running. Used for the small
 * facts one component tells another - "the app grid is open" lights the
 * 현재 활동 button - without a bus or a spawn. */
gboolean lp_send(const char *name, const char *const *argv);

/* ── running things ──────────────────────────────────────────────── */

/* Detached: the child is reparented to init and reaped there, so a
 * launcher that runs for weeks never collects zombies. */
void lp_spawn(const char *const *argv);
void lp_spawn_cmdline(const char *cmdline);

/* Runs argv and hands its stdout to fn on the main loop; NULL when it
 * could not start or failed. Never blocks the UI. */
typedef void (*LpOutputFn)(const char *out, gpointer data);
void lp_run_async(const char *const *argv, LpOutputFn fn, gpointer data);

/* True when argv[0] can be found on PATH. */
gboolean lp_have(const char *prog);

/* ── windows ─────────────────────────────────────────────────────── */

enum { LP_EDGE_TOP = 1, LP_EDGE_BOTTOM = 2, LP_EDGE_LEFT = 4, LP_EDGE_RIGHT = 8 };

/* A layer-shell window with the given namespace, layer and anchors. */
GtkWindow *lp_layer_window(const char *ns, GtkLayerShellLayer layer,
                           int edges);

/* ── touch ───────────────────────────────────────────────────────── */

/* A long press (finger or mouse) or a right click calls fn with the
 * point in widget coordinates. There is no hover-only affordance
 * anywhere in the shell; this is the one way to reach a menu. */
typedef void (*LpHoldFn)(GtkWidget *w, double x, double y, gpointer data);
void lp_on_hold(GtkWidget *w, LpHoldFn fn, gpointer data);

/* TRUE, once, if a hold fired on w since the last press. A GtkButton
 * still emits "clicked" when the finger lifts after a long press, and
 * a menu that opens and then launches the app underneath it is the
 * bug this exists for. Call it first thing in the clicked handler. */
gboolean lp_hold_consumed(GtkWidget *w);
/* A tap (touch) or click (mouse) that lands and lifts on `w`, delivered
 * by a gesture of our own instead of GtkButton::clicked. Under sway a
 * button on a layer surface missed the first touch after the finger had
 * been elsewhere: GTK 3 delivers the touch's press before the crossing
 * that puts the pointer "in" the button, and a GtkButton only clicks
 * when it believes it was entered first. The second tap worked, which
 * is exactly how it looked: the dock ignored every first tap. Connect
 * this and not "clicked" - both would fire on the taps that did work. */
typedef void (*LpTapFn)(GtkWidget *w, gpointer data);
void lp_on_tap(GtkWidget *w, LpTapFn fn, gpointer data);

/* A GtkMenu popped at a point inside w, from a hold. */
void lp_menu_popup_at(GtkWidget *menu, GtkWidget *w, double x, double y);
/* Destroy a menu once it has closed - after the item that closed it has
 * run. GTK closes a menu before it activates the chosen item, so a menu
 * destroyed in its own "deactivate" loses the item's handler first and a
 * choice does nothing. */
void lp_menu_destroy_when_closed(GtkWidget *menu);

/* ── small things ────────────────────────────────────────────────── */

/* A themed icon at px logical pixels. */
GtkWidget *lp_icon(const char *name, int px);

/* The person's name as it should be shown: the GECOS name, else the
 * login name. */
char *lp_user_display_name(void);

/* A picture of w as it is now, at the output's scale, for painting
 * while w moves (see lp-sheet.c for why). NULL before w is allocated. */
cairo_surface_t *lp_widget_snapshot(GtkWidget *w);

/* ── measuring ───────────────────────────────────────────────────── */

/* With LP_MOTION_TRACE=1 (the same switch lp-motion's frame trace
 * uses), lp_trace_draw() prints "lp-shell: draw <what> cpu_ms=<n>" for a
 * draw that began at lp_trace_now(). Frame gaps say how smooth a motion
 * looked; this says how much of each gap was our own drawing, which is
 * the part that carries over to other hardware. It is this thread's CPU
 * time, not the wall clock, so a busy build machine preempting the
 * process does not show up as slow drawing. */
gboolean lp_trace_on(void);
gint64 lp_trace_now(void);
void lp_trace_draw(const char *what, gint64 start);

#endif /* LP_SHELL_H */
