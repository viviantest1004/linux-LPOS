/*
 * keyboard.c - Keyboard: input sources, the key that switches between
 * them, key repeat, and every shortcut.
 *
 * ── Input sources are a list, and English (US) stays in it ──
 *
 * input-sources.md: the sources are an ordered list, not a Korean/English
 * toggle, so a third language is one more row rather than a mode inside
 * a switch; the switch key moves to the next one. English (US) cannot be
 * removed - it is what is printed on the keys, what the login and lock
 * screens and the recovery console type with - but the remove button is
 * not greyed out: it is pressed, it says why, and it offers what the
 * person usually wanted, English at the bottom of the list.
 *
 * Kept in ~/.config/lp/input.conf:
 *
 *     sources=en,ko            in order; the first is what a new window starts in
 *     switch_keys=hangul;ralt;shift-space
 *
 * and mirrored into the file the on-screen keyboard and the input method
 * read, ~/.config/lp/osk.ini [keyboard]: layouts=en;ko (the set 한/영
 * cycles through), physical-korean (compose Hangul from the laptop's own
 * keys too) and switch-keys. Only those keys are written; lp-osk's own
 * (height, the language last in use) stay as they were, and `lp-osk
 * reload` puts the change in use at once.
 *
 * ── The keys that switch 한/영 ──
 *
 * The Hangul key always. Right Alt tapped on its own and Shift+Space are
 * on from the start, Ctrl+Space is there to turn on (it is also every
 * code editor's completion key), and Super+Space is a shortcut like the
 * others below, so it can be moved. The same choice has to reach two
 * input methods: lp-osk (sway; it reads osk.ini) and fcitx5 (wayfire's
 * laptop keys; [Hotkey/TriggerKeys] in ~/.config/fcitx5/config, and
 * Right Alt as a Hangul key is wayfire's xkb option korean:ralt_hangul).
 *
 * ── Repeat and shortcuts: one list, both compositors ──
 *
 * [input] kb_repeat_delay / kb_repeat_rate, and the bindings in the
 * sections of the plugins that own them ([command], [expo], [grid] ...),
 * in wayfire.ini, which wayfire applies at once. That file is also the
 * record for sway: every change writes ~/.config/sway/lp-keys.conf from
 * it (sway's config includes it) and reloads sway, so a shortcut changed
 * here works under whichever compositor the machine runs. The ones sway
 * has no equivalent for (the overviews) are left out of that file.
 *
 * A shortcut is changed by pressing the new keys in a dialog, which
 * catches a combination that is already taken and names what has it,
 * and offers to take it over. Shortcuts people add themselves are
 * [command] binding_lp_N / command_lp_N pairs, with their names in
 * ~/.config/lp/shortcuts.conf.
 */
#include "core.h"

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── input sources ──────────────────────────────────────────────────── */

typedef struct { const char *id, *en, *ko, *tag; } source_t;

static const source_t SOURCES[] = {
    { "en", "English (US)", "영어 (US)", "EN" },
    { "ko", "Korean (Dubeolsik)", "한국어 (두벌식)", "한" },
};

static const source_t *source_by_id(const char *id)
{
    for (guint i = 0; i < G_N_ELEMENTS(SOURCES); i++)
        if (!strcmp(SOURCES[i].id, id)) return &SOURCES[i];
    return NULL;
}

static char *input_conf(void) { return lp_config_path("input.conf"); }

/* The list, in order, known ids only, English always somewhere in it. */
static GPtrArray *sources_get(void)
{
    char *c = input_conf();
    char *v = kv_get(c, "sources");
    g_free(c);
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    char **parts = g_strsplit(v ? v : "en,ko", ",", -1);
    gboolean en = FALSE;
    for (int i = 0; parts[i]; i++) {
        char *id = g_strstrip(parts[i]);
        if (!source_by_id(id)) continue;
        gboolean dup = FALSE;
        for (guint k = 0; k < a->len; k++) dup |= !strcmp(g_ptr_array_index(a, k), id);
        if (dup) continue;
        en |= !strcmp(id, "en");
        g_ptr_array_add(a, g_strdup(id));
    }
    if (!en) g_ptr_array_add(a, g_strdup("en"));
    g_strfreev(parts);
    g_free(v);
    return a;
}

static gboolean sources_save(GPtrArray *a)
{
    GString *s = g_string_new(NULL), *osk = g_string_new(NULL);
    for (guint i = 0; i < a->len; i++) {
        if (i) { g_string_append_c(s, ','); g_string_append_c(osk, ';'); }
        g_string_append(s, g_ptr_array_index(a, i));
        g_string_append(osk, g_ptr_array_index(a, i));
    }
    char *c = input_conf();
    gboolean ok = kv_set(c, "sources", s->str);
    g_free(c);
    if (ok) {
        char *ini = lp_config_path("osk.ini");
        ok = ini_set(ini, "keyboard", "layouts", osk->str);
        g_free(ini);
    }
    g_string_free(s, TRUE);
    g_string_free(osk, TRUE);
    return ok;
}

typedef struct {
    GtkWidget *page;
    GtkWidget *list;          /* the sources group */
    GtkWidget *add_row;
    GtkWidget *shortcuts;
} kbd_t;

static kbd_t *KB;

static void kbd_free(gpointer p)
{
    if (KB == p) KB = NULL;
    g_free(p);
}

static GtkWidget *source_row(const char *id);

/* Rows carry their source id; the list's order is the rows' order. */
static int row_index_of(const char *id)
{
    int i = 0;
    for (GtkWidget *c = gtk_widget_get_first_child(KB->list); c; c = gtk_widget_get_next_sibling(c)) {
        const char *rid = g_object_get_data(G_OBJECT(c), "lp-source");
        if (!rid) continue;
        if (!strcmp(rid, id)) return i;
        i++;
    }
    return -1;
}

static GtkWidget *row_of(const char *id)
{
    for (GtkWidget *c = gtk_widget_get_first_child(KB->list); c; c = gtk_widget_get_next_sibling(c)) {
        const char *rid = g_object_get_data(G_OBJECT(c), "lp-source");
        if (rid && !strcmp(rid, id) && gtk_widget_get_sensitive(c)) return c;
    }
    return NULL;
}

static void refresh_positions(void)
{
    GPtrArray *a = sources_get();
    for (guint i = 0; i < a->len; i++) {
        GtkWidget *r = row_of(g_ptr_array_index(a, i));
        if (!r) continue;
        row_set_detail(r, i == 0 ? T("First - new windows start with this one",
                                     "첫 번째 - 새 창은 이것으로 시작합니다") : NULL);
        GtkWidget *up = g_object_get_data(G_OBJECT(r), "lp-up");
        GtkWidget *down = g_object_get_data(G_OBJECT(r), "lp-down");
        gtk_widget_set_sensitive(up, i > 0);
        gtk_widget_set_sensitive(down, i + 1 < a->len);
    }
    if (KB->add_row) {
        gboolean all = a->len >= G_N_ELEMENTS(SOURCES);
        gtk_widget_set_sensitive(row_control(KB->add_row), !all);
        row_set_detail(KB->add_row, all ? T("Every input source this machine has is in the list",
                                            "이 기계의 입력 소스가 모두 목록에 있습니다") : NULL);
    }
    g_ptr_array_free(a, TRUE);
}

/* Move a source by `by` places: the row closes where it was and opens
 * where it goes (ui.c's insert motion), so the eye can follow it. */
static void move_source(const char *id, int by)
{
    GPtrArray *a = sources_get();
    int from = -1;
    for (guint i = 0; i < a->len; i++)
        if (!strcmp(g_ptr_array_index(a, i), id)) from = i;
    int to = from + by;
    if (from < 0 || to < 0 || to >= (int)a->len) { g_ptr_array_free(a, TRUE); return; }
    char *s = g_ptr_array_steal_index(a, from);
    g_ptr_array_insert(a, to, s);
    if (sources_save(a)) {
        GtkWidget *old = row_of(id);
        int at = row_index_of(id);
        if (old) row_remove_animated(old);
        row_insert_animated(KB->list, source_row(id), at + by + (by > 0 ? 1 : 0));
        refresh_positions();
        const source_t *src = source_by_id(id);
        lp_toast(FALSE, T("%s is now number %d", "%s 이(가) 이제 %d번째입니다"),
                 T(src->en, src->ko), to + 1);
    }
    g_ptr_array_free(a, TRUE);
}

