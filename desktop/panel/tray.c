/*
 * tray.c - what is running without a window, in the top bar. See tray.h.
 *
 *   [LP] Files          Sat Sep 27  23:47        [⌨] [♫] [✉] [▾ ◖ ▮ ⏻]
 *                                                     ^^^^^^^^ these
 *
 * Closing the last window of some applications does not end them: a
 * music player keeps playing, a chat program keeps listening, a
 * download finishes. That is often what the person wanted. What was
 * wrong is that nothing on the screen then said so - on a machine with
 * 512 MB an application nobody can see is memory and battery gone for
 * a reason nobody can find, and the only way to stop it was Task
 * Manager. Here each one gets a button beside the keyboard's: a tap
 * brings it back, a hold or a right click opens its menu, and the menu
 * always ends in 종료 (Quit).
 *
 * Two kinds of program are found two ways.
 *
 * ── tray icons: StatusNotifierItem ──
 *
 * The X11 tray (XEmbed) puts another program's window inside the
 * panel's, and Wayland has nothing like it. What replaced it is
 * StatusNotifierItem, which is D-Bus only: the application exports an
 * object with an icon, a title, a status and a menu, and tells the
 * *watcher* (org.kde.StatusNotifierWatcher) that it exists; *hosts* -
 * whatever draws the icons - ask the watcher for the list. KDE,
 * AppIndicator, Qt, Electron and Chromium all speak it.
 *
 * On a KDE desktop plasma owns the watcher. Here nothing else would,
 * and an application that finds no watcher decides there is no tray
 * and either shows nothing or will not let its window close. So the
 * bar is the watcher as well as the host. If the name is already taken
 * - a tool started by hand, another shell - the bar does not fight for
 * it: it registers there as a host and draws that watcher's list, and
 * stays queued for the name, so that if the other one leaves the bar
 * takes over, and the applications, which watch the watcher's name,
 * register again by themselves.
 *
 * An item is read again, whole, whenever it says something changed
 * (NewIcon, NewTitle, NewStatus, NewToolTip). A burst of those costs
 * one read in flight and one after it, never a queue. Status "Passive"
 * means "nothing to say just now" and hides the button.
 *
 * ── its menu, read ahead ──
 *
 * An item's menu lives in the application (com.canonical.dbusmenu). It
 * is read when the item appears and again whenever the application
 * says it changed, and a press builds the GtkMenu from that copy at
 * once. Asking the application at the moment of the press would put
 * its reply between the finger and the menu - and the application that
 * is not answering is exactly the one somebody opens the menu to quit.
 * 종료 is ours, under the application's items, and waits for nothing.
 *
 * ── applications without a tray icon: /proc ──
 *
 * Most programs speak no tray protocol at all. For those the bar
 * compares two lists, every five seconds and a second after the window
 * list changes: the person's own processes, matched to an installed
 * application by the program its Exec line starts, and the windows the
 * compositor reports (wlr-foreign-toplevel, where the dock's dots come
 * from). An application with a process and no window, that had a
 * window while this process ran, is in the background: it is a window
 * that was closed, and nothing else is. Left out:
 *
 *   - whatever never had a window the bar could match to it: daemons,
 *     helpers, the on-screen keyboard - when in doubt, no button;
 *   - the session's own processes: lp-*, the input method,
 *     notifications, sound, portals, the compositor, the bus - and
 *     anything under the loops lp-shell-start keeps running;
 *   - settings tools, accessibility tools, anything with a keyboard or
 *     input-method icon, and terminals (their windows are shells);
 *   - anything under a shell in a terminal: a program started from a
 *     prompt belongs to the terminal's window, not to the tray;
 *   - a process that is a tray icon already, or is under one (the bus
 *     says which process owns a connection): one application, one
 *     button;
 *   - a process younger than five seconds: an application that has just
 *     been started has no window *yet*, and a button that flashed up on
 *     every launch would be noise.
 *
 * The installed applications are read once, and again only when
 * GAppInfoMonitor says they changed. Of the processes, only the
 * person's own are read beyond a stat().
 *
 * ── quitting ──
 *
 * 종료 sends SIGTERM - what closing a window does, asked of a process
 * that no longer has one - to the item's process (the one that owns its
 * bus connection) or to every process of the application. Anything
 * still there three seconds later gets a question in an ordinary dialog
 * window, 응답하지 않습니다. 강제로 끝낼까요?, and SIGKILL only on
 * 강제 종료. A signal goes only to a process whose real and effective
 * uid are the person's own, checked again just before it is sent, and
 * only while the process's start time is the one it had when 종료 was
 * pressed: three seconds is long enough for a PID to be handed to
 * something else.
 */
#define _GNU_SOURCE 1

#include "tray.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <gdk/gdkwayland.h>
#include <gio/gdesktopappinfo.h>
#include <wayland-client.h>

#include "lp-apps.h"
#include "lp-shell.h"
#include "lp-toplevel.h"

#define ICON_PX         18
#define SCAN_EVERY_S    5       /* the look at /proc when nothing prompts it */
#define SCAN_AFTER_MS   1000    /* ... and this long after the windows changed */
#define SETTLE_S        5.0     /* younger than this, its window may be coming */
#define TERM_WAIT_MS    3000    /* SIGTERM, then this long before asking */
#define MENU_DEPTH      3       /* submenus deeper than this are not drawn */

#define WATCHER_NAME    "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH    "/StatusNotifierWatcher"
#define WATCHER_IFACE   "org.kde.StatusNotifierWatcher"
#define ITEM_IFACE      "org.kde.StatusNotifierItem"
#define ITEM_PATH       "/StatusNotifierItem"
#define MENU_IFACE      "com.canonical.dbusmenu"

typedef enum { KIND_SNI, KIND_APP } Kind;

typedef struct {
    Kind kind;
    char *key;              /* SNI: "<bus name><object path>"; "app:<desktop id>" */
    guint version;          /* bumped whenever its buttons need drawing again */
    GDesktopAppInfo *app;   /* the application; for a tray icon a guess, or NULL */

    /* KIND_SNI */
    char *bus, *path;
    guint watch;            /* its bus name: gone means the item is gone */
    guint sig;              /* NewIcon, NewTitle, ... */
    GCancellable *cancel;   /* every call in flight for it; cancelled when it goes */
    gboolean loaded;        /* its properties have been read once */
    gboolean gone;          /* its name vanished; dropped on the next idle */
    gboolean reading, again;
    guint32 pid;            /* its connection's process; 0 until known */
    char *icon_name, *attn_name, *theme_path;
    GdkPixbuf *pixmap, *attn_pixmap;
    char *title, *tip, *status, *id, *menu;
    gboolean item_is_menu;

    /* its menu: the path followed, and the last layout read from it */
    char *menu_sub;
    guint menu_sig;
    GVariant *layout;       /* (ia{sv}av) */
    gboolean menu_reading, menu_again;
} Item;

static GList *items;            /* in the order they appeared */
static GHashTable *by_key;      /* key -> Item */
static GList *boxes;            /* every lp_tray_new() box still alive */
static guint sync_id;

static GDBusConnection *bus;
static enum { W_NONE, W_OURS, W_OTHER } watcher;
static char *host_name;
static guint other_sig;         /* the other watcher's signals */
static guint other_gen;         /* bumped whenever the watcher changes hands */

static gboolean can_see_windows;    /* the compositor reports windows */
static gboolean toplevels_seen;     /* ... and the bar has passed them on */
static guint scan_soon_id;
static GHashTable *exe_apps;    /* program name -> GDesktopAppInfo */
static GHashTable *id_apps;     /* window app_id -> GDesktopAppInfo, or NULL */
static gboolean apps_stale = TRUE;
static GHashTable *quitting;    /* key -> Quit */
static GHashTable *had_window;  /* desktop ids seen with a window, while they run */

static void boxes_sync(void);
static void scan_later(void);
static void menu_read(Item *it);

/* ── small things ────────────────────────────────────────────────── */

static void unref0(gpointer p)
{
    if (p)
        g_object_unref(p);
}

static void free_pixels(guchar *p, gpointer d)
{
    (void)d;
    g_free(p);
}

static void free_data(gpointer d, GClosure *c)
{
    (void)c;
    g_free(d);
}

/* An empty string is no string. TRUE when the slot changed. */
static gboolean set_str(char **slot, const char *v)
{
    if (v && !*v)
        v = NULL;
    if (g_strcmp0(*slot, v) == 0)
        return FALSE;
    g_free(*slot);
    *slot = g_strdup(v);
    return TRUE;
}

static gboolean in_list(const char *s, const char *const *list)
{
    for (; s && *list; list++)
        if (strcmp(s, *list) == 0)
            return TRUE;
    return FALSE;
}

static void tables(void)
{
    if (by_key)
        return;
    by_key = g_hash_table_new(g_str_hash, g_str_equal);
    exe_apps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    id_apps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, unref0);
    quitting = g_hash_table_new(g_str_hash, g_str_equal);
    had_window = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

/* ── names ───────────────────────────────────────────────────────── */

/* The session's own. A name only matters here if it is also some
 * installed application's program - fcitx5 ships a .desktop file, and
 * would otherwise be "in the background" for as long as the session
 * lasts. */
static const char *const session_prefixes[] = {
    "lp-", "pipewire", "xdg-", "dbus-", "gvfs", "at-spi", "sway", "fcitx",
    "ibus", "systemd", "gnome-keyring", "gsd-", "evolution-", "tracker",
    NULL
};
static const char *const session_names[] = {
    "mako", "wireplumber", "wayfire", "Xwayland", "dconf-service",
    "kanshi", "ssh-agent", "gpg-agent", "(sd-pam)", "udevadm", "polkit",
    "lxpolkit", "wl-paste", "wl-copy", "wlsunset", "gammastep", "swaync",
    "dunst", "waybar", "onboard", "squeekboard", "florence", NULL
};

