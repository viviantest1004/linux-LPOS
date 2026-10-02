/*
 * users.c - Users: your own password, the other accounts on this
 * machine, who is an administrator, and signing in automatically.
 *
 * ── Two kinds of account ──
 *
 * account-permissions.md: an administrator is an account in group sudo;
 * everyone else is standard. That is the whole model - the same rule
 * sudo, lp-privd and the recovery shell apply - so "make administrator"
 * is `usermod -aG sudo NAME` and "make standard" is `gpasswd -d NAME
 * sudo`, and nothing else is kept anywhere.
 *
 * ── Every change to an account asks for a password ──
 *
 * Changing your own password asks for the current one - that is passwd's
 * own rule, and it is what stops someone at an unlocked screen from
 * locking the owner out. Anything that touches another account (adding,
 * removing, a new password, administrator or not) and the automatic
 * sign-in go through the administrator password dialog (sys.c), every
 * time, with the five-minute allowance sudo itself keeps. Passwords go
 * to the tools on standard input (passwd, chpasswd), never on a command
 * line, where every process on the machine could read them.
 *
 * Two things are refused before anything runs: removing your own
 * account, and leaving the machine without an administrator (removing,
 * or making standard, the last one). A machine with no administrator
 * can only be repaired from the recovery system.
 *
 * ── Recovery ──
 *
 * The recovery shell accepts the password of an administrator of the
 * installed system, or - when that system cannot be read - the recovery
 * password the installer stored on the recovery partition. When the
 * first administrator changes their password, the old one would still
 * open Recovery; so they are offered "Also use it for Recovery", which
 * asks lp-privd (as administrator) to copy the account's new password
 * hash to LP-RECOVERY:/etc/lp/recovery-password. The offer appears only
 * when this machine's lp-privd has that verb (`lp-privd help` lists
 * recovery-password), so it is never a button that does nothing.
 *
 * ── Automatic sign-in ──
 *
 * /etc/lp/autologin holds the name of the account the machine starts
 * the desktop for without asking; no file means it asks. It is the
 * machine's, not the person's, so it is written as administrator.
 */
#include "core.h"

#include <grp.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>

#define AUTOLOGIN "/etc/lp/autologin"

typedef struct {
    char    *name, *full, *home;
    uid_t    uid;
    gboolean admin;
} acct_t;

static void acct_free(gpointer p)
{
    acct_t *a = p;
    g_free(a->name); g_free(a->full); g_free(a->home);
    g_free(a);
}

static gboolean in_sudo(const char *name, gid_t primary)
{
    struct group *g = getgrnam("sudo");
    if (!g) return FALSE;
    if (g->gr_gid == primary) return TRUE;
    for (char **m = g->gr_mem; m && *m; m++)
        if (!strcmp(*m, name)) return TRUE;
    return FALSE;
}

/* People's accounts: uid 1000 and up, not nobody. Read fresh every time;
 * another administrator or a terminal may have changed them. */
static GPtrArray *accounts(void)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(acct_free);
    setpwent();
    struct passwd *pw;
    while ((pw = getpwent())) {
        if (pw->pw_uid < 1000 || pw->pw_uid >= 60000) continue;
        acct_t *x = g_new0(acct_t, 1);
        x->name = g_strdup(pw->pw_name);
        char *comma = pw->pw_gecos ? strchr(pw->pw_gecos, ',') : NULL;
        x->full = pw->pw_gecos && *pw->pw_gecos
                ? (comma ? g_strndup(pw->pw_gecos, comma - pw->pw_gecos) : g_strdup(pw->pw_gecos))
                : NULL;
        x->home = g_strdup(pw->pw_dir);
        x->uid = pw->pw_uid;
        x->admin = in_sudo(pw->pw_name, pw->pw_gid);
        g_ptr_array_add(a, x);
    }
    endpwent();
    return a;
}

static int admin_count(GPtrArray *a)
{
    int n = 0;
    for (guint i = 0; i < a->len; i++)
        n += ((acct_t *)g_ptr_array_index(a, i))->admin;
    return n;
}

