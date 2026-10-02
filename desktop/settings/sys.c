/*
 * sys.c - how the settings app changes the machine: running commands,
 * running them as administrator, and editing config files one key at a
 * time.
 *
 * ── Commands ──
 *
 * Everything goes through GSubprocess with an argv array, never a shell
 * string: an SSID or a user's full name is data typed by a person, and
 * the moment it is pasted into "sh -c" it is also code. Children get
 * LC_ALL=C.UTF-8 because several panels parse what comes back (wpctl,
 * timedatectl, sudo's refusal), and a translated "password is required"
 * would silently stop matching.
 *
 * Standard input is always a pipe, closed after whatever the caller gave.
 * A child that unexpectedly asks a question (passwd, sudo without -n)
 * then reads end-of-file and fails at once, rather than waiting forever
 * on a terminal this app does not have.
 *
 * ── Administrator ──
 *
 * The desktop runs as an ordinary account (uid 1000), so changing the
 * time zone, the host name or another account needs root, and sudo is
 * how this system grants it. The owner's rule is that on the normal
 * system nothing becomes administrator without the person's own password
 * (only the recovery shell is root without one), so the password is
 * asked for in our own dialog every time, with one allowance - the same
 * five minutes sudo itself keeps (COMMON.md: auth-admin-keep):
 *
 *   1. ask for the password in our own dialog (skipped only within five
 *      minutes of a password this window checked)
 *   2. `sudo -k`             forget any timestamp somebody else left, so
 *                            the check below is a real check
 *   3. `sudo -S -v`          check the password, with nothing else on stdin
 *   4. `sudo -n cmd <data>`  the timestamp from step 3 lets it through
 *   5. `sudo -S cmd`         only if this sudo keeps no timestamp
 *
 * Step 3 is the one that matters. Handing sudo the password and the
 * command's stdin in one stream works when the password is right; when it
 * is wrong, sudo reads the next lines as further password attempts - and
 * for chpasswd the next line is somebody's new password. Checking first
 * means a wrong password never meets the data.
 *
 * Step 1's five minutes are this process's own clock, not a guess about
 * sudo's: `sudo -n` is only tried inside them, and if sudo has forgotten
 * sooner the dialog comes back. A silent `sudo -n` outside them could
 * succeed on a timestamp this window never earned, which is exactly the
 * "administrator without a password" the owner ruled out.
 *
 * The password lives in memory only between the dialog and the command,
 * and is overwritten before it is freed.
 *
 * ── Files ──
 *
 * Every file is replaced atomically and durably (COMMON.md, Persistence):
 * written to a temporary file in the same directory, fsync'd, renamed
 * over the old one, and the directory fsync'd. A setting that survives a
 * reboot but not a power cut is a setting that is lost on the one day it
 * matters, and a half-written file read by lp-idle or wayfire at the next
 * login is worse than an old one.
 *
 * That includes wayfire.ini. An earlier version of this file wrote it in
 * place, believing wayfire only notices changes to the inode it watches.
 * It notices the replacement too: wayfire 0.7 watches ~/.config for
 * IN_CREATE and the file for IN_MODIFY, re-reads on any event, and the
 * rename that replaces the file drops the last link to the inode it was
 * watching - which the kernel reports as IN_IGNORED on that watch, waking
 * it to read the new file. (Wayfire 0.8 watches for IN_MOVED_TO by name,
 * which the rename is.) The verification in the track report runs the
 * write against a watcher with exactly wayfire 0.7's watch set.
 */
#include "core.h"

#include <glib/gstdio.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

int lp_quiet = 0;

/* ── running commands ───────────────────────────────────────────────── */

static GSubprocessLauncher *launcher(void)
{
    GSubprocessLauncher *l = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_setenv(l, "LC_ALL", "C.UTF-8", TRUE);
    return l;
}

/* What the banner says when a command was stopped for taking too long. */
static char *timeout_words(const char *const *argv, guint ms)
{
    char *base = g_path_get_basename(argv[0]);
    char *w = g_strdup_printf(T("%s did not answer within %u seconds, so it was stopped",
                                "%s 이(가) %u초 안에 답하지 않아 멈췄습니다"),
                              base, (ms + 999) / 1000);
    g_free(base);
    return w;
}

/* ── synchronous, with a limit ──
 *
 * lp_run_full is called from button handlers and from the functions that
 * build a page, on the one thread that also draws the window. It used to
 * wait for as long as the command took, and a command that never answers
 * - bluetoothctl with no bluetoothd behind it, a sound service that hangs
 * - left the whole window frozen: no redraw, no click, no way out but
 * killing it. Now the wait runs on a private main context (what GLib's own
 * g_subprocess_communicate does inside) with a timer beside it, and when
 * the timer fires first the child is killed and the caller gets
 * LP_RUN_TIMEOUT and a sentence saying so. Commands on this path are the
 * ones that answer at once; the few that legitimately take longer (a Wi-Fi
 * scan, stepping through display modes) pass their own limit, and
 * administrator jobs never come this way - they are asynchronous. */

typedef struct {
    GSubprocess  *sp;
    GCancellable *cancel;
    GAsyncResult *res;
    gboolean      timed_out;
} wait_t;

static void wait_done(GObject *src, GAsyncResult *res, gpointer p)
{
    (void)src;
    ((wait_t *)p)->res = g_object_ref(res);
}

static gboolean wait_expired(gpointer p)
{
    wait_t *w = p;
    w->timed_out = TRUE;
    g_subprocess_force_exit(w->sp);
    /* Also stop reading: a child's own children can hold its pipes open
     * after it is gone. */
    g_cancellable_cancel(w->cancel);
    return G_SOURCE_REMOVE;
}

