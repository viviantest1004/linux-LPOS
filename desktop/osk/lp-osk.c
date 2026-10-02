#define _GNU_SOURCE 1
/*
 * lp-osk.c - the on-screen keyboard's command line, its one running
 * instance, its settings, and when it shows itself.
 *
 *   lp-osk [daemon]        run (hidden until needed); the session starts this
 *   lp-osk show|hide|toggle
 *   lp-osk lang en|ko|toggle
 *   lp-osk status          one line of JSON
 *   lp-osk watch           that line again every time it changes
 *
 * ── One instance, one socket ──
 *
 * The top bar's keyboard button, the Settings dialogs and a gesture all
 * run `lp-osk toggle` (or show) as a fresh process. That process connects
 * to $XDG_RUNTIME_DIR/lp-osk.sock, hands its words to the running
 * keyboard, prints what comes back and exits. If nothing is listening,
 * show and toggle turn the process itself into the keyboard: the first
 * tap on the button works even if the session never started one.
 *
 * The socket is ours and not lp-shell's lp_single_instance, because this
 * one answers: `status` has a reply, `watch` stays open for the top bar,
 * and the tests read key geometry and typed text back through it. Two
 * processes starting at once are kept apart by an flock on a lock file
 * held for the daemon's lifetime, so there is never a window where both
 * decide they are the first.
 *
 * ── When it shows itself ──
 *
 * When a text field takes focus the input method is activated (type.c).
 * With auto-show "touch", the default, the keyboard then shows if the
 * last thing the person used was the touchscreen (evdev.c); "always" and
 * "off" do what they say. Closing it by hand is remembered until focus
 * moves to another field - a keyboard that pops back up the instant it
 * was closed is the other way people learn to hate these. A keyboard that
 * showed itself also hides itself: when the field loses focus (after a
 * short grace, so moving between two fields does not bounce it), and when
 * someone starts typing on the laptop's own keys.
 *
 * ── Settings ──
 *
 * ~/.config/lp/osk.ini, a GKeyFile, written atomically and durably (temp
 * file, fsync, rename, fsync of the directory - g_file_set_contents_full
 * with CONSISTENT|DURABLE), so a power cut leaves the old file or the new
 * one, never half of either:
 *
 *   [keyboard]
 *   height=0.34              fraction of the screen's height
 *   auto-show=touch          touch | always | off
 *   layouts=en;ko            the layouts 한/영 cycles through
 *   language=en              the one in use when it was last changed
 *   physical-korean=true     compose Hangul from the laptop keyboard too
 *   switch-keys=hangul;ralt;shift-space
 *                            what switches 한/영: the Hangul key always,
 *                            then any of ralt, shift-space, ctrl-space
 *
 * Settings writes the same file and then runs `lp-osk reload`, so a
 * change is in use at once rather than at the next login - and since
 * this program rewrites the file whole, every key Settings owns is one
 * this program reads and writes back too.
 */
#include "osk.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <gtk-layer-shell.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lp-i18n.h"

OskConfig cfg = {
    .height_frac = 0.34,
    .auto_mode = AUTO_TOUCH,
    .lang = LANG_EN,
    .layouts = LAYOUT_EN | LAYOUT_KO,
    .ime = TRUE,
    .switch_keys = SWITCH_RALT | SWITCH_SHIFTSPACE,
};

static gboolean test_mode;
static gboolean auto_shown;       /* shown by focus, not by a person */
static gboolean dismissed;        /* closed by hand while this field has focus */
static guint    hide_later;
static GList   *watchers;         /* GOutputStream* of `lp-osk watch` */

/* ── settings ─────────────────────────────────────────────────────── */

static char *config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "lp", "osk.ini", NULL);
}