/* The administrator the installer made: the lowest uid in group sudo. */
static gboolean is_first_admin(const char *name)
{
    GPtrArray *a = accounts();
    acct_t *first = NULL;
    for (guint i = 0; i < a->len; i++) {
        acct_t *x = g_ptr_array_index(a, i);
        if (x->admin && (!first || x->uid < first->uid)) first = x;
    }
    gboolean r = first && !strcmp(first->name, name);
    g_ptr_array_free(a, TRUE);
    return r;
}

static const char *me(void)
{
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_name : g_get_user_name();
}

static gboolean valid_name(const char *n)
{
    /* Debian's NAME_REGEX without the dollar: lower case, a letter or _
     * first, at most 32. */
    if (!n || !*n || strlen(n) > 32) return FALSE;
    if (!(g_ascii_islower(n[0]) || n[0] == '_')) return FALSE;
    for (const char *p = n; *p; p++)
        if (!(g_ascii_islower(*p) || g_ascii_isdigit(*p) || *p == '_' || *p == '-')) return FALSE;
    return TRUE;
}

static const char *password_problem(const char *a, const char *b)
{
    if (!a || !*a) return T("Type a password.", "비밀번호를 입력하십시오.");
    if (strcmp(a, b)) return T("The two passwords are not the same.", "두 비밀번호가 다릅니다.");
    if (strchr(a, '\n') || strchr(a, '\r')) return T("A password cannot contain a line break.",
                                                     "비밀번호에 줄바꿈은 넣을 수 없습니다.");
    if (g_utf8_strlen(a, -1) < 4) return T("Use at least 4 characters.", "4자 이상으로 하십시오.");
    return NULL;
}

/* ── your own password ──────────────────────────────────────────────── */

static gboolean privd_can(const char *verb)
{
    static const char *const v[] = { "lp-privd", "help", NULL };
    char *out = NULL;
    lp_run_full(v, NULL, &out, NULL);
    gboolean yes = FALSE;
    if (out) {
        char **l = g_strsplit(out, "\n", -1);
        for (int i = 0; l[i] && !yes; i++) {
            char *t = g_strstrip(l[i]);
            yes = g_str_has_prefix(t, verb) && (t[strlen(verb)] == ' ' || !t[strlen(verb)]);
        }
        g_strfreev(l);
    }
    g_free(out);
    return yes;
}

static void recovery_done(int st, const char *out, const char *err, gpointer p)
{
    (void)p;
    if (st == 0)
        lp_toast(FALSE, T("Recovery now opens with your new password", "이제 복구 모드도 새 비밀번호로 열립니다"));
    else if (st != -2) {
        char *why = lp_first_line(err, out);
        lp_toast(TRUE, T("The recovery password did not change: %s", "복구 비밀번호를 바꾸지 못했습니다: %s"), why);
        g_free(why);
    }
}

static void recovery_yes(lp_dialog_t *d, gpointer p)
{
    (void)p;
    const char *v[] = { "lp-privd", "recovery-password", me(), NULL };
    lp_admin_run(v, NULL, T("Changing the password that opens Recovery needs an administrator - "
                            "type your new password.",
                            "복구 모드를 여는 비밀번호를 바꾸려면 관리자 권한이 필요합니다 - "
                            "새 비밀번호를 입력하십시오."),
                 NULL, recovery_done, NULL);
    lp_dialog_close(d);
}

static void offer_recovery(void)
{
    lp_dialog_t *d = lp_dialog_new(T("Also use it for Recovery?", "복구 모드에도 쓸까요?"),
                                   T("Use for Recovery", "복구 모드에도 쓰기"), FALSE, recovery_yes, NULL);
    lp_dialog_text(d, T("If this system ever cannot start, Recovery asks for a password. Until now "
                        "that is the one set when LP was installed.",
                        "이 시스템이 시작하지 못하면 복구 모드가 비밀번호를 묻습니다. 지금은 LP 를 "
                        "설치할 때 정한 비밀번호입니다."), NULL);
    lp_dialog_present(d);
}