int lp_run_full_timeout(const char *const *argv, const char *in,
                        char **out, char **err, guint ms)
{
    if (out) *out = NULL;
    if (err) *err = NULL;

    GError *e = NULL;
    GSubprocessLauncher *l = launcher();
    GSubprocess *sp = g_subprocess_launcher_spawnv(l, argv, &e);
    g_object_unref(l);
    if (!sp) {
        if (err) *err = g_strdup(e->message);
        g_error_free(e);
        return -1;
    }

    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    wait_t w = { sp, g_cancellable_new(), NULL, FALSE };
    g_subprocess_communicate_utf8_async(sp, in ? in : "", w.cancel, wait_done, &w);
    GSource *timer = g_timeout_source_new(ms ? ms : LP_RUN_SYNC_MS);
    g_source_set_callback(timer, wait_expired, &w, NULL);
    g_source_attach(timer, ctx);
    while (!w.res)
        g_main_context_iteration(ctx, TRUE);
    g_source_destroy(timer);
    g_source_unref(timer);

    char *o = NULL, *r = NULL;
    int status = -1;
    if (g_subprocess_communicate_utf8_finish(sp, w.res, &o, &r, &e)) {
        if (g_subprocess_get_if_exited(sp))
            status = g_subprocess_get_exit_status(sp);
    } else if (w.timed_out) {
        status = LP_RUN_TIMEOUT;
        r = timeout_words(argv, ms ? ms : LP_RUN_SYNC_MS);
        g_printerr("lp-settings: %s\n", r);
        g_error_free(e);
    } else {
        r = g_strdup(e->message);
        g_error_free(e);
    }
    g_object_unref(w.res);
    g_object_unref(w.cancel);
    g_object_unref(sp);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);

    if (o) g_strchomp(o);
    if (out) *out = o; else g_free(o);
    if (err) *err = r; else g_free(r);
    return status;
}

int lp_run_full(const char *const *argv, const char *in, char **out, char **err)
{
    return lp_run_full_timeout(argv, in, out, err, LP_RUN_SYNC_MS);
}

char *lp_run(const char *const *argv)
{
    char *out = NULL;
    int st = lp_run_full(argv, NULL, &out, NULL);
    if (st != 0) {
        g_free(out);
        return NULL;
    }
    return out;
}

typedef struct {
    GWeakRef   owner;
    gboolean   had_owner;
    lp_done_fn done;
    gpointer   data;
    int        status;
    char      *out, *err;
    /* Only for lp_run_async_timeout: */
    GSubprocess  *sp;
    GCancellable *cancel;
    guint      timer, ms;
    char      *what;
    gboolean   timed_out;
} job_t;

/* Commands started and not yet answered. `lp-settings --restore` has no
 * window and no main loop of its own; it runs the loop until this is
 * zero, so a restore that starts something in the background (gsettings,
 * makoctl) is not cut off by the process exiting under it. */
static int jobs_pending;

int lp_jobs_pending(void) { return jobs_pending; }

static void job_deliver(job_t *j)
{
    jobs_pending--;
    GObject *o = j->had_owner ? g_weak_ref_get(&j->owner) : NULL;
    if (j->done && (!j->had_owner || o))
        j->done(j->status, j->out ? j->out : "", j->err ? j->err : "",
                j->data);
    if (o) g_object_unref(o);
    g_weak_ref_clear(&j->owner);
    g_free(j->out);
    g_free(j->err);
    g_free(j->what);
    g_free(j);
}

static gboolean job_deliver_idle(gpointer p)
{
    job_deliver(p);
    return G_SOURCE_REMOVE;
}

static gboolean job_expired(gpointer p)
{
    job_t *j = p;
    j->timer = 0;
    j->timed_out = TRUE;
    g_subprocess_force_exit(j->sp);
    g_cancellable_cancel(j->cancel);
    return G_SOURCE_REMOVE;
}

static void job_finished(GObject *src, GAsyncResult *res, gpointer p)
{
    job_t *j = p;
    GSubprocess *sp = G_SUBPROCESS(src);
    GError *e = NULL;

    if (j->timer) {
        g_source_remove(j->timer);
        j->timer = 0;
    }
    j->status = -1;
    if (g_subprocess_communicate_utf8_finish(sp, res, &j->out, &j->err, &e)) {
        if (g_subprocess_get_if_exited(sp))
            j->status = g_subprocess_get_exit_status(sp);
    } else if (j->timed_out) {
        const char *v[] = { j->what, NULL };
        j->status = LP_RUN_TIMEOUT;
        g_free(j->err);
        j->err = timeout_words(v, j->ms);
        g_printerr("lp-settings: %s\n", j->err);
        g_error_free(e);
    } else {
        g_free(j->err);
        j->err = g_strdup(e->message);
        g_error_free(e);
    }
    if (j->out) g_strchomp(j->out);
    g_clear_object(&j->cancel);
    j->sp = NULL;
    g_object_unref(sp);
    job_deliver(j);
}

void lp_run_async_timeout(const char *const *argv, const char *in, guint ms,
                          GtkWidget *owner, lp_done_fn done, gpointer data)
{
    job_t *j = g_new0(job_t, 1);
    jobs_pending++;
    j->done = done;
    j->data = data;
    j->had_owner = owner != NULL;
    g_weak_ref_init(&j->owner, owner);

    GError *e = NULL;
    GSubprocessLauncher *l = launcher();
    GSubprocess *sp = g_subprocess_launcher_spawnv(l, argv, &e);
    g_object_unref(l);
    if (!sp) {
        /* Still reported later rather than now: callers are written for
         * an answer that arrives after they return, and one that arrives
         * inside the call runs their completion before their setup. */
        j->status = -1;
        j->err = g_strdup(e->message);
        g_error_free(e);
        g_idle_add(job_deliver_idle, j);
        return;
    }
    if (ms) {
        j->sp = sp;
        j->cancel = g_cancellable_new();
        j->ms = ms;
        j->what = g_strdup(argv[0]);
        j->timer = g_timeout_add(ms, job_expired, j);
    }
    g_subprocess_communicate_utf8_async(sp, in ? in : "", j->cancel,
                                        job_finished, j);
}

void lp_run_async(const char *const *argv, const char *in, GtkWidget *owner,
                  lp_done_fn done, gpointer data)
{
    lp_run_async_timeout(argv, in, 0, owner, done, data);
}

/* The Debian base's program of that name, when there is one. PATH puts
 * LP's own /bin first, and a few of its programs share a name with the
 * base's but not its manner: /bin/lsblk has no --json, /bin/apt answers
 * only root, and /bin/passwd keeps the hash in /data/shadow, which on
 * the desktop nothing else reads - sudo and the lock screen check
 * /etc/shadow, where the base's passwd writes. */
