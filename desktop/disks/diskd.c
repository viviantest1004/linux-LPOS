/* diskd.c - talking to lp-diskd (see diskd.h for what and why).
 *
 * One request is one connection: connect, write the line (and, for an
 * image, the file's descriptor in the same sendmsg), read lines until
 * the verdict. Reading is asynchronous on the main loop, so a resize
 * that takes minutes streams its progress into the window without the
 * window ever blocking - and closing the window does not stop the job:
 * the daemon finishes it whether anybody is listening or not.
 *
 * The password sheet is lp-kit's dialog (desktop/software/lp-kit.c), the
 * same card on a spring every administrator question on this desktop
 * arrives in. It is built here rather than with lp_priv_run because the
 * daemon is a different one and because a request may carry a
 * descriptor, which has to be sent again after the password.
 *
 * What is careful here, and why:
 *
 *  - The password is in memory for as long as it takes to hex-encode it
 *    into the request buffer. The entry is emptied at once; the buffer
 *    is overwritten as soon as it is written to the socket and again
 *    when freed. GTK's own copy inside the entry is gone when the entry
 *    is emptied; that is as far as a GTK program can go.
 *
 *  - A wrong password makes the daemon refuse the next try for a while
 *    ("fail auth wait N s"); the sheet counts that down instead of
 *    letting the person type into a request that will be refused.
 */
#define _GNU_SOURCE 1
#include "diskd.h"
#include "lp-kit.h"

#include <gio/gunixfdmessage.h>
#include <gio/gunixsocketaddress.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *dk_socket(void)
{
    const char *e = g_getenv("LP_DISKD_SOCK");
    return (e && *e) ? e : "/run/lp-diskd.sock";
}

/* ── records ──────────────────────────────────────────────────────── */

GHashTable *dk_rec_parse(const char *rest)
{
    GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char **f = g_strsplit(rest ? rest : "", "\t", -1);
    for (int i = 0; f[i]; i++) {
        char *eq = strchr(f[i], '=');
        if (!eq)
            continue;
        g_hash_table_replace(h, g_strndup(f[i], (gsize)(eq - f[i])), g_strdup(eq + 1));
    }
    g_strfreev(f);
    return h;
}

const char *dk_rec_str(GHashTable *r, const char *key)
{
    const char *v = r ? g_hash_table_lookup(r, key) : NULL;
    return v ? v : "";
}

guint64 dk_rec_u64(GHashTable *r, const char *key)
{
    return g_ascii_strtoull(dk_rec_str(r, key), NULL, 10);
}

gint64 dk_rec_i64(GHashTable *r, const char *key)
{
    return g_ascii_strtoll(dk_rec_str(r, key), NULL, 10);
}

static char *hexof(const char *s, const char *prefix)
{
    static const char hx[] = "0123456789abcdef";
    size_t n = strlen(s), p = strlen(prefix);
    char *out = g_malloc(p + 2 * n + 1);
    memcpy(out, prefix, p);
    for (size_t i = 0; i < n; i++) {
        out[p + 2 * i] = hx[(guchar)s[i] >> 4];
        out[p + 2 * i + 1] = hx[(guchar)s[i] & 15];
    }
    out[p + 2 * n] = '\0';
    return out;
}

char *dk_label_field(const char *label)
{
    if (!label || !*label)
        return g_strdup("-");
    gboolean plain = label[0] != ' ' && label[0] != '-';
    for (const char *c = label; *c && plain; c++)
        plain = g_ascii_isalnum(*c) || strchr(" _.-", *c);
    return plain ? g_strdup(label) : hexof(label, "x:");
}

char *dk_key_field(const char *pass)
{
    return hexof(pass, "k:");
}