static void own_pw_done(int st, const char *out, const char *err, gpointer p)
{
    lp_dialog_t *d = p;
    lp_dialog_busy(d, FALSE);
    if (st == 0) {
        lp_dialog_close(d);
        lp_toast(FALSE, T("Your password is changed", "비밀번호를 바꿨습니다"));
        if (is_first_admin(me()) && privd_can("recovery-password"))
            offer_recovery();
        return;
    }
    char *why = lp_first_line(err, out);
    /* passwd's refusals, in words a person expects. Looked for in all it
     * said: its last line is only "password unchanged", after the
     * reason. */
    char *all = g_strconcat(err ? err : "", "\n", out ? out : "", NULL);
    gboolean wrong = strstr(all, "Authentication token manipulation") ||
                     strstr(all, "Authentication failure") || strstr(all, "incorrect");
    g_free(all);
    if (wrong)
        lp_dialog_error(d, T("The current password is not right.", "현재 비밀번호가 맞지 않습니다."));
    else
        lp_dialog_error(d, why);
    g_free(why);
}

static void own_pw_ok(lp_dialog_t *d, gpointer p)
{
    (void)p;
    GtkWidget *cur = lp_dialog_get_data(d, "lp-cur"), *n1 = lp_dialog_get_data(d, "lp-n1"),
              *n2 = lp_dialog_get_data(d, "lp-n2");
    const char *c = gtk_editable_get_text(GTK_EDITABLE(cur));
    const char *a = gtk_editable_get_text(GTK_EDITABLE(n1)), *b = gtk_editable_get_text(GTK_EDITABLE(n2));
    const char *bad = !*c ? T("Type your current password.", "현재 비밀번호를 입력하십시오.")
                          : password_problem(a, b);
    if (bad) { lp_dialog_error(d, bad); return; }
    if (strchr(c, '\n')) { lp_dialog_error(d, T("The current password is not right.", "현재 비밀번호가 맞지 않습니다.")); return; }
    /* passwd asks: current, new, new again - each a line on stdin. The
     * base's passwd, not /bin's: that one keeps the hash in /data/shadow
     * and refused ordinary accounts outright ("only root can change a
     * password here"), while sudo and the lock screen read /etc/shadow. */
    char *in = g_strdup_printf("%s\n%s\n%s\n", c, a, b);
    lp_dialog_busy(d, TRUE);
    const char *v[] = { lp_base_tool("passwd"), NULL };
    lp_run_async(v, in, lp_dialog_window(d), own_pw_done, d);
    memset(in, 0, strlen(in));
    g_free(in);
}

static void on_own_password(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    lp_dialog_t *d = lp_dialog_new(T("Change your password", "비밀번호 바꾸기"), T("Change", "바꾸기"),
                                   FALSE, own_pw_ok, NULL);
    lp_dialog_set_data(d, "lp-cur", lp_dialog_entry(d, T("Current password", "현재 비밀번호"), NULL, TRUE), NULL);
    lp_dialog_set_data(d, "lp-n1", lp_dialog_entry(d, T("New password", "새 비밀번호"), NULL, TRUE), NULL);
    lp_dialog_set_data(d, "lp-n2", lp_dialog_entry(d, T("New password again", "새 비밀번호 다시"), NULL, TRUE), NULL);
    lp_dialog_present(d);
}

/* ── other accounts ─────────────────────────────────────────────────── */

typedef struct {
    char *name, *ok;
    int   step;
    char *pw;                 /* for the chpasswd step of "add" */
    gboolean admin;
    /* The sheet the job came from, by its window and weakly: the person
     * can close it while useradd or usermod runs, and a plain pointer to
     * it was then used after it was freed - Settings closed when the
     * command answered. job_dlg() is the sheet, or NULL once it is gone
     * or going. */
    GWeakRef dlgwin;
    /* The account's own dialog, under the "Remove?" one: it closes too
     * when the account is gone, or it stays up offering to change an
     * account that no longer exists. Weak - it may be closed first. */
    GWeakRef parent;
} job_t;