/* Categories of the session's own parts and of the tools that set it
 * up: a settings window left running is the settings tool, not an
 * application somebody is using. */
static const char *const session_categories[] = {
    "Settings", "DesktopSettings", "HardwareSettings", "Accessibility",
    "TerminalEmulator", "X-GNOME-Settings-Panel", "X-XFCE-SettingsDialog",
    "X-LXQt", "X-KDE-settings-hardware", NULL
};

/* Icons of keyboards and input methods: whatever draws one of these
 * is the on-screen keyboard, the input method or its settings. */
static const char *const session_icon_prefixes[] = {
    "input-keyboard", "input-method", "accessories-on-screen-keyboard",
    "preferences-", "fcitx", "ibus", "lp-", NULL
};

static const char *const shells[] = {
    "sh", "bash", "dash", "zsh", "fish", "ksh", "mksh", "tcsh", "csh",
    "lpsh", NULL
};

static const char *const terminals[] = {
    "foot", "footclient", "kgx", "gnome-terminal-server", "xterm",
    "uxterm", "alacritty", "kitty", "konsole", "xfce4-terminal",
    "lxterminal", "mate-terminal", "tilix", "terminator", "sakura",
    "urxvt", "rxvt", "st", "wezterm-gui", "qterminal", NULL
};

/* Programs whose first argument that is not an option names the
 * program that is really running. */
static const char *const interpreters[] = {
    "python", "python2", "python3", "perl", "ruby", "node", "nodejs",
    "gjs", "lua", "java", "mono", "tclsh", "wish", NULL
};

/* Programs that start another in a way /proc cannot follow back: an
 * application launched through one of these is not looked for. */
static const char *const launchers[] = {
    "flatpak", "snap", "sudo", "pkexec", "gksu", "xdg-open", "gio",
    "gtk-launch", "exo-open", "systemd-run", "bwrap", "firejail", "wine",
    NULL
};

static gboolean has_prefix_in(const char *s, const char *const *list)
{
    for (; s && *list; list++)
        if (g_str_has_prefix(s, *list))
            return TRUE;
    return FALSE;
}

static gboolean is_session_name(const char *n)
{
    return n && (has_prefix_in(n, session_prefixes) || in_list(n, session_names));
}

static gboolean is_interpreter(const char *n)
{
    return n && (in_list(n, interpreters) || g_str_has_prefix(n, "python2.") ||
                 g_str_has_prefix(n, "python3.") || g_str_has_prefix(n, "lua5"));
}

/* The name an application's processes run under: the program its Exec
 * line starts, after any `env VAR=...`, and for an interpreter the
 * script it is handed. NULL when /proc could not tell. */
static char *exec_key(GAppInfo *ai)
{
    const char *cl = g_app_info_get_commandline(ai);
    char **argv = NULL;
    int argc = 0;
    if (!cl || !g_shell_parse_argv(cl, &argc, &argv, NULL))
        return NULL;
    int i = 0;
    char *name = g_path_get_basename(argv[0]);
    if (strcmp(name, "env") == 0) {
        for (i = 1; i < argc && (strchr(argv[i], '=') || argv[i][0] == '-'); i++) {
        }
        g_free(name);
        name = i < argc ? g_path_get_basename(argv[i]) : NULL;
    }
    char *key = NULL;
    if (!name || in_list(name, launchers)) {
        /* nothing to follow */
    } else if (in_list(name, shells)) {
        /* `sh script` names a program; `sh -c '...'` is a command line. */
        if (i + 1 < argc && argv[i + 1][0] != '-' && argv[i + 1][0] != '%')
            key = g_path_get_basename(argv[i + 1]);
    } else if (is_interpreter(name)) {
        for (i++; i < argc && argv[i][0] == '-'; i++) {
        }
        if (i < argc && argv[i][0] != '%')
            key = g_path_get_basename(argv[i]);
    } else {
        key = g_steal_pointer(&name);
    }
    g_free(name);
    g_strfreev(argv);
    return key;
}

/* ── the installed applications ──────────────────────────────────── */

static void on_apps_changed(GAppInfoMonitor *m, gpointer d)
{
    (void)m; (void)d;
    apps_stale = TRUE;
}

/* id is <name>.desktop, in any case. */
static gboolean id_is(GDesktopAppInfo *d, const char *name)
{
    const char *id = g_app_info_get_id(G_APP_INFO(d));
    size_t n = strlen(name);
    return id && g_ascii_strncasecmp(id, name, n) == 0 &&
           strcmp(id + n, ".desktop") == 0;
}

/* Whether an installed application can be one "in the background" at
 * all. When in doubt, no: a button for a part of the session - the
 * on-screen keyboard, the input method, a settings tool - is worse than
 * no button for an application. */
static gboolean app_may_background(GDesktopAppInfo *d, const char *key)
{
    /* A program with no entry in the app grid is a part of something
     * else; one that runs in a terminal has the terminal's window. */
    if (!key || is_session_name(key) || in_list(key, terminals) ||
        in_list(key, shells) || is_interpreter(key) ||
        g_desktop_app_info_get_nodisplay(d) ||
        g_desktop_app_info_get_boolean(d, "Terminal"))
        return FALSE;
    const char *id = g_app_info_get_id(G_APP_INFO(d));
    if (!id || g_str_has_prefix(id, "lp-") || g_str_has_prefix(id, "org.fcitx.") ||
        g_str_has_prefix(id, "org.freedesktop.IBus"))
        return FALSE;
    /* Started with the session, and started again if it ends. */
    if (g_desktop_app_info_has_key(d, "X-GNOME-Autostart-Phase") ||
        g_desktop_app_info_has_key(d, "X-GNOME-AutoRestart") ||
        g_desktop_app_info_has_key(d, "X-KDE-autostart-phase"))
        return FALSE;
    const char *cats = g_desktop_app_info_get_categories(d);
    if (cats) {
        char **c = g_strsplit(cats, ";", -1);
        gboolean bad = FALSE;
        for (char **p = c; *p && !bad; p++)
            bad = **p && has_prefix_in(*p, session_categories);
        g_strfreev(c);
        if (bad)
            return FALSE;
    }
    GIcon *gi = g_app_info_get_icon(G_APP_INFO(d));
    if (G_IS_THEMED_ICON(gi)) {
        const char *const *names = g_themed_icon_get_names(G_THEMED_ICON(gi));
        for (; names && *names; names++)
            if (has_prefix_in(*names, session_icon_prefixes))
                return FALSE;
    }
    return TRUE;
}

static void apps_load(void)
{
    if (!apps_stale)
        return;
    apps_stale = FALSE;
    g_hash_table_remove_all(exe_apps);
    g_hash_table_remove_all(id_apps);
    GList *all = g_app_info_get_all();
    for (GList *l = all; l; l = l->next) {
        if (!G_IS_DESKTOP_APP_INFO(l->data))
            continue;
        GDesktopAppInfo *d = l->data;
        char *key = exec_key(G_APP_INFO(d));
        if (!app_may_background(d, key)) {
            g_free(key);
            continue;
        }
        /* Two entries that run one program (a browser and its private
         * window): the one named after the program wins. */
        GDesktopAppInfo *cur = g_hash_table_lookup(exe_apps, key);
        if (!cur || (!id_is(cur, key) && id_is(d, key)))
            g_hash_table_replace(exe_apps, key, g_object_ref(d));
        else
            g_free(key);
    }
    g_list_free_full(all, g_object_unref);
}

/* The application a window's app_id belongs to, remembered: past the
 * two quick guesses lp_app_for_id reads every .desktop file. */
static GDesktopAppInfo *app_for_id(const char *app_id)
{
    if (!app_id || !*app_id)
        return NULL;
    apps_load();
    gpointer d;
    if (!g_hash_table_lookup_extended(id_apps, app_id, NULL, &d)) {
        d = lp_app_for_id(app_id);
        g_hash_table_insert(id_apps, g_strdup(app_id), d);
    }
    return d;
}

static gboolean app_has_window(GDesktopAppInfo *app)
{
    const char *id = g_app_info_get_id(G_APP_INFO(app));
    for (GList *l = lp_toplevels(); l; l = l->next) {
        LpToplevel *t = l->data;
        if (!t->app_id || !*t->app_id)
            continue;
        /* The desktop id, StartupWMClass or program name, or whatever
         * the dock would have matched the window to. */
        if (lp_app_owns(app, t->app_id))
            return TRUE;
        GDesktopAppInfo *o = app_for_id(t->app_id);
        if (o && g_strcmp0(g_app_info_get_id(G_APP_INFO(o)), id) == 0)
            return TRUE;
    }
    return FALSE;
}

/* ── processes ───────────────────────────────────────────────────── */

typedef struct {
    int pid, ppid;
    char state;
    guint64 start;              /* clock ticks after boot */
    char *exe, *argv0, *script; /* base names; any may be NULL */
} Proc;

static void proc_free(gpointer d)
{
    Proc *p = d;
    g_free(p->exe);
    g_free(p->argv0);
    g_free(p->script);
    g_free(p);
}

static gssize read_small(const char *path, char *buf, gsize size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    gssize n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return n;
}

/* State, parent and start time. The command name in parentheses may
 * hold spaces and parentheses itself, so the fields are counted from
 * the last ')'. */
static gboolean read_stat(int pid, char *state, int *ppid, guint64 *start)
{
    char path[64], buf[1024];
    g_snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (read_small(path, buf, sizeof buf) <= 0)
        return FALSE;
    char *p = strrchr(buf, ')');
    if (!p || p[1] != ' ')
        return FALSE;
    char *save = NULL;
    int field = 3;
    for (char *t = strtok_r(p + 2, " ", &save); t; t = strtok_r(NULL, " ", &save), field++) {
        if (field == 3)
            *state = t[0];
        else if (field == 4)
            *ppid = atoi(t);
        else if (field == 22) {
            *start = g_ascii_strtoull(t, NULL, 10);
            return TRUE;
        }
    }
    return FALSE;
}