char *dk_size(guint64 bytes)
{
    static const char *const unit[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    int u = 0;
    guint64 div = 1;
    while (u < 5 && bytes / div >= 1024) {
        div *= 1024;
        u++;
    }
    if (u == 0)
        return g_strdup_printf("%" G_GUINT64_FORMAT " B", bytes);
    guint64 tenths = (bytes / div) * 10 + ((bytes % div) * 10 + div / 2) / div;
    if (tenths >= 1000 || tenths % 10 == 0)
        return g_strdup_printf("%" G_GUINT64_FORMAT " %s", (tenths + 5) / 10, unit[u]);
    return g_strdup_printf("%" G_GUINT64_FORMAT ".%" G_GUINT64_FORMAT " %s",
                           tenths / 10, tenths % 10, unit[u]);
}

/* ── one connection ───────────────────────────────────────────────── */

typedef void (*TalkLine)(const char *line, gpointer data);
typedef void (*TalkEnd)(const char *last, gpointer data);    /* NULL: no daemon */

typedef struct {
    char              *req;
    int                fd;
    TalkLine           on_line;
    TalkEnd            on_end;
    gpointer           data;
    GSocketConnection *conn;
    GDataInputStream  *in;
} Talk;

static void talk_free(Talk *t)
{
    if (t->req) {
        memset(t->req, 0, strlen(t->req));
        g_free(t->req);
    }
    g_clear_object(&t->in);
    if (t->conn)
        g_io_stream_close(G_IO_STREAM(t->conn), NULL, NULL);
    g_clear_object(&t->conn);
    g_free(t);
}

static void talk_read(Talk *t);

static void on_talk_line(GObject *src, GAsyncResult *res, gpointer data)
{
    Talk *t = data;
    gsize len = 0;
    char *line = g_data_input_stream_read_line_finish_utf8(
        G_DATA_INPUT_STREAM(src), res, &len, NULL);
    if (!line) {
        t->on_end("fail failed the connection to lp-diskd was lost", t->data);
        talk_free(t);
        return;
    }
    if (g_str_has_prefix(line, "done") || g_str_has_prefix(line, "fail")) {
        t->on_end(line, t->data);
        g_free(line);
        talk_free(t);
        return;
    }
    if (t->on_line)
        t->on_line(line, t->data);
    g_free(line);
    talk_read(t);
}

static void talk_read(Talk *t)
{
    g_data_input_stream_read_line_async(t->in, G_PRIORITY_DEFAULT, NULL,
                                        on_talk_line, t);
}

static void on_talk_connected(GObject *src, GAsyncResult *res, gpointer data)
{
    Talk *t = data;
    GError *err = NULL;
    t->conn = g_socket_client_connect_finish(G_SOCKET_CLIENT(src), res, &err);
    g_object_unref(src);
    if (!t->conn) {
        g_clear_error(&err);
        t->on_end(NULL, t->data);
        talk_free(t);
        return;
    }
    /* One line, with the descriptor (if any) riding in the same message:
     * the daemon reads it with recvmsg and refuses a descriptor sent to
     * a verb that takes none. */
    GSocket *sock = g_socket_connection_get_socket(t->conn);
    GOutputVector v = { t->req, strlen(t->req) };
    GSocketControlMessage *msgs[1] = { NULL };
    int nmsg = 0;
    if (t->fd >= 0) {
        msgs[0] = g_unix_fd_message_new();
        if (g_unix_fd_message_append_fd(G_UNIX_FD_MESSAGE(msgs[0]), t->fd, &err))
            nmsg = 1;
        g_clear_error(&err);
    }
    gssize w = g_socket_send_message(sock, NULL, &v, 1, nmsg ? msgs : NULL, nmsg,
                                     G_SOCKET_MSG_NONE, NULL, &err);
    if (msgs[0])
        g_object_unref(msgs[0]);
    if (w != (gssize)v.size) {
        g_clear_error(&err);
        t->on_end(NULL, t->data);
        talk_free(t);
        return;
    }
    memset(t->req, 0, strlen(t->req));
    t->in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(t->conn)));
    g_data_input_stream_set_newline_type(t->in, G_DATA_STREAM_NEWLINE_TYPE_LF);
    talk_read(t);
}

/* Takes ownership of req (ending in "\n"); fd is borrowed. */
static void talk(char *req, int fd, TalkLine on_line, TalkEnd on_end, gpointer data)
{
    Talk *t = g_new0(Talk, 1);
    t->req = req;
    t->fd = fd;
    t->on_line = on_line;
    t->on_end = on_end;
    t->data = data;
    GSocketClient *c = g_socket_client_new();
    GSocketAddress *a = g_unix_socket_address_new(dk_socket());
    g_socket_client_connect_async(c, G_SOCKET_CONNECTABLE(a), NULL, on_talk_connected, t);
    g_object_unref(a);
}

/* ── a request, and the password it may need ─────────────────────── */

typedef struct {
    GtkWindow   *parent;
    char        *req;
    int          fd;
    char        *why_text;
    DkLine       on_line;
    DkDone       on_done;
    gpointer     data;
    LpKitDialog *pw;
    GtkWidget   *entry;
    GtkWidget   *ok;
    guint        wait_timer;
    int          wait_left;
    int          asked;
} Req;