static lp_dialog_t *job_dlg(job_t *j)
{
    GObject *w = g_weak_ref_get(&j->dlgwin);
    if (!w)
        return NULL;
    lp_dialog_t *d = g_object_get_data(w, "lp-closing") ? NULL
                   : g_object_get_data(w, "lp-dialog");
    g_object_unref(w);      /* the window lives on: it is on the screen */
    return d;
}

static void job_set_dlg(job_t *j, lp_dialog_t *d)
{
    g_weak_ref_init(&j->dlgwin, d ? G_OBJECT(lp_dialog_window(d)) : NULL);
}

static void job_free(job_t *j)
{
    if (j->pw) { memset(j->pw, 0, strlen(j->pw)); g_free(j->pw); }
    g_free(j->name); g_free(j->ok);
    g_weak_ref_clear(&j->parent);
    g_weak_ref_clear(&j->dlgwin);
    g_free(j);
}

static void acct_done(int st, const char *out, const char *err, gpointer p)
{
    job_t *j = p;
    if (st == 0) {
        lp_toast(FALSE, "%s", j->ok);
        lp_dialog_t *d = job_dlg(j);
        if (d) lp_dialog_close(d);
        GObject *pw = g_weak_ref_get(&j->parent);
        if (pw) {
            lp_dialog_t *pd = g_object_get_data(pw, "lp-dialog");
            if (pd && !g_object_get_data(pw, "lp-closing")) lp_dialog_close(pd);
            g_object_unref(pw);
        }
        lp_refresh();
    } else if (st == -2) {
        lp_dialog_t *d = job_dlg(j);
        if (d) lp_dialog_busy(d, FALSE);
    } else {
        char *why = lp_first_line(err, out);
        lp_dialog_t *d = job_dlg(j);
        if (d) { lp_dialog_busy(d, FALSE); lp_dialog_error(d, why); }
        else lp_toast(TRUE, "%s", why);
        g_free(why);
    }
    job_free(j);
}

static const char *WHY_ADMIN;

static void run_acct(const char *const *argv, const char *in, job_t *j)
{
    lp_dialog_t *d = job_dlg(j);
    if (d) lp_dialog_busy(d, TRUE);
    lp_admin_run(argv, in, WHY_ADMIN, NULL, acct_done, j);
}

/* Add: useradd, then chpasswd with the password on stdin. */
static void add_step2(int st, const char *out, const char *err, gpointer p)
{
    job_t *j = p;
    if (st != 0) { acct_done(st, out, err, j); return; }
    char *line = g_strdup_printf("%s:%s\n", j->name, j->pw);
    static const char *const v[] = { "chpasswd", NULL };
    lp_admin_run(v, line, WHY_ADMIN, NULL, acct_done, j);
    memset(line, 0, strlen(line));
    g_free(line);
}