const char *lp_base_tool(const char *name)
{
    static GHashTable *seen;
    if (!seen)
        seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    const char *hit = g_hash_table_lookup(seen, name);
    if (hit) return hit;
    char *p = g_build_filename("/usr/bin", name, NULL);
    if (!g_file_test(p, G_FILE_TEST_IS_EXECUTABLE)) {
        g_free(p);
        p = g_strdup(name);
    }
    g_hash_table_insert(seen, g_strdup(name), p);
    return p;
}

gboolean lp_spawn_bg(const char *const *argv)
{
    GError *e = NULL;
    /* Detached into its own session: a night-light daemon or the Disks
     * app must outlive this window, and must not die with it on SIGHUP. */
    gboolean ok = g_spawn_async(NULL, (char **)argv, NULL,
                                G_SPAWN_SEARCH_PATH |
                                G_SPAWN_STDOUT_TO_DEV_NULL |
                                G_SPAWN_STDERR_TO_DEV_NULL,
                                (GSpawnChildSetupFunc)(void (*)(void))setsid,
                                NULL, NULL, &e);
    if (!ok) {
        lp_toast(TRUE, "%s", e->message);
        g_error_free(e);
    }
    return ok;
}

char *lp_first_line(const char *err, const char *out)
{
    const char *src = (err && *err) ? err : out;
    if (!src || !*src)
        return g_strdup(T("it failed without saying why",
                          "이유를 알리지 않고 실패했습니다"));
    /* The last non-empty line: tools print context first and the verdict
     * last ("sudo: 1 incorrect password attempt"). */
    char **l = g_strsplit(src, "\n", -1);
    const char *pick = NULL;
    for (int i = 0; l[i]; i++) {
        char *t = g_strstrip(l[i]);
        if (*t) pick = t;
    }
    char *r = g_strdup(pick ? pick : src);
    g_strfreev(l);
    return r;
}

/* ── administrator ──────────────────────────────────────────────────── */

gboolean lp_is_admin(void)
{
    if (getuid() == 0)
        return TRUE;
    struct passwd *pw = getpwuid(getuid());
    if (!pw)
        return FALSE;

    gid_t groups[128];
    int n = G_N_ELEMENTS(groups);
    if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &n) < 0)
        return FALSE;
    for (int i = 0; i < n; i++) {
        struct group *g = getgrgid(groups[i]);
        /* sudo on Debian; wheel and admin on the systems people come from. */
        if (g && (!strcmp(g->gr_name, "sudo") || !strcmp(g->gr_name, "wheel") ||
                  !strcmp(g->gr_name, "admin")))
            return TRUE;
    }
    return FALSE;
}

/* auth-admin-keep: until when (monotonic microseconds) a password this
 * window checked still counts. */
#define ADMIN_KEEP_US (5 * 60 * G_USEC_PER_SEC)
static gint64 admin_until;

typedef struct {
    char      **argv;
    char       *in;
    char       *why;
    char       *pw;
    GWeakRef    owner;
    gboolean    had_owner;
    lp_done_fn  done;
    gpointer    data;
    lp_dialog_t *dlg;
    GtkWidget  *entry;
} admin_t;

static void admin_free(admin_t *a)
{
    if (a->pw) {
        memset(a->pw, 0, strlen(a->pw));
        g_free(a->pw);
    }
    g_strfreev(a->argv);
    if (a->in) {
        memset(a->in, 0, strlen(a->in));
        g_free(a->in);
    }
    g_free(a->why);
    g_weak_ref_clear(&a->owner);
    g_free(a);
}

static void admin_finish(admin_t *a, int status, const char *out,
                         const char *err)
{
    GObject *o = a->had_owner ? g_weak_ref_get(&a->owner) : NULL;
    if (a->done && (!a->had_owner || o))
        a->done(status, out, err, a->data);
    if (o) g_object_unref(o);
    admin_free(a);
}

static gboolean wants_password(const char *err)
{
    return err && (strstr(err, "password is required") ||
                   strstr(err, "a terminal is required") ||
                   strstr(err, "askpass") ||
                   strstr(err, "password required"));
}

static char **sudo_argv(const char *const *pre, char **argv)
{
    GPtrArray *v = g_ptr_array_new();
    for (int i = 0; pre[i]; i++)
        g_ptr_array_add(v, g_strdup(pre[i]));
    for (int i = 0; argv[i]; i++)
        g_ptr_array_add(v, g_strdup(argv[i]));
    g_ptr_array_add(v, NULL);
    return (char **)g_ptr_array_free(v, FALSE);
}

static void admin_ask(admin_t *a);

static void admin_last_done(int st, const char *out, const char *err, gpointer p)
{
    admin_finish(p, st, out, err);
}

static void admin_after_run(int st, const char *out, const char *err,
                            gpointer p)
{
    admin_t *a = p;
    if (st == 0 || !wants_password(err)) {
        admin_finish(a, st, out, err);
        return;
    }
    if (!a->pw) {
        /* Inside our five minutes, but sudo has already forgotten: ask. */
        admin_until = 0;
        admin_ask(a);
        return;
    }
    /* This sudo keeps no timestamp (timestamp_timeout=0), so the password
     * goes in front of the data after all. It has already been checked,
     * so the data cannot be mistaken for a second attempt. */
    static const char *const pre[] = { "sudo", "-S", "-p", "", "--", NULL };
    char **v = sudo_argv(pre, a->argv);
    char *in = g_strconcat(a->pw, "\n", a->in ? a->in : "", NULL);
    lp_run_async((const char *const *)v, in, NULL, admin_last_done, a);
    memset(in, 0, strlen(in));
    g_free(in);
    g_strfreev(v);
}

static void admin_run_cmd(admin_t *a)
{
    static const char *const pre[] = { "sudo", "-n", "--", NULL };
    char **v = sudo_argv(pre, a->argv);
    lp_run_async((const char *const *)v, a->in, NULL, admin_after_run, a);
    g_strfreev(v);
}