static void config_load(void)
{
    char *path = config_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        GError *e = NULL;
        double h = g_key_file_get_double(kf, "keyboard", "height", &e);
        if (!e && h > 0.1 && h < 0.7)
            cfg.height_frac = h;
        g_clear_error(&e);
        char *a = g_key_file_get_string(kf, "keyboard", "auto-show", NULL);
        if (a)
            cfg.auto_mode = !strcmp(a, "always") ? AUTO_ALWAYS :
                            !strcmp(a, "off") ? AUTO_OFF : AUTO_TOUCH;
        g_free(a);
        char **l = g_key_file_get_string_list(kf, "keyboard", "layouts", NULL, NULL);
        if (l) {
            cfg.layouts = LAYOUT_EN;
            for (int i = 0; l[i]; i++)
                if (!strcmp(l[i], "ko"))
                    cfg.layouts |= LAYOUT_KO;
            g_strfreev(l);
        }
        char *lang = g_key_file_get_string(kf, "keyboard", "language", NULL);
        if (lang)
            cfg.lang = !strcmp(lang, "ko") && (cfg.layouts & LAYOUT_KO) ? LANG_KO : LANG_EN;
        g_free(lang);
        gboolean ime = g_key_file_get_boolean(kf, "keyboard", "physical-korean", &e);
        if (!e)
            cfg.ime = ime;
        g_clear_error(&e);
        char **sk = g_key_file_get_string_list(kf, "keyboard", "switch-keys", NULL, NULL);
        if (sk) {
            cfg.switch_keys = 0;
            for (int i = 0; sk[i]; i++)
                cfg.switch_keys |= !strcmp(sk[i], "ralt") ? SWITCH_RALT
                                 : !strcmp(sk[i], "shift-space") ? SWITCH_SHIFTSPACE
                                 : !strcmp(sk[i], "ctrl-space") ? SWITCH_CTRLSPACE : 0;
            g_strfreev(sk);
        }
    }
    g_key_file_free(kf);
    g_free(path);
}

void osk_config_save(void)
{
    char *path = config_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    char *body = g_strdup_printf(
        "# lp-osk settings. lp-osk rewrites this file whole; comments are not kept.\n"
        "[keyboard]\n"
        "height=%.3f\n"
        "auto-show=%s\n"
        "layouts=en%s\n"
        "language=%s\n"
        "physical-korean=%s\n"
        "switch-keys=hangul%s%s%s\n",
        cfg.height_frac,
        cfg.auto_mode == AUTO_ALWAYS ? "always" : cfg.auto_mode == AUTO_OFF ? "off" : "touch",
        cfg.layouts & LAYOUT_KO ? ";ko" : "",
        cfg.lang == LANG_KO ? "ko" : "en",
        cfg.ime ? "true" : "false",
        cfg.switch_keys & SWITCH_RALT ? ";ralt" : "",
        cfg.switch_keys & SWITCH_SHIFTSPACE ? ";shift-space" : "",
        cfg.switch_keys & SWITCH_CTRLSPACE ? ";ctrl-space" : "");
    GError *e = NULL;
    if (!g_file_set_contents_full(path, body, -1,
            G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
            0600, &e)) {
        g_printerr("lp-osk: cannot save %s: %s\n", path, e->message);
        g_clear_error(&e);
    }
    g_free(body);
    g_free(dir);
    g_free(path);
}

/* ── state for the outside world ──────────────────────────────────── */

static char *state_json(void)
{
    gint64 age;
    int k = lastinput_query(&age);
    static const char *const kinds[] = { "unknown", "touch", "keyboard", "pointer" };
    return g_strdup_printf(
        "{\"visible\":%s,\"language\":\"%s\",\"page\":\"%s\",\"im\":\"%s\","
        "\"composing\":%s,\"purpose\":%u,\"auto\":\"%s\",\"last_input\":\"%s\","
        "\"devices\":\"%s\"}",
        ui_visible() ? "true" : "false", type_lang() == LANG_KO ? "ko" : "en",
        ui_page_name(), type_im_status(), type_composing() ? "true" : "false",
        type_purpose(),
        cfg.auto_mode == AUTO_ALWAYS ? "always" : cfg.auto_mode == AUTO_OFF ? "off" : "touch",
        kinds[k & 3], lastinput_status());
}

void osk_state_changed(void)
{
    if (!watchers)
        return;
    char *j = state_json();
    char *line = g_strconcat(j, "\n", NULL);
    for (GList *l = watchers; l;) {
        GList *next = l->next;
        GOutputStream *os = l->data;
        if (!g_output_stream_write_all(os, line, strlen(line), NULL, NULL, NULL)) {
            watchers = g_list_delete_link(watchers, l);
            g_object_unref(os);
        }
        l = next;
    }
    g_free(line);
    g_free(j);
}

/* ── policy ───────────────────────────────────────────────────────── */

static void show_by_person(void)
{
    auto_shown = FALSE;
    dismissed = FALSE;
    if (hide_later) {
        g_source_remove(hide_later);
        hide_later = 0;
    }
    ui_show();
}