static void add_ok(lp_dialog_t *d, gpointer p)
{
    (void)p;
    const char *full = gtk_editable_get_text(GTK_EDITABLE(lp_dialog_get_data(d, "lp-full")));
    const char *name = gtk_editable_get_text(GTK_EDITABLE(lp_dialog_get_data(d, "lp-name")));
    const char *a = gtk_editable_get_text(GTK_EDITABLE(lp_dialog_get_data(d, "lp-p1")));
    const char *b = gtk_editable_get_text(GTK_EDITABLE(lp_dialog_get_data(d, "lp-p2")));
    gboolean admin = gtk_check_button_get_active(GTK_CHECK_BUTTON(lp_dialog_get_data(d, "lp-admin")));
    if (!valid_name(name)) {
        lp_dialog_error(d, T("A user name is lower-case letters, digits, - and _, starting with a letter.",
                             "사용자 이름은 영어 소문자, 숫자, -, _ 로 쓰고 글자로 시작합니다."));
        return;
    }
    if (getpwnam(name)) { lp_dialog_error(d, T("That user name is taken.", "이미 있는 사용자 이름입니다.")); return; }
    if (strchr(full, ':') || strchr(full, '\n') || strchr(full, ',')) {
        lp_dialog_error(d, T("The full name cannot contain : , or a line break.", "전체 이름에 : , 줄바꿈은 넣을 수 없습니다."));
        return;
    }
    const char *bad = password_problem(a, b);
    if (bad) { lp_dialog_error(d, bad); return; }
    job_t *j = g_new0(job_t, 1);
    j->name = g_strdup(name);
    j->pw = g_strdup(a);
    job_set_dlg(j, d);
    j->ok = g_strdup_printf(T("Added %s", "%s 을(를) 만들었습니다"), name);
    /* The name is the last word: without it useradd answers with its
     * usage text, and the dialog showed that text's last line. */
    const char *v[] = { "useradd", "--create-home", "--shell", "/bin/bash", "--comment", full,
                        name, NULL, NULL, NULL };
    if (admin) { v[6] = "--groups"; v[7] = "sudo"; v[8] = name; }
    lp_dialog_busy(d, TRUE);
    lp_admin_run(v, NULL, WHY_ADMIN, NULL, add_step2, j);
}

static void on_add(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    lp_dialog_t *d = lp_dialog_new(T("Add an account", "계정 만들기"), T("Add", "만들기"), FALSE, add_ok, NULL);
    lp_dialog_set_data(d, "lp-full", lp_dialog_entry(d, T("Full name", "전체 이름"), NULL, FALSE), NULL);
    lp_dialog_set_data(d, "lp-name", lp_dialog_entry(d, T("User name", "사용자 이름"), NULL, FALSE), NULL);
    lp_dialog_set_data(d, "lp-p1", lp_dialog_entry(d, T("Password", "비밀번호"), NULL, TRUE), NULL);
    lp_dialog_set_data(d, "lp-p2", lp_dialog_entry(d, T("Password again", "비밀번호 다시"), NULL, TRUE), NULL);
    GtkWidget *cb = gtk_check_button_new_with_label(T("Administrator - may change the whole system",
                                                      "관리자 - 시스템 전체를 바꿀 수 있습니다"));
    g_object_set_data_full(G_OBJECT(cb), "lp-title", g_strdup(T("Administrator", "관리자")), g_free);
    gtk_box_append(GTK_BOX(lp_dialog_body(d)), cb);
    lp_dialog_set_data(d, "lp-admin", cb, NULL);
    lp_dialog_present(d);
}

static void set_pw_ok(lp_dialog_t *d, gpointer p)
{
    (void)p;
    const char *name = lp_dialog_get_data(d, "lp-user");
    const char *a = gtk_editable_get_text(GTK_EDITABLE(lp_dialog_get_data(d, "lp-p1")));
    const char *b = gtk_editable_get_text(GTK_EDITABLE(lp_dialog_get_data(d, "lp-p2")));
    const char *bad = password_problem(a, b);
    if (bad) { lp_dialog_error(d, bad); return; }
    job_t *j = g_new0(job_t, 1);
    j->name = g_strdup(name);
    job_set_dlg(j, d);
    j->ok = g_strdup_printf(T("New password for %s", "%s 의 비밀번호를 바꿨습니다"), name);
    char *line = g_strdup_printf("%s:%s\n", name, a);
    static const char *const v[] = { "chpasswd", NULL };
    run_acct(v, line, j);
    memset(line, 0, strlen(line));
    g_free(line);
}

static void weak_free(gpointer p)
{
    g_weak_ref_clear(p);
    g_free(p);
}