/* TRUE when pid is a live process whose real and effective uid are
 * ours, and is not this one; *start is its start time. Asked again
 * before every signal. */
static gboolean proc_ours(int pid, guint64 *start)
{
    if (pid <= 1 || pid == getpid())
        return FALSE;
    char path[64], buf[4096];
    g_snprintf(path, sizeof path, "/proc/%d/status", pid);
    if (read_small(path, buf, sizeof buf) <= 0)
        return FALSE;
    const char *u = strstr(buf, "\nUid:");
    unsigned long ruid, euid;
    if (!u || sscanf(u + 5, "%lu %lu", &ruid, &euid) != 2 ||
        ruid != (unsigned long)getuid() || euid != (unsigned long)getuid())
        return FALSE;
    char state = 0;
    int ppid = 0;
    if (!read_stat(pid, &state, &ppid, start))
        return FALSE;
    return state != 'Z' && state != 'X';
}

static Proc *proc_read(int pid)
{
    Proc *p = g_new0(Proc, 1);
    p->pid = pid;
    if (!read_stat(pid, &p->state, &p->ppid, &p->start) ||
        p->state == 'Z' || p->state == 'X') {
        proc_free(p);
        return NULL;
    }
    char path[64], buf[4096];
    g_snprintf(path, sizeof path, "/proc/%d/exe", pid);
    ssize_t n = readlink(path, buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        /* The program was replaced under it (an upgrade): still itself. */
        if (g_str_has_suffix(buf, " (deleted)"))
            buf[n - 10] = '\0';
        p->exe = g_path_get_basename(buf);
    }
    g_snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    gssize len = read_small(path, buf, sizeof buf);
    if (len > 0 && buf[0]) {
        /* A process that rewrote its title into one string ("chromium
         * --type=renderer ...") is cut at the first space. */
        if ((gssize)strlen(buf) + 1 >= len) {
            char *sp = strchr(buf, ' ');
            if (sp)
                *sp = '\0';
        }
        p->argv0 = g_path_get_basename(buf);
        gboolean sh = in_list(p->exe, shells) || in_list(p->argv0, shells);
        if (sh || is_interpreter(p->exe) || is_interpreter(p->argv0)) {
            /* The same rule as exec_key, so the two sides agree. */
            const char *a = buf + strlen(buf) + 1;
            while (!sh && a < buf + len && *a == '-')
                a += strlen(a) + 1;
            if (a < buf + len && *a && *a != '-')
                p->script = g_path_get_basename(a);
        }
    }
    return p;
}

/* The person's processes, pid -> Proc. */
static GHashTable *procs_read(void)
{
    GHashTable *t = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                          NULL, proc_free);
    GDir *d = g_dir_open("/proc", 0, NULL);
    if (!d)
        return t;
    uid_t me = getuid();
    const char *e;
    while ((e = g_dir_read_name(d))) {
        if (!g_ascii_isdigit(e[0]))
            continue;
        char path[64];
        struct stat st;
        g_snprintf(path, sizeof path, "/proc/%s", e);
        if (stat(path, &st) != 0 || st.st_uid != me)
            continue;
        Proc *p = proc_read(atoi(e));
        if (p)
            g_hash_table_insert(t, GINT_TO_POINTER(p->pid), p);
    }
    g_dir_close(d);
    return t;
}

static GDesktopAppInfo *proc_app(Proc *p)
{
    const char *names[] = { p->exe, p->argv0, p->script };
    /* A terminal is never "in the background": its windows are the
     * person's shells, and what runs in them is theirs to end. */
    for (int i = 0; i < 3; i++)
        if (is_session_name(names[i]) || in_list(names[i], terminals))
            return NULL;
    for (int i = 0; i < 3; i++) {
        GDesktopAppInfo *a = names[i] ? g_hash_table_lookup(exe_apps, names[i]) : NULL;
        if (a)
            return a;
    }
    return NULL;
}

static gboolean proc_is(Proc *p, const char *const *list)
{
    return in_list(p->exe, list) || in_list(p->argv0, list);
}

static gboolean proc_is_lp(Proc *p)
{
    return (p->exe && g_str_has_prefix(p->exe, "lp-")) ||
           (p->argv0 && g_str_has_prefix(p->argv0, "lp-")) ||
           (p->script && g_str_has_prefix(p->script, "lp-"));
}

/* Under a terminal, or under a shell that a terminal or an LP program
 * started (the loops in lp-shell-start are such shells). Only the
 * person's own processes are in `procs`, so the walk stops where the
 * session's ancestry leaves them. */
static gboolean in_terminal(GHashTable *procs, Proc *p)
{
    Proc *a = g_hash_table_lookup(procs, GINT_TO_POINTER(p->ppid));
    for (int depth = 0; a && depth < 32; depth++) {
        if (proc_is(a, terminals))
            return TRUE;
        Proc *up = g_hash_table_lookup(procs, GINT_TO_POINTER(a->ppid));
        if (up && proc_is(a, shells) && (proc_is(up, terminals) || proc_is_lp(up)))
            return TRUE;
        a = up;
    }
    return FALSE;
}

static gboolean under(GHashTable *procs, Proc *p, GHashTable *pids)
{
    for (int depth = 0; p && depth < 32; depth++) {
        if (g_hash_table_contains(pids, GINT_TO_POINTER(p->pid)))
            return TRUE;
        p = g_hash_table_lookup(procs, GINT_TO_POINTER(p->ppid));
    }
    return FALSE;
}

/* The processes that may be an application in the background, by
 * application: desktop id -> GPtrArray of Proc (not owned). An
 * application that is a tray icon is left out whole: its other
 * processes run the same program. */
static GHashTable *candidates(GHashTable *procs)
{
    GHashTable *sni = g_hash_table_new(NULL, NULL);
    GHashTable *veto = g_hash_table_new(g_str_hash, g_str_equal);
    for (GList *l = items; l; l = l->next) {
        Item *it = l->data;
        if (it->kind != KIND_SNI)
            continue;
        if (it->pid)
            g_hash_table_add(sni, GINT_TO_POINTER((int)it->pid));
        if (it->app)
            g_hash_table_add(veto, (gpointer)g_app_info_get_id(G_APP_INFO(it->app)));
    }
    GHashTable *by_app = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                               (GDestroyNotify)g_ptr_array_unref);
    GHashTableIter hi;
    gpointer k, v;
    g_hash_table_iter_init(&hi, procs);
    while (g_hash_table_iter_next(&hi, NULL, &v)) {
        Proc *p = v;
        GDesktopAppInfo *app = proc_app(p);
        const char *id = app ? g_app_info_get_id(G_APP_INFO(app)) : NULL;
        if (!id || in_terminal(procs, p))
            continue;
        if (under(procs, p, sni)) {
            g_hash_table_add(veto, (gpointer)id);
            continue;
        }
        GPtrArray *a = g_hash_table_lookup(by_app, id);
        if (!a) {
            a = g_ptr_array_new();
            g_hash_table_insert(by_app, (gpointer)id, a);
        }
        g_ptr_array_add(a, p);
    }
    g_hash_table_iter_init(&hi, veto);
    while (g_hash_table_iter_next(&hi, &k, NULL))
        g_hash_table_remove(by_app, k);
    g_hash_table_unref(sni);
    g_hash_table_unref(veto);
    return by_app;
}

static double uptime_now(void)
{
    char buf[128];
    return read_small("/proc/uptime", buf, sizeof buf) > 0 ? g_ascii_strtod(buf, NULL) : 0;
}

/* ── items ───────────────────────────────────────────────────────── */

static void watcher_emit(const char *signal, const char *arg)
{
    if (bus && watcher != W_OTHER)
        g_dbus_connection_emit_signal(bus, NULL, WATCHER_PATH, WATCHER_IFACE, signal,
                                      arg ? g_variant_new("(s)", arg) : NULL, NULL);
}

static void item_free(Item *it)
{
    if (it->cancel) {
        g_cancellable_cancel(it->cancel);
        g_object_unref(it->cancel);
    }
    if (it->watch)
        g_bus_unwatch_name(it->watch);
    if (bus && it->sig)
        g_dbus_connection_signal_unsubscribe(bus, it->sig);
    if (bus && it->menu_sig)
        g_dbus_connection_signal_unsubscribe(bus, it->menu_sig);
    g_free(it->key);
    g_free(it->bus);
    g_free(it->path);
    g_free(it->icon_name);
    g_free(it->attn_name);
    g_free(it->theme_path);
    g_free(it->title);
    g_free(it->tip);
    g_free(it->status);
    g_free(it->id);
    g_free(it->menu);
    g_free(it->menu_sub);
    g_clear_object(&it->pixmap);
    g_clear_object(&it->attn_pixmap);
    g_clear_object(&it->app);
    if (it->layout)
        g_variant_unref(it->layout);
    g_free(it);
}

static void item_insert(Item *it)
{
    items = g_list_append(items, it);
    g_hash_table_insert(by_key, it->key, it);
    boxes_sync();
}

static void item_drop(Item *it)
{
    items = g_list_remove(items, it);
    g_hash_table_remove(by_key, it->key);
    if (it->kind == KIND_SNI)
        watcher_emit("StatusNotifierItemUnregistered", it->key);
    item_free(it);
    boxes_sync();
}

/* A tray icon from the session itself, not from an application: the
 * input method's (fcitx5 puts a keyboard in the tray under wayfire, and
 * the bar already shows the input language as EN / 한 beside the
 * on-screen keyboard button - two keyboards side by side), or anything
 * else the session runs. Known by the item's Id and icon, and by the
 * name of the process that owns it. */