static void req_free(Req *r)
{
    if (r->wait_timer)
        g_source_remove(r->wait_timer);
    if (r->parent)
        g_object_remove_weak_pointer(G_OBJECT(r->parent), (gpointer *)&r->parent);
    if (r->req) {
        memset(r->req, 0, strlen(r->req));    /* may hold a passphrase */
        g_free(r->req);
    }
    if (r->fd >= 0)
        close(r->fd);
    g_free(r->why_text);
    g_free(r);
}

static void split_fail(const char *line, char **why, char **text)
{
    const char *p = line + 4;
    while (*p == ' ')
        p++;
    const char *sp = strchr(p, ' ');
    *why = sp ? g_strndup(p, (gsize)(sp - p)) : g_strdup(p);
    *text = g_strdup(sp ? sp + 1 : "");
}

static void req_finish(Req *r, gboolean ok, const char *why, const char *text)
{
    if (r->pw) {
        lp_kit_dialog_close(r->pw);
        r->pw = NULL;
    }
    if (r->on_done)
        r->on_done(ok, why, text, r->data);
    req_free(r);
}

static void req_line(const char *line, gpointer data)
{
    Req *r = data;
    if (!r->on_line)
        return;
    const char *sp = line;
    while (*sp && *sp != ' ' && *sp != '\t')
        sp++;
    char *kind = g_strndup(line, (gsize)(sp - line));
    r->on_line(kind, *sp ? sp + 1 : "", r->data);
    g_free(kind);
}

static void ask_password(Req *r);

static void req_end(const char *last, gpointer data)
{
    Req *r = data;
    if (!last) {
        req_finish(r, FALSE, "unreachable",
                   T("The disk service (lp-diskd) is not running.",
                     "디스크 서비스(lp-diskd)가 실행 중이 아닙니다."));
        return;
    }
    if (g_str_has_prefix(last, "done")) {
        req_finish(r, TRUE, NULL, last[4] ? last + 5 : "");
        return;
    }
    char *why, *text;
    split_fail(last, &why, &text);
    if (!strcmp(why, "auth") && !strcmp(text, "password required") && r->asked < 4) {
        g_free(why);
        g_free(text);
        ask_password(r);
        return;
    }
    req_finish(r, FALSE, why, text);
    g_free(why);
    g_free(text);
}

static void req_send(Req *r)
{
    talk(g_strdup(r->req), r->fd, req_line, req_end, r);
}

static gboolean pw_tick(gpointer data)
{
    Req *r = data;
    if (!r->pw) {
        r->wait_timer = 0;
        return G_SOURCE_REMOVE;
    }
    if (--r->wait_left <= 0) {
        r->wait_timer = 0;
        lp_kit_dialog_error(r->pw, T("Try again now.", "이제 다시 입력하세요."));
        gtk_widget_set_sensitive(r->ok, TRUE);
        gtk_widget_set_sensitive(r->entry, TRUE);
        gtk_widget_grab_focus(r->entry);
        return G_SOURCE_REMOVE;
    }
    char *m = g_strdup_printf(T("Wrong password. Wait %d s before trying again.",
                                "암호가 틀렸습니다. %d초 뒤에 다시 입력하세요."),
                              r->wait_left);
    lp_kit_dialog_error(r->pw, m);
    g_free(m);
    return G_SOURCE_CONTINUE;
}

static void auth_end(const char *last, gpointer data)
{
    Req *r = data;
    if (!r->pw)
        return;
    lp_kit_dialog_busy(r->pw, FALSE);
    if (!last) {
        lp_kit_dialog_error(r->pw, T("The disk service (lp-diskd) is not running.",
                                     "디스크 서비스(lp-diskd)가 실행 중이 아닙니다."));
        return;
    }
    if (g_str_has_prefix(last, "done")) {
        lp_kit_dialog_close(r->pw);
        r->pw = NULL;
        req_send(r);
        return;
    }
    char *why, *text;
    split_fail(last, &why, &text);
    if (!strcmp(why, "auth") && g_str_has_prefix(text, "wait ")) {
        int s = atoi(text + 5);
        r->wait_left = (s > 0 ? s : 2) + 1;
        gtk_widget_set_sensitive(r->ok, FALSE);
        gtk_widget_set_sensitive(r->entry, FALSE);
        if (r->wait_timer)
            g_source_remove(r->wait_timer);
        r->wait_timer = g_timeout_add_seconds(1, pw_tick, r);
        pw_tick(r);
    } else if (!strcmp(why, "auth")) {
        lp_kit_dialog_error(r->pw, T("Wrong password. Try again.",
                                     "암호가 틀렸습니다. 다시 입력하세요."));
        gtk_widget_grab_focus(r->entry);
    } else if (!strcmp(why, "denied")) {
        lp_kit_dialog_error(r->pw,
            T("This account is not an administrator. Ask an administrator to do this.",
              "이 계정은 관리자가 아닙니다. 관리자에게 부탁하세요."));
        gtk_widget_set_sensitive(r->ok, FALSE);
        gtk_widget_set_sensitive(r->entry, FALSE);
    } else {
        lp_kit_dialog_error(r->pw, text);
    }
    g_free(why);
    g_free(text);
}