static void on_up(GtkButton *b, gpointer p) { (void)b; move_source(p, -1); }
static void on_down(GtkButton *b, gpointer p) { (void)b; move_source(p, +1); }

static void english_to_bottom(lp_dialog_t *d, gpointer p)
{
    (void)p;
    GPtrArray *a = sources_get();
    int pos = -1;
    for (guint i = 0; i < a->len; i++)
        if (!strcmp(g_ptr_array_index(a, i), "en")) pos = i;
    int by = (int)a->len - 1 - pos;
    g_ptr_array_free(a, TRUE);
    lp_dialog_close(d);
    if (by > 0) move_source("en", by);
}

static void on_remove(GtkButton *b, gpointer p)
{
    (void)b;
    const char *id = p;
    if (!strcmp(id, "en")) {
        /* input-sources.md 2-2: refuse, say what it protects, and offer
         * what the person most likely wanted. The offer is only made
         * when it would change something. */
        GPtrArray *a = sources_get();
        gboolean last = !strcmp(g_ptr_array_index(a, a->len - 1), "en");
        g_ptr_array_free(a, TRUE);
        lp_dialog_t *d = lp_dialog_new(T("English (US) stays", "영어 (US) 는 지울 수 없습니다"),
                                       last ? NULL : T("Move to the bottom", "맨 아래로 내리기"),
                                       FALSE, english_to_bottom, NULL);
        lp_dialog_text(d, T("It is what is printed on the keys, and what the login screen, the lock "
                            "screen and the recovery console type passwords with. A machine without "
                            "it can end up with no way to type a password.",
                            "키보드에 인쇄된 글자이고, 로그인 화면, 잠금 화면, 복구 콘솔이 비밀번호를 "
                            "입력받는 배열입니다. 이것이 없으면 비밀번호를 칠 방법이 없어질 수 있습니다."),
                       NULL);
        if (!last)
            lp_dialog_text(d, T("Moved to the bottom, it stops being what new windows start with.",
                                "맨 아래로 내리면 새 창이 이것으로 시작하지 않습니다."), "lp-note");
        lp_dialog_present(d);
        return;
    }
    GPtrArray *a = sources_get();
    for (guint i = 0; i < a->len; i++)
        if (!strcmp(g_ptr_array_index(a, i), id)) { g_ptr_array_remove_index(a, i); break; }
    if (sources_save(a)) {
        GtkWidget *r = row_of(id);
        if (r) row_remove_animated(r);
        refresh_positions();
        const source_t *src = source_by_id(id);
        lp_toast(FALSE, T("Removed %s", "%s 을(를) 뺐습니다"), T(src->en, src->ko));
    }
    g_ptr_array_free(a, TRUE);
}

static GtkWidget *small_button(const char *icon, const char *tip, GCallback cb, gpointer data)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

static GtkWidget *source_row(const char *id)
{
    const source_t *s = source_by_id(id);
    GtkWidget *row = row_shell(T(s->en, s->ko), NULL);
    GtkWidget *h = row_box(row);
    GtkWidget *tag = gtk_label_new(s->tag);
    gtk_widget_add_css_class(tag, "lp-key");
    gtk_widget_set_valign(tag, GTK_ALIGN_CENTER);
    gtk_box_prepend(GTK_BOX(h), tag);
    /* The id string is static (SOURCES), so it can be the handlers' data. */
    GtkWidget *up = small_button("go-up-symbolic", T("Move up", "위로"), G_CALLBACK(on_up), (gpointer)s->id);
    GtkWidget *down = small_button("go-down-symbolic", T("Move down", "아래로"), G_CALLBACK(on_down), (gpointer)s->id);
    GtkWidget *rm = small_button("list-remove-symbolic", T("Remove", "빼기"), G_CALLBACK(on_remove), (gpointer)s->id);
    gtk_box_append(GTK_BOX(h), up);
    gtk_box_append(GTK_BOX(h), down);
    gtk_box_append(GTK_BOX(h), rm);
    g_object_set_data(G_OBJECT(row), "lp-up", up);
    g_object_set_data(G_OBJECT(row), "lp-down", down);
    g_object_set_data(G_OBJECT(row), "lp-control", rm);
    g_object_set_data(G_OBJECT(row), "lp-source", (gpointer)s->id);
    return row;
}

static void add_chosen(lp_dialog_t *d, gpointer p)
{
    GtkWidget *dd = lp_dialog_get_data(d, "lp-dd");
    GPtrArray *missing = p;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (i >= missing->len) return;
    const char *id = g_ptr_array_index(missing, i);
    GPtrArray *a = sources_get();
    g_ptr_array_add(a, g_strdup(id));
    if (sources_save(a)) {
        /* After the last source row, before "Add". */
        row_insert_animated(KB->list, source_row(id), (int)a->len - 1);
        refresh_positions();
        const source_t *s = source_by_id(id);
        lp_toast(FALSE, T("Added %s - the 한/영 key switches to it", "%s 을(를) 더했습니다 - 한/영 키로 바꿉니다"),
                 T(s->en, s->ko));
    }
    g_ptr_array_free(a, TRUE);
    lp_dialog_close(d);
}

static void on_add(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    GPtrArray *a = sources_get();
    GPtrArray *missing = g_ptr_array_new();
    GPtrArray *names = g_ptr_array_new();
    for (guint i = 0; i < G_N_ELEMENTS(SOURCES); i++) {
        gboolean have = FALSE;
        for (guint k = 0; k < a->len; k++) have |= !strcmp(g_ptr_array_index(a, k), SOURCES[i].id);
        if (have) continue;
        g_ptr_array_add(missing, (gpointer)SOURCES[i].id);
        g_ptr_array_add(names, (gpointer)T(SOURCES[i].en, SOURCES[i].ko));
    }
    g_ptr_array_add(names, NULL);
    g_ptr_array_free(a, TRUE);
    if (!missing->len) {
        g_ptr_array_free(missing, TRUE);
        g_ptr_array_free(names, TRUE);
        return;
    }
    lp_dialog_t *d = lp_dialog_new(T("Add an input source", "입력 소스 더하기"), T("Add", "더하기"),
                                   FALSE, add_chosen, missing);
    lp_dialog_set_data(d, "lp-missing", missing, (GDestroyNotify)g_ptr_array_unref);
    GtkWidget *dd = gtk_drop_down_new_from_strings((const char *const *)names->pdata);
    g_object_set_data_full(G_OBJECT(dd), "lp-title", g_strdup(T("Input source", "입력 소스")), g_free);
    gtk_box_append(GTK_BOX(lp_dialog_body(d)), dd);
    lp_dialog_set_data(d, "lp-dd", dd, NULL);
    lp_dialog_text(d, T("Only languages this machine has an input method for are listed.",
                        "이 기계에 입력기가 있는 언어만 나옵니다."), "lp-note");
    g_ptr_array_free(names, TRUE);
    lp_dialog_present(d);
}

/* ── the keys that switch 한/영, Korean on the laptop keyboard ─────────── */

enum { SW_RALT = 1, SW_SHIFTSPACE = 2, SW_CTRLSPACE = 4 };
#define SW_DEFAULT (SW_RALT | SW_SHIFTSPACE)

static unsigned switch_keys_get(void)
{
    char *ini = lp_config_path("osk.ini");
    char *v = ini_get(ini, "keyboard", "switch-keys");
    g_free(ini);
    if (!v) return SW_DEFAULT;
    unsigned k = 0;
    char **l = g_strsplit(v, ";", -1);
    for (int i = 0; l[i]; i++) {
        char *t = g_strstrip(l[i]);
        k |= !strcmp(t, "ralt") ? SW_RALT : !strcmp(t, "shift-space") ? SW_SHIFTSPACE
           : !strcmp(t, "ctrl-space") ? SW_CTRLSPACE : 0;
    }
    g_strfreev(l);
    g_free(v);
    return k;
}

/* fcitx5's trigger keys and wayfire's Right Alt, from the same choice.
 * Returns whether anything changed on disk. */