static void hide_by_person(void)
{
    auto_shown = FALSE;
    dismissed = TRUE;
    ui_hide();
}

void osk_im_activated(gboolean fresh)
{
    if (hide_later) {
        g_source_remove(hide_later);
        hide_later = 0;
    }
    if (fresh)
        dismissed = FALSE;          /* a new field: the old "no" is spent */
    if (ui_visible() || dismissed)
        return;
    gboolean show = FALSE;
    if (cfg.auto_mode == AUTO_ALWAYS)
        show = TRUE;
    else if (cfg.auto_mode == AUTO_TOUCH)
        show = lastinput_query(NULL) == INPUT_TOUCH;
    if (show) {
        auto_shown = TRUE;
        ui_show();
    }
}

static gboolean on_hide_later(gpointer d)
{
    (void)d;
    hide_later = 0;
    if (auto_shown) {
        auto_shown = FALSE;
        ui_hide();
    }
    return G_SOURCE_REMOVE;
}

void osk_im_deactivated(void)
{
    /* Focus going from one field to the next deactivates and activates
     * again within a frame or two; wait a little before believing it. */
    if (auto_shown && !hide_later)
        hide_later = g_timeout_add(250, on_hide_later, NULL);
}

void osk_dismissed(void)
{
    auto_shown = FALSE;
    dismissed = TRUE;
}

void osk_hardware_key(void)
{
    if (auto_shown && ui_visible()) {
        auto_shown = FALSE;
        dismissed = TRUE;
        ui_hide();
    }
}

/* ── the socket ───────────────────────────────────────────────────── */

static char *runtime_path(const char *name)
{
    return g_build_filename(g_get_user_runtime_dir(), name, NULL);
}

