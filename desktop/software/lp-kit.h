/* lp-kit.h - what the three storage-and-software applications share:
 * lp-files, lp-tasks and lp-software.
 *
 * Each of them needs the same half-dozen things that GTK does not give
 * in the shape this desktop wants them: a dialog that arrives as a
 * sheet on a spring instead of popping, a progress bar that glides to
 * the real number instead of jumping, a list row that slides in instead
 * of appearing, settings written so that pulling the power cord cannot
 * leave half a file, and - for the two that change the system - a
 * conversation with lp-privd that includes asking for the person's
 * password. Written three times they would drift three ways; the
 * password dialog in particular is the kind of code that must be right
 * once rather than nearly right in three places.
 *
 * It lives in desktop/software/ only because that is a directory this
 * track owns; lp-files and lp-tasks compile ../software/lp-kit.c into
 * themselves. It is GTK 4 only (the applications are GTK 4) and uses
 * desktop/common/lp-motion for every spring. If a fourth application
 * wants it, desktop/common/ is the better home.
 */
#ifndef LP_KIT_H
#define LP_KIT_H

#include <gtk/gtk.h>
#include "lp-i18n.h"
#include "lp-motion.h"

G_BEGIN_DECLS

/* ── looks ─────────────────────────────────────────────────────────── */

/* Load motion.css (compiled in), the kit's own rules, and then the
 * application's `css`, all at APPLICATION priority - below the user's
 * ~/.config/gtk-4.0/gtk.css, which is where the theme lives, so the
 * theme still has the last word. Call once, after gtk_init. */
void lp_kit_style(const char *css);

/* A scrolled window for touch: kinetic scrolling on (always; nothing in
 * these applications ever turns it off), overlay scrollbars that appear
 * only while scrolling, and no horizontal scrolling unless asked for. */
GtkWidget *lp_kit_scroller(GtkWidget *child, gboolean horizontal);

/* A stack whose pages slide (260ms) in the direction of their order,
 * or crossfade in 90ms under reduced motion. */
GtkWidget *lp_kit_stack(void);
/* Switch views of the same content (grid/list): a 200ms crossfade. */
GtkWidget *lp_kit_fade_stack(void);
void       lp_kit_stack_show(GtkStack *stack, const char *name);

/* Rows that arrive and leave. lp_kit_reveal_in wraps `child` in a
 * revealer that opens on the next frame (220ms, the insert spring's
 * duration), so the row slides in rather than appearing; call it for
 * rows added after the list was first shown, and pass instant=TRUE for
 * the first fill, which must not ripple. lp_kit_reveal_out closes it
 * (154ms, 0.7x) and then calls `gone` so the caller can remove it. */
GtkWidget *lp_kit_reveal_in(GtkWidget *child, gboolean instant);
void       lp_kit_reveal_out(GtkWidget *revealer, GCallback gone, gpointer data);

/* Long-press (touch) and secondary click (mouse) both open the same
 * context menu. `cb` gets widget-relative coordinates. */
typedef void (*LpKitContextFn)(GtkWidget *w, double x, double y, gpointer data);
void lp_kit_context(GtkWidget *w, LpKitContextFn cb, gpointer data);

/* A button 48 logical px tall with an icon and/or a label - the touch
 * target size these applications use everywhere a finger will go. */
GtkWidget *lp_kit_button(const char *icon, const char *label, const char *css);

/* ── an eased progress bar ─────────────────────────────────────────── */

/* A GtkProgressBar whose fraction follows a critically damped spring
 * towards the real value, so a job that reports 0, 40, 41, 100 is seen
 * to move rather than to jump. A negative value means "no number yet":
 * the bar pulses, and only then. */
GtkWidget *lp_kit_progress_new(void);
void       lp_kit_progress_set(GtkWidget *bar, double fraction);

/* ── settings that survive the power cord ─────────────────────────── */

/* ~/.config/lp/<app>.ini as a key file (empty when missing or broken). */
GKeyFile *lp_kit_state_load(const char *app);
/* Written atomically and durably: a temporary file in the same
 * directory, fsync, rename over, fsync the directory. */
gboolean  lp_kit_state_save(GKeyFile *kf, const char *app);

/* ── sheets ────────────────────────────────────────────────────────── */