static gboolean switch_keys_fcitx(unsigned k)
{
    gboolean changed = FALSE;
    char *fc = g_build_filename(g_get_user_config_dir(), "fcitx5", "config", NULL);
    const char *want[4] = { "Hangul", NULL, NULL, NULL };
    int n = 1;
    if (k & SW_SHIFTSPACE) want[n++] = "Shift+space";
    if (k & SW_CTRLSPACE) want[n++] = "Control+space";
    for (int i = 0; i < 4; i++) {
        char key[4];
        g_snprintf(key, sizeof key, "%d", i);
        char *old = ini_get(fc, "Hotkey/TriggerKeys", key);
        if (i < n && g_strcmp0(old, want[i])) {
            ini_set(fc, "Hotkey/TriggerKeys", key, want[i]);
            changed = TRUE;
        } else if (i >= n && old) {
            ini_unset(fc, "Hotkey/TriggerKeys", key);
            changed = TRUE;
        }
        g_free(old);
    }
    g_free(fc);

    /* Only into a wayfire.ini that is there: made here it would hold this
     * one key, and wayfire started on it would have no plugins at all. */
    char *wf = wayfire_ini();
    if (!g_file_test(wf, G_FILE_TEST_EXISTS)) {
        g_free(wf);
        return changed;
    }
    char *xo = ini_get(wf, "input", "xkb_options");
    GString *o = g_string_new(NULL);
    char **l = g_strsplit(xo ? xo : "", ",", -1);
    for (int i = 0; l[i]; i++) {
        char *t = g_strstrip(l[i]);
        if (!*t || !strcmp(t, "korean:ralt_hangul")) continue;
        if (o->len) g_string_append_c(o, ',');
        g_string_append(o, t);
    }
    g_strfreev(l);
    if (k & SW_RALT)
        g_string_append(o, o->len ? ",korean:ralt_hangul" : "korean:ralt_hangul");
    if (g_strcmp0(xo, o->str)) {
        ini_set(wf, "input", "xkb_options", o->str);
        changed = TRUE;
    }
    g_string_free(o, TRUE);
    g_free(xo);
    g_free(wf);
    return changed;
}

/* fcitx5-remote is a D-Bus call, and the session bus starts fcitx5 for
 * it when it is not running (Debian ships org.fcitx.Fcitx5.service).
 * Under sway nothing else starts fcitx5 - lp-osk does the typing there -
 * and one started this way made the top bar's EN/한 follow it instead of
 * lp-osk. A fcitx5 that is not running reads its files when it starts,
 * so only a running one is told. */
static gboolean fcitx_running(void)
{
    if (!lp_have("fcitx5-remote")) return FALSE;
    char *uid = g_strdup_printf("%u", (unsigned)getuid());
    const char *v[] = { "pgrep", "-x", "-u", uid, "fcitx5", NULL };
    int st = lp_run_full(v, NULL, NULL, NULL);
    g_free(uid);
    return st == 0;
}

/* Tell the running input methods. Both are optional: whichever is not
 * running reads the files when it starts. */
static void switch_keys_tell(gboolean fcitx)
{
    const char *osk[] = { "lp-osk", "reload", NULL };
    lp_spawn_bg(osk);
    if (fcitx && fcitx_running()) {
        const char *fr[] = { "fcitx5-remote", "-r", NULL };
        lp_spawn_bg(fr);
    }
}

static void switch_keys_set(unsigned k)
{
    char *list = g_strdup_printf("hangul%s%s%s", k & SW_RALT ? ";ralt" : "",
                                 k & SW_SHIFTSPACE ? ";shift-space" : "",
                                 k & SW_CTRLSPACE ? ";ctrl-space" : "");
    char *c = input_conf();
    kv_set(c, "switch_keys", list);
    g_free(c);
    char *ini = lp_config_path("osk.ini");
    ini_set(ini, "keyboard", "switch-keys", list);
    g_free(ini);
    g_free(list);
    switch_keys_tell(switch_keys_fcitx(k));
}

static void on_switch_key(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps;
    unsigned bit = GPOINTER_TO_UINT(p);
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    unsigned k = switch_keys_get();
    k = on ? (k | bit) : (k & ~bit);
    switch_keys_set(k);
    const char *name = bit == SW_RALT ? T("Right Alt", "오른쪽 Alt")
                     : bit == SW_SHIFTSPACE ? "Shift+Space" : "Ctrl+Space";
    if (on)
        lp_toast(FALSE, T("%s now switches 한/영", "이제 %s 로 한/영을 바꿉니다"), name);
    else
        lp_toast(FALSE, T("%s no longer switches 한/영", "%s 로는 한/영을 바꾸지 않습니다"), name);
}

static void on_physical(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    char *ini = lp_config_path("osk.ini");
    if (ini_set(ini, "keyboard", "physical-korean", on ? "true" : "false"))
        lp_toast(FALSE, on ? T("Korean can be typed on the laptop's own keys", "노트북 키보드로도 한글을 칩니다")
                           : T("Korean only from the on-screen keyboard", "한글은 화상 키보드로만 칩니다"));
    g_free(ini);
}

/* ── repeat ─────────────────────────────────────────────────────────── */

typedef struct { const char *key; char *value; } wf_kv_t;

static void wf_kv_write(gpointer p)
{
    wf_kv_t *w = p;
    char *ini = wayfire_ini();
    ini_set(ini, "input", w->key, w->value);
    g_free(ini);
}

static void wf_kv_free(gpointer p)
{
    wf_kv_t *w = p;
    g_free(w->value);
    g_free(w);
}

static void on_repeat(GtkRange *r, gpointer p)
{
    const char *which = p;           /* "delay" or "rate" */
    int v = (int)(gtk_range_get_value(r) + 0.5);
    wf_kv_t *w = g_new0(wf_kv_t, 1);
    w->key = !strcmp(which, "delay") ? "kb_repeat_delay" : "kb_repeat_rate";
    w->value = g_strdup_printf("%d", v);
    lp_later(w->key, 200, wf_kv_write, w, wf_kv_free);
    char vs[16];
    g_snprintf(vs, sizeof vs, "%d", v);
    const char *a[] = { "input", "type:keyboard", !strcmp(which, "delay") ? "repeat_delay" : "repeat_rate",
                        vs, NULL };
    lp_swaymsg(a);
}

static char *ms_words(double v) { return g_strdup_printf("%d ms", (int)(v + 0.5)); }
static char *rate_words(double v) { return g_strdup_printf(T("%d/s", "초당 %d"), (int)(v + 0.5)); }

static int wf_int(const char *sec, const char *key, int dflt)
{
    char *ini = wayfire_ini();
    char *v = ini_get(ini, sec, key);
    g_free(ini);
    int r = v && *v ? atoi(v) : dflt;
    g_free(v);
    return r;
}

/* ── shortcuts ──────────────────────────────────────────────────────── */

typedef struct { const char *sec, *key, *en, *ko, *dflt; } shortcut_t;

/* The shortcuts the desktop ships, with the bindings it ships them with
 * (desktop/session/wayfire.ini - change both together) - which is also
 * what Reset puts back. The common ones from other systems are all here
 * at once: Super+Up and Super+Down for the window, Alt+F4, Ctrl+Alt+T,
 * Super+Shift+S, Ctrl+Shift+Esc, Super+Space. A gesture after the keys
 * ("| pinch in 3") is kept when the keys are changed. */
