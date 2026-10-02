/*
 * core.h - what every settings panel is allowed to use.
 *
 * The settings app used to be one 1900-line file with twelve pages in it.
 * That shape made every page's growth everybody's problem: adding Wi-Fi
 * scanning meant scrolling past the keyboard shortcuts, and two people
 * could not work on Sound and Display without editing the same file. So
 * the app is now a small core and one file per panel under panels/, and
 * this header is the whole contract between them.
 *
 * A panel is a table entry (lp_panel_t) plus a build function that returns
 * a fresh page. The core owns the window, the sidebar, the search, the
 * banner that reports what an action did, and the helpers every panel
 * needs to change the system - run a command, run one as administrator,
 * edit a key in a config file. Panels do not talk to each other.
 *
 * Three rules every panel follows, because the app is only worth opening
 * if they hold:
 *
 *   1. A control changes the system. A switch that only redraws itself
 *      teaches people that the next switch is lying too. Where something
 *      cannot be changed on this machine the row says so, in words.
 *   2. Every action reports its result in the banner - "Connected",
 *      or the backend's own error line. Silence after a tap reads as a
 *      tap that did not land, and on a touch screen that gets tapped
 *      again.
 *   3. Nothing blocks the window. Scanning for Wi-Fi takes seconds, and
 *      a frozen settings window on a touch screen is indistinguishable
 *      from a crashed one. Anything that can take longer than a frame
 *      goes through lp_run_async().
 */
#ifndef LP_SETTINGS_CORE_H
#define LP_SETTINGS_CORE_H

/* getpwent, getifaddrs, statvfs, setenv and friends are hidden by plain
 * -std=c11. Used undeclared, the compiler assumes they return int and the
 * top half of a 64-bit pointer is cut off - the first call crashes. */
#define _DEFAULT_SOURCE 1
#define _GNU_SOURCE 1

#include <gtk/gtk.h>
#include "lp-i18n.h"

/* ── the registry ──────────────────────────────────────────────────── */

typedef struct {
    const char *id;                 /* "network": --panel and the test driver */
    const char *en, *ko;            /* sidebar name */
    const char *icon;               /* symbolic icon name */
    GtkWidget *(*build)(void);      /* a new page every time it is shown */
    /* What search finds this panel by: pairs of English and Korean, the
     * titles of the rows people look for. NULL-terminated. When a search
     * hit is opened, the row whose title matches is scrolled to and
     * flashed, so these should be real row titles where possible. */
    const char *const *keys;
    /* Optional: make the running session match what this panel stored
     * (lp-settings --restore, at login). Must not need a window. */
    void (*restore)(void);
} lp_panel_t;

extern const lp_panel_t *const lp_panels[];     /* NULL-terminated, panels.c */

/* Show a panel by id, optionally flashing the row titled `row`. */
void lp_show_panel(const char *id, const char *row);
/* Build the current panel again - after something structural changed
 * (a user was added, a device paired) and patching rows would be more
 * code than it is worth. */
void lp_refresh(void);
/* The main window, for dialogs to be transient for. */
GtkWindow *lp_window(void);

/* ── the banner ────────────────────────────────────────────────────── */

/* One line at the top of the content area that says what the last action
 * did. err=TRUE colours it as a failure. Every action ends in one of these. */