static gboolean sni_is_session(Item *it)
{
    static const char *const ids[] = { "fcitx", "Fcitx", "ibus", "IBus", NULL };
    static const char *const icons[] = { "fcitx", "input-keyboard", "input-method",
                                         "ibus", "org.fcitx", NULL };
    if (has_prefix_in(it->id, ids) || has_prefix_in(it->icon_name, icons))
        return TRUE;
    if (it->pid) {
        char *path = g_strdup_printf("/proc/%u/comm", it->pid);
        char *comm = NULL;
        gboolean session = FALSE;
        if (g_file_get_contents(path, &comm, NULL, NULL)) {
            g_strchomp(comm);
            session = is_session_name(comm);
        }
        g_free(comm);
        g_free(path);
        if (session)
            return TRUE;
    }
    return FALSE;
}

static gboolean item_shown(Item *it)
{
    if (it->kind == KIND_APP)
        return TRUE;
    return it->loaded && !it->gone && g_strcmp0(it->status, "Passive") != 0 &&
           !sni_is_session(it);
}

/* What it is called, for its tooltip and for the question about it. */
static char *item_name(Item *it)
{
    if (it->kind == KIND_SNI && it->title)
        return g_strdup(it->title);
    if (it->app)
        return g_strdup(lp_app_name(G_APP_INFO(it->app)));
    if (it->id)
        return g_strdup(it->id);
    return g_strdup(T("Application", "앱"));
}

static char *item_tip(Item *it)
{
    char *name = item_name(it), *tip;
    if (it->kind == KIND_APP)
        tip = g_strdup_printf(T("%s — running in the background",
                                "%s — 백그라운드에서 실행 중"), name);
    else if (it->tip && g_strcmp0(it->tip, name) != 0)
        tip = g_strdup_printf("%s\n%s", name, it->tip);
    else
        tip = g_strdup(name);
    g_free(name);
    return tip;
}

/* ── drawing ─────────────────────────────────────────────────────── */

/* The default theme, with an item's own icon directory added once. */
static GtkIconTheme *theme_with(const char *dir)
{
    static GHashTable *added;
    GtkIconTheme *th = gtk_icon_theme_get_default();
    if (dir) {
        if (!added)
            added = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        if (!g_hash_table_contains(added, dir)) {
            g_hash_table_add(added, g_strdup(dir));
            gtk_icon_theme_append_search_path(th, dir);
        }
    }
    return th;
}

static void image_from_name(GtkWidget *img, const char *name)
{
    gtk_image_set_from_icon_name(GTK_IMAGE(img), name, GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size(GTK_IMAGE(img), ICON_PX);
}

static void image_from_app(GtkWidget *img, GDesktopAppInfo *app)
{
    GIcon *gi = app ? g_app_info_get_icon(G_APP_INFO(app)) : NULL;
    if (!gi) {
        image_from_name(img, "application-x-executable");
        return;
    }
    gtk_image_set_from_gicon(GTK_IMAGE(img), gi, GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size(GTK_IMAGE(img), ICON_PX);
}

/* A picture at ICON_PX logical pixels, drawn at the output's scale. */
static void image_from_pixbuf(GtkWidget *img, GdkPixbuf *pb, int scale)
{
    int px = ICON_PX * scale;
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    int big = MAX(w, h);
    GdkPixbuf *sized = NULL;
    if (big != px)
        sized = gdk_pixbuf_scale_simple(pb, MAX(1, w * px / big), MAX(1, h * px / big),
                                        GDK_INTERP_BILINEAR);
    cairo_surface_t *s = gdk_cairo_surface_create_from_pixbuf(sized ? sized : pb,
                                                              scale, NULL);
    gtk_image_set_from_surface(GTK_IMAGE(img), s);
    cairo_surface_destroy(s);
    g_clear_object(&sized);
}

static void paint(GtkWidget *btn, Item *it)
{
    GtkWidget *img = gtk_bin_get_child(GTK_BIN(btn));
    int scale = gtk_widget_get_scale_factor(btn);
    if (it->kind == KIND_APP) {
        image_from_app(img, it->app);
    } else {
        /* The attention icon while it asks for attention; then a theme
         * name (or a file - some programs send a path), then the
         * item's own pixels, then its application's icon. */
        gboolean attn = g_strcmp0(it->status, "NeedsAttention") == 0;
        const char *name = attn && it->attn_name ? it->attn_name : it->icon_name;
        GdkPixbuf *pix = attn && it->attn_pixmap ? it->attn_pixmap : it->pixmap;
        GdkPixbuf *file = NULL;
        if (name && g_path_is_absolute(name))
            file = gdk_pixbuf_new_from_file_at_size(name, ICON_PX * scale,
                                                    ICON_PX * scale, NULL);
        if (file) {
            image_from_pixbuf(img, file, scale);
            g_object_unref(file);
        } else if (name && !g_path_is_absolute(name) &&
                   gtk_icon_theme_has_icon(theme_with(it->theme_path), name)) {
            image_from_name(img, name);
        } else if (pix) {
            image_from_pixbuf(img, pix, scale);
        } else {
            image_from_app(img, it->app);
        }
    }
    char *tip = item_tip(it);
    gtk_widget_set_tooltip_text(btn, tip);
    g_free(tip);
}

/* ── menus ───────────────────────────────────────────────────────── */

typedef struct {
    char *bus, *path;
    gint32 id;
} MenuAct;

static MenuAct *menu_act_new(Item *it, gint32 id)
{
    MenuAct *a = g_new0(MenuAct, 1);
    a->bus = g_strdup(it->bus);
    a->path = g_strdup(it->menu_sub);
    a->id = id;
    return a;
}

static void menu_act_free(gpointer d, GClosure *c)
{
    (void)c;
    MenuAct *a = d;
    g_free(a->bus);
    g_free(a->path);
    g_free(a);
}

static void about_to_show(const char *name, const char *path, gint32 id)
{
    if (bus && name && path)
        g_dbus_connection_call(bus, name, path, MENU_IFACE, "AboutToShow",
                               g_variant_new("(i)", id), NULL,
                               G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void on_menu_item(GtkMenuItem *mi, gpointer d)
{
    (void)mi;
    MenuAct *a = d;
    if (bus)
        g_dbus_connection_call(bus, a->bus, a->path, MENU_IFACE, "Event",
                               g_variant_new("(isvu)", a->id, "clicked",
                                             g_variant_new_int32(0),
                                             (guint32)gtk_get_current_event_time()),
                               NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* A submenu opening: the application may fill it in for next time. */
static void on_submenu_show(GtkWidget *w, gpointer d)
{
    (void)w;
    MenuAct *a = d;
    about_to_show(a->bus, a->path, a->id);
}

/* The children of one dbusmenu node, as GtkMenuItems. Returns how many
 * were added; separators that would lead, trail or double are not. */
static int menu_fill(GtkWidget *menu, Item *it, GVariant *kids, int depth)
{
    int n = 0;
    gboolean sep = FALSE;
    GVariantIter iter;
    GVariant *v;
    g_variant_iter_init(&iter, kids);
    while ((v = g_variant_iter_next_value(&iter))) {
        GVariant *node = g_variant_get_variant(v);
        g_variant_unref(v);
        if (!g_variant_is_of_type(node, G_VARIANT_TYPE("(ia{sv}av)"))) {
            g_variant_unref(node);
            continue;
        }
        gint32 id, state = -1;
        GVariant *props, *sub;
        g_variant_get(node, "(i@a{sv}@av)", &id, &props, &sub);
        gboolean visible = TRUE, enabled = TRUE;
        const char *type = NULL, *label = NULL, *toggle = NULL, *display = NULL;
        g_variant_lookup(props, "visible", "b", &visible);
        g_variant_lookup(props, "enabled", "b", &enabled);
        g_variant_lookup(props, "type", "&s", &type);
        g_variant_lookup(props, "label", "&s", &label);
        g_variant_lookup(props, "toggle-type", "&s", &toggle);
        g_variant_lookup(props, "toggle-state", "i", &state);
        g_variant_lookup(props, "children-display", "&s", &display);
        if (!visible) {
            /* skipped */
        } else if (g_strcmp0(type, "separator") == 0) {
            sep = n > 0;
        } else {
            if (sep)
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
            sep = FALSE;
            /* dbusmenu's labels use GTK's own mnemonic convention: _x
             * underlines x, __ is an underscore. */
            GtkWidget *mi;
            if (g_strcmp0(toggle, "checkmark") == 0 || g_strcmp0(toggle, "radio") == 0) {
                mi = gtk_check_menu_item_new_with_mnemonic(label ? label : "");
                gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(mi),
                                                      strcmp(toggle, "radio") == 0);
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), state == 1);
            } else {
                mi = gtk_menu_item_new_with_mnemonic(label ? label : "");
            }
            gtk_widget_set_sensitive(mi, enabled);
            gboolean parent = g_variant_n_children(sub) > 0 ||
                              g_strcmp0(display, "submenu") == 0;
            if (parent) {
                GtkWidget *sm = gtk_menu_new();
                gtk_style_context_add_class(gtk_widget_get_style_context(sm), "lp-menu");
                if (depth + 1 < MENU_DEPTH && menu_fill(sm, it, sub, depth + 1) > 0) {
                    g_signal_connect_data(sm, "show", G_CALLBACK(on_submenu_show),
                                          menu_act_new(it, id), menu_act_free, 0);
                    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), sm);
                } else {
                    /* Empty, or too deep to draw: shown, not offered. */
                    gtk_widget_destroy(sm);
                    gtk_widget_set_sensitive(mi, FALSE);
                }
            } else {
                g_signal_connect_data(mi, "activate", G_CALLBACK(on_menu_item),
                                      menu_act_new(it, id), menu_act_free, 0);
            }
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
            n++;
        }
        g_variant_unref(props);
        g_variant_unref(sub);
        g_variant_unref(node);
    }
    return n;
}

static gboolean destroy_idle(gpointer w)
{
    gtk_widget_destroy(w);
    return G_SOURCE_REMOVE;
}

/* A menu is taken down before the chosen item is activated; destroying
 * it there would take the item's handler with it. */
static void on_menu_done(GtkMenuShell *m, gpointer d)
{
    (void)d;
    g_idle_add(destroy_idle, m);
}

static void on_quit(GtkMenuItem *mi, gpointer d);

static void menu_show(Item *it, GtkWidget *w)
{
    GtkWidget *menu = gtk_menu_new();
    int n = 0;
    if (it->kind == KIND_SNI && it->layout) {
        GVariant *kids = g_variant_get_child_value(it->layout, 2);
        n = menu_fill(menu, it, kids, 0);
        g_variant_unref(kids);
        /* For next time: the application may bring its menu up to
         * date, and says so with LayoutUpdated. */
        about_to_show(it->bus, it->menu_sub, 0);
    }
    if (n)
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget *quit = gtk_menu_item_new_with_label(T("Quit", "종료"));
    g_signal_connect_data(quit, "activate", G_CALLBACK(on_quit),
                          g_strdup(it->key), free_data, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "lp-menu");
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "deactivate", G_CALLBACK(on_menu_done), NULL);
    /* Under the button, right edges together: the tray sits at the
     * right of the bar and the menu opens back towards the middle. */
    gtk_menu_popup_at_widget(GTK_MENU(menu), w, GDK_GRAVITY_SOUTH_EAST,
                             GDK_GRAVITY_NORTH_EAST, NULL);
}