static const shortcut_t SHORTCUTS[] = {
    { "command",    "binding_inputlang", "Switch the input language (한/영)", "입력 언어 전환 (한/영)",
      "<super> KEY_SPACE" },
    { "command",    "binding_terminal", "Open a terminal", "터미널 열기", "<super> KEY_T | <ctrl> <alt> KEY_T" },
    { "command",    "binding_files", "Open Files", "파일 열기", "<super> KEY_E" },
    { "command",    "binding_settings", "Open Settings", "설정 열기", "<super> KEY_I" },
    { "command",    "binding_apps", "Show all apps", "모든 앱 보기", "<super> KEY_A" },
    { "command",    "binding_quick", "Quick settings", "빠른 설정", "<super> KEY_N" },
    { "command",    "binding_tasks", "Task Manager", "작업 관리자", "<ctrl> <shift> KEY_ESC" },
    { "command",    "binding_osk", "On-screen keyboard", "화상 키보드", "<super> KEY_K" },
    { "command",    "binding_lock", "Lock the screen", "화면 잠금", "<super> KEY_L" },
    { "command",    "binding_shot", "Screenshot", "스크린샷", "KEY_SYSRQ" },
    { "command",    "binding_shot_area", "Screenshot of an area", "영역 스크린샷",
      "<shift> KEY_SYSRQ | <super> <shift> KEY_S" },
    { "command",    "binding_shot_window", "Screenshot of the window", "창 스크린샷", "<alt> KEY_SYSRQ" },
    { "command",    "binding_shot_app", "Screenshot app", "스크린샷 앱", "<ctrl> <shift> KEY_SYSRQ" },
    { "core",       "close_top_view", "Close the window", "창 닫기", "<super> KEY_Q | <alt> KEY_F4" },
    { "wm-actions", "toggle_maximize", "Maximize or restore the window", "창 최대화 / 복원",
      "<super> KEY_UP | <super> KEY_M" },
    { "wm-actions", "minimize", "Minimize the window", "창 최소화", "<super> KEY_DOWN | <super> KEY_H" },
    { "wm-actions", "toggle_fullscreen", "Full screen", "전체 화면", "<super> KEY_F | KEY_F11" },
    { "grid",       "slot_l", "Window to the left half", "창을 왼쪽 절반으로", "<super> KEY_LEFT" },
    { "grid",       "slot_r", "Window to the right half", "창을 오른쪽 절반으로", "<super> KEY_RIGHT" },
    { "switcher",   "next_view", "Switch windows", "창 전환", "<alt> KEY_TAB" },
    { "switcher",   "prev_view", "Switch windows backwards", "창 거꾸로 전환", "<alt> <shift> KEY_TAB" },
    { "scale",      "toggle", "Show all windows", "모든 창 보기", "<super> KEY_S | pinch in 3" },
    { "expo",       "toggle", "Show all workspaces", "모든 작업공간 보기", "<super> KEY_TAB | pinch in 4" },
    { "vswitch",    "binding_left", "Workspace on the left", "왼쪽 작업공간",
      "<super> <ctrl> KEY_LEFT | swipe right 3" },
    { "vswitch",    "binding_right", "Workspace on the right", "오른쪽 작업공간",
      "<super> <ctrl> KEY_RIGHT | swipe left 3" },
    { "command",    "binding_power", "Log out, restart or shut down", "로그아웃 / 다시 시작 / 끄기",
      "<ctrl> <alt> KEY_DELETE" },
    { "command",    "binding_logout", "End the session now", "세션 바로 끝내기", "<ctrl> <alt> KEY_BACKSPACE" },
};

#define K(x) { x, #x }
static const struct { int code; const char *name; } KEYNAMES[] = {
    K(KEY_ESC), K(KEY_1), K(KEY_2), K(KEY_3), K(KEY_4), K(KEY_5), K(KEY_6), K(KEY_7), K(KEY_8),
    K(KEY_9), K(KEY_0), K(KEY_MINUS), K(KEY_EQUAL), K(KEY_BACKSPACE), K(KEY_TAB), K(KEY_Q),
    K(KEY_W), K(KEY_E), K(KEY_R), K(KEY_T), K(KEY_Y), K(KEY_U), K(KEY_I), K(KEY_O), K(KEY_P),
    K(KEY_LEFTBRACE), K(KEY_RIGHTBRACE), K(KEY_ENTER), K(KEY_A), K(KEY_S), K(KEY_D), K(KEY_F),
    K(KEY_G), K(KEY_H), K(KEY_J), K(KEY_K), K(KEY_L), K(KEY_SEMICOLON), K(KEY_APOSTROPHE),
    K(KEY_GRAVE), K(KEY_BACKSLASH), K(KEY_Z), K(KEY_X), K(KEY_C), K(KEY_V), K(KEY_B), K(KEY_N),
    K(KEY_M), K(KEY_COMMA), K(KEY_DOT), K(KEY_SLASH), K(KEY_SPACE), K(KEY_F1), K(KEY_F2),
    K(KEY_F3), K(KEY_F4), K(KEY_F5), K(KEY_F6), K(KEY_F7), K(KEY_F8), K(KEY_F9), K(KEY_F10),
    K(KEY_F11), K(KEY_F12), K(KEY_SYSRQ), K(KEY_PRINT), K(KEY_HOME), K(KEY_UP), K(KEY_PAGEUP),
    K(KEY_LEFT), K(KEY_RIGHT), K(KEY_END), K(KEY_DOWN), K(KEY_PAGEDOWN), K(KEY_INSERT),
    K(KEY_DELETE), K(KEY_MUTE), K(KEY_VOLUMEDOWN), K(KEY_VOLUMEUP), K(KEY_PLAYPAUSE),
    K(KEY_NEXTSONG), K(KEY_PREVIOUSSONG), K(KEY_BRIGHTNESSDOWN), K(KEY_BRIGHTNESSUP),
    K(KEY_MICMUTE), K(KEY_STOPCD),
    K(KEY_KP0), K(KEY_KP1), K(KEY_KP2), K(KEY_KP3), K(KEY_KP4), K(KEY_KP5), K(KEY_KP6),
    K(KEY_KP7), K(KEY_KP8), K(KEY_KP9), K(KEY_KPENTER), K(KEY_KPPLUS), K(KEY_KPMINUS),
};
#undef K

static const char *key_name(int code)
{
    for (guint i = 0; i < G_N_ELEMENTS(KEYNAMES); i++)
        if (KEYNAMES[i].code == code) return KEYNAMES[i].name;
    return NULL;
}

static gboolean key_known(const char *name)
{
    for (guint i = 0; i < G_N_ELEMENTS(KEYNAMES); i++)
        if (!strcmp(KEYNAMES[i].name, name)) return TRUE;
    return FALSE;
}

/* Wayfire's form into what a person reads: "<super> <shift> KEY_S |
 * KEY_PRINT" -> "Super+Shift+S or Print". */
static char *pretty(const char *binding)
{
    if (!binding || !*binding || !strcmp(binding, "none"))
        return g_strdup(T("Not set", "지정 안 됨"));
    GString *out = g_string_new(NULL);
    char **alts = g_strsplit(binding, "|", -1);
    for (int a = 0; alts[a]; a++) {
        char **tok = g_strsplit_set(g_strstrip(alts[a]), " \t", -1);
        GString *one = g_string_new(NULL);
        for (int i = 0; tok[i]; i++) {
            char *t = tok[i];
            if (!*t) continue;
            if (one->len) g_string_append_c(one, '+');
            if (*t == '<') {
                char *m = g_strndup(t + 1, strlen(t) - 2);
                if (!strcmp(m, "super")) g_string_append(one, "Super");
                else if (!strcmp(m, "ctrl")) g_string_append(one, "Ctrl");
                else if (!strcmp(m, "alt")) g_string_append(one, "Alt");
                else if (!strcmp(m, "shift")) g_string_append(one, "Shift");
                else g_string_append(one, m);
                g_free(m);
            } else if (g_str_has_prefix(t, "KEY_")) {
                const char *k = t + 4;
                if (!strcmp(k, "SYSRQ") || !strcmp(k, "PRINT")) g_string_append(one, "Print");
                else if (strlen(k) == 1) g_string_append(one, k);
                else {
                    char *low = g_ascii_strdown(k, -1);
                    low[0] = g_ascii_toupper(low[0]);
                    g_string_append(one, low);
                    g_free(low);
                }
            } else {
                g_string_append(one, t);
            }
        }
        /* KEY_SYSRQ | KEY_PRINT are the same key on two keyboards; say it once. */
        if (!strstr(out->str, one->str)) {
            if (out->len) g_string_append(out, T(" or ", " 또는 "));
            g_string_append(out, one->str);
        }
        g_string_free(one, TRUE);
        g_strfreev(tok);
    }
    g_strfreev(alts);
    return g_string_free(out, FALSE);
}

/* One alternative, normalised so two spellings of the same keys compare
 * equal: modifiers in a fixed order, then the key. */
static char *normalise(const char *alt)
{
    char **tok = g_strsplit_set(alt, " \t", -1);
    gboolean super = FALSE, ctrl = FALSE, altm = FALSE, shift = FALSE;
    const char *key = NULL;
    for (int i = 0; tok[i]; i++) {
        if (!strcmp(tok[i], "<super>")) super = TRUE;
        else if (!strcmp(tok[i], "<ctrl>")) ctrl = TRUE;
        else if (!strcmp(tok[i], "<alt>")) altm = TRUE;
        else if (!strcmp(tok[i], "<shift>")) shift = TRUE;
        else if (*tok[i]) key = tok[i];
    }
    char *r = g_strdup_printf("%s%s%s%s%s", super ? "<super> " : "", ctrl ? "<ctrl> " : "",
                              altm ? "<alt> " : "", shift ? "<shift> " : "", key ? key : "");
    g_strfreev(tok);
    return r;
}

static gboolean binding_has(const char *binding, const char *combo)
{
    if (!binding) return FALSE;
    char *want = normalise(combo);
    char **alts = g_strsplit(binding, "|", -1);
    gboolean hit = FALSE;
    for (int i = 0; alts[i] && !hit; i++) {
        char *n = normalise(g_strstrip(alts[i]));
        hit = !strcmp(n, want);
        g_free(n);
    }
    g_strfreev(alts);
    g_free(want);
    return hit;
}