static void admin_validated(int st, const char *out, const char *err, gpointer p)
{
    (void)out;
    admin_t *a = p;
    /* Cancelled while sudo was checking, and the sheet is on its way
     * out: the answer is not wanted (admin_dialog_gone reports the
     * cancel when the window goes). */
    if (g_object_get_data(G_OBJECT(lp_dialog_window(a->dlg)), "lp-closing"))
        return;
    lp_dialog_busy(a->dlg, FALSE);

    if (st != 0) {
        if (err && strstr(err, "not in the sudoers"))
            lp_dialog_error(a->dlg, T("This account is not an administrator. "
                                      "Ask someone who is.",
                                      "이 계정은 관리자가 아닙니다. "
                                      "관리자에게 부탁하십시오."));
        else if (st == -1)
            lp_dialog_error(a->dlg, T("sudo is not installed.",
                                      "sudo 가 설치되어 있지 않습니다."));
        else
            lp_dialog_error(a->dlg, T("That password is not right. Try again.",
                                      "비밀번호가 맞지 않습니다. 다시 입력하십시오."));
        gtk_editable_set_text(GTK_EDITABLE(a->entry), "");
        gtk_widget_grab_focus(a->entry);
        return;
    }
    admin_until = g_get_monotonic_time() + ADMIN_KEEP_US;

    /* The dialog is not needed any more; the admin_t lives on until the
     * command itself has answered. */
    lp_dialog_set_data(a->dlg, "lp-admin", NULL, NULL);
    lp_dialog_close(a->dlg);
    a->dlg = NULL;
    admin_run_cmd(a);
}

static void admin_forgot(int st, const char *out, const char *err, gpointer p)
{
    (void)st; (void)out; (void)err;
    admin_t *a = p;
    char *in = g_strconcat(a->pw, "\n", NULL);
    static const char *const v[] = { "sudo", "-S", "-p", "", "-v", NULL };
    /* Owned by the password sheet, as the -k before it: closed while
     * sudo was checking (Cancel, Escape), the sheet's going frees `a`,
     * and an answer that still came to admin_validated used it - and
     * a->dlg, NULL by then - and Settings closed. An owned job whose
     * owner is gone does not call back. */
    lp_run_async(v, in, lp_dialog_window(a->dlg), admin_validated, a);
    memset(in, 0, strlen(in));
    g_free(in);
}

static void admin_dialog_ok(lp_dialog_t *d, gpointer p)
{
    admin_t *a = p;
    const char *pw = gtk_editable_get_text(GTK_EDITABLE(a->entry));
    if (!pw || !*pw) {
        lp_dialog_error(d, T("Type the password first.",
                             "먼저 비밀번호를 입력하십시오."));
        return;
    }
    if (a->pw) {
        memset(a->pw, 0, strlen(a->pw));
        g_free(a->pw);
    }
    a->pw = g_strdup(pw);
    lp_dialog_busy(d, TRUE);
    /* -k first: a timestamp left by a terminal's sudo must not make the
     * check below pass without looking at the password. */
    static const char *const k[] = { "sudo", "-k", NULL };
    lp_run_async(k, NULL, lp_dialog_window(d), admin_forgot, a);
}

/* Closed without an answer: the action did not happen, and the caller
 * hears so - its switch goes back to where it was. */
static void admin_dialog_gone(gpointer p)
{
    admin_t *a = p;
    a->dlg = NULL;
    admin_finish(a, -2, "", T("Cancelled.", "취소했습니다."));
}

static void admin_ask(admin_t *a)
{
    a->dlg = lp_dialog_new(T("Administrator password", "관리자 비밀번호"),
                           T("Authenticate", "인증"), FALSE,
                           admin_dialog_ok, a);
    lp_dialog_text(a->dlg, a->why, NULL);
    struct passwd *pw = getpwuid(getuid());
    char *label = g_strdup_printf(T("Password for %s", "%s 의 비밀번호"),
                                  pw ? pw->pw_name : "?");
    a->entry = lp_dialog_entry(a->dlg, label, NULL, TRUE);
    g_free(label);
    /* The dialog owns the pending request from here: closing it by any
     * route - Cancel, Escape, the window going away - reports a cancel. */
    lp_dialog_set_data(a->dlg, "lp-admin", a, admin_dialog_gone);
    lp_dialog_present(a->dlg);
}

void lp_admin_run(const char *const *argv, const char *in, const char *why,
                  GtkWidget *owner, lp_done_fn done, gpointer data)
{
    if (getuid() == 0) {
        lp_run_async(argv, in, owner, done, data);
        return;
    }

    admin_t *a = g_new0(admin_t, 1);
    a->argv = g_strdupv((char **)argv);
    a->in = in ? g_strdup(in) : NULL;
    a->why = g_strdup(why ? why : T("This change needs an administrator.",
                                    "이 변경은 관리자 권한이 필요합니다."));
    a->done = done;
    a->data = data;
    a->had_owner = owner != NULL;
    g_weak_ref_init(&a->owner, owner);

    if (g_get_monotonic_time() < admin_until)
        admin_run_cmd(a);
    else
        admin_ask(a);
}

gboolean lp_admin_fresh(void)
{
    return getuid() == 0 || g_get_monotonic_time() < admin_until;
}

/* ── a root-owned file, replaced atomically ─────────────────────────────
 *
 * Six commands under the one password: mkdir for a directory that is not
 * there yet, tee writes the new text beside the target, chmod, sync puts
 * it on the disk, mv renames it over the target (rename, so a reader sees
 * the old file or the new one and never half of either), and sync on the
 * directory makes the rename itself durable. Each is a plain argv; no
 * shell sees the path or the text. */

typedef struct {
    char      *dest, *tmp, *body, *why;
    int        step;
    GWeakRef   owner;
    gboolean   had_owner;
    lp_done_fn done;
    gpointer   data;
} awrite_t;