/* ── the buttons ─────────────────────────────────────────────────── */

static Item *item_of(GtkWidget *btn)
{
    const char *key = g_object_get_data(G_OBJECT(btn), "lp-tray-key");
    return key && by_key ? g_hash_table_lookup(by_key, key) : NULL;
}

/* The middle of the button's bottom edge, in the output's coordinates:
 * the bar is anchored to its output's top-left corner, so its window's
 * coordinates are the output's. */
static void screen_point(GtkWidget *w, int *x, int *y)
{
    int wx = 0, wy = 0;
    gtk_widget_translate_coordinates(w, gtk_widget_get_toplevel(w),
                                     gtk_widget_get_allocated_width(w) / 2,
                                     gtk_widget_get_allocated_height(w), &wx, &wy);
    GdkRectangle g = { 0, 0, 0, 0 };
    GdkWindow *win = gtk_widget_get_window(w);
    GdkMonitor *m = win ? gdk_display_get_monitor_at_window(gtk_widget_get_display(w), win)
                        : NULL;
    if (m)
        gdk_monitor_get_geometry(m, &g);
    *x = g.x + wx;
    *y = g.y + wy;
}

static void on_tap(GtkWidget *w, gpointer d)
{
    (void)d;
    if (lp_hold_consumed(w))
        return;
    Item *it = item_of(w);
    if (!it)
        return;
    if (it->kind == KIND_APP) {
        /* Started again. A single-instance application opens a window
         * in the process that is already running - which is the point. */
        lp_app_launch(G_APP_INFO(it->app));
        return;
    }
    if (it->item_is_menu && it->layout) {
        menu_show(it, w);
        return;
    }
    if (!bus)
        return;
    int x, y;
    screen_point(w, &x, &y);
    /* No reply is waited for: an item that has no Activate answers
     * with an error nobody needs to see. */
    g_dbus_connection_call(bus, it->bus, it->path, ITEM_IFACE, "Activate",
                           g_variant_new("(ii)", x, y), NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void on_hold(GtkWidget *w, double x, double y, gpointer d)
{
    (void)x; (void)y; (void)d;
    Item *it = item_of(w);
    if (it)
        menu_show(it, w);
}

static void on_scale(GObject *o, GParamSpec *ps, gpointer d)
{
    (void)ps; (void)d;
    g_object_set_data(o, "lp-tray-painted", NULL);
    boxes_sync();
}

static GtkWidget *button_new(Item *it)
{
    GtkWidget *b = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(b), gtk_image_new());
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "lp-tray-item");
    g_object_set_data_full(G_OBJECT(b), "lp-tray-key", g_strdup(it->key), g_free);
    lp_on_tap(b, on_tap, NULL);
    lp_on_hold(b, on_hold, NULL);
    g_signal_connect(b, "notify::scale-factor", G_CALLBACK(on_scale), NULL);
    gtk_widget_show_all(b);
    return b;
}

/* One box brought in line with the items: buttons kept where they can
 * be (a button that is replaced mid-press loses the press), drawn again
 * only when their item changed, in the items' order. */
static void sync_box(GtkWidget *box)
{
    GHashTable *have = g_hash_table_new(g_str_hash, g_str_equal);
    GList *kids = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = kids; l; l = l->next)
        g_hash_table_insert(have, g_object_get_data(l->data, "lp-tray-key"), l->data);
    g_list_free(kids);
    int pos = 0;
    for (GList *l = items; l; l = l->next) {
        Item *it = l->data;
        if (!item_shown(it))
            continue;
        GtkWidget *b = g_hash_table_lookup(have, it->key);
        if (b) {
            g_hash_table_remove(have, it->key);
        } else {
            b = button_new(it);
            gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
        }
        guint painted = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "lp-tray-painted"));
        if (painted != it->version + 1) {
            paint(b, it);
            g_object_set_data(G_OBJECT(b), "lp-tray-painted",
                              GUINT_TO_POINTER(it->version + 1));
        }
        gtk_box_reorder_child(GTK_BOX(box), b, pos++);
    }
    GList *old = g_hash_table_get_values(have);
    g_hash_table_unref(have);
    g_list_free_full(old, (GDestroyNotify)gtk_widget_destroy);
    /* Empty, it takes no room - not even the bar's spacing around it. */
    gtk_widget_set_visible(box, pos > 0);
}

static gboolean sync_now(gpointer d)
{
    (void)d;
    sync_id = 0;
    for (GList *l = boxes; l; l = l->next)
        sync_box(l->data);
    return G_SOURCE_REMOVE;
}

/* Once per main-loop turn, however many items changed in it. */
static void boxes_sync(void)
{
    if (!sync_id && boxes)
        sync_id = g_idle_add(sync_now, NULL);
}

static void on_box_destroy(GtkWidget *box, gpointer d)
{
    (void)d;
    boxes = g_list_remove(boxes, box);
}

/* ── quitting ────────────────────────────────────────────────────── */

typedef struct {
    int pid;
    guint64 start;
} Victim;

typedef struct {
    char *key, *name;
    GArray *victims;
    guint timer;
    GtkWidget *dialog;
} Quit;

static gboolean victim_alive(Victim *v)
{
    guint64 start = 0;
    return proc_ours(v->pid, &start) && start == v->start;
}

static gboolean any_alive(Quit *q)
{
    for (guint i = 0; i < q->victims->len; i++)
        if (victim_alive(&g_array_index(q->victims, Victim, i)))
            return TRUE;
    return FALSE;
}

static void quit_end(Quit *q)
{
    g_hash_table_remove(quitting, q->key);
    if (q->timer)
        g_source_remove(q->timer);
    if (q->dialog) {
        g_signal_handlers_disconnect_by_data(q->dialog, q);
        gtk_widget_destroy(q->dialog);
    }
    g_free(q->key);
    g_free(q->name);
    g_array_unref(q->victims);
    g_free(q);
    /* An application's button goes with its processes. */
    scan_later();
}

/* While the question is up: if they end by themselves, it goes. */
static gboolean quit_watch(gpointer d)
{
    Quit *q = d;
    if (any_alive(q))
        return G_SOURCE_CONTINUE;
    q->timer = 0;
    quit_end(q);
    return G_SOURCE_REMOVE;
}

static void on_force(GtkDialog *dlg, int resp, gpointer d)
{
    (void)dlg;
    Quit *q = d;
    if (resp == GTK_RESPONSE_ACCEPT) {
        for (guint i = 0; i < q->victims->len; i++) {
            Victim *v = &g_array_index(q->victims, Victim, i);
            if (victim_alive(v))
                kill(v->pid, SIGKILL);
        }
    }
    quit_end(q);
}

static gboolean quit_check(gpointer d)
{
    Quit *q = d;
    q->timer = 0;
    if (!any_alive(q)) {
        quit_end(q);
        return G_SOURCE_REMOVE;
    }
    /* A window of its own, not a sheet of the bar's: it is a question
     * about an application, and waits in the window list like one. */
    GtkWidget *dlg = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                            T("%s is not responding.",
                                              "%s 앱이 응답하지 않습니다."), q->name);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s",
        T("Force it to quit? Anything it has not saved will be lost.",
          "강제로 끝낼까요? 저장하지 않은 내용은 사라집니다."));
    gtk_window_set_title(GTK_WINDOW(dlg), T("Not Responding", "응답 없음"));
    gtk_dialog_add_button(GTK_DIALOG(dlg), T("Cancel", "취소"), GTK_RESPONSE_CANCEL);
    GtkWidget *kill_btn = gtk_dialog_add_button(GTK_DIALOG(dlg), T("Force Quit", "강제 종료"),
                                                GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class(gtk_widget_get_style_context(kill_btn), "destructive-action");
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_CANCEL);
    g_signal_connect(dlg, "response", G_CALLBACK(on_force), q);
    q->dialog = dlg;
    q->timer = g_timeout_add_seconds(1, quit_watch, q);
    gtk_window_present(GTK_WINDOW(dlg));
    return G_SOURCE_REMOVE;
}

static void quit_start(const char *key, const char *name, const int *pids, guint n)
{
    if (g_hash_table_contains(quitting, key))
        return;
    Quit *q = g_new0(Quit, 1);
    q->key = g_strdup(key);
    q->name = g_strdup(name);
    q->victims = g_array_new(FALSE, FALSE, sizeof(Victim));
    for (guint i = 0; i < n; i++) {
        Victim v = { pids[i], 0 };
        if (proc_ours(v.pid, &v.start) && kill(v.pid, SIGTERM) == 0)
            g_array_append_val(q->victims, v);
    }
    if (q->victims->len == 0) {
        g_free(q->key);
        g_free(q->name);
        g_array_unref(q->victims);
        g_free(q);
        return;
    }
    g_hash_table_insert(quitting, q->key, q);
    q->timer = g_timeout_add(TERM_WAIT_MS, quit_check, q);
    scan_later();
}