static void remove_ok(lp_dialog_t *d, gpointer p)
{
    (void)p;
    const char *name = lp_dialog_get_data(d, "lp-user");
    gboolean files = gtk_check_button_get_active(GTK_CHECK_BUTTON(lp_dialog_get_data(d, "lp-files")));
    job_t *j = g_new0(job_t, 1);
    j->name = g_strdup(name);
    job_set_dlg(j, d);
    j->ok = g_strdup_printf(files ? T("Removed %s and their files", "%s 과(와) 그 파일을 지웠습니다")
                                  : T("Removed %s; their files are kept", "%s 을(를) 지웠습니다. 파일은 남겨 두었습니다"), name);
    GObject *parent = g_weak_ref_get(lp_dialog_get_data(d, "lp-parent"));
    g_weak_ref_set(&j->parent, parent);
    if (parent) g_object_unref(parent);
    const char *v[] = { "userdel", files ? "--remove" : name, files ? name : NULL, NULL };
    run_acct(v, NULL, j);
}

typedef struct { char *name; gboolean admin; } manage_t;

static void manage_free(gpointer p)
{
    manage_t *m = p;
    g_free(m->name);
    g_free(m);
}

static void on_manage_type(GtkWidget *seg, int i, gpointer p)
{
    (void)seg;
    lp_dialog_t *d = p;
    manage_t *m = lp_dialog_get_data(d, "lp-manage");
    gboolean want_admin = i == 1;
    if (want_admin == m->admin) return;
    if (!want_admin) {
        GPtrArray *a = accounts();
        int n = admin_count(a);
        g_ptr_array_free(a, TRUE);
        if (n <= 1) {
            lp_dialog_error(d, T("This is the only administrator. Make someone else an administrator first.",
                                 "유일한 관리자입니다. 먼저 다른 계정을 관리자로 만드십시오."));
            /* The control goes back to what is true. */
            GtkWidget *b = gtk_widget_get_last_child(seg);
            LP_QUIET(gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), TRUE));
            return;
        }
    }
    job_t *j = g_new0(job_t, 1);
    j->name = g_strdup(m->name);
    j->ok = g_strdup_printf(want_admin ? T("%s is now an administrator", "%s 이(가) 이제 관리자입니다")
                                       : T("%s is now a standard account", "%s 이(가) 이제 표준 계정입니다"), m->name);
    job_set_dlg(j, d);
    if (want_admin) {
        const char *v[] = { "usermod", "--append", "--groups", "sudo", m->name, NULL };
        run_acct(v, NULL, j);
    } else {
        const char *v[] = { "gpasswd", "--delete", m->name, "sudo", NULL };
        run_acct(v, NULL, j);
    }
}

static void on_manage_pw(GtkButton *b, gpointer p)
{
    (void)b;
    manage_t *m = p;
    lp_dialog_t *d = lp_dialog_new(T("New password", "새 비밀번호"), T("Set", "정하기"), FALSE, set_pw_ok, NULL);
    lp_dialog_set_data(d, "lp-user", g_strdup(m->name), g_free);
    char *t = g_strdup_printf(T("The new password for %s. Tell them; it replaces theirs at once.",
                                "%s 의 새 비밀번호입니다. 바로 바뀌니 알려 주십시오."), m->name);
    lp_dialog_text(d, t, NULL);
    g_free(t);
    lp_dialog_set_data(d, "lp-p1", lp_dialog_entry(d, T("Password", "비밀번호"), NULL, TRUE), NULL);
    lp_dialog_set_data(d, "lp-p2", lp_dialog_entry(d, T("Password again", "비밀번호 다시"), NULL, TRUE), NULL);
    lp_dialog_present(d);
}