static void awrite_step(int st, const char *out, const char *err, gpointer p)
{
    awrite_t *w = p;
    if (w->step > 0 && st != 0) {
        GObject *o = w->had_owner ? g_weak_ref_get(&w->owner) : NULL;
        if (w->done && (!w->had_owner || o))
            w->done(st, out, err, w->data);
        if (o) g_object_unref(o);
        goto out;
    }
    char *dir = g_path_get_dirname(w->dest);
    const char *v0[] = { "mkdir", "-p", dir, NULL };
    const char *v1[] = { "tee", w->tmp, NULL };
    const char *v2[] = { "chmod", "0644", w->tmp, NULL };
    const char *v3[] = { "sync", w->tmp, NULL };
    const char *v4[] = { "mv", "-f", w->tmp, w->dest, NULL };
    const char *v5[] = { "sync", dir, NULL };
    const char *const *steps[] = { v0, v1, v2, v3, v4, v5 };
    int i = w->step++;
    if (i < 6) {
        lp_admin_run(steps[i], i == 1 ? w->body : NULL, w->why, NULL, awrite_step, w);
        g_free(dir);
        return;
    }
    g_free(dir);
    {
        GObject *o = w->had_owner ? g_weak_ref_get(&w->owner) : NULL;
        if (w->done && (!w->had_owner || o))
            w->done(0, "", "", w->data);
        if (o) g_object_unref(o);
    }
out:
    g_weak_ref_clear(&w->owner);
    g_free(w->dest); g_free(w->tmp); g_free(w->body); g_free(w->why);
    g_free(w);
}

void lp_admin_write_file(const char *dest, const char *body, const char *why,
                         GtkWidget *owner, lp_done_fn done, gpointer data)
{
    awrite_t *w = g_new0(awrite_t, 1);
    w->dest = g_strdup(dest);
    w->tmp = g_strdup_printf("%s.lp-new", dest);
    w->body = g_strdup(body);
    w->why = g_strdup(why);
    w->done = done;
    w->data = data;
    w->had_owner = owner != NULL;
    g_weak_ref_init(&w->owner, owner);
    awrite_step(0, "", "", w);
}

/* ── the latest value, and only that ────────────────────────────────────
 *
 * A slider dragged across its range asks for thirty values a second. If
 * each became a process the moment it was asked for, a slow backend (the
 * first wpctl after the audio server woke) would have a queue of stale
 * volumes to work through after the finger stopped. So per key there is
 * at most one command running and at most one waiting; a newer value
 * replaces the waiting one. */

typedef struct {
    gboolean running;
    char   **pending;
} latest_t;

static GHashTable *latest;

static void latest_done(int st, const char *out, const char *err, gpointer p)
{
    char *key = p;
    latest_t *l = g_hash_table_lookup(latest, key);
    if (st != 0) {
        char *why = lp_first_line(err, out);
        lp_toast(TRUE, "%s", why);
        g_free(why);
    }
    if (l) {
        l->running = FALSE;
        if (l->pending) {
            char **v = l->pending;
            l->pending = NULL;
            l->running = TRUE;
            lp_run_async((const char *const *)v, NULL, NULL, latest_done, key);
            g_strfreev(v);
            return;
        }
    }
    g_free(key);
}

void lp_run_latest(const char *key, const char *const *argv)
{
    if (!latest)
        latest = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    latest_t *l = g_hash_table_lookup(latest, key);
    if (!l) {
        l = g_new0(latest_t, 1);
        g_hash_table_insert(latest, g_strdup(key), l);
    }
    if (l->running) {
        g_strfreev(l->pending);
        l->pending = g_strdupv((char **)argv);
        return;
    }
    l->running = TRUE;
    lp_run_async(argv, NULL, NULL, latest_done, g_strdup(key));
}

/* ── after the finger stops ──────────────────────────────────────────
 *
 * A slider that is also a file (the touchpad speed is a line in
 * wayfire.ini, night light's warmth a line in nightlight.conf) must not
 * be written thirty times a second: each write is an fsync, and each one
 * makes wayfire re-read its whole config or restarts wlsunset. So the
 * write waits until the value has been still for a moment; a newer value
 * for the same key replaces the waiting one. */

typedef struct {
    guint          id;
    void         (*fn)(gpointer);
    gpointer       data;
    GDestroyNotify free_fn;
} later_t;

static GHashTable *laters;

static void later_free(gpointer p)
{
    later_t *l = p;
    if (l->id) g_source_remove(l->id);
    if (l->free_fn) l->free_fn(l->data);
    g_free(l);
}

static gboolean later_fire(gpointer key)
{
    gpointer k = NULL, v = NULL;
    /* Out of the table before it runs, so fn may ask for another. */
    if (g_hash_table_steal_extended(laters, key, &k, &v)) {
        later_t *l = v;
        l->id = 0;
        l->fn(l->data);
        later_free(l);
        g_free(k);
    }
    return G_SOURCE_REMOVE;
}

void lp_later(const char *key, guint ms, void (*fn)(gpointer), gpointer data,
              GDestroyNotify free_fn)
{
    if (!laters)
        laters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, later_free);
    later_t *l = g_new0(later_t, 1);
    l->fn = fn;
    l->data = data;
    l->free_fn = free_fn;
    char *k = g_strdup(key);
    g_hash_table_replace(laters, k, l);
    l->id = g_timeout_add(ms, later_fire, k);
}

/* ── files ──────────────────────────────────────────────────────────── */

char *lp_slurp(const char *path)
{
    char *buf = NULL;
    gsize len = 0;
    if (!path || !g_file_get_contents(path, &buf, &len, NULL))
        return NULL;
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    return buf;
}

/* The directory, fsync'd after the rename: the rename itself is only on
 * disk once the directory's own block is. Without it, a power cut right
 * after a write can bring back the old file - or, for a file that did
 * not exist before, no file at all. */
static void sync_dir(const char *path)
{
    char *dir = g_path_get_dirname(path);
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    g_free(dir);
}

gboolean lp_write_file_mode(const char *path, const char *body, int mode)
{
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    /* A file that is already there keeps its permissions: settings.ini or
     * wayfire.ini made 0600 by hand should not become world-readable
     * because a switch was flipped. */
    GStatBuf st;
    if (mode < 0)
        mode = (g_stat(path, &st) == 0) ? (int)(st.st_mode & 07777) : 0644;

    GError *e = NULL;
    if (!g_file_set_contents_full(path, body, -1,
                                  G_FILE_SET_CONTENTS_CONSISTENT |
                                  G_FILE_SET_CONTENTS_DURABLE,
                                  mode, &e)) {
        lp_toast(TRUE, T("Could not write %s: %s", "%s 을(를) 쓰지 못했습니다: %s"),
                 path, e->message);
        g_error_free(e);
        return FALSE;
    }
    sync_dir(path);
    return TRUE;
}

gboolean lp_write_file(const char *path, const char *body)
{
    return lp_write_file_mode(path, body, -1);
}