typedef struct {
    char *key, *name;
} PidAsk;

static void quit_pid_done(GObject *src, GAsyncResult *res, gpointer d)
{
    PidAsk *a = d;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (r) {
        guint32 pid = 0;
        g_variant_get(r, "(u)", &pid);
        int p = (int)pid;
        quit_start(a->key, a->name, &p, 1);
        g_variant_unref(r);
    }
    g_free(a->key);
    g_free(a->name);
    g_free(a);
}

static GArray *app_pids(const char *id);

static void on_quit(GtkMenuItem *mi, gpointer d)
{
    (void)mi;
    const char *key = d;
    Item *it = g_hash_table_lookup(by_key, key);
    if (!it)
        return;
    Quit *q = g_hash_table_lookup(quitting, key);
    if (q) {
        if (q->dialog)
            gtk_window_present(GTK_WINDOW(q->dialog));
        return;
    }
    char *name = item_name(it);
    if (it->kind == KIND_APP) {
        GArray *pids = app_pids(g_app_info_get_id(G_APP_INFO(it->app)));
        quit_start(key, name, (const int *)(void *)pids->data, pids->len);
        g_array_unref(pids);
        g_free(name);
    } else if (bus) {
        /* Asked now rather than remembered: a well-known name can have
         * changed hands since the item appeared. */
        PidAsk *a = g_new0(PidAsk, 1);
        a->key = g_strdup(key);
        a->name = name;
        g_dbus_connection_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                               "org.freedesktop.DBus", "GetConnectionUnixProcessID",
                               g_variant_new("(s)", it->bus), G_VARIANT_TYPE("(u)"),
                               G_DBUS_CALL_FLAGS_NONE, -1, NULL, quit_pid_done, a);
    } else {
        g_free(name);
    }
}

/* ── tray icons ──────────────────────────────────────────────────── */

static const char *dict_str(GVariant *d, const char *k)
{
    const char *s = NULL;
    if (!g_variant_lookup(d, k, "&s", &s) && !g_variant_lookup(d, k, "&o", &s))
        s = NULL;
    return s;
}

/* Of the sizes offered, the smallest that is at least twice ICON_PX (a
 * scale-2 output), else the largest. */
static gboolean size_better(int cand, int cur)
{
    const int want = ICON_PX * 2;
    if (cur < want)
        return cand > cur;
    return cand >= want && cand < cur;
}

static GdkPixbuf *pixmap_best(GVariant *dict, const char *key)
{
    GVariant *arr = g_variant_lookup_value(dict, key, G_VARIANT_TYPE("a(iiay)"));
    if (!arr)
        return NULL;
    GVariant *best = NULL, *data;
    gint32 w, h, bw = 0, bh = 0;
    GVariantIter iter;
    g_variant_iter_init(&iter, arr);
    while (g_variant_iter_next(&iter, "(ii@ay)", &w, &h, &data)) {
        gboolean ok = w > 0 && h > 0 && w <= 1024 && h <= 1024 &&
                      g_variant_get_size(data) == (gsize)w * (gsize)h * 4;
        if (ok && size_better(MAX(w, h), MAX(bw, bh))) {
            if (best)
                g_variant_unref(best);
            best = data;
            bw = w;
            bh = h;
        } else {
            g_variant_unref(data);
        }
    }
    g_variant_unref(arr);
    if (!best)
        return NULL;
    const guchar *src = g_variant_get_data(best);
    gsize n = (gsize)bw * (gsize)bh;
    guchar *px = g_malloc(n * 4);
    /* ARGB32 in network byte order - A, R, G, B in memory - to the R,
     * G, B, A a GdkPixbuf holds. */
    for (gsize i = 0; i < n; i++) {
        px[4 * i + 0] = src[4 * i + 1];
        px[4 * i + 1] = src[4 * i + 2];
        px[4 * i + 2] = src[4 * i + 3];
        px[4 * i + 3] = src[4 * i + 0];
    }
    g_variant_unref(best);
    return gdk_pixbuf_new_from_data(px, GDK_COLORSPACE_RGB, TRUE, 8, bw, bh, bw * 4,
                                    free_pixels, NULL);
}

static GDesktopAppInfo *app_of_pid(guint32 pid)
{
    apps_load();
    Proc *p = proc_read((int)pid);
    GDesktopAppInfo *a = p ? proc_app(p) : NULL;
    if (p)
        proc_free(p);
    return a;
}

/* Which application it belongs to, for its name, a fallback icon, and
 * so that the same application is not also shown from /proc: by the
 * process that owns its connection, else by its Id. */
static void sni_guess_app(Item *it)
{
    if (it->app)
        return;
    GDesktopAppInfo *a = it->pid ? app_of_pid(it->pid) : NULL;
    if (!a)
        a = app_for_id(it->id);
    if (a) {
        it->app = g_object_ref(a);
        it->version++;
    }
}

static void on_menu_signal(GDBusConnection *c, const char *sender, const char *path,
                           const char *iface, const char *member, GVariant *params,
                           gpointer d)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)params;
    Item *it = g_hash_table_lookup(by_key, d);
    if (it && (strcmp(member, "LayoutUpdated") == 0 ||
               strcmp(member, "ItemsPropertiesUpdated") == 0))
        menu_read(it);
}

/* Follow the item's Menu property: a new path is subscribed to and read. */
static void menu_follow(Item *it)
{
    const char *p = it->menu && strcmp(it->menu, "/") != 0 ? it->menu : NULL;
    if (!bus || g_strcmp0(p, it->menu_sub) == 0)
        return;
    if (it->menu_sig)
        g_dbus_connection_signal_unsubscribe(bus, it->menu_sig);
    it->menu_sig = 0;
    if (it->layout)
        g_variant_unref(it->layout);
    it->layout = NULL;
    g_free(it->menu_sub);
    it->menu_sub = g_strdup(p);
    if (!p)
        return;
    it->menu_sig = g_dbus_connection_signal_subscribe(bus, it->bus, MENU_IFACE, NULL, p, NULL,
                                                      G_DBUS_SIGNAL_FLAGS_NONE, on_menu_signal,
                                                      g_strdup(it->key), g_free);
    /* Some applications only fill their menu in when told it is about
     * to be shown; the LayoutUpdated that follows reads it again. */
    about_to_show(it->bus, p, 0);
    menu_read(it);
}

static void menu_read_done(GObject *src, GAsyncResult *res, gpointer d)
{
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!r && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        return;                 /* the item has gone, and d with it */
    }
    Item *it = d;
    it->menu_reading = FALSE;
    if (r) {
        if (it->layout)
            g_variant_unref(it->layout);
        it->layout = g_variant_get_child_value(r, 1);
        g_variant_unref(r);
    } else {
        g_error_free(err);      /* no menu: the menu is 종료 alone */
    }
    if (it->menu_again) {
        it->menu_again = FALSE;
        menu_read(it);
    }
}

static void menu_read(Item *it)
{
    if (!bus || !it->menu_sub)
        return;
    if (it->menu_reading) {
        it->menu_again = TRUE;
        return;
    }
    it->menu_reading = TRUE;
    g_dbus_connection_call(bus, it->bus, it->menu_sub, MENU_IFACE, "GetLayout",
                           g_variant_new("(ii@as)", 0, -1, g_variant_new_strv(NULL, 0)),
                           G_VARIANT_TYPE("(u(ia{sv}av))"), G_DBUS_CALL_FLAGS_NONE,
                           5000, it->cancel, menu_read_done, it);
}

static void sni_parse(Item *it, GVariant *d)
{
    gboolean changed = !it->loaded;
    changed |= set_str(&it->icon_name, dict_str(d, "IconName"));
    changed |= set_str(&it->attn_name, dict_str(d, "AttentionIconName"));
    changed |= set_str(&it->theme_path, dict_str(d, "IconThemePath"));
    changed |= set_str(&it->title, dict_str(d, "Title"));
    changed |= set_str(&it->status, dict_str(d, "Status"));
    changed |= set_str(&it->id, dict_str(d, "Id"));
    set_str(&it->menu, dict_str(d, "Menu"));
    gboolean is_menu = FALSE;
    g_variant_lookup(d, "ItemIsMenu", "b", &is_menu);
    it->item_is_menu = is_menu;

    /* ToolTip is (icon name, icon pixmap, title, text); the title is
     * what a tooltip line can carry - the text may be markup. */
    const char *tt = NULL;
    GVariant *tip = g_variant_lookup_value(d, "ToolTip", G_VARIANT_TYPE("(sa(iiay)ss)"));
    if (tip)
        g_variant_get_child(tip, 2, "&s", &tt);
    changed |= set_str(&it->tip, tt);
    if (tip)
        g_variant_unref(tip);

    /* Pixels are compared by nothing cheaper than the pixels: new ones
     * are taken and the button drawn again. */
    g_clear_object(&it->pixmap);
    g_clear_object(&it->attn_pixmap);
    it->pixmap = pixmap_best(d, "IconPixmap");
    it->attn_pixmap = pixmap_best(d, "AttentionIconPixmap");
    if (it->pixmap || it->attn_pixmap)
        changed = TRUE;

    it->loaded = TRUE;
    sni_guess_app(it);
    if (changed)
        it->version++;
    menu_follow(it);
    boxes_sync();
}

static void sni_read(Item *it);

static void sni_read_done(GObject *src, GAsyncResult *res, gpointer d)
{
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!r && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        return;                 /* the item has gone, and d with it */
    }
    Item *it = d;
    it->reading = FALSE;
    if (r) {
        GVariant *dict = g_variant_get_child_value(r, 0);
        sni_parse(it, dict);
        g_variant_unref(dict);
        g_variant_unref(r);
    } else {
        g_printerr("lp-panel: tray: %s: %s\n", it->key, err->message);
        g_error_free(err);
    }
    if (it->again) {
        it->again = FALSE;
        sni_read(it);
    }
}