static void on_manage_remove(GtkButton *b, gpointer p)
{
    manage_t *m = p;
    GPtrArray *a = accounts();
    int n = admin_count(a);
    g_ptr_array_free(a, TRUE);
    if (m->admin && n <= 1) {
        lp_toast(TRUE, T("This is the only administrator and cannot be removed.",
                         "유일한 관리자라서 지울 수 없습니다."));
        return;
    }
    char *title = g_strdup_printf(T("Remove %s?", "%s 을(를) 지울까요?"), m->name);
    lp_dialog_t *d = lp_dialog_new(title, T("Remove", "지우기"), TRUE, remove_ok, NULL);
    lp_dialog_set_data(d, "lp-user", g_strdup(m->name), g_free);
    GWeakRef *parent = g_new0(GWeakRef, 1);
    g_weak_ref_init(parent, gtk_widget_get_root(GTK_WIDGET(b)));
    lp_dialog_set_data(d, "lp-parent", parent, weak_free);
    g_free(title);
    lp_dialog_text(d, T("They will not be able to sign in any more.", "이 계정으로 더는 로그인할 수 없습니다."), NULL);
    GtkWidget *cb = gtk_check_button_new_with_label(T("Also delete their home folder and files",
                                                      "홈 폴더와 파일도 지우기"));
    g_object_set_data_full(G_OBJECT(cb), "lp-title", g_strdup(T("Delete files", "파일 지우기")), g_free);
    gtk_box_append(GTK_BOX(lp_dialog_body(d)), cb);
    lp_dialog_set_data(d, "lp-files", cb, NULL);
    lp_dialog_present(d);
}

static void on_account_tapped(GtkWidget *row, gpointer p)
{
    (void)p;
    const char *name = g_object_get_data(G_OBJECT(row), "lp-user");
    gboolean admin = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "lp-admin"));
    manage_t *m = g_new0(manage_t, 1);
    m->name = g_strdup(name);
    m->admin = admin;
    lp_dialog_t *d = lp_dialog_new(name, NULL, FALSE, NULL, NULL);
    lp_dialog_set_data(d, "lp-manage", m, manage_free);
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "lp-group");
    const char *types[] = { T("Standard", "표준"), T("Administrator", "관리자"), NULL };
    row_segmented(list, T("Account type", "계정 유형"), NULL, types, admin ? 1 : 0,
                  G_CALLBACK(on_manage_type), d);
    row_button(list, T("Password", "비밀번호"), NULL, T("Set…", "정하기…"), G_CALLBACK(on_manage_pw), m);
    row_button(list, T("Remove this account", "이 계정 지우기"), NULL, T("Remove…", "지우기…"),
               G_CALLBACK(on_manage_remove), m);
    gtk_box_append(GTK_BOX(lp_dialog_body(d)), list);
    lp_dialog_present(d);
}

/* ── automatic sign-in ──────────────────────────────────────────────── */

static void autologin_done(int st, const char *out, const char *err, gpointer p)
{
    GtkWidget *sw = p;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    if (st == 0) {
        lp_toast(FALSE, on ? T("This machine will start to your desktop without asking for your password",
                               "이 기계는 비밀번호를 묻지 않고 바탕화면으로 시작합니다")
                           : T("This machine will ask for a password when it starts",
                               "이 기계는 시작할 때 비밀번호를 묻습니다"));
        return;
    }
    if (st != -2) {
        char *why = lp_first_line(err, out);
        lp_toast(TRUE, "%s", why);
        g_free(why);
    }
    LP_QUIET(gtk_switch_set_active(GTK_SWITCH(sw), !on));
}

static void rm_step2(int st, const char *out, const char *err, gpointer p)
{
    if (st != 0) { autologin_done(st, out, err, p); return; }
    static const char *const v[] = { "sync", "/etc/lp", NULL };
    lp_admin_run(v, NULL, WHY_ADMIN, NULL, autologin_done, p);
}

static void on_autologin(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    if (on) {
        char *body = g_strdup_printf("%s\n", me());
        lp_admin_write_file(AUTOLOGIN, body, T("Signing in automatically is a setting for the whole "
                                               "machine and needs an administrator.",
                                               "자동 로그인은 기계 전체 설정이라 관리자 권한이 필요합니다."),
                            GTK_WIDGET(sw), autologin_done, sw);
        g_free(body);
    } else {
        /* rm, then sync of the directory: a removal is only durable once
         * the directory is. */
        static const char *const v[] = { "rm", "-f", AUTOLOGIN, NULL };
        lp_admin_run(v, NULL, T("Signing in automatically is a setting for the whole machine and "
                                "needs an administrator.",
                                "자동 로그인은 기계 전체 설정이라 관리자 권한이 필요합니다."),
                     GTK_WIDGET(sw), rm_step2, sw);
    }
}