gboolean lp_remove_file(const char *path)
{
    if (g_remove(path) != 0 && errno != ENOENT) {
        lp_toast(TRUE, T("Could not remove %s: %s", "%s 을(를) 지우지 못했습니다: %s"),
                 path, g_strerror(errno));
        return FALSE;
    }
    sync_dir(path);
    return TRUE;
}

char *lp_config_path(const char *name)
{
    return g_build_filename(g_get_user_config_dir(), "lp", name, NULL);
}

char *wayfire_ini(void)
{
    const char *env = g_getenv("WAYFIRE_CONFIG_FILE");
    if (env && *env)
        return g_strdup(env);
    return g_build_filename(g_get_user_config_dir(), "wayfire.ini", NULL);
}

/* ── key=value ──────────────────────────────────────────────────────── */

static gboolean kv_line_is(const char *line, const char *key, const char **val)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '#' || *line == ';')
        return FALSE;
    size_t kl = strlen(key);
    if (strncmp(line, key, kl) != 0)
        return FALSE;
    const char *p = line + kl;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=')
        return FALSE;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (val) *val = p;
    return TRUE;
}

char *kv_get(const char *path, const char *key)
{
    char *body = lp_slurp(path);
    if (!body) return NULL;
    char **l = g_strsplit(body, "\n", -1);
    char *r = NULL;
    for (int i = 0; l[i] && !r; i++) {
        const char *v;
        if (kv_line_is(l[i], key, &v))
            r = g_strchomp(g_strdup(v));
    }
    g_strfreev(l);
    g_free(body);
    return r;
}

int kv_get_int(const char *path, const char *key, int dflt)
{
    char *v = kv_get(path, key);
    if (!v || !*v) { g_free(v); return dflt; }
    char *end = NULL;
    long n = strtol(v, &end, 10);
    int r = (end && end != v) ? (int)n : dflt;
    g_free(v);
    return r;
}

gboolean kv_set(const char *path, const char *key, const char *value)
{
    char *body = lp_slurp(path);
    GString *out = g_string_new(NULL);
    gboolean done = FALSE;

    if (body) {
        char **l = g_strsplit(body, "\n", -1);
        for (int i = 0; l[i]; i++) {
            if (!done && kv_line_is(l[i], key, NULL)) {
                g_string_append_printf(out, "%s=%s\n", key, value);
                done = TRUE;
            } else {
                g_string_append_printf(out, "%s\n", l[i]);
            }
        }
        g_strfreev(l);
    } else {
        g_string_append(out, "# Written by Settings (lp-settings). One key=value per line;\n"
                             "# editing by hand is fine, Settings reads it back.\n");
    }
    if (!done)
        g_string_append_printf(out, "%s=%s\n", key, value);

    gboolean ok = lp_write_file(path, out->str);
    g_string_free(out, TRUE);
    g_free(body);
    return ok;
}

/* ── INI ───────────────────────────────────────────────────────────── */

static gboolean ini_is_section(const char *line, char **name)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line != '[')
        return FALSE;
    const char *end = strchr(line, ']');
    if (!end)
        return FALSE;
    if (name) *name = g_strstrip(g_strndup(line + 1, end - line - 1));
    return TRUE;
}

char *ini_get(const char *path, const char *section, const char *key)
{
    char *body = lp_slurp(path);
    if (!body) return NULL;
    char **l = g_strsplit(body, "\n", -1);
    gboolean in = FALSE;
    char *r = NULL;
    for (int i = 0; l[i] && !r; i++) {
        char *name = NULL;
        if (ini_is_section(l[i], &name)) {
            in = g_strcmp0(name, section) == 0;
            g_free(name);
            continue;
        }
        const char *v;
        if (in && kv_line_is(l[i], key, &v))
            r = g_strchomp(g_strdup(v));
    }
    g_strfreev(l);
    g_free(body);
    return r;
}

static gboolean ini_store(const char *path, const char *body)
{
    return lp_write_file(path, body);
}

/* "key = value" or "key=value": whichever the file already uses more.
 * wayfire.ini is written with spaces; mimeapps.list and GTK's
 * settings.ini without, and xdg-mime's own reader (grep "^mime=") does
 * not find a line written the other way. */
static gboolean ini_compact(char **l)
{
    int spaced = 0, compact = 0;
    for (int i = 0; l[i]; i++) {
        const char *t = l[i];
        while (*t == ' ' || *t == '\t') t++;
        if (!*t || *t == '#' || *t == ';' || *t == '[') continue;
        const char *eq = strchr(t, '=');
        if (!eq) continue;
        if (eq > t && (eq[-1] == ' ' || eq[-1] == '\t')) spaced++;
        else compact++;
    }
    return compact > spaced;
}

/* value NULL removes the key. */
static gboolean ini_edit(const char *path, const char *section,
                         const char *key, const char *value)
{
    char *body = lp_slurp(path);
    char **l = g_strsplit(body ? body : "", "\n", -1);
    int n = g_strv_length(l);
    /* A new file follows the convention of what it is: GTK and
     * freedesktop files are compact, wayfire's is spaced. */
    const char *fmt = (body ? ini_compact(l) : !g_str_has_suffix(path, "wayfire.ini"))
                    ? "%s=%s\n" : "%s = %s\n";

    int sec_start = -1, sec_end = -1, hit = -1;
    for (int i = 0; i < n; i++) {
        char *name = NULL;
        if (ini_is_section(l[i], &name)) {
            if (sec_start >= 0 && sec_end < 0)
                sec_end = i;
            if (sec_start < 0 && g_strcmp0(name, section) == 0)
                sec_start = i;
            g_free(name);
            continue;
        }
        if (sec_start >= 0 && sec_end < 0 && hit < 0 && kv_line_is(l[i], key, NULL))
            hit = i;
    }
    if (sec_start >= 0 && sec_end < 0)
        sec_end = n;

    GString *out = g_string_new(NULL);
    if (hit >= 0) {
        for (int i = 0; i < n; i++) {
            if (i == hit) {
                if (value)
                    g_string_append_printf(out, fmt, key, value);
            } else if (!(i == n - 1 && !*l[i])) {
                g_string_append_printf(out, "%s\n", l[i]);
            }
        }
    } else if (!value) {
        g_strfreev(l);
        g_free(body);
        g_string_free(out, TRUE);
        return TRUE;
    } else if (sec_start >= 0) {
        /* After the last non-blank line of the section, so the blank line
         * that separates it from the next one stays where it was. */
        int at = sec_end;
        while (at - 1 > sec_start) {
            char *t = g_strstrip(g_strdup(l[at - 1]));
            gboolean blank = !*t;
            g_free(t);
            if (!blank) break;
            at--;
        }
        for (int i = 0; i < n; i++) {
            if (i == at)
                g_string_append_printf(out, fmt, key, value);
            if (!(i == n - 1 && !*l[i]))
                g_string_append_printf(out, "%s\n", l[i]);
        }
        if (at >= n)
            g_string_append_printf(out, fmt, key, value);
    } else {
        for (int i = 0; i < n; i++)
            if (!(i == n - 1 && !*l[i]))
                g_string_append_printf(out, "%s\n", l[i]);
        if (out->len && out->str[out->len - 1] == '\n' &&
            !(out->len > 1 && out->str[out->len - 2] == '\n'))
            g_string_append_c(out, '\n');
        g_string_append_printf(out, "[%s]\n", section);
        g_string_append_printf(out, fmt, key, value);
    }

    gboolean ok = ini_store(path, out->str);
    g_string_free(out, TRUE);
    g_strfreev(l);
    g_free(body);
    return ok;
}