static void sni_read(Item *it)
{
    if (!bus)
        return;
    if (it->reading) {
        it->again = TRUE;
        return;
    }
    it->reading = TRUE;
    g_dbus_connection_call(bus, it->bus, it->path, "org.freedesktop.DBus.Properties",
                           "GetAll", g_variant_new("(s)", ITEM_IFACE),
                           G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 5000,
                           it->cancel, sni_read_done, it);
}

static void pid_done(GObject *src, GAsyncResult *res, gpointer d)
{
    /* On any error d is left alone: cancelled means the item has gone,
     * and d with it; otherwise the PID stays unknown. */
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (!r)
        return;
    Item *it = d;
    g_variant_get(r, "(u)", &it->pid);
    g_variant_unref(r);
    sni_guess_app(it);
    boxes_sync();
    /* The same application may be showing from /proc: not for long. */
    scan_later();
}

static void on_item_signal(GDBusConnection *c, const char *sender, const char *path,
                           const char *iface, const char *member, GVariant *params,
                           gpointer d)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)params;
    Item *it = g_hash_table_lookup(by_key, d);
    if (it && (g_str_has_prefix(member, "New") || strcmp(member, "PropertiesChanged") == 0))
        sni_read(it);
}

static gboolean drop_gone(gpointer d)
{
    Item *it = g_hash_table_lookup(by_key, d);
    if (it && it->gone)
        item_drop(it);
    return G_SOURCE_REMOVE;
}

static void on_item_vanished(GDBusConnection *c, const char *name, gpointer d)
{
    (void)c; (void)name;
    Item *it = g_hash_table_lookup(by_key, d);
    if (!it || it->gone)
        return;
    /* Dropped on the next idle, not inside the watch's own callback,
     * which dropping cancels. */
    it->gone = TRUE;
    g_idle_add_full(G_PRIORITY_DEFAULT, drop_gone, g_strdup(d), g_free);
}

/* A new item, or NULL if it is already known or not a valid address. */
static Item *sni_add(const char *name, const char *path)
{
    if (!bus || !g_dbus_is_name(name) || !g_variant_is_object_path(path))
        return NULL;
    char *key = g_strconcat(name, path, NULL);
    if (g_hash_table_contains(by_key, key)) {
        g_free(key);
        return NULL;
    }
    Item *it = g_new0(Item, 1);
    it->kind = KIND_SNI;
    it->key = key;
    it->bus = g_strdup(name);
    it->path = g_strdup(path);
    it->cancel = g_cancellable_new();
    item_insert(it);
    /* A name that has no owner is reported vanished at once: a stale
     * registration disappears by itself. */
    it->watch = g_bus_watch_name_on_connection(bus, name, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                               NULL, on_item_vanished, g_strdup(key), g_free);
    it->sig = g_dbus_connection_signal_subscribe(bus, name, NULL, NULL, path, NULL,
                                                 G_DBUS_SIGNAL_FLAGS_NONE, on_item_signal,
                                                 g_strdup(key), g_free);
    g_dbus_connection_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "GetConnectionUnixProcessID",
                           g_variant_new("(s)", name), G_VARIANT_TYPE("(u)"),
                           G_DBUS_CALL_FLAGS_NONE, -1, it->cancel, pid_done, it);
    sni_read(it);
    return it;
}

static void sni_clear(void)
{
    for (GList *l = items, *next; l; l = next) {
        next = l->next;
        if (((Item *)l->data)->kind == KIND_SNI)
            item_drop(l->data);
    }
}

/* ── the watcher: ours ───────────────────────────────────────────── */

static const char watcher_xml[] =
    "<node>"
    " <interface name='" WATCHER_IFACE "'>"
    "  <method name='RegisterStatusNotifierItem'>"
    "   <arg name='service' type='s' direction='in'/>"
    "  </method>"
    "  <method name='RegisterStatusNotifierHost'>"
    "   <arg name='service' type='s' direction='in'/>"
    "  </method>"
    "  <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
    "  <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    "  <property name='ProtocolVersion' type='i' access='read'/>"
    "  <signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>"
    "  <signal name='StatusNotifierItemUnregistered'><arg type='s'/></signal>"
    "  <signal name='StatusNotifierHostRegistered'/>"
    " </interface>"
    "</node>";

static void watcher_call(GDBusConnection *c, const char *sender, const char *path,
                         const char *iface, const char *method, GVariant *params,
                         GDBusMethodInvocation *inv, gpointer d)
{
    (void)c; (void)path; (void)iface; (void)d;
    if (watcher == W_OTHER) {
        g_dbus_method_invocation_return_error_literal(inv, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                      "not the StatusNotifierWatcher");
        return;
    }
    const char *svc = "";
    g_variant_get(params, "(&s)", &svc);
    if (strcmp(method, "RegisterStatusNotifierItem") == 0) {
        /* A bus name, whose object is then /StatusNotifierItem; or -
         * what libappindicator sends - an object path on the caller's
         * own connection. */
        const char *name = svc[0] == '/' ? sender : svc;
        const char *obj = svc[0] == '/' ? svc : ITEM_PATH;
        if (!g_dbus_is_name(name) || !g_variant_is_object_path(obj)) {
            g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                  "not a bus name or an object path: %s", svc);
            return;
        }
        Item *it = sni_add(name, obj);
        if (it)
            watcher_emit("StatusNotifierItemRegistered", it->key);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (strcmp(method, "RegisterStatusNotifierHost") == 0) {
        /* Another host is welcome; the bar is one already, so there is
         * nothing to remember but the signal. */
        watcher_emit("StatusNotifierHostRegistered", NULL);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                              "no method %s", method);
    }
}

static GVariant *watcher_get(GDBusConnection *c, const char *sender, const char *path,
                             const char *iface, const char *prop, GError **err, gpointer d)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)d;
    if (strcmp(prop, "RegisteredStatusNotifierItems") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        for (GList *l = items; l; l = l->next)
            if (((Item *)l->data)->kind == KIND_SNI)
                g_variant_builder_add(&b, "s", ((Item *)l->data)->key);
        return g_variant_builder_end(&b);
    }
    if (strcmp(prop, "IsStatusNotifierHostRegistered") == 0)
        return g_variant_new_boolean(TRUE);
    if (strcmp(prop, "ProtocolVersion") == 0)
        return g_variant_new_int32(0);
    g_set_error(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "no property %s", prop);
    return NULL;
}

static const GDBusInterfaceVTable watcher_vtable = {
    .method_call = watcher_call,
    .get_property = watcher_get,
};

/* ── the watcher: someone else's ─────────────────────────────────── */

/* An item as a watcher lists it: "<bus name><object path>", or a bare
 * bus name meaning /StatusNotifierItem. */
static gboolean parse_item(const char *s, char **name, char **path)
{
    const char *slash = strchr(s, '/');
    if (slash == s)
        return FALSE;
    *name = slash ? g_strndup(s, slash - s) : g_strdup(s);
    *path = g_strdup(slash ? slash : ITEM_PATH);
    return TRUE;
}

static void other_add(const char *s)
{
    char *name, *path;
    if (!parse_item(s, &name, &path))
        return;
    sni_add(name, path);
    g_free(name);
    g_free(path);
}

static void other_remove(const char *s)
{
    char *name, *path;
    if (!parse_item(s, &name, &path))
        return;
    char *key = g_strconcat(name, path, NULL);
    Item *it = g_hash_table_lookup(by_key, key);
    if (it)
        item_drop(it);
    g_free(key);
    g_free(name);
    g_free(path);
}

static void on_other_signal(GDBusConnection *c, const char *sender, const char *path,
                            const char *iface, const char *member, GVariant *params,
                            gpointer d)
{
    (void)c; (void)sender; (void)path; (void)iface;
    if (GPOINTER_TO_UINT(d) != other_gen || !g_variant_is_of_type(params, G_VARIANT_TYPE("(s)")))
        return;
    const char *s;
    g_variant_get(params, "(&s)", &s);
    if (strcmp(member, "StatusNotifierItemRegistered") == 0)
        other_add(s);
    else if (strcmp(member, "StatusNotifierItemUnregistered") == 0)
        other_remove(s);
}

static void other_items_done(GObject *src, GAsyncResult *res, gpointer d)
{
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (!r)
        return;
    if (GPOINTER_TO_UINT(d) == other_gen && watcher == W_OTHER) {
        GVariant *v = NULL;
        g_variant_get(r, "(v)", &v);
        if (g_variant_is_of_type(v, G_VARIANT_TYPE("as"))) {
            GVariantIter iter;
            const char *s;
            g_variant_iter_init(&iter, v);
            while (g_variant_iter_next(&iter, "&s", &s))
                other_add(s);
        }
        g_variant_unref(v);
    }
    g_variant_unref(r);
}

static void other_stop(void)
{
    other_gen++;
    if (bus && other_sig)
        g_dbus_connection_signal_unsubscribe(bus, other_sig);
    other_sig = 0;
}