/* A shortcut on screen: where it lives and what it is called. Custom
 * ones are [command] binding_lp_N. */
typedef struct { char *sec, *key, *name; } sc_t;

static void sc_free(gpointer p)
{
    sc_t *s = p;
    g_free(s->sec); g_free(s->key); g_free(s->name);
    g_free(s);
}

static char *shortcut_names(void) { return lp_config_path("shortcuts.conf"); }

/* Every shortcut there is: the shipped table, then [command] bindings
 * that are not in it (the person's own, or added by hand). */
static GPtrArray *all_shortcuts(void)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(sc_free);
    for (guint i = 0; i < G_N_ELEMENTS(SHORTCUTS); i++) {
        sc_t *s = g_new0(sc_t, 1);
        s->sec = g_strdup(SHORTCUTS[i].sec);
        s->key = g_strdup(SHORTCUTS[i].key);
        s->name = g_strdup(T(SHORTCUTS[i].en, SHORTCUTS[i].ko));
        g_ptr_array_add(a, s);
    }
    char *ini = wayfire_ini();
    char **keys = ini_keys(ini, "command");
    char *names = shortcut_names();
    for (int i = 0; keys[i]; i++) {
        if (!g_str_has_prefix(keys[i], "binding_")) continue;
        gboolean known = FALSE;
        for (guint k = 0; k < G_N_ELEMENTS(SHORTCUTS); k++)
            known |= !strcmp(SHORTCUTS[k].sec, "command") && !strcmp(SHORTCUTS[k].key, keys[i]);
        if (known) continue;
        sc_t *s = g_new0(sc_t, 1);
        s->sec = g_strdup("command");
        s->key = g_strdup(keys[i]);
        char *nm = kv_get(names, keys[i] + 8);
        if (!nm) {
            char *ck = g_strdup_printf("command_%s", keys[i] + 8);
            nm = ini_get(ini, "command", ck);
            g_free(ck);
        }
        s->name = nm ? nm : g_strdup(keys[i] + 8);
        g_ptr_array_add(a, s);
    }
    g_free(names);
    g_strfreev(keys);
    g_free(ini);
    return a;
}

static void fill_shortcuts(void);

typedef struct {
    char        *sec, *key, *name;
    char        *combo;           /* what was pressed, wayfire's form */
    GtkWidget   *shown;           /* the big label */
    GtkWidget   *entry;           /* "or type it" */
    gboolean     custom_new;      /* adding, not changing */
    GtkWidget   *name_entry, *cmd_entry;
} capture_t;

static void capture_free(gpointer p)
{
    capture_t *c = p;
    g_free(c->sec); g_free(c->key); g_free(c->name); g_free(c->combo);
    g_free(c);
}

static gboolean on_capture_key(GtkEventControllerKey *k, guint keyval, guint keycode,
                               GdkModifierType state, gpointer p)
{
    (void)k;
    capture_t *c = p;
    /* Typing into the text fields is typing, not a shortcut. */
    GtkWidget *focus = gtk_root_get_focus(gtk_widget_get_root(c->shown));
    if (focus && (gtk_widget_is_ancestor(focus, c->entry) || focus == c->entry ||
                  (c->name_entry && gtk_widget_is_ancestor(focus, c->name_entry)) ||
                  (c->cmd_entry && gtk_widget_is_ancestor(focus, c->cmd_entry))))
        return FALSE;
    switch (keyval) {
    case GDK_KEY_Shift_L: case GDK_KEY_Shift_R: case GDK_KEY_Control_L: case GDK_KEY_Control_R:
    case GDK_KEY_Alt_L: case GDK_KEY_Alt_R: case GDK_KEY_Super_L: case GDK_KEY_Super_R:
    case GDK_KEY_Meta_L: case GDK_KEY_Meta_R: case GDK_KEY_ISO_Level3_Shift:
        return FALSE;                  /* wait for the key itself */
    case GDK_KEY_Escape:
        return FALSE;                  /* leaves the dialog */
    default: break;
    }
    const char *name = key_name((int)keycode - 8);
    if (!name) {
        gtk_label_set_text(GTK_LABEL(c->shown), T("That key cannot be used for a shortcut",
                                                  "그 키는 단축키로 쓸 수 없습니다"));
        return TRUE;
    }
    GString *s = g_string_new(NULL);
    if (state & GDK_SUPER_MASK) g_string_append(s, "<super> ");
    if (state & GDK_CONTROL_MASK) g_string_append(s, "<ctrl> ");
    if (state & GDK_ALT_MASK) g_string_append(s, "<alt> ");
    if (state & GDK_SHIFT_MASK) g_string_append(s, "<shift> ");
    g_string_append(s, name);
    gtk_editable_set_text(GTK_EDITABLE(c->entry), s->str);
    g_string_free(s, TRUE);
    return TRUE;
}

/* The entry is the one place the combination lives; the key handler
 * writes into it, and so does a person who would rather type it. */
static void on_combo_text(GtkEditable *e, gpointer p)
{
    capture_t *c = p;
    const char *t = gtk_editable_get_text(e);
    g_free(c->combo);
    c->combo = NULL;
    char *n = normalise(t);
    const char *last = strrchr(n, ' ');
    last = last ? last + 1 : n;
    if (*t && key_known(last) && !strchr(t, '|')) {
        c->combo = n;
        char *pr = pretty(n);
        gtk_label_set_text(GTK_LABEL(c->shown), pr);
        g_free(pr);
    } else {
        g_free(n);
        gtk_label_set_text(GTK_LABEL(c->shown), *t ? T("Not a shortcut wayfire understands",
                                                       "wayfire 가 알 수 없는 단축키입니다")
                                                   : T("Press the new keys now", "새 키를 지금 누르십시오"));
    }
}

/* ── the same shortcuts for sway ─────────────────────────────────────── */

/* A shortcut's binding: the file's, or - for a key an older wayfire.ini
 * does not have - what the desktop ships. */
static char *binding_of(const char *ini, const char *sec, const char *key)
{
    char *v = ini_get(ini, sec, key);
    if (v) return v;
    for (guint i = 0; i < G_N_ELEMENTS(SHORTCUTS); i++)
        if (!strcmp(SHORTCUTS[i].sec, sec) && !strcmp(SHORTCUTS[i].key, key))
            return g_strdup(SHORTCUTS[i].dflt);
    return NULL;
}

/* What a wayfire plugin's binding does, as a sway command ([command]
 * bindings are `exec` of their command). Plugins sway has nothing like
 * - scale, expo - are missing, and so left out of sway's file. */
static const struct { const char *sec, *key, *cmd; } SWAY_DOES[] = {
    { "core",       "close_top_view",    "kill" },
    { "wm-actions", "toggle_maximize",   "maximize toggle" },
    { "wm-actions", "minimize",          "minimize" },
    { "wm-actions", "toggle_fullscreen", "fullscreen toggle" },
    /* sway sizes a workspace to the output less the bars, and ppt and
     * position are relative to it: half of what is free, under the top
     * bar and above the dock. */
    { "grid",       "slot_l", "maximize disable, resize set width 50ppt height 100ppt, move position 0 0" },
    { "grid",       "slot_r", "maximize disable, resize set width 50ppt height 100ppt, move position 50ppt 0" },
    { "switcher",   "next_view", "focus next" },
    { "switcher",   "prev_view", "focus prev" },
    { "vswitch",    "binding_left", "workspace prev_on_output" },
    { "vswitch",    "binding_right", "workspace next_on_output" },
    { "vswitch",    "binding_win_left", "move container to workspace prev_on_output, workspace prev_on_output" },
    { "vswitch",    "binding_win_right", "move container to workspace next_on_output, workspace next_on_output" },
};

static int key_code(const char *name)
{
    for (guint i = 0; i < G_N_ELEMENTS(KEYNAMES); i++)
        if (!strcmp(KEYNAMES[i].name, name)) return KEYNAMES[i].code;
    return -1;
}

/* "<super> <shift> KEY_S | pinch in 3" as sway bindings. By key code, not
 * key name, so they are where wayfire's are whatever the layout, and a
 * gesture or anything else sway cannot bind is skipped. */