gboolean ini_set(const char *path, const char *section, const char *key,
                 const char *value)
{
    return ini_edit(path, section, key, value);
}

gboolean ini_unset(const char *path, const char *section, const char *key)
{
    return ini_edit(path, section, key, NULL);
}

gboolean ini_drop_section(const char *path, const char *section)
{
    char *body = lp_slurp(path);
    if (!body) return TRUE;
    char **l = g_strsplit(body, "\n", -1);
    GString *out = g_string_new(NULL);
    gboolean skip = FALSE, changed = FALSE;
    for (int i = 0; l[i]; i++) {
        char *name = NULL;
        if (ini_is_section(l[i], &name)) {
            skip = g_strcmp0(name, section) == 0;
            changed |= skip;
            g_free(name);
        }
        if (!skip)
            g_string_append_printf(out, "%s\n", l[i]);
    }
    gboolean ok = changed ? ini_store(path, out->str) : TRUE;
    g_string_free(out, TRUE);
    g_strfreev(l);
    g_free(body);
    return ok;
}

char **ini_sections(const char *path)
{
    GPtrArray *v = g_ptr_array_new();
    char *body = lp_slurp(path);
    if (body) {
        char **l = g_strsplit(body, "\n", -1);
        for (int i = 0; l[i]; i++) {
            char *name = NULL;
            if (ini_is_section(l[i], &name))
                g_ptr_array_add(v, name);
        }
        g_strfreev(l);
        g_free(body);
    }
    g_ptr_array_add(v, NULL);
    return (char **)g_ptr_array_free(v, FALSE);
}

char **ini_keys(const char *path, const char *section)
{
    GPtrArray *v = g_ptr_array_new();
    char *body = lp_slurp(path);
    if (body) {
        char **l = g_strsplit(body, "\n", -1);
        gboolean in = FALSE;
        for (int i = 0; l[i]; i++) {
            char *name = NULL;
            if (ini_is_section(l[i], &name)) {
                in = g_strcmp0(name, section) == 0;
                g_free(name);
                continue;
            }
            if (!in) continue;
            char *t = g_strstrip(g_strdup(l[i]));
            char *eq = strchr(t, '=');
            if (*t && *t != '#' && *t != ';' && eq) {
                *eq = '\0';
                g_ptr_array_add(v, g_strdup(g_strstrip(t)));
            }
            g_free(t);
        }
        g_strfreev(l);
        g_free(body);
    }
    g_ptr_array_add(v, NULL);
    return (char **)g_ptr_array_free(v, FALSE);
}

/* ── sizes ─────────────────────────────────────────────────────────── */