/* A dialog drawn as a card on a spring (LP_SPRING_SHEET): it grows from
 * 0.96 and fades in over 260ms, and leaves in 182ms from wherever it
 * is. Modal, transient for `parent`. The response callback gets the id
 * of the button pressed, or LP_KIT_CANCEL for Escape / the window being
 * closed; the dialog is not closed for the caller - call
 * lp_kit_dialog_close when done, which lets a dialog stay open to show
 * an error (a wrong password) or a busy state. */
#define LP_KIT_CANCEL 0
#define LP_KIT_OK     1

typedef struct _LpKitDialog LpKitDialog;
typedef void (*LpKitResponse)(LpKitDialog *d, int response, gpointer data);

LpKitDialog *lp_kit_dialog_new(GtkWindow *parent, const char *title,
                               const char *text);
GtkWidget   *lp_kit_dialog_body(LpKitDialog *d);     /* a vertical box */
/* style: NULL, "suggested-action" or "destructive-action". Buttons are
 * laid out in the order added, the last one being the default. */
GtkWidget   *lp_kit_dialog_button(LpKitDialog *d, const char *label,
                                  int response, const char *style);
void         lp_kit_dialog_on_response(LpKitDialog *d, LpKitResponse cb,
                                       gpointer data);
void         lp_kit_dialog_error(LpKitDialog *d, const char *text);
void         lp_kit_dialog_busy(LpKitDialog *d, gboolean busy);
void         lp_kit_dialog_present(LpKitDialog *d);
void         lp_kit_dialog_close(LpKitDialog *d);
GtkWindow   *lp_kit_dialog_window(LpKitDialog *d);

/* Yes/no with an OK label of the caller's choosing. */
typedef void (*LpKitConfirmFn)(gboolean ok, gpointer data);
void lp_kit_confirm(GtkWindow *parent, const char *title, const char *text,
                    const char *ok_label, gboolean destructive,
                    LpKitConfirmFn cb, gpointer data);

/* ── lp-privd ──────────────────────────────────────────────────────── */

/* Ask lp-privd to do something. `fields` is the verb and its arguments
 * (NULL-terminated); they are sent TAB-separated, exactly as given, and
 * the daemon checks every one - nothing here builds a command line.
 *
 * on_line gets every "progress"/"log"/"dev" line as it arrives: kind is
 * the first word, pct is the number for progress (else -1), text the
 * rest. on_done gets the verdict once: ok, and for a failure the
 * daemon's reason word (denied, invalid, refused, busy, missing,
 * failed) or "cancelled" when the person closed the password dialog,
 * or "unreachable" when there is no daemon.
 *
 * A "fail auth" answer is handled here: the password sheet is shown,
 * the password is sent with the `auth` verb, and the request is sent
 * again. The person sees one dialog and then the job. `why_en/why_ko`
 * is the sentence the dialog opens with ("Installing software needs
 * ..."). The job keeps running in the daemon if the window goes away. */
typedef void (*LpPrivLine)(const char *kind, int pct, const char *text,
                           gpointer data);
typedef void (*LpPrivDone)(gboolean ok, const char *why, const char *text,
                           gpointer data);

void lp_priv_run(GtkWindow *parent, const char *const *fields,
                 const char *why_text, LpPrivLine on_line,
                 LpPrivDone on_done, gpointer data);

/* The socket path: $LP_PRIVD_SOCK or /run/lp-privd.sock. */
const char *lp_priv_socket(void);

/* ── being driven by a test ────────────────────────────────────────── */

/* When $LP_KIT_DRIVE names a FIFO, every line written to it is a
 * command, so a headless test can press buttons without a pointer (the
 * headless compositor has no input devices):
 *
 *   click NAME        "clicked" on the button / activate the widget
 *                     whose gtk_widget_set_name() is NAME, in any window
 *   text NAME VALUE   set an entry's text
 *   activate NAME     gtk_widget_activate (an entry: its default)
 *   <anything else>   passed to `other`, for application verbs
 *
 * Without the variable this does nothing and costs nothing. */
typedef void (*LpKitDriveFn)(const char *verb, const char *arg, gpointer data);
void lp_kit_drive(LpKitDriveFn other, gpointer data);
GtkWidget *lp_kit_find(const char *name);

G_END_DECLS

#endif /* LP_KIT_H */