static void sway_bind(GString *out, const char *binding, const char *cmd)
{
    if (!binding) return;
    char **alts = g_strsplit(binding, "|", -1);
    for (int a = 0; alts[a]; a++) {
        char **tok = g_strsplit_set(g_strstrip(alts[a]), " \t", -1);
        GString *mods = g_string_new(NULL);
        int code = -1;
        gboolean bad = FALSE;
        for (int i = 0; tok[i]; i++) {
            const char *t = tok[i];
            if (!*t) continue;
            if (!strcmp(t, "<super>")) g_string_append(mods, "Mod4+");
            else if (!strcmp(t, "<ctrl>")) g_string_append(mods, "Ctrl+");
            else if (!strcmp(t, "<alt>")) g_string_append(mods, "Mod1+");
            else if (!strcmp(t, "<shift>")) g_string_append(mods, "Shift+");
            else {
                int c = key_code(t);
                if (c >= 0 && code < 0) code = c;
                else bad = TRUE;
            }
        }
        /* sway 1.7 reads a first key code that is also a mouse button's
         * event code (BTN_*, 0x100 and up) as that button: `bindcode 256`
         * is BTN_0, not the mic-mute key. Those keys go by keysym. */
        if (!bad && code == KEY_MICMUTE)
            g_string_append_printf(out, "bindsym --no-warn %sXF86AudioMicMute %s\n", mods->str, cmd);
        else if (!bad && code >= 0 && code + 8 < 0x100)
            g_string_append_printf(out, "bindcode --no-warn %s%d %s\n", mods->str, code + 8, cmd);
        g_string_free(mods, TRUE);
        g_strfreev(tok);
    }
    g_strfreev(alts);
}

char *lp_keyboard_sway_keys(void)
{
    GString *o = g_string_new(
        "# Written by Settings > Keyboard from ~/.config/wayfire.ini, the one list\n"
        "# of shortcuts for both compositors; sway's config includes it. Change the\n"
        "# shortcuts in Settings: this file is written again at every change and\n"
        "# at every login.\n");
    char *ini = wayfire_ini();
    for (guint i = 0; i < G_N_ELEMENTS(SWAY_DOES); i++) {
        char *v = binding_of(ini, SWAY_DOES[i].sec, SWAY_DOES[i].key);
        sway_bind(o, v, SWAY_DOES[i].cmd);
        g_free(v);
    }
    char **keys = ini_keys(ini, "command");
    for (int i = 0; keys[i]; i++) {
        const char *name = g_str_has_prefix(keys[i], "binding_") ? keys[i] + 8
                         : g_str_has_prefix(keys[i], "repeatable_binding_") ? keys[i] + 19 : NULL;
        if (!name) continue;
        char *ck = g_strdup_printf("command_%s", name);
        char *cmd = ini_get(ini, "command", ck);
        char *v = ini_get(ini, "command", keys[i]);
        if (cmd && *cmd && !strchr(cmd, '\n')) {
            char *ex = g_strdup_printf("exec %s", cmd);
            sway_bind(o, v, ex);
            g_free(ex);
        }
        g_free(v); g_free(cmd); g_free(ck);
    }
    g_strfreev(keys);
    /* Not keys, but read at the same login: the pointer speeds Settings >
     * Mouse & Touchpad keeps in wayfire.ini, which sway would otherwise
     * forget at every login (the slider sends them to a running sway). */
    static const struct { const char *key, *type; } SPEEDS[] = {
        { "mouse_cursor_speed", "pointer" }, { "touchpad_cursor_speed", "touchpad" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(SPEEDS); i++) {
        char *v = ini_get(ini, "input", SPEEDS[i].key);
        char *end = NULL;
        double d = v ? g_ascii_strtod(v, &end) : 0;
        if (v && end != v && d >= -1 && d <= 1) {
            char sp[32];
            g_ascii_formatd(sp, sizeof sp, "%.2f", d);
            g_string_append_printf(o, "input type:%s pointer_accel %s\n", SPEEDS[i].type, sp);
        }
        g_free(v);
    }
    g_free(ini);
    return g_string_free(o, FALSE);
}

/* Write sway's file if it changed, and have a running sway read it. */
static void sway_keys_write(void)
{
    /* No wayfire.ini (an account made after install): no list to write
     * from, and an empty one would take every app key away from sway. */
    char *wf = wayfire_ini();
    gboolean have = g_file_test(wf, G_FILE_TEST_EXISTS);
    g_free(wf);
    if (!have) return;
    char *t = lp_keyboard_sway_keys();
    char *path = g_build_filename(g_get_user_config_dir(), "sway", "lp-keys.conf", NULL);
    char *old = lp_slurp(path);
    char *now = g_strchomp(g_strdup(t));
    if (g_strcmp0(old, now)) {
        char *dir = g_path_get_dirname(path);
        g_mkdir_with_parents(dir, 0700);
        g_free(dir);
        if (lp_write_file(path, t)) {
            const char *a[] = { "reload", NULL };
            lp_swaymsg(a);
        }
    }
    g_free(now); g_free(old); g_free(path); g_free(t);
}

/* A new combination for a shipped shortcut replaces its keys; a gesture
 * it also had (a pinch, a swipe) stays. */
static void save_binding(const char *sec, const char *key, const char *value)
{
    char *ini = wayfire_ini();
    char *old = binding_of(ini, sec, key);
    GString *v = g_string_new(value);
    char **alts = g_strsplit(old ? old : "", "|", -1);
    for (int i = 0; alts[i]; i++) {
        char *a = g_strstrip(alts[i]);
        if (*a && !strstr(a, "KEY_") && strcmp(a, "none"))
            g_string_append_printf(v, " | %s", a);
    }
    g_strfreev(alts);
    ini_set(ini, sec, key, v->str);
    g_string_free(v, TRUE);
    g_free(old);
    g_free(ini);
    sway_keys_write();
}

static void capture_apply(lp_dialog_t *d, capture_t *c, sc_t *taken)
{
    if (taken) {
        /* The other shortcut gives the combination up: the alternatives
         * it had besides this one stay. */
        char *ini = wayfire_ini();
        char *old = binding_of(ini, taken->sec, taken->key);
        GString *keep = g_string_new(NULL);
        char **alts = g_strsplit(old ? old : "", "|", -1);
        for (int i = 0; alts[i]; i++) {
            char *a = g_strstrip(alts[i]);
            if (!*a || binding_has(a, c->combo)) continue;
            if (keep->len) g_string_append(keep, " | ");
            g_string_append(keep, a);
        }
        g_strfreev(alts);
        ini_set(ini, taken->sec, taken->key, keep->len ? keep->str : "none");
        g_string_free(keep, TRUE);
        g_free(old);
        g_free(ini);
    }
    if (c->custom_new) {
        const char *name = gtk_editable_get_text(GTK_EDITABLE(c->name_entry));
        const char *cmd = gtk_editable_get_text(GTK_EDITABLE(c->cmd_entry));
        char *ini = wayfire_ini();
        int n = 1;
        for (;; n++) {
            char *k = g_strdup_printf("binding_lp_%d", n);
            char *v = ini_get(ini, "command", k);
            g_free(k);
            if (!v) break;
            g_free(v);
        }
        char *bk = g_strdup_printf("binding_lp_%d", n), *ck = g_strdup_printf("command_lp_%d", n);
        char *nk = g_strdup_printf("lp_%d", n);
        /* The command first: wayfire pairs them by name, and a binding
         * that arrives before its command binds nothing. */
        ini_set(ini, "command", ck, cmd);
        ini_set(ini, "command", bk, c->combo);
        char *names = shortcut_names();
        kv_set(names, nk, name);
        g_free(names); g_free(bk); g_free(ck); g_free(nk); g_free(ini);
        sway_keys_write();
        char *pr = pretty(c->combo);
        lp_toast(FALSE, T("%s now runs %s", "%s 을(를) 누르면 %s"), pr, name);
        g_free(pr);
    } else {
        save_binding(c->sec, c->key, c->combo);
        char *pr = pretty(c->combo);
        lp_toast(FALSE, T("%s: %s", "%s: %s"), c->name, pr);
        g_free(pr);
    }
    lp_dialog_close(d);
    fill_shortcuts();
}

/* The question is asked over the capture dialog; if that one was closed
 * in the meantime there is nothing left to apply. */
static void takeover_ok(lp_dialog_t *d, gpointer p)
{
    (void)p;
    GtkWidget *win = lp_dialog_get_data(d, "lp-parent-win");
    lp_dialog_t *parent = win && !g_object_get_data(G_OBJECT(win), "lp-closing")
                        ? g_object_get_data(G_OBJECT(win), "lp-dialog") : NULL;
    capture_t *c = parent ? lp_dialog_get_data(parent, "lp-capture") : NULL;
    sc_t *taken = lp_dialog_get_data(d, "lp-taken");
    if (c) capture_apply(parent, c, taken);
    lp_dialog_close(d);
}

static void capture_ok(lp_dialog_t *d, gpointer p)
{
    (void)p;
    capture_t *c = lp_dialog_get_data(d, "lp-capture");
    if (c->custom_new) {
        char *name = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(c->name_entry))));
        char *cmd = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(c->cmd_entry))));
        const char *bad = !*name || !*cmd ? T("Give it a name and a command.", "이름과 명령을 적으십시오.")
                        : strchr(cmd, '\n') || strchr(name, '\n') || strchr(name, '=')
                          ? T("One line each, and no = in the name.", "한 줄로, 이름에 = 없이 적어 주십시오.")
                          : NULL;
        g_free(name); g_free(cmd);
        if (bad) {
            lp_dialog_error(d, bad);
            return;
        }
    }
    if (!c->combo) {
        lp_dialog_error(d, T("Press the keys first.", "먼저 키를 누르십시오."));
        return;
    }
    /* Who else has it? */
    GPtrArray *all = all_shortcuts();
    char *ini = wayfire_ini();
    sc_t *taken = NULL;
    for (guint i = 0; i < all->len && !taken; i++) {
        sc_t *s = g_ptr_array_index(all, i);
        if (!c->custom_new && !strcmp(s->sec, c->sec) && !strcmp(s->key, c->key)) continue;
        char *v = binding_of(ini, s->sec, s->key);
        if (binding_has(v, c->combo)) taken = s;
        g_free(v);
    }
    g_free(ini);
    if (!taken) {
        capture_apply(d, c, NULL);
        g_ptr_array_free(all, TRUE);
        return;
    }
    /* Taken: say by what, and let the person decide. */
    sc_t *copy = g_new0(sc_t, 1);
    copy->sec = g_strdup(taken->sec); copy->key = g_strdup(taken->key); copy->name = g_strdup(taken->name);
    g_ptr_array_free(all, TRUE);
    char *pr = pretty(c->combo);
    char *title = g_strdup_printf(T("%s is already used", "%s 은(는) 이미 쓰고 있습니다"), pr);
    lp_dialog_t *q = lp_dialog_new(title, T("Use it here instead", "여기에 쓰기"), FALSE, takeover_ok, NULL);
    lp_dialog_set_data(q, "lp-parent-win", g_object_ref(lp_dialog_window(d)), g_object_unref);
    char *body = g_strdup_printf(T("It is the shortcut for \"%s\". Used here, \"%s\" loses it.",
                                   "'%s' 의 단축키입니다. 여기에 쓰면 '%s' 에서는 빠집니다."),
                                 copy->name, copy->name);
    lp_dialog_text(q, body, NULL);
    lp_dialog_set_data(q, "lp-taken", copy, sc_free);
    lp_dialog_present(q);
    g_free(body); g_free(title); g_free(pr);
}