/* ── building ───────────────────────────────────────────────────────── */

static GtkWidget *build(void)
{
    WHY_ADMIN = T("Changing accounts needs an administrator.", "계정을 바꾸려면 관리자 권한이 필요합니다.");
    GPtrArray *a = accounts();
    const char *my = me();
    acct_t *self = NULL;
    for (guint i = 0; i < a->len; i++)
        if (!strcmp(((acct_t *)g_ptr_array_index(a, i))->name, my)) self = g_ptr_array_index(a, i);
    gboolean admin = lp_is_admin();

    char *sub = g_strdup_printf(T("%s · %s", "%s · %s"), self && self->full ? self->full : my,
                                admin ? T("Administrator", "관리자") : T("Standard account", "표준 계정"));
    GtkWidget *page = page_new(T("Users", "사용자"), sub);
    g_free(sub);

    GtkWidget *g = group_new(page, T("Your account", "내 계정"));
    row_value(g, T("User name", "사용자 이름"), self && self->home ? self->home : NULL, my);
    row_button(g, T("Password", "비밀번호"), T("Asks for the current one first", "현재 비밀번호를 먼저 묻습니다"),
               T("Change…", "바꾸기…"), G_CALLBACK(on_own_password), NULL);
    char *al = lp_slurp(AUTOLOGIN);
    GtkWidget *alr = row_switch(g, T("Sign in automatically", "자동 로그인"),
                                T("Start to this desktop without the password. The lock screen still asks for it.",
                                  "비밀번호 없이 이 바탕화면으로 시작합니다. 잠금 화면은 여전히 비밀번호를 묻습니다."),
                                al && !strcmp(g_strstrip(al), my), G_CALLBACK(on_autologin), NULL);
    g_free(al);
    if (!admin) row_lock(alr);

    GtkWidget *og = group_new(page, T("Other accounts", "다른 계정"));
    int others = 0;
    for (guint i = 0; i < a->len; i++) {
        acct_t *x = g_ptr_array_index(a, i);
        if (!strcmp(x->name, my)) continue;
        others++;
        GtkWidget *r = row_chevron(og, x->full ? x->full : x->name,
                                   x->full ? x->name : NULL,
                                   x->admin ? T("Administrator", "관리자") : T("Standard", "표준"),
                                   admin ? G_CALLBACK(on_account_tapped) : NULL, NULL);
        g_object_set_data_full(G_OBJECT(r), "lp-user", g_strdup(x->name), g_free);
        g_object_set_data(G_OBJECT(r), "lp-admin", GINT_TO_POINTER(x->admin));
        /* The driver and search find it by user name too. */
        g_object_set_data_full(G_OBJECT(r), "lp-title", g_strdup(x->name), g_free);
    }
    if (!others)
        row_value(og, T("Only you have an account here", "이 기계의 계정은 나뿐입니다"), NULL, NULL);
    GtkWidget *add = row_button(og, T("Add an account", "계정 만들기"), NULL, T("Add…", "만들기…"),
                                G_CALLBACK(on_add), NULL);
    if (!admin) row_lock(add);
    g_ptr_array_free(a, TRUE);
    return page;
}

static const char *const KEYS[] = {
    "Password", "비밀번호",
    "Sign in automatically", "자동 로그인",
    "Add an account", "계정 만들기",
    "Administrator", "관리자",
    "Account type", "계정 유형",
    "Change password", "비밀번호 바꾸기",
    NULL
};

const lp_panel_t lp_panel_users = {
    "users", "Users", "사용자", "system-users-symbolic", build, KEYS, NULL
};