static void other_start(const char *owner)
{
    other_stop();
    gpointer gen = GUINT_TO_POINTER(other_gen);
    /* Subscribed before the list is asked for: an item that comes in
     * between is then seen twice, and sni_add knows it the second time. */
    other_sig = g_dbus_connection_signal_subscribe(bus, owner, WATCHER_IFACE, NULL,
                                                   WATCHER_PATH, NULL,
                                                   G_DBUS_SIGNAL_FLAGS_NONE,
                                                   on_other_signal, gen, NULL);
    g_dbus_connection_call(bus, owner, WATCHER_PATH, WATCHER_IFACE,
                           "RegisterStatusNotifierHost", g_variant_new("(s)", host_name),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_dbus_connection_call(bus, owner, WATCHER_PATH, "org.freedesktop.DBus.Properties",
                           "Get", g_variant_new("(ss)", WATCHER_IFACE,
                                                "RegisteredStatusNotifierItems"),
                           G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                           other_items_done, gen);
}

/* Whoever owns the watcher's name: us (the items register with us), or
 * someone else (we are a host there). Every change of hands empties the
 * list; the items come back from wherever they are now registered. */
static void on_watcher_appeared(GDBusConnection *c, const char *name, const char *owner,
                                gpointer d)
{
    (void)name; (void)d;
    if (g_strcmp0(owner, g_dbus_connection_get_unique_name(c)) == 0) {
        if (watcher == W_OTHER) {
            other_stop();
            sni_clear();
        }
        watcher = W_OURS;
        /* libappindicator falls back to no icon at all while there is
         * no host, and comes back on this. */
        watcher_emit("StatusNotifierHostRegistered", NULL);
    } else {
        watcher = W_OTHER;
        other_stop();
        sni_clear();
        other_start(owner);
    }
}

static void on_watcher_vanished(GDBusConnection *c, const char *name, gpointer d)
{
    (void)c; (void)name; (void)d;
    other_stop();
    watcher = W_NONE;
    sni_clear();
}

static void on_bus_closed(GDBusConnection *c, gboolean remote, GError *err, gpointer d)
{
    (void)c; (void)remote; (void)err; (void)d;
    other_stop();
    watcher = W_NONE;
    sni_clear();
    g_clear_object(&bus);
}

static void on_bus(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)src; (void)d;
    GError *err = NULL;
    bus = g_bus_get_finish(res, &err);
    if (!bus) {
        /* No session bus: no tray icons. The applications found in
         * /proc still are. */
        g_printerr("lp-panel: tray: no session bus: %s\n", err->message);
        g_error_free(err);
        return;
    }
    /* The default is to exit when the bus goes; the bar outlives it. */
    g_dbus_connection_set_exit_on_close(bus, FALSE);
    g_signal_connect(bus, "closed", G_CALLBACK(on_bus_closed), NULL);

    host_name = g_strdup_printf("org.kde.StatusNotifierHost-%d", (int)getpid());
    g_bus_own_name_on_connection(bus, host_name, G_BUS_NAME_OWNER_FLAGS_NONE,
                                 NULL, NULL, NULL, NULL);

    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(watcher_xml, NULL);
    guint obj = g_dbus_connection_register_object(bus, WATCHER_PATH, node->interfaces[0],
                                                  &watcher_vtable, NULL, NULL, &err);
    g_dbus_node_info_unref(node);
    if (obj) {
        /* Queued if taken, and given up if someone asks to replace us. */
        g_bus_own_name_on_connection(bus, WATCHER_NAME,
                                     G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT,
                                     NULL, NULL, NULL, NULL);
    } else {
        g_printerr("lp-panel: tray: %s\n", err->message);
        g_clear_error(&err);
    }
    g_bus_watch_name_on_connection(bus, WATCHER_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                   on_watcher_appeared, on_watcher_vanished, NULL, NULL);
}

/* ── applications in the background ──────────────────────────────── */

static void app_item_add(GDesktopAppInfo *app)
{
    Item *it = g_new0(Item, 1);
    it->kind = KIND_APP;
    it->key = g_strconcat("app:", g_app_info_get_id(G_APP_INFO(app)), NULL);
    it->app = g_object_ref(app);
    item_insert(it);
}

/* Every application that has a window now is remembered as one that
 * had one. Only those can be "in the background": what the bar is for
 * is the window that was closed while its program went on. A program
 * that never showed a window - a daemon, a helper, the on-screen
 * keyboard, an application whose window the bar could not match to it
 * - is not a closed window, and is left alone. */
static void windows_note(void)
{
    apps_load();
    GHashTableIter hi;
    gpointer v;
    g_hash_table_iter_init(&hi, exe_apps);
    while (g_hash_table_iter_next(&hi, NULL, &v)) {
        const char *id = g_app_info_get_id(G_APP_INFO(v));
        if (id && !g_hash_table_contains(had_window, id) && app_has_window(v))
            g_hash_table_add(had_window, g_strdup(id));
    }
}

/* The applications in the background, desktop id -> GDesktopAppInfo
 * (not owned; keys are the apps' own ids): a settled process, a window
 * seen while it ran, and none now. An application whose processes are
 * all gone is forgotten, so starting it again starts from nothing. */
static GHashTable *background_apps(GHashTable *procs, double now, double hz)
{
    GHashTable *by_app = candidates(procs);
    GHashTable *show = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTableIter hi;
    gpointer k, v;
    g_hash_table_iter_init(&hi, had_window);
    while (g_hash_table_iter_next(&hi, &k, NULL))
        if (!g_hash_table_contains(by_app, k))
            g_hash_table_iter_remove(&hi);
    g_hash_table_iter_init(&hi, by_app);
    while (g_hash_table_iter_next(&hi, &k, &v)) {
        GPtrArray *a = v;
        gboolean settled = FALSE;
        for (guint i = 0; i < a->len && !settled; i++)
            settled = now - (double)((Proc *)a->pdata[i])->start / hz >= SETTLE_S;
        GDesktopAppInfo *app = proc_app(a->pdata[0]);
        if (!app)
            continue;
        const char *id = g_app_info_get_id(G_APP_INFO(app));
        if (app_has_window(app))
            g_hash_table_add(had_window, g_strdup(id));
        else if (settled && g_hash_table_contains(had_window, id))
            g_hash_table_insert(show, (gpointer)id, app);
    }
    g_hash_table_unref(by_app);
    return show;
}

static void scan(void)
{
    if (!can_see_windows || !toplevels_seen)
        return;
    apps_load();
    GHashTable *procs = procs_read();
    GHashTable *show = background_apps(procs, uptime_now(), (double)sysconf(_SC_CLK_TCK));
    GHashTableIter hi;
    gpointer v;
    /* Gone, or back in a window; what stays is taken out of `show`,
     * which leaves the new ones. */
    for (GList *l = items, *next; l; l = next) {
        next = l->next;
        Item *it = l->data;
        if (it->kind == KIND_APP &&
            !g_hash_table_remove(show, g_app_info_get_id(G_APP_INFO(it->app))))
            item_drop(it);
    }
    g_hash_table_iter_init(&hi, show);
    while (g_hash_table_iter_next(&hi, NULL, &v))
        app_item_add(v);
    g_hash_table_unref(show);
    g_hash_table_unref(procs);
}

/* Every process of an application, young ones too - for 종료. */
static GArray *app_pids(const char *id)
{
    GArray *pids = g_array_new(FALSE, FALSE, sizeof(int));
    apps_load();
    GHashTable *procs = procs_read();
    GHashTable *by_app = candidates(procs);
    GPtrArray *a = id ? g_hash_table_lookup(by_app, id) : NULL;
    for (guint i = 0; a && i < a->len; i++)
        g_array_append_val(pids, ((Proc *)a->pdata[i])->pid);
    g_hash_table_unref(by_app);
    g_hash_table_unref(procs);
    return pids;
}

static gboolean scan_tick(gpointer d)
{
    (void)d;
    scan();
    return G_SOURCE_CONTINUE;
}

static gboolean scan_soon(gpointer d)
{
    (void)d;
    scan_soon_id = 0;
    scan();
    return G_SOURCE_REMOVE;
}

/* A second from the first change, not from the last: a window whose
 * title changes every second must not put the look off for ever. */
static void scan_later(void)
{
    if (!scan_soon_id)
        scan_soon_id = g_timeout_add(SCAN_AFTER_MS, scan_soon, NULL);
}

/* Whether the compositor offers wlr-foreign-toplevel-management, asked
 * of the registry without binding it (lp-toplevel.c does that): without
 * it the window list is always empty, and every application would be
 * "in the background". */
static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface,
                       uint32_t version)
{
    (void)r; (void)name; (void)version;
    if (strcmp(iface, "zwlr_foreign_toplevel_manager_v1") == 0)
        *(gboolean *)d = TRUE;
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{
    (void)d; (void)r; (void)name;
}

static gboolean compositor_lists_windows(void)
{
    static const struct wl_registry_listener listener = {
        .global = reg_global,
        .global_remove = reg_remove,
    };
    GdkDisplay *gd = gdk_display_get_default();
    if (!gd || !GDK_IS_WAYLAND_DISPLAY(gd))
        return FALSE;
    gboolean found = FALSE;
    struct wl_display *wd = gdk_wayland_display_get_wl_display(gd);
    struct wl_registry *reg = wl_display_get_registry(wd);
    wl_registry_add_listener(reg, &listener, &found);
    wl_display_roundtrip(wd);
    wl_registry_destroy(reg);
    return found;
}

/* ── the three calls ─────────────────────────────────────────────── */

void lp_tray_init(void)
{
    static gboolean done;
    if (done)
        return;
    done = TRUE;
    tables();
    /* Kept for as long as the bar runs. */
    g_signal_connect(g_app_info_monitor_get(), "changed", G_CALLBACK(on_apps_changed), NULL);
    can_see_windows = compositor_lists_windows();
    g_timeout_add_seconds(SCAN_EVERY_S, scan_tick, NULL);
    g_bus_get(G_BUS_TYPE_SESSION, NULL, on_bus, NULL);
}

GtkWidget *lp_tray_new(void)
{
    tables();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "lp-tray");
    /* Its own visibility is its own business (sync_box): show_all on
     * the bar would otherwise show it empty. */
    gtk_widget_set_no_show_all(box, TRUE);
    g_signal_connect(box, "destroy", G_CALLBACK(on_box_destroy), NULL);
    boxes = g_list_append(boxes, box);
    sync_box(box);
    return box;
}

void lp_tray_toplevels_changed(void)
{
    tables();
    toplevels_seen = TRUE;
    /* Now, not at the next look: a window open for less than a second
     * is still one that was closed. */
    windows_note();
    scan_later();
}