static lp_dialog_t *capture_dialog(capture_t *c, const char *title)
{
    lp_dialog_t *d = lp_dialog_new(title, T("Set", "지정"), FALSE, capture_ok, NULL);
    lp_dialog_set_data(d, "lp-capture", c, capture_free);
    if (c->custom_new) {
        c->name_entry = lp_dialog_entry(d, T("Name", "이름"), NULL, FALSE);
        c->cmd_entry = lp_dialog_entry(d, T("Command", "명령"), NULL, FALSE);
    }
    lp_dialog_text(d, T("Press the keys for the shortcut. Escape cancels.",
                        "단축키로 쓸 키를 누르십시오. Esc 는 취소입니다."), NULL);
    c->shown = gtk_label_new(T("Press the new keys now", "새 키를 지금 누르십시오"));
    gtk_widget_add_css_class(c->shown, "lp-big");
    gtk_widget_set_margin_top(c->shown, 8);
    gtk_widget_set_margin_bottom(c->shown, 8);
    gtk_box_append(GTK_BOX(lp_dialog_body(d)), c->shown);
    c->entry = lp_dialog_entry(d, T("Or type it, e.g. <super> KEY_Y", "또는 직접 적기, 예: <super> KEY_Y"),
                               NULL, FALSE);
    g_object_set_data_full(G_OBJECT(c->entry), "lp-title", g_strdup("shortcut"), g_free);
    g_signal_connect(c->entry, "changed", G_CALLBACK(on_combo_text), c);
    GtkEventController *k = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(k, GTK_PHASE_CAPTURE);
    g_signal_connect(k, "key-pressed", G_CALLBACK(on_capture_key), c);
    gtk_widget_add_controller(lp_dialog_window(d), k);
    return d;
}

static void on_shortcut_tapped(GtkWidget *row, gpointer p)
{
    (void)p;
    capture_t *c = g_new0(capture_t, 1);
    c->sec = g_strdup(g_object_get_data(G_OBJECT(row), "lp-sec"));
    c->key = g_strdup(g_object_get_data(G_OBJECT(row), "lp-key"));
    c->name = g_strdup(g_object_get_data(G_OBJECT(row), "lp-title"));
    lp_dialog_t *d = capture_dialog(c, c->name);
    lp_dialog_present(d);
    /* The key controller wants the window focused, not the entry. */
    gtk_widget_grab_focus(lp_dialog_ok_button(d));
}

static void on_add_shortcut(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    capture_t *c = g_new0(capture_t, 1);
    c->custom_new = TRUE;
    lp_dialog_t *d = capture_dialog(c, T("Add a shortcut", "단축키 더하기"));
    lp_dialog_present(d);
}

static void on_remove_custom(GtkButton *b, gpointer p)
{
    (void)p;
    GtkWidget *row = g_object_get_data(G_OBJECT(b), "lp-row");
    const char *key = g_object_get_data(G_OBJECT(row), "lp-key");   /* binding_lp_N */
    char *ini = wayfire_ini();
    char *ck = g_strdup_printf("command_%s", key + 8);
    ini_unset(ini, "command", key);
    ini_unset(ini, "command", ck);
    g_free(ck); g_free(ini);
    sway_keys_write();
    lp_toast(FALSE, T("Removed the shortcut", "단축키를 지웠습니다"));
    row_remove_animated(row);
}

static void fill_shortcuts(void)
{
    if (!KB) return;
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(KB->shortcuts)))
        gtk_list_box_remove(GTK_LIST_BOX(KB->shortcuts), c);

    char *ini = wayfire_ini();
    GPtrArray *all = all_shortcuts();
    for (guint i = 0; i < all->len; i++) {
        sc_t *s = g_ptr_array_index(all, i);
        char *v = binding_of(ini, s->sec, s->key);
        char *pr = pretty(v);
        GtkWidget *r = row_chevron(KB->shortcuts, s->name, NULL, pr, G_CALLBACK(on_shortcut_tapped), NULL);
        gtk_widget_add_css_class(row_control(r), "lp-mono");
        g_object_set_data_full(G_OBJECT(r), "lp-sec", g_strdup(s->sec), g_free);
        g_object_set_data_full(G_OBJECT(r), "lp-key", g_strdup(s->key), g_free);
        if (g_str_has_prefix(s->key, "binding_lp_")) {
            GtkWidget *rm = small_button("list-remove-symbolic", T("Remove", "지우기"),
                                         G_CALLBACK(on_remove_custom), NULL);
            g_object_set_data(G_OBJECT(rm), "lp-row", r);
            gtk_box_append(GTK_BOX(row_box(r)), rm);
        }
        g_free(pr); g_free(v);
    }
    g_ptr_array_free(all, TRUE);
    g_free(ini);
    row_button(KB->shortcuts, T("Your own shortcut", "내 단축키"),
               T("Runs a command when the keys are pressed", "키를 누르면 명령을 실행합니다"),
               T("Add…", "더하기…"), G_CALLBACK(on_add_shortcut), NULL);
}

/* ── building ───────────────────────────────────────────────────────── */