void lp_toast(gboolean err, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
/* Re-read ~/.config/lp/style and appearance.conf into this window's
 * colours. */
void lp_apply_palette(void);
gboolean lp_style_is_light(void);
/* display.c: exec wlsunset as nightlight.conf says (or exit 0 if off). */
int  lp_night_light_exec(void);

/* ── pages and rows ────────────────────────────────────────────────────
 *
 * Spec 2-1: every item is the same row, and the right-hand side is one of
 * a few things - a value, a switch, a chevron to a sub-screen, and (added
 * here, because touch needs them in the row rather than behind it) a
 * slider, a drop-down or a button. Every row is at least 48 logical pixels
 * tall, and a switch row toggles when the row itself is tapped, not only
 * the switch - a 48px target is the row, not the 40px switch inside it.
 */

GtkWidget *page_new(const char *title, const char *subtitle);
void       page_set_subtitle(GtkWidget *page, const char *subtitle);
GtkWidget *group_new(GtkWidget *page, const char *heading);
/* A line of explanatory text under a group, for the one thing the rows
 * cannot say. */
GtkWidget *page_note(GtkWidget *page, const char *text);

GtkWidget *row_shell(const char *title, const char *detail);
GtkWidget *row_box(GtkWidget *row);              /* the row's horizontal box */
GtkWidget *row_control(GtkWidget *row);          /* the switch/scale/... in it */
void       row_set_detail(GtkWidget *row, const char *detail);
void       row_add(GtkWidget *list, GtkWidget *row);
/* Lists that change while they are on screen (Wi-Fi networks, input
 * sources): a row slides open when it arrives and closes before it goes,
 * 220ms in and 154ms out (feel.md: insert), instead of the list jumping. */
void       row_add_animated(GtkWidget *list, GtkWidget *row);
void       row_insert_animated(GtkWidget *list, GtkWidget *row, int pos);
void       row_remove_animated(GtkWidget *row);

GtkWidget *row_value(GtkWidget *list, const char *title, const char *detail,
                     const char *value);
void       row_set_value(GtkWidget *row, const char *value);
GtkWidget *row_switch(GtkWidget *list, const char *title, const char *detail,
                      gboolean on, GCallback cb, gpointer data);
GtkWidget *row_button(GtkWidget *list, const char *title, const char *detail,
                      const char *label, GCallback cb, gpointer data);
/* A row whose right side is any widget the panel made (a level bar, a
 * pair of buttons). It becomes the row's control. */
GtkWidget *row_widget(GtkWidget *list, const char *title, const char *detail,
                      GtkWidget *widget);
/* A row that opens something when tapped anywhere on it. */
GtkWidget *row_chevron(GtkWidget *list, const char *title, const char *detail,
                       const char *value, GCallback cb, gpointer data);
GtkWidget *row_choice(GtkWidget *list, const char *title, const char *detail,
                      const char *const *items, guint selected,
                      GCallback cb, gpointer data);
GtkWidget *row_scale(GtkWidget *list, const char *title, const char *detail,
                     double min, double max, double step, double value,
                     GCallback cb, gpointer data);
/* A drop-down over a fixed table of values: what is shown is the label in
 * the screen's language, what the handler reads back (row_option_value
 * on the drop-down it was handed) is the value to write. A current value
 * that is not in the table selects `dflt` rather than pretending. */
typedef struct { const char *value, *en, *ko; } lp_opt_t;
GtkWidget  *row_options(GtkWidget *list, const char *title, const char *detail,
                        const lp_opt_t *opts, const char *current, guint dflt,
                        GCallback cb, gpointer data);
const char *row_option_value(GObject *dropdown);
/* A row whose right side is a group of toggle buttons, one of which is
 * active - for choices of two to four where a drop-down would hide them. */
GtkWidget *row_segmented(GtkWidget *list, const char *title,
                         const char *detail, const char *const *items,
                         int active, GCallback cb, gpointer data);
int        segmented_active(GtkWidget *seg);

/* A row the current account may not change: the control is insensitive and
 * the detail line says who can. Spec: a restricted setting looks restricted
 * everywhere, and says who to ask. */
void       row_lock(GtkWidget *row);

/* access.c: the Text size and Pointer size rows, for every page that
 * shows one (Accessibility, Appearance, Touch & mouse). There is one
 * value of each, in ~/.config/lp/accessibility.conf, and one way of
 * applying it, so the pages cannot disagree. */
GtkWidget *lp_text_size_row(GtkWidget *list);
GtkWidget *lp_pointer_size_row(GtkWidget *list);

/* While lp_quiet is non-zero, row handlers return at once. Code that puts
 * a control back after a failed action wraps the set in LP_QUIET(), so the
 * correction does not run the action a second time. */
extern int lp_quiet;
#define LP_QUIET(stmt) do { lp_quiet++; stmt; lp_quiet--; } while (0)

/* ── dialogs ────────────────────────────────────────────────────────── */

typedef struct lp_dialog lp_dialog_t;
typedef void (*lp_dialog_fn)(lp_dialog_t *d, gpointer data);

/* A modal window with a title, a body box and a button row. `ok` is the
 * confirming label (NULL for a dialog with only a close button); `danger`
 * colours it red. on_ok returns through lp_dialog_close() when it is done
 * - a dialog that must wait for a command keeps itself open until then. */
lp_dialog_t *lp_dialog_new(const char *title, const char *ok, gboolean danger,
                           lp_dialog_fn on_ok, gpointer data);
GtkWidget   *lp_dialog_body(lp_dialog_t *d);
/* The height for a scrolled list inside a dialog: `want`, or less on a
 * screen too short for it once `chrome` pixels of the rest of the dialog
 * (title, fields, buttons) are counted - so the buttons stay above the
 * dock on a 768-line screen. */
int          lp_dialog_fit(int want, int chrome);
GtkWidget   *lp_dialog_window(lp_dialog_t *d);
GtkWidget   *lp_dialog_ok_button(lp_dialog_t *d);
void         lp_dialog_text(lp_dialog_t *d, const char *text, const char *css);
/* An entry with a label above it. Password entries get a button that
 * raises the on-screen keyboard, because a touch-only machine has no other
 * way to type into them if the keyboard did not come up by itself. */
GtkWidget   *lp_dialog_entry(lp_dialog_t *d, const char *label,
                             const char *initial, gboolean password);
void         lp_dialog_error(lp_dialog_t *d, const char *msg);
void         lp_dialog_busy(lp_dialog_t *d, gboolean busy);
void         lp_dialog_present(lp_dialog_t *d);
void         lp_dialog_close(lp_dialog_t *d);
void         lp_dialog_set_data(lp_dialog_t *d, const char *key, gpointer v,
                                GDestroyNotify free_fn);
gpointer     lp_dialog_get_data(lp_dialog_t *d, const char *key);

/* ── running things ─────────────────────────────────────────────────── */

typedef void (*lp_done_fn)(int status, const char *out, const char *err,
                           gpointer data);

/* The status a command gets when it was killed for not answering in
 * time (-1 is "could not be started", -2 "the administrator password was
 * cancelled"); its err is a sentence that says so. */
#define LP_RUN_TIMEOUT  (-3)
/* How long a synchronous command may take before it is killed. The
 * window does not redraw or take a tap while one runs, so this is the
 * longest the app may look frozen. */
#define LP_RUN_SYNC_MS  8000

/* Synchronous, for commands that answer at once (wpctl get-volume, cat).
 * Returns stdout, chomped, or NULL on a non-zero exit. */
char    *lp_run(const char *const *argv);
/* The same with status and stderr. Both stop the command after
 * LP_RUN_SYNC_MS; _timeout takes its own limit for the few that take
 * seconds by nature (a Wi-Fi scan). */
int      lp_run_full(const char *const *argv, const char *in,
                     char **out, char **err);
int      lp_run_full_timeout(const char *const *argv, const char *in,
                             char **out, char **err, guint ms);
/* Asynchronous. `owner` is the widget the result is for: if it has been
 * destroyed by the time the command finishes (the person moved to another
 * panel), the callback is not called at all. owner may be NULL. There is
 * no limit - an administrator's job may take minutes - except with
 * _timeout, for commands that can wait forever on a service that is not
 * there (bluetoothctl without bluetoothd, pw-play into a dummy output):
 * after `ms` the command is killed and done() gets LP_RUN_TIMEOUT. */
void     lp_run_async(const char *const *argv, const char *in,
                      GtkWidget *owner, lp_done_fn done, gpointer data);
void     lp_run_async_timeout(const char *const *argv, const char *in, guint ms,
                              GtkWidget *owner, lp_done_fn done, gpointer data);
/* "/usr/bin/NAME" when the Debian base has a program by that name, else
 * NAME: for the few whose LP /bin namesake, first on PATH, behaves
 * differently (lsblk, apt, passwd; sys.c says how). */
const char *lp_base_tool(const char *name);
/* How many lp_run_async commands have not answered yet. */
int      lp_jobs_pending(void);
/* Fire and forget - for daemons (wlsunset) and apps (lp-disks). */
gboolean lp_spawn_bg(const char *const *argv);

/* As administrator. uid 0 runs it directly. Otherwise through sudo:
 * `sudo -n` first, which succeeds silently inside sudo's timestamp; if
 * sudo wants a password, a dialog asks for it (`why` says what for) and
 * `sudo -S -v` checks it before the command runs, so that a wrong password
 * can never be answered with the command's own stdin. */
void     lp_admin_run(const char *const *argv, const char *in,
                      const char *why, GtkWidget *owner,
                      lp_done_fn done, gpointer data);
/* Replace a root-owned file atomically and durably (tee, sync, mv, sync
 * of the directory, all under one password). */
void     lp_admin_write_file(const char *dest, const char *body,
                             const char *why, GtkWidget *owner,
                             lp_done_fn done, gpointer data);
/* TRUE while a password checked in this window still counts (5 min). */
gboolean lp_admin_fresh(void);
/* Is the account running this a member of the sudo group (or root)? */
gboolean lp_is_admin(void);
/* For live controls (sliders): at most one command per key running and
 * one waiting; a newer argv replaces the waiting one. Failures go to the
 * banner. */
void     lp_run_latest(const char *key, const char *const *argv);
/* Run fn(data) once `key` has had no newer request for `ms`: for
 * sliders whose value is a line in a file. data is freed with free_fn. */
void     lp_later(const char *key, guint ms, void (*fn)(gpointer), gpointer data,
                  GDestroyNotify free_fn);
/* First line of err, or of out, for the banner. */
char    *lp_first_line(const char *err, const char *out);

/* ── files ──────────────────────────────────────────────────────────── */

char    *lp_slurp(const char *path);             /* whole file, chomped */
/* Atomic and durable (temp file, fsync, rename, fsync of the directory -
 * COMMON.md Persistence). An existing file keeps its mode; a new one is
 * 0644, or `mode` when that is not -1. */
gboolean lp_write_file(const char *path, const char *body);
gboolean lp_write_file_mode(const char *path, const char *body, int mode);
/* unlink, then fsync the directory, so the removal is durable too. */
gboolean lp_remove_file(const char *path);
char    *lp_config_path(const char *name);       /* ~/.config/lp/<name> */

/* key=value files (~/.config/lp/power.conf and friends). Comments and
 * unknown keys are kept on rewrite - other programs own keys in some of
 * these files too. */
char    *kv_get(const char *path, const char *key);
int      kv_get_int(const char *path, const char *key, int dflt);
gboolean kv_set(const char *path, const char *key, const char *value);

/* INI files: wayfire.ini, GTK's settings.ini. Only the one key is touched;
 * comments, order and every other section stay as they were, because
 * wayfire.ini is also edited by hand and by the desktop-shell track. */
char    *ini_get(const char *path, const char *section, const char *key);
gboolean ini_set(const char *path, const char *section, const char *key,
                 const char *value);
gboolean ini_unset(const char *path, const char *section, const char *key);
gboolean ini_drop_section(const char *path, const char *section);
char   **ini_sections(const char *path);         /* NULL-terminated, g_strfreev */
char   **ini_keys(const char *path, const char *section);

/* wayfire's config: $WAYFIRE_CONFIG_FILE, else ~/.config/wayfire.ini.
 * Wayfire watches it and applies a change on its own, so writing it is
 * both the persistent record and, for input and output options, the live
 * change. */
char    *wayfire_ini(void);

char    *lp_human(guint64 bytes);

/* ── the rest of the desktop ────────────────────────────────────────────
 *
 * Our files are the record of every setting; these mirror a value into
 * the places other programs read it from. */

/* gsettings set SCHEMA KEY VALUE, in the background. libadwaita apps and
 * GTK 4 through the portal read org.gnome.desktop.interface; without a
 * session bus it fails, quietly (stderr), and our file still holds the
 * value for `lp-settings --restore` to try again at the next login. */
void     lp_gsettings_set(const char *schema, const char *key, const char *value);
/* [Settings] KEY=VALUE in both ~/.config/gtk-3.0/settings.ini and
 * gtk-4.0/settings.ini (NULL removes it); GTK apps started later read
 * them. The value of the gtk-4.0 file, or NULL. */
gboolean lp_gtk_settings_set(const char *key, const char *value);
char    *lp_gtk_settings_get(const char *key);
/* Under sway (SWAYSOCK set) some changes can be applied live with
 * swaymsg as well as written to wayfire.ini for the session the image
 * ships. Runs in the background; errors go to stderr. */
gboolean lp_sway(void);
void     lp_swaymsg(const char *const *args);
/* Is a program on PATH? */
gboolean lp_have(const char *prog);

/* ── JSON ───────────────────────────────────────────────────────────────
 *
 * lp-net and lp-tune answer in JSON and the base has no json-glib. This is
 * a small reader, enough for their shapes: objects, arrays, strings,
 * numbers, true/false/null. */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;
typedef struct jnode jnode_t;
struct jnode {
    jtype_t   type;
    double    num;
    gboolean  b;
    char     *str;
    char     *key;          /* when a member of an object */
    jnode_t  *child;        /* first element / member */
    jnode_t  *next;
};

jnode_t    *json_parse(const char *text);        /* NULL on malformed input */
void        json_free(jnode_t *n);
jnode_t    *json_get(const jnode_t *obj, const char *key);
/* "wifi.ssid" style paths into nested objects. */
const char *json_str(const jnode_t *obj, const char *path, const char *dflt);
double      json_num(const jnode_t *obj, const char *path, double dflt);
gboolean    json_bool(const jnode_t *obj, const char *path, gboolean dflt);

/* ── the test driver ────────────────────────────────────────────────── */

/* LP_SETTINGS_SCRIPT=<file> makes the app run a script of steps against
 * its own widgets (see driver.c). Returns FALSE when not set. */
gboolean lp_driver_start(void);

#endif