static void pw_response(LpKitDialog *d, int response, gpointer data)
{
    Req *r = data;
    if (response != LP_KIT_OK) {
        r->pw = NULL;
        lp_kit_dialog_close(d);
        req_finish(r, FALSE, "cancelled", T("Cancelled", "취소했습니다"));
        return;
    }
    const char *pw = gtk_editable_get_text(GTK_EDITABLE(r->entry));
    size_t n = strlen(pw);
    if (n == 0 || n > 255) {
        lp_kit_dialog_error(d, T("Type your password.", "암호를 입력하세요."));
        return;
    }
    char *hex = hexof(pw, "auth\t");
    char *req = g_strconcat(hex, "\n", NULL);
    memset(hex, 0, strlen(hex));
    g_free(hex);
    gtk_editable_set_text(GTK_EDITABLE(r->entry), "");
    lp_kit_dialog_error(d, NULL);
    lp_kit_dialog_busy(d, TRUE);
    talk(req, -1, NULL, auth_end, r);
}

static void ask_password(Req *r)
{
    r->asked++;
    char *who = g_strdup_printf(T("Enter the password for %s.",
                                  "%s 사용자의 암호를 입력하세요."), g_get_user_name());
    char *text = g_strdup_printf("%s %s", r->why_text ? r->why_text : "", who);
    LpKitDialog *d = lp_kit_dialog_new(r->parent, T("Administrator password", "관리자 암호"),
                                       text);
    g_free(who);
    g_free(text);
    r->entry = gtk_password_entry_new();
    gtk_widget_set_name(r->entry, "password");
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(r->entry), TRUE);
    g_object_set(r->entry, "activates-default", TRUE, NULL);
    gtk_box_append(GTK_BOX(lp_kit_dialog_body(d)), r->entry);
    GtkWidget *note = gtk_label_new(
        T("It is kept for 5 minutes, so the next change does not ask again.",
          "5분 동안 기억하므로 그 사이의 다음 변경은 다시 묻지 않습니다."));
    gtk_widget_add_css_class(note, "lp-sheet-text");
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(note), 46);
    gtk_box_append(GTK_BOX(lp_kit_dialog_body(d)), note);
    lp_kit_dialog_button(d, T("Cancel", "취소"), LP_KIT_CANCEL, NULL);
    r->ok = lp_kit_dialog_button(d, T("Authenticate", "인증"), LP_KIT_OK, "suggested-action");
    lp_kit_dialog_on_response(d, pw_response, r);
    r->pw = d;
    lp_kit_dialog_present(d);
    gtk_widget_grab_focus(r->entry);
}

void dk_request(GtkWindow *parent, const char *const *fields, int fd,
                const char *why_text, DkLine on_line, DkDone on_done, gpointer data)
{
    Req *r = g_new0(Req, 1);
    r->parent = parent;
    if (parent)
        g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&r->parent);
    r->fd = fd >= 0 ? dup(fd) : -1;
    r->why_text = g_strdup(why_text ? why_text
                           : T("Changing disks needs an administrator.",
                               "디스크를 바꾸려면 관리자 권한이 필요합니다."));
    r->on_line = on_line;
    r->on_done = on_done;
    r->data = data;
    GString *s = g_string_new(NULL);
    for (int i = 0; fields[i]; i++) {
        if (strpbrk(fields[i], "\t\n\r")) {
            memset(s->str, 0, s->len);
            g_string_free(s, TRUE);
            req_finish(r, FALSE, "invalid", T("Not a valid value", "올바른 값이 아닙니다"));
            return;
        }
        if (i)
            g_string_append_c(s, '\t');
        g_string_append(s, fields[i]);
    }
    g_string_append_c(s, '\n');
    r->req = g_string_free(s, FALSE);
    req_send(r);
}