char *lp_human(guint64 bytes)
{
    /* Powers of 1000, the way disks are sold and the way Ubuntu's Disks
     * and Files count them. A "512 GB" drive shown as 476 GB is the most
     * common question a storage screen gets. */
    const char *unit[] = { "B", "kB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1000.0 && u < 4) { v /= 1000.0; u++; }
    if (u == 0)
        return g_strdup_printf("%.0f %s", v, unit[u]);
    return g_strdup_printf(v < 10.0 ? "%.1f %s" : "%.0f %s", v, unit[u]);
}

/* ── the rest of the desktop ─────────────────────────────────────────── */

static void quiet_done(int st, const char *out, const char *err, gpointer p)
{
    char *what = p;
    if (st != 0) {
        char *why = lp_first_line(err, out);
        g_printerr("lp-settings: %s: %s\n", what, why);
        g_free(why);
    }
    g_free(what);
}

void lp_gsettings_set(const char *schema, const char *key, const char *value)
{
    const char *v[] = { "gsettings", "set", schema, key, value, NULL };
    lp_run_async(v, NULL, NULL, quiet_done, g_strdup_printf("gsettings %s %s", schema, key));
}

static char *gtk_ini(const char *ver)
{
    return g_build_filename(g_get_user_config_dir(), ver, "settings.ini", NULL);
}

gboolean lp_gtk_settings_set(const char *key, const char *value)
{
    gboolean ok = TRUE;
    const char *vers[] = { "gtk-3.0", "gtk-4.0" };
    for (int i = 0; i < 2; i++) {
        char *p = gtk_ini(vers[i]);
        ok &= value ? ini_set(p, "Settings", key, value) : ini_unset(p, "Settings", key);
        g_free(p);
    }
    return ok;
}

char *lp_gtk_settings_get(const char *key)
{
    char *p = gtk_ini("gtk-4.0");
    char *v = ini_get(p, "Settings", key);
    g_free(p);
    return v;
}

gboolean lp_sway(void)
{
    const char *s = g_getenv("SWAYSOCK");
    return s && *s;
}

void lp_swaymsg(const char *const *args)
{
    if (!lp_sway()) return;
    GPtrArray *v = g_ptr_array_new();
    g_ptr_array_add(v, (gpointer)"swaymsg");
    for (int i = 0; args[i]; i++)
        g_ptr_array_add(v, (gpointer)args[i]);
    g_ptr_array_add(v, NULL);
    lp_run_async((const char *const *)v->pdata, NULL, NULL, quiet_done, g_strdup("swaymsg"));
    g_ptr_array_free(v, TRUE);
}

gboolean lp_have(const char *prog)
{
    char *p = g_find_program_in_path(prog);
    gboolean ok = p != NULL;
    g_free(p);
    return ok;
}

/* ── JSON ──────────────────────────────────────────────────────────── */

typedef struct { const char *p; int depth; } jp_t;

static void jskip(jp_t *s)
{
    while (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')
        s->p++;
}

static char *jstring(jp_t *s)
{
    if (*s->p != '"') return NULL;
    s->p++;
    GString *b = g_string_new(NULL);
    while (*s->p && *s->p != '"') {
        if (*s->p == '\\') {
            s->p++;
            switch (*s->p) {
            case 'n': g_string_append_c(b, '\n'); break;
            case 't': g_string_append_c(b, '\t'); break;
            case 'r': g_string_append_c(b, '\r'); break;
            case 'b': g_string_append_c(b, '\b'); break;
            case 'f': g_string_append_c(b, '\f'); break;
            case 'u': {
                gunichar c = 0;
                for (int i = 1; i <= 4; i++) {
                    int d = g_ascii_xdigit_value(s->p[i]);
                    if (d < 0) { g_string_free(b, TRUE); return NULL; }
                    c = c * 16 + (gunichar)d;
                }
                s->p += 4;
                /* A surrogate pair is two escapes for one character: an
                 * SSID with an emoji in it arrives this way. */
                if (c >= 0xD800 && c <= 0xDBFF && s->p[1] == '\\' && s->p[2] == 'u') {
                    gunichar lo = 0;
                    gboolean ok = TRUE;
                    for (int i = 3; i <= 6; i++) {
                        int d = g_ascii_xdigit_value(s->p[i]);
                        if (d < 0) { ok = FALSE; break; }
                        lo = lo * 16 + (gunichar)d;
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                        s->p += 6;
                    }
                }
                g_string_append_unichar(b, c);
                break;
            }
            case '\0': g_string_free(b, TRUE); return NULL;
            default: g_string_append_c(b, *s->p); break;
            }
            s->p++;
        } else {
            g_string_append_c(b, *s->p++);
        }
    }
    if (*s->p != '"') { g_string_free(b, TRUE); return NULL; }
    s->p++;
    return g_string_free(b, FALSE);
}

static jnode_t *jvalue(jp_t *s);

static jnode_t *jcontainer(jp_t *s, gboolean obj)
{
    jnode_t *n = g_new0(jnode_t, 1);
    n->type = obj ? J_OBJ : J_ARR;
    char close = obj ? '}' : ']';
    s->p++;
    jskip(s);
    if (*s->p == close) { s->p++; return n; }
    jnode_t **tail = &n->child;
    for (;;) {
        char *key = NULL;
        jskip(s);
        if (obj) {
            key = jstring(s);
            jskip(s);
            if (!key || *s->p != ':') { g_free(key); json_free(n); return NULL; }
            s->p++;
        }
        jnode_t *v = jvalue(s);
        if (!v) { g_free(key); json_free(n); return NULL; }
        v->key = key;
        *tail = v;
        tail = &v->next;
        jskip(s);
        if (*s->p == ',') { s->p++; continue; }
        if (*s->p == close) { s->p++; return n; }
        json_free(n);
        return NULL;
    }
}

static jnode_t *jvalue(jp_t *s)
{
    if (++s->depth > 64) return NULL;
    jskip(s);
    jnode_t *n = NULL;
    if (*s->p == '{' || *s->p == '[') {
        n = jcontainer(s, *s->p == '{');
    } else if (*s->p == '"') {
        char *str = jstring(s);
        if (str) { n = g_new0(jnode_t, 1); n->type = J_STR; n->str = str; }
    } else if (!strncmp(s->p, "true", 4) || !strncmp(s->p, "false", 5)) {
        n = g_new0(jnode_t, 1);
        n->type = J_BOOL;
        n->b = s->p[0] == 't';
        s->p += n->b ? 4 : 5;
    } else if (!strncmp(s->p, "null", 4)) {
        n = g_new0(jnode_t, 1);
        s->p += 4;
    } else {
        char *end = NULL;
        double d = g_ascii_strtod(s->p, &end);
        if (end && end != s->p) {
            n = g_new0(jnode_t, 1);
            n->type = J_NUM;
            n->num = d;
            s->p = end;
        }
    }
    s->depth--;
    return n;
}

jnode_t *json_parse(const char *text)
{
    if (!text) return NULL;
    jp_t s = { text, 0 };
    jnode_t *n = jvalue(&s);
    return n;
}

void json_free(jnode_t *n)
{
    while (n) {
        jnode_t *next = n->next;
        json_free(n->child);
        g_free(n->str);
        g_free(n->key);
        g_free(n);
        n = next;
    }
}

jnode_t *json_get(const jnode_t *obj, const char *key)
{
    if (!obj || obj->type != J_OBJ) return NULL;
    for (jnode_t *c = obj->child; c; c = c->next)
        if (g_strcmp0(c->key, key) == 0)
            return c;
    return NULL;
}

static const jnode_t *json_path(const jnode_t *obj, const char *path)
{
    char **parts = g_strsplit(path, ".", -1);
    const jnode_t *n = obj;
    for (int i = 0; parts[i] && n; i++)
        n = json_get(n, parts[i]);
    g_strfreev(parts);
    return n;
}

const char *json_str(const jnode_t *obj, const char *path, const char *dflt)
{
    const jnode_t *n = json_path(obj, path);
    return (n && n->type == J_STR) ? n->str : dflt;
}

double json_num(const jnode_t *obj, const char *path, double dflt)
{
    const jnode_t *n = json_path(obj, path);
    return (n && n->type == J_NUM) ? n->num : dflt;
}

gboolean json_bool(const jnode_t *obj, const char *path, gboolean dflt)
{
    const jnode_t *n = json_path(obj, path);
    return (n && n->type == J_BOOL) ? n->b : dflt;
}