static GtkWidget *build(void)
{
    kbd_t *k = g_new0(kbd_t, 1);
    KB = k;
    GPtrArray *a = sources_get();
    GString *sub = g_string_new(NULL);
    for (guint i = 0; i < a->len; i++) {
        const source_t *s = source_by_id(g_ptr_array_index(a, i));
        if (sub->len) g_string_append(sub, " · ");
        g_string_append(sub, T(s->en, s->ko));
    }
    k->page = page_new(T("Keyboard", "키보드"), sub->str);
    g_string_free(sub, TRUE);
    g_object_set_data_full(G_OBJECT(k->page), "lp-kbd", k, kbd_free);

    k->list = group_new(k->page, T("Input sources", "입력 소스"));
    for (guint i = 0; i < a->len; i++)
        row_add(k->list, source_row(g_ptr_array_index(a, i)));
    k->add_row = row_button(k->list, T("Add an input source", "입력 소스 더하기"), NULL,
                            T("Add…", "더하기…"), G_CALLBACK(on_add), NULL);
    g_ptr_array_free(a, TRUE);
    refresh_positions();

    GtkWidget *g = group_new(k->page, T("Switch 한/영 with", "한/영 바꾸는 키"));
    unsigned sk = switch_keys_get();
    GtkWidget *hk = row_value(g, T("The 한/영 key", "한/영 키"),
                              T("Always: the input method answers it before any window sees it",
                                "항상: 창보다 먼저 입력기가 받습니다"), T("Always", "항상"));
    gtk_widget_add_css_class(row_control(hk), "lp-dim");
    row_switch(g, T("Right Alt on its own", "오른쪽 Alt 단독"),
               T("Tapped and let go; held with another key it is still Alt",
                 "눌렀다 떼면 전환, 다른 키와 함께 누르면 그대로 Alt"),
               (sk & SW_RALT) != 0, G_CALLBACK(on_switch_key), GUINT_TO_POINTER(SW_RALT));
    row_switch(g, "Shift+Space", NULL,
               (sk & SW_SHIFTSPACE) != 0, G_CALLBACK(on_switch_key), GUINT_TO_POINTER(SW_SHIFTSPACE));
    row_switch(g, "Ctrl+Space", T("Also the completion key in code editors",
                                  "코드 편집기의 자동 완성 키와 겹칩니다"),
               (sk & SW_CTRLSPACE) != 0, G_CALLBACK(on_switch_key), GUINT_TO_POINTER(SW_CTRLSPACE));
    char *wfi = wayfire_ini();
    char *sup = binding_of(wfi, "command", "binding_inputlang");
    g_free(wfi);
    char *supp = pretty(sup);
    char *supd = g_strdup_printf(T("%s, under Shortcuts below, where it can be changed",
                                   "%s - 아래 단축키에서 바꿀 수 있습니다"), supp);
    GtkWidget *sr = row_value(g, T("Shortcut", "단축키"), supd, supp);
    gtk_widget_add_css_class(row_control(sr), "lp-mono");
    g_free(supd); g_free(supp); g_free(sup);

    g = group_new(k->page, NULL);
    char *ini = lp_config_path("osk.ini");
    char *pk = ini_get(ini, "keyboard", "physical-korean");
    g_free(ini);
    row_switch(g, T("Korean on the laptop keyboard", "노트북 키보드로 한글 입력"),
               T("Hangul is composed from the physical keys too, not only on screen",
                 "화상 키보드뿐 아니라 실제 키로도 한글을 조합합니다"),
               !pk || strcmp(pk, "false") != 0, G_CALLBACK(on_physical), NULL);
    g_free(pk);

    GtkWidget *rg = group_new(k->page, T("Key repeat", "키 반복"));
    GtkWidget *r = row_scale(rg, T("Repeat delay", "반복 시작 지연"),
                             T("How long a key is held before it repeats", "키를 누르고 있으면 반복이 시작되기까지"),
                             150, 1000, 10, wf_int("input", "kb_repeat_delay", 400),
                             G_CALLBACK(on_repeat), (gpointer)"delay");
    g_object_set_data(G_OBJECT(row_control(r)), "lp-fmt", (gpointer)ms_words);
    LP_QUIET(gtk_range_set_value(GTK_RANGE(row_control(r)), wf_int("input", "kb_repeat_delay", 400) + 1));
    LP_QUIET(gtk_range_set_value(GTK_RANGE(row_control(r)), wf_int("input", "kb_repeat_delay", 400)));
    r = row_scale(rg, T("Repeat speed", "반복 속도"), NULL, 10, 80, 1,
                  wf_int("input", "kb_repeat_rate", 40), G_CALLBACK(on_repeat), (gpointer)"rate");
    g_object_set_data(G_OBJECT(row_control(r)), "lp-fmt", (gpointer)rate_words);
    LP_QUIET(gtk_range_set_value(GTK_RANGE(row_control(r)), wf_int("input", "kb_repeat_rate", 40) + 1));
    LP_QUIET(gtk_range_set_value(GTK_RANGE(row_control(r)), wf_int("input", "kb_repeat_rate", 40)));

    k->shortcuts = group_new(k->page, T("Shortcuts", "단축키"));
    fill_shortcuts();
    page_note(k->page, T("Tap a shortcut to change it. A combination that is already used "
                         "says by what before anything changes.",
                         "단축키를 누르면 바꿀 수 있습니다. 이미 쓰는 조합이면 무엇이 쓰는지 "
                         "먼저 알려 줍니다."));
    return k->page;
}

static const char *const KEYS[] = {
    "Input sources", "입력 소스",
    "Add an input source", "입력 소스 더하기",
    "Switch 한/영 with", "한/영 바꾸는 키",
    "Right Alt on its own", "오른쪽 Alt 단독",
    "Switch the input language (한/영)", "입력 언어 전환 (한/영)",
    "Korean on the laptop keyboard", "노트북 키보드로 한글 입력",
    "Repeat delay", "반복 시작 지연",
    "Repeat speed", "반복 속도",
    "Shortcuts", "단축키",
    "Your own shortcut", "내 단축키",
    "Lock the screen", "화면 잠금",
    "Screenshot", "스크린샷",
    "Hangul", "한/영",
    NULL
};

/* Shortcuts this desktop added after the first images shipped: an older
 * wayfire.ini gets their commands, so the keys listed here work. */
static const struct { const char *name, *cmd; } ADDED_COMMANDS[] = {
    { "inputlang", "lp-input-lang toggle" },
    { "power",     "lp-quick confirm logout" },
};

/* At login: the files are the record; make sway's file, fcitx5's keys
 * and the added commands match them. */
static void keyboard_restore(void)
{
    char *ini = wayfire_ini();
    if (g_file_test(ini, G_FILE_TEST_EXISTS))
        for (guint i = 0; i < G_N_ELEMENTS(ADDED_COMMANDS); i++) {
            char *ck = g_strdup_printf("command_%s", ADDED_COMMANDS[i].name);
            char *bk = g_strdup_printf("binding_%s", ADDED_COMMANDS[i].name);
            char *have = ini_get(ini, "command", ck);
            if (!have) {
                char *b = binding_of(ini, "command", bk);
                ini_set(ini, "command", ck, ADDED_COMMANDS[i].cmd);
                if (b) ini_set(ini, "command", bk, b);
                g_free(b);
            }
            g_free(have); g_free(ck); g_free(bk);
        }
    g_free(ini);
    if (switch_keys_fcitx(switch_keys_get()) && fcitx_running()) {
        const char *fr[] = { "fcitx5-remote", "-r", NULL };
        lp_spawn_bg(fr);
    }
    sway_keys_write();
}

const lp_panel_t lp_panel_keyboard = {
    "keyboard", "Keyboard", "키보드", "input-keyboard-symbolic", build, KEYS, keyboard_restore
};

/* For the Reset panel: everything back to what the desktop ships. */
void lp_keyboard_reset_shortcuts(void)
{
    char *ini = wayfire_ini();
    for (guint i = 0; i < G_N_ELEMENTS(SHORTCUTS); i++)
        ini_set(ini, SHORTCUTS[i].sec, SHORTCUTS[i].key, SHORTCUTS[i].dflt);
    g_free(ini);
    sway_keys_write();
}

void lp_keyboard_reset_input(void)
{
    char *c = input_conf();
    kv_set(c, "sources", "en,ko");
    g_free(c);
    char *ini = lp_config_path("osk.ini");
    ini_set(ini, "keyboard", "layouts", "en;ko");
    ini_unset(ini, "keyboard", "switch-key");
    ini_set(ini, "keyboard", "physical-korean", "true");
    g_free(ini);
    switch_keys_set(SW_DEFAULT);
    char *wf = wayfire_ini();
    ini_unset(wf, "input", "kb_repeat_delay");
    ini_unset(wf, "input", "kb_repeat_rate");
    g_free(wf);
}