static void reply(GString *out, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void reply(GString *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_string_append_vprintf(out, fmt, ap);
    va_end(ap);
}

static int key_kind(const char *s)
{
    return !strcmp(s, "touch") ? INPUT_TOUCH : !strcmp(s, "key") ? INPUT_KEY :
           !strcmp(s, "pointer") ? INPUT_POINTER : INPUT_UNKNOWN;
}

/* `lp-osk test tap a b c` types keys at a finger's pace: down, 40ms, up,
 * 70ms, next. Taps fired back to back in one go would be nothing a person
 * can do - the chatter filter rightly takes a second tap on the same key
 * within 35ms for the screen bouncing - and they would never let a frame
 * be drawn between two keys, which is where the previews and the fades
 * live. So the reply comes when the last key is up. */
typedef struct {
    char             **keys;
    int                i, n;
    gboolean           down;
    GSocketConnection *conn;
    char               err[128];
} TapJob;

static void tap_finish(TapJob *j)
{
    GOutputStream *os = g_io_stream_get_output_stream(G_IO_STREAM(j->conn));
    char *msg = j->err[0] ? g_strdup_printf("error: %s\n", j->err) : g_strdup("ok\n");
    g_output_stream_write_all(os, msg, strlen(msg), NULL, NULL, NULL);
    g_io_stream_close(G_IO_STREAM(j->conn), NULL, NULL);
    g_object_unref(j->conn);
    g_strfreev(j->keys);
    g_free(msg);
    g_free(j);
}

static gboolean tap_step(gpointer data)
{
    TapJob *j = data;
    if (j->i >= j->n) {
        tap_finish(j);
        return G_SOURCE_REMOVE;
    }
    if (!ui_test_touch(1, j->down ? 2 : 0, j->keys[j->i], 0, 0, j->err, sizeof j->err)) {
        if (j->down)          /* never leave a finger on the glass */
            ui_test_touch(1, 3, "-", 0, 0, NULL, 0);
        tap_finish(j);
        return G_SOURCE_REMOVE;
    }
    j->down = !j->down;
    if (!j->down)
        j->i++;
    g_timeout_add(j->down ? 40 : 70, tap_step, j);
    return G_SOURCE_REMOVE;
}

static void tap_start(char **keys, int n, GSocketConnection *conn)
{
    TapJob *j = g_new0(TapJob, 1);
    j->keys = g_new0(char *, n + 1);
    for (int i = 0; i < n; i++)
        j->keys[i] = g_strdup(keys[i]);
    j->n = n;
    j->conn = g_object_ref(conn);
    tap_step(j);
}

/* `lp-osk test ...`: drives the keys through the same code a finger does. */
static gboolean do_test(char **a, int n, GString *out, GSocketConnection *conn,
                        gboolean *deferred)
{
    if (!test_mode) {
        reply(out, "error: test commands need LP_OSK_TEST=1 in the keyboard's environment\n");
        return FALSE;
    }
    if (n >= 1 && !strcmp(a[0], "tap")) {
        if (!conn) {
            reply(out, "error: tap needs a connection to answer on\n");
            return FALSE;
        }
        tap_start(a + 1, n - 1, conn);
        *deferred = TRUE;
        return TRUE;
    }
    if (n >= 3 && !strcmp(a[0], "touch")) {
        /* touch <id> down|move|up|cancel <key|-> [dx dy] */
        int id = atoi(a[1]);
        int ph = !strcmp(a[2], "down") ? 0 : !strcmp(a[2], "move") ? 1 :
                 !strcmp(a[2], "up") ? 2 : 3;
        const char *key = n > 3 ? a[3] : "-";
        double dx = n > 4 ? g_ascii_strtod(a[4], NULL) : 0;
        double dy = n > 5 ? g_ascii_strtod(a[5], NULL) : 0;
        char err[128];
        if (!ui_test_touch(id, ph, key, dx, dy, err, sizeof err)) {
            reply(out, "error: %s\n", err);
            return FALSE;
        }
        reply(out, "ok\n");
        return TRUE;
    }
    if (n >= 1 && !strcmp(a[0], "describe")) {
        ui_describe(out);
        return TRUE;
    }
    if (n >= 2 && !strcmp(a[0], "lastinput")) {
        lastinput_force(key_kind(a[1]));
        reply(out, "ok\n");
        return TRUE;
    }
    if (n >= 2 && !strcmp(a[0], "auto")) {
        cfg.auto_mode = !strcmp(a[1], "always") ? AUTO_ALWAYS :
                        !strcmp(a[1], "off") ? AUTO_OFF : AUTO_TOUCH;
        reply(out, "ok\n");
        return TRUE;
    }
    reply(out, "error: test tap <key>... | touch <id> down|move|up|cancel <key|-> [dx dy]"
               " | describe | lastinput touch|key|pointer | auto touch|always|off\n");
    return FALSE;
}

/* One request: words separated by tabs, ended by a newline. The reply is
 * text; the connection closes after it (except for watch). */
static gboolean handle(char **a, int n, GString *out, gboolean *keep,
                       GSocketConnection *conn, gboolean *deferred)
{
    *keep = FALSE;
    *deferred = FALSE;
    const char *cmd = n ? a[0] : "daemon";   /* bare `lp-osk`: already running */
    if (!strcmp(cmd, "show") || !strcmp(cmd, "daemon")) {
        if (!strcmp(cmd, "show"))
            show_by_person();
    } else if (!strcmp(cmd, "hide")) {
        hide_by_person();
    } else if (!strcmp(cmd, "toggle")) {
        if (ui_visible())
            hide_by_person();
        else
            show_by_person();
    } else if (!strcmp(cmd, "lang")) {
        const char *l = n > 1 ? a[1] : "toggle";
        if (!strcmp(l, "toggle"))
            type_toggle_lang();
        else
            type_set_lang(!strcmp(l, "ko") ? LANG_KO : LANG_EN);
    } else if (!strcmp(cmd, "reload")) {
        /* Settings changed osk.ini. The language in use stays what it is
         * (unless Korean was just removed from the list). */
        int lang = cfg.lang;
        config_load();
        cfg.lang = lang;
        if (lang == LANG_KO && !(cfg.layouts & LAYOUT_KO))
            type_set_lang(LANG_EN);
        ui_relabel();
        osk_state_changed();
    } else if (!strcmp(cmd, "status")) {
        char *j = state_json();
        reply(out, "%s\n", j);
        g_free(j);
        return TRUE;
    } else if (!strcmp(cmd, "watch")) {
        char *j = state_json();
        reply(out, "%s\n", j);
        g_free(j);
        *keep = TRUE;
        return TRUE;
    } else if (!strcmp(cmd, "test")) {
        return do_test(a + 1, n - 1, out, conn, deferred);
    } else {
        reply(out, "error: unknown command \"%s\"\n", cmd);
        return FALSE;
    }
    reply(out, "ok\n");
    return TRUE;
}

typedef struct {
    GSocketConnection *conn;
    GDataInputStream  *in;
} Client;

static void client_line(GObject *src, GAsyncResult *res, gpointer data)
{
    Client *c = data;
    gsize len;
    char *line = g_data_input_stream_read_line_finish_utf8(c->in, res, &len, NULL);
    GOutputStream *os = g_io_stream_get_output_stream(G_IO_STREAM(c->conn));
    gboolean keep = FALSE, deferred = FALSE;
    if (line) {
        char **a = g_strsplit(line, "\t", 64);
        int n = (int)g_strv_length(a);
        if (n == 1 && !*a[0])
            n = 0;
        GString *out = g_string_new(NULL);
        handle(a, n, out, &keep, c->conn, &deferred);
        g_output_stream_write_all(os, out->str, out->len, NULL, NULL, NULL);
        g_string_free(out, TRUE);
        g_strfreev(a);
        g_free(line);
    }
    if (deferred) {
        /* the test job holds the connection and answers on it */
    } else if (keep) {
        watchers = g_list_prepend(watchers, g_object_ref(os));
        /* The connection object must outlive this request; it is dropped
         * with the watcher when a write to it fails. */
        g_object_set_data_full(G_OBJECT(os), "conn", g_object_ref(c->conn),
                               g_object_unref);
    } else {
        g_io_stream_close(G_IO_STREAM(c->conn), NULL, NULL);
    }
    g_object_unref(c->in);
    g_object_unref(c->conn);
    g_free(c);
    (void)src;
}

static gboolean on_incoming(GSocketService *svc, GSocketConnection *conn,
                            GObject *src, gpointer data)
{
    (void)svc; (void)src; (void)data;
    /* Only this user's processes can reach a socket in a 0700 runtime
     * directory; the peer check says so again rather than trusting it. */
    GSocket *s = g_socket_connection_get_socket(conn);
    GCredentials *cr = g_socket_get_credentials(s, NULL);
    if (cr) {
        uid_t uid = g_credentials_get_unix_user(cr, NULL);
        g_object_unref(cr);
        if (uid != getuid())
            return TRUE;          /* dropped: GIO closes it */
    }
    Client *c = g_new0(Client, 1);
    c->conn = g_object_ref(conn);
    c->in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
    g_data_input_stream_read_line_async(c->in, G_PRIORITY_DEFAULT, NULL,
                                        client_line, c);
    return TRUE;
}

/* The client side: send argv, print the reply. -1 when nobody listens. */
static int send_request(int argc, char **argv, gboolean stream)
{
    char *path = runtime_path("lp-osk.sock");
    GSocketAddress *addr = g_unix_socket_address_new(path);
    g_free(path);
    GSocketClient *cl = g_socket_client_new();
    GSocketConnection *conn = g_socket_client_connect(cl, G_SOCKET_CONNECTABLE(addr),
                                                      NULL, NULL);
    g_object_unref(addr);
    g_object_unref(cl);
    if (!conn)
        return -1;
    GString *req = g_string_new(NULL);
    for (int i = 0; i < argc; i++) {
        if (i)
            g_string_append_c(req, '\t');
        g_string_append(req, argv[i]);
    }
    g_string_append_c(req, '\n');
    GOutputStream *os = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    g_output_stream_write_all(os, req->str, req->len, NULL, NULL, NULL);
    g_string_free(req, TRUE);
    GInputStream *is = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    char buf[4096];
    gssize n;
    int rc = 0;
    gboolean first = TRUE;
    while ((n = g_input_stream_read(is, buf, sizeof buf, NULL, NULL)) > 0) {
        if (first && n >= 6 && !strncmp(buf, "error:", 6))
            rc = 1;
        first = FALSE;
        /* "ok" is for scripts that care; a person running show sees nothing */
        if (!(n == 3 && !strncmp(buf, "ok\n", 3)))
            fwrite(buf, 1, (size_t)n, rc ? stderr : stdout);
        if (stream)
            fflush(stdout);
    }
    g_object_unref(conn);
    return rc;
}

/* ── start ────────────────────────────────────────────────────────── */

static gboolean on_term(gpointer d)
{
    (void)d;
    /* Hand back the laptop keyboard and every key still held before
     * going: a key left pressed on a virtual keyboard repeats forever. */
    type_shutdown();
    char *sock = runtime_path("lp-osk.sock");
    unlink(sock);
    g_free(sock);
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

static void usage(void)
{
    printf("%s",
        T("usage: lp-osk [daemon]           run the on-screen keyboard (hidden until needed)\n"
          "       lp-osk show|hide|toggle   show or hide it\n"
          "       lp-osk lang en|ko|toggle  choose the language (한/영)\n"
          "       lp-osk status             state as JSON\n"
          "       lp-osk watch              state as JSON, again at every change\n",
          "사용법: lp-osk [daemon]           화상 키보드 실행 (필요할 때까지 숨김)\n"
          "        lp-osk show|hide|toggle   보이기 / 숨기기 / 전환\n"
          "        lp-osk lang en|ko|toggle  언어 선택 (한/영)\n"
          "        lp-osk status             상태를 JSON 으로\n"
          "        lp-osk watch              상태가 바뀔 때마다 JSON 으로\n"));
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    const char *cmd = argc > 1 ? argv[1] : "daemon";
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
        usage();
        return 0;
    }
    static const char *const known[] = { "daemon", "show", "hide", "toggle",
                                         "lang", "reload", "status", "watch", "test", NULL };
    if (!g_strv_contains(known, cmd)) {
        fprintf(stderr, T("lp-osk: unknown command \"%s\" (try lp-osk --help)\n",
                          "lp-osk: 알 수 없는 명령 \"%s\" (lp-osk --help 참고)\n"), cmd);
        return 2;
    }

    /* Someone is already the keyboard: tell it and go. */
    int rc = send_request(argc - 1, argv + 1, !strcmp(cmd, "watch"));
    if (rc >= 0)
        return rc;

    /* Nobody is. Only these start one. */
    if (strcmp(cmd, "daemon") && strcmp(cmd, "show") && strcmp(cmd, "toggle")) {
        if (!strcmp(cmd, "hide") || !strcmp(cmd, "reload"))
            return 0;                  /* nothing to hide; it reads the file when it starts */
        fprintf(stderr, T("lp-osk: the keyboard is not running\n",
                          "lp-osk: 화상 키보드가 실행 중이 아닙니다\n"));
        return 1;
    }

    char *lockp = runtime_path("lp-osk.lock");
    int lock = open(lockp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    g_free(lockp);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
        /* Another one is starting this very moment: wait for its socket. */
        for (int i = 0; i < 40; i++) {
            g_usleep(50000);
            rc = send_request(argc - 1, argv + 1, FALSE);
            if (rc >= 0)
                return rc;
        }
        fprintf(stderr, "lp-osk: another instance holds the lock but does not answer\n");
        return 1;
    }

    gtk_init(&argc, &argv);
    GdkDisplay *gd = gdk_display_get_default();
    if (!gd || !gtk_layer_is_supported()) {
        fprintf(stderr, "lp-osk: needs a Wayland compositor with wlr-layer-shell\n");
        return 1;
    }
    test_mode = g_strcmp0(g_getenv("LP_OSK_TEST"), "1") == 0;
    config_load();
    ui_init();
    if (!type_init())
        fprintf(stderr, "lp-osk: no virtual keyboard support; keys will not type\n");
    lastinput_init();

    char *sockp = runtime_path("lp-osk.sock");
    unlink(sockp);            /* left by a keyboard that crashed; we hold the lock */
    GSocketAddress *addr = g_unix_socket_address_new(sockp);
    GSocketService *svc = g_socket_service_new();
    GError *e = NULL;
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(svc), addr,
            G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &e)) {
        fprintf(stderr, "lp-osk: %s: %s\n", sockp, e->message);
        g_clear_error(&e);
    }
    chmod(sockp, 0600);
    g_object_unref(addr);
    g_free(sockp);
    g_signal_connect(svc, "incoming", G_CALLBACK(on_incoming), NULL);
    g_socket_service_start(svc);

    g_unix_signal_add(SIGTERM, on_term, NULL);
    g_unix_signal_add(SIGINT, on_term, NULL);

    /* The request that started us is carried out like any other. */
    GString *out = g_string_new(NULL);
    gboolean keep, deferred;
    handle(argv + 1, argc - 1, out, &keep, NULL, &deferred);
    g_string_free(out, TRUE);

    gtk_main();
    return 0;
}
