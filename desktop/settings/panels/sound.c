/*
 * sound.c - Sound: which device plays and which one listens, how loud,
 * muted or not, left-right balance, a test, and each application's own
 * volume.
 *
 * ── Through wpctl, WirePlumber's own tool ──
 *
 * The session's audio is PipeWire with WirePlumber as its policy (the
 * image ships both; there is no PulseAudio and so no pactl). wpctl is
 * the command WirePlumber provides and it covers everything here but
 * balance:
 *
 *     wpctl status                         the devices and streams, as a tree
 *     wpctl get-volume @DEFAULT_AUDIO_SINK@ "Volume: 0.40 [MUTED]"
 *     wpctl set-volume <id> 0.55           volume, 1.0 = 100%
 *     wpctl set-mute <id> 1|0
 *     wpctl set-default <id>               the output or input to use
 *
 * WirePlumber remembers the volume and the chosen device itself (in
 * $XDG_STATE_HOME/wireplumber), and puts them back at the next login, so
 * this panel keeps no copy of them - a second record would be a second
 * thing to disagree.
 *
 * ── Balance ──
 *
 * wpctl sets every channel to one value, so balance goes through pw-cli,
 * as per-channel volumes on the output node:
 *
 *     pw-cli set-param <id> Props '{ channelVolumes: [ L, R ] }'
 *
 * Those are linear gains, and wpctl's volume is the cube root of one (the
 * "cubic" scale PulseAudio made familiar), so a volume v with the balance
 * leaning right by b sends L = (v * (1 - b))^3 and R = v^3. While the
 * balance is off centre the volume slider goes the same way, or moving it
 * would put the balance back in the middle. The balance itself is kept in
 * ~/.config/lp/sound.conf (balance=-100..100) so the slider shows it.
 *
 * ── Live ──
 *
 * The sliders apply while the finger moves (ui.c limits them to thirty
 * calls a second) through lp_run_latest, which keeps one wpctl running
 * per slider and only the newest value waiting behind it.
 */
#include "core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int      id;
    char    *name;
    double   vol;
    gboolean muted, is_default;
} node_t;

typedef struct {
    GtkWidget *page;
    GPtrArray *sinks, *sources, *streams;   /* node_t */
    GtkWidget *out_dd, *in_dd;
    GtkWidget *out_vol, *out_mute, *balance;
    GtkWidget *in_vol, *in_mute;
    GtkWidget *apps;
} snd_t;

static snd_t *SN;

static void node_free(gpointer p)
{
    node_t *n = p;
    g_free(n->name);
    g_free(n);
}

static void snd_free(gpointer p)
{
    snd_t *s = p;
    g_ptr_array_free(s->sinks, TRUE);
    g_ptr_array_free(s->sources, TRUE);
    g_ptr_array_free(s->streams, TRUE);
    if (SN == s) SN = NULL;
    g_free(s);
}

/* ── reading `wpctl status` ─────────────────────────────────────────── */

/* The tree is drawn with box characters; what matters is the text after
 * them. Returns a pointer into `line` past the drawing and the spaces. */
static const char *past_tree(const char *line, int *indent)
{
    const char *p = line;
    int n = 0;
    while (*p) {
        if (*p == ' ' || *p == '\t') { p++; n++; continue; }
        /* │ ├ └ ─ are three bytes each in UTF-8, all starting 0xE2 0x94. */
        if ((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x94 && p[2]) {
            p += 3; n++;
            continue;
        }
        break;
    }
    if (indent) *indent = n;
    return p;
}

/* "*   47. Built-in Audio Analog Stereo   [vol: 0.40 MUTED]" */
static node_t *parse_entry(const char *t)
{
    gboolean def = FALSE;
    if (*t == '*') { def = TRUE; t++; }
    while (*t == ' ') t++;
    if (!g_ascii_isdigit(*t)) return NULL;
    char *end = NULL;
    long id = strtol(t, &end, 10);
    if (!end || *end != '.') return NULL;
    t = end + 1;
    while (*t == ' ') t++;
    node_t *n = g_new0(node_t, 1);
    n->id = (int)id;
    n->is_default = def;
    n->vol = -1;
    const char *br = strstr(t, "[vol:");
    n->name = g_strstrip(br ? g_strndup(t, br - t) : g_strdup(t));
    if (br) {
        n->vol = g_ascii_strtod(br + 5, NULL);
        n->muted = strstr(br, "MUTED") != NULL;
    }
    return n;
}

static void parse_status(snd_t *s, const char *text)
{
    g_ptr_array_set_size(s->sinks, 0);
    g_ptr_array_set_size(s->sources, 0);
    g_ptr_array_set_size(s->streams, 0);
    GPtrArray *into = NULL;
    gboolean audio = FALSE;
    int stream_indent = -1;
    char **l = g_strsplit(text, "\n", -1);
    for (int i = 0; l[i]; i++) {
        if (!*l[i]) continue;
        if (l[i][0] != ' ' && (unsigned char)l[i][0] != 0xE2) {
            audio = g_str_has_prefix(l[i], "Audio");
            into = NULL;
            continue;
        }
        if (!audio) continue;
        int ind = 0;
        const char *t = past_tree(l[i], &ind);
        if (!*t) continue;
        if (g_str_has_suffix(t, ":")) {
            into = !strcmp(t, "Sinks:") ? s->sinks : !strcmp(t, "Sources:") ? s->sources
                 : !strcmp(t, "Streams:") ? s->streams : NULL;
            stream_indent = -1;
            continue;
        }
        if (!into) continue;
        /* Under Streams the ports of each stream follow it, indented
         * further and pointing somewhere ("output_FL > Speakers"). */
        if (into == s->streams) {
            if (strstr(t, " > ") || strstr(t, " < ")) continue;
            if (stream_indent >= 0 && ind > stream_indent) continue;
            stream_indent = ind;
        }
        node_t *n = parse_entry(t);
        if (n) g_ptr_array_add(into, n);
    }
    g_strfreev(l);
}

static node_t *default_of(GPtrArray *a)
{
    for (guint i = 0; i < a->len; i++)
        if (((node_t *)g_ptr_array_index(a, i))->is_default)
            return g_ptr_array_index(a, i);
    return a->len ? g_ptr_array_index(a, 0) : NULL;
}

/* ── applying ───────────────────────────────────────────────────────── */

/* The balance while a drag is writing it, before sound.conf has it;
 * 1000 until the slider has been moved. */
static int balance_live = 1000;

static int balance_get(void)
{
    if (balance_live != 1000)
        return CLAMP(balance_live, -100, 100);
    char *c = lp_config_path("sound.conf");
    int b = kv_get_int(c, "balance", 0);
    g_free(c);
    return CLAMP(b, -100, 100);
}

static void send_volume(int id, double v, int bal, const char *key)
{
    char ids[16];
    g_snprintf(ids, sizeof ids, "%d", id);
    if (bal == 0) {
        char vs[32];
        g_ascii_formatd(vs, sizeof vs, "%.2f", v);
        const char *a[] = { "wpctl", "set-volume", ids, vs, NULL };
        lp_run_latest(key, a);
        return;
    }
    double b = bal / 100.0;
    double l = v * (b > 0 ? 1 - b : 1), r = v * (b < 0 ? 1 + b : 1);
    char ls[32], rs[32];
    g_ascii_formatd(ls, sizeof ls, "%.4f", l * l * l);
    g_ascii_formatd(rs, sizeof rs, "%.4f", r * r * r);
    char *props = g_strdup_printf("{ channelVolumes: [ %s, %s ] }", ls, rs);
    const char *a[] = { "pw-cli", "set-param", ids, "Props", props, NULL };
    lp_run_latest(key, a);
    g_free(props);
}

/* "Speakers · 40%" under the title, from the slider and the switch as
 * they are now - it was written once, when the page was built, and still
 * said 100% after the volume had been turned down and the output muted. */
static void out_subtitle(snd_t *s)
{
    node_t *sink = default_of(s->sinks);
    if (!sink || !s->out_vol) return;
    int v = (int)lround(gtk_range_get_value(GTK_RANGE(row_control(s->out_vol))));
    char *sub = sink->muted ? g_strdup_printf(T("%s · muted", "%s · 음소거"), sink->name)
                            : g_strdup_printf(T("%s · %d%%", "%s · %d%%"), sink->name, v);
    page_set_subtitle(s->page, sub);
    g_free(sub);
}

static void on_out_volume(GtkRange *r, gpointer p)
{
    (void)p;
    node_t *n = SN ? default_of(SN->sinks) : NULL;
    if (!n) return;
    send_volume(n->id, gtk_range_get_value(r) / 100.0, balance_get(), "sink-volume");
    out_subtitle(SN);
}

static void on_in_volume(GtkRange *r, gpointer p)
{
    (void)p;
    node_t *n = SN ? default_of(SN->sources) : NULL;
    if (!n) return;
    send_volume(n->id, gtk_range_get_value(r) / 100.0, 0, "source-volume");
}

static void balance_save(gpointer p)
{
    char *c = lp_config_path("sound.conf");
    kv_set(c, "balance", p);
    g_free(c);
}

static void on_balance(GtkRange *r, gpointer p)
{
    (void)p;
    int b = (int)lround(gtk_range_get_value(r));
    /* The sound follows the finger; the file is written once it stops. */
    balance_live = b;
    lp_later("sound-balance", 250, balance_save, g_strdup_printf("%d", b), g_free);
    node_t *n = SN ? default_of(SN->sinks) : NULL;
    if (n && SN->out_vol)
        send_volume(n->id, gtk_range_get_value(GTK_RANGE(row_control(SN->out_vol))) / 100.0,
                    b, "sink-volume");
}

static void on_mute(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps;
    const char *which = p;           /* "out" or "in" */
    if (!SN) return;
    node_t *n = default_of(!strcmp(which, "out") ? SN->sinks : SN->sources);
    if (!n) return;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    char ids[16];
    g_snprintf(ids, sizeof ids, "%d", n->id);
    const char *a[] = { "wpctl", "set-mute", ids, on ? "1" : "0", NULL };
    char *err = NULL, *out = NULL;
    if (lp_run_full(a, NULL, &out, &err) == 0) {
        n->muted = on;
        lp_toast(FALSE, on ? T("Muted %s", "%s 음소거") : T("Unmuted %s", "%s 음소거 해제"), n->name);
        if (!strcmp(which, "out")) out_subtitle(SN);
    } else {
        char *why = lp_first_line(err, out);
        lp_toast(TRUE, T("Could not change mute: %s", "음소거를 바꾸지 못했습니다: %s"), why);
        g_free(why);
        LP_QUIET(gtk_switch_set_active(GTK_SWITCH(sw), !on));
    }
    g_free(err); g_free(out);
}

static void reload(snd_t *s);

static void on_device(GObject *dd, GParamSpec *ps, gpointer p)
{
    (void)ps;
    const char *which = p;
    if (!SN) return;
    GPtrArray *a = !strcmp(which, "out") ? SN->sinks : SN->sources;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (i >= a->len) return;
    node_t *n = g_ptr_array_index(a, i);
    char ids[16];
    g_snprintf(ids, sizeof ids, "%d", n->id);
    const char *v[] = { "wpctl", "set-default", ids, NULL };
    char *err = NULL, *out = NULL;
    if (lp_run_full(v, NULL, &out, &err) == 0)
        lp_toast(FALSE, !strcmp(which, "out") ? T("Sound now plays through %s", "이제 %s 에서 소리가 납니다")
                                             : T("Now listening with %s", "이제 %s 로 듣습니다"), n->name);
    else {
        char *why = lp_first_line(err, out);
        lp_toast(TRUE, T("Could not switch the device: %s", "장치를 바꾸지 못했습니다: %s"), why);
        g_free(why);
    }
    g_free(err); g_free(out);
    reload(SN);
}

static void on_stream_volume(GtkRange *r, gpointer p)
{
    int id = GPOINTER_TO_INT(p);
    char ids[16], vs[32], key[32];
    g_snprintf(ids, sizeof ids, "%d", id);
    g_snprintf(key, sizeof key, "stream-%d", id);
    g_ascii_formatd(vs, sizeof vs, "%.2f", gtk_range_get_value(r) / 100.0);
    const char *a[] = { "wpctl", "set-volume", ids, vs, NULL };
    lp_run_latest(key, a);
}

/* Left, then right: the test says both that sound comes out and that it
 * comes out of the side the balance says. Each sound is a second long;
 * pw-play into an output that never takes the samples (a virtual
 * machine's "Dummy Output") never returns, so each gets ten seconds. */
#define TEST_MS (10 * 1000)

static void on_test_done(int st, const char *out, const char *err, gpointer p)
{
    (void)p;
    if (st == 0) return;
    char *why = lp_first_line(err, out);
    lp_toast(TRUE, st == LP_RUN_TIMEOUT
                   ? T("The test sound did not finish - the output device is not playing: %s",
                       "시험 소리가 끝나지 않았습니다 - 출력 장치가 소리를 내지 않습니다: %s")
                   : T("Could not play the test sound: %s", "시험 소리를 내지 못했습니다: %s"),
             why);
    g_free(why);
}

static void on_test_right(int st, const char *out, const char *err, gpointer p)
{
    if (st != 0) {
        on_test_done(st, out, err, p);
        return;
    }
    static const char *const v[] = { "pw-play",
        "/usr/share/sounds/freedesktop/stereo/audio-channel-front-right.oga", NULL };
    lp_run_async_timeout(v, NULL, TEST_MS, NULL, on_test_done, NULL);
}

static void on_test(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    lp_toast(FALSE, T("Playing: left, then right", "재생 중: 왼쪽, 그다음 오른쪽"));
    static const char *const v[] = { "pw-play",
        "/usr/share/sounds/freedesktop/stereo/audio-channel-front-left.oga", NULL };
    lp_run_async_timeout(v, NULL, TEST_MS, NULL, on_test_right, NULL);
}

/* ── building ───────────────────────────────────────────────────────── */

static char *pct(double v) { return g_strdup_printf("%d%%", (int)lround(v)); }

static char *balance_words(double v)
{
    int b = (int)lround(v);
    if (b == 0) return g_strdup(T("Centre", "가운데"));
    return g_strdup_printf(b < 0 ? T("L %d", "왼쪽 %d") : T("R %d", "오른쪽 %d"), abs(b));
}

static GtkWidget *device_row(GtkWidget *list, const char *title, GPtrArray *a, const char *which)
{
    if (!a->len) {
        return row_value(list, title, NULL, T("None found", "찾지 못함"));
    }
    GPtrArray *names = g_ptr_array_new();
    guint sel = 0;
    for (guint i = 0; i < a->len; i++) {
        node_t *n = g_ptr_array_index(a, i);
        g_ptr_array_add(names, n->name);
        if (n->is_default) sel = i;
    }
    g_ptr_array_add(names, NULL);
    GtkWidget *r = row_choice(list, title, NULL, (const char *const *)names->pdata, sel,
                              G_CALLBACK(on_device), (gpointer)which);
    g_ptr_array_free(names, TRUE);
    return r;
}

static void clear(GtkWidget *list)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(list)))
        gtk_list_box_remove(GTK_LIST_BOX(list), c);
}

static void on_status(int st, const char *out, const char *err, gpointer p)
{
    snd_t *s = p;
    GtkWidget *og = g_object_get_data(G_OBJECT(s->page), "lp-out");
    GtkWidget *ig = g_object_get_data(G_OBJECT(s->page), "lp-in");
    clear(og); clear(ig); clear(s->apps);
    if (st != 0) {
        char *why = st == -1 ? g_strdup(T("wpctl is not installed (package wireplumber)",
                                          "wpctl 이 설치되어 있지 않습니다 (wireplumber 패키지)"))
                             : lp_first_line(err, out);
        page_set_subtitle(s->page, T("The sound service is not answering",
                                     "소리 서비스가 답하지 않습니다"));
        row_value(og, T("Sound is not available", "소리를 쓸 수 없습니다"), why, NULL);
        g_free(why);
        s->out_vol = s->in_vol = NULL;
        return;
    }
    parse_status(s, out);
    node_t *sink = default_of(s->sinks), *src = default_of(s->sources);

    device_row(og, T("Output device", "출력 장치"), s->sinks, "out");
    double v = sink && sink->vol >= 0 ? sink->vol * 100 : 0;
    s->out_vol = row_scale(og, T("Volume", "볼륨"), NULL, 0, 100, 1, MIN(v, 100),
                           G_CALLBACK(on_out_volume), NULL);
    g_object_set_data(G_OBJECT(row_control(s->out_vol)), "lp-fmt", (gpointer)pct);
    LP_QUIET(gtk_range_set_value(GTK_RANGE(row_control(s->out_vol)), MIN(v, 100)));
    s->out_mute = row_switch(og, T("Mute output", "출력 음소거"), NULL, sink && sink->muted,
                             G_CALLBACK(on_mute), (gpointer)"out");
    s->balance = row_scale(og, T("Balance", "좌우 균형"), T("Left – right", "왼쪽 – 오른쪽"),
                           -100, 100, 1, balance_get(), G_CALLBACK(on_balance), NULL);
    g_object_set_data(G_OBJECT(row_control(s->balance)), "lp-fmt", (gpointer)balance_words);
    gtk_scale_add_mark(GTK_SCALE(row_control(s->balance)), 0, GTK_POS_BOTTOM, NULL);
    LP_QUIET(gtk_range_set_value(GTK_RANGE(row_control(s->balance)), balance_get()));
    row_button(og, T("Test", "시험"), T("Plays a sound on the left, then on the right",
                                        "왼쪽, 오른쪽 순서로 소리를 냅니다"),
               T("Play", "재생"), G_CALLBACK(on_test), NULL);
    if (!sink) {
        gtk_widget_set_sensitive(row_control(s->out_vol), FALSE);
        gtk_widget_set_sensitive(row_control(s->out_mute), FALSE);
        gtk_widget_set_sensitive(row_control(s->balance), FALSE);
    }

    device_row(ig, T("Input device", "입력 장치"), s->sources, "in");
    double iv = src && src->vol >= 0 ? src->vol * 100 : 0;
    s->in_vol = row_scale(ig, T("Input volume", "입력 볼륨"), NULL, 0, 100, 1, MIN(iv, 100),
                          G_CALLBACK(on_in_volume), NULL);
    g_object_set_data(G_OBJECT(row_control(s->in_vol)), "lp-fmt", (gpointer)pct);
    LP_QUIET(gtk_range_set_value(GTK_RANGE(row_control(s->in_vol)), MIN(iv, 100)));
    s->in_mute = row_switch(ig, T("Mute input", "입력 음소거"), NULL, src && src->muted,
                            G_CALLBACK(on_mute), (gpointer)"in");
    if (!src) {
        gtk_widget_set_sensitive(row_control(s->in_vol), FALSE);
        gtk_widget_set_sensitive(row_control(s->in_mute), FALSE);
    }

    for (guint i = 0; i < s->streams->len; i++) {
        node_t *n = g_ptr_array_index(s->streams, i);
        double sv = n->vol >= 0 ? n->vol * 100 : 100;
        GtkWidget *r = row_scale(s->apps, n->name, NULL, 0, 100, 1, MIN(sv, 100),
                                 G_CALLBACK(on_stream_volume), GINT_TO_POINTER(n->id));
        g_object_set_data(G_OBJECT(row_control(r)), "lp-fmt", (gpointer)pct);
        LP_QUIET(gtk_range_set_value(GTK_RANGE(row_control(r)), MIN(sv, 100)));
    }
    if (!s->streams->len)
        row_value(s->apps, T("No application is playing sound", "소리를 내는 앱이 없습니다"),
                  NULL, NULL);

    if (sink)
        out_subtitle(s);
    else
        page_set_subtitle(s->page, T("No output device", "출력 장치 없음"));
}

static void reload(snd_t *s)
{
    static const char *const v[] = { "wpctl", "status", NULL };
    lp_run_async(v, NULL, s->page, on_status, s);
}

static GtkWidget *build(void)
{
    snd_t *s = g_new0(snd_t, 1);
    SN = s;
    s->sinks = g_ptr_array_new_with_free_func(node_free);
    s->sources = g_ptr_array_new_with_free_func(node_free);
    s->streams = g_ptr_array_new_with_free_func(node_free);
    s->page = page_new(T("Sound", "소리"), T("Reading the sound devices…", "소리 장치를 읽는 중…"));
    g_object_set_data_full(G_OBJECT(s->page), "lp-sound", s, snd_free);
    g_object_set_data(G_OBJECT(s->page), "lp-out", group_new(s->page, T("Output", "출력")));
    g_object_set_data(G_OBJECT(s->page), "lp-in", group_new(s->page, T("Input", "입력")));
    s->apps = group_new(s->page, T("Applications", "앱별 볼륨"));
    reload(s);
    return s->page;
}

static const char *const KEYS[] = {
    "Output device", "출력 장치",
    "Volume", "볼륨",
    "Mute output", "출력 음소거",
    "Balance", "좌우 균형",
    "Test", "시험",
    "Input device", "입력 장치",
    "Input volume", "입력 볼륨",
    "Microphone", "마이크",
    "Speakers", "스피커",
    NULL
};

const lp_panel_t lp_panel_sound = {
    "sound", "Sound", "소리", "audio-volume-high-symbolic", build, KEYS, NULL
};

/* For the Reset panel (reset-and-factory-reset.md 2-4): volumes and the
 * balance back, the chosen devices left alone - put back, sound would
 * start coming out of a different device, and the volume keys would go
 * on turning up the one that is silent. */
gboolean lp_sound_reset(char **why)
{
    char *c = lp_config_path("sound.conf");
    kv_set(c, "balance", "0");
    g_free(c);
    balance_live = 1000;
    char *err = NULL, *o = NULL;
    int st = 0;
    /* By id, the same device the page shows (default_of): "@DEFAULT_AUDIO_
     * SINK@" is an error when WirePlumber has not marked a default - a
     * virtual machine's lone Dummy Output - and so is the source on a
     * machine with no microphone ("'-1' is not a valid ID"), which made
     * the whole reset report a failure. */
    static const char *const sv[] = { "wpctl", "status", NULL };
    char *status = lp_run(sv);
    snd_t tmp = { 0 };
    tmp.sinks = g_ptr_array_new_with_free_func(node_free);
    tmp.sources = g_ptr_array_new_with_free_func(node_free);
    tmp.streams = g_ptr_array_new_with_free_func(node_free);
    if (status) parse_status(&tmp, status);
    node_t *targets[] = { default_of(tmp.sinks), default_of(tmp.sources) };
    const char *levels[] = { "0.40", "1.00" };
    for (int k = 0; k < 2 && st == 0; k++) {
        if (!targets[k]) continue;
        char ids[16];
        g_snprintf(ids, sizeof ids, "%d", targets[k]->id);
        const char *a[] = { "wpctl", "set-volume", ids, levels[k], NULL };
        g_free(err); g_free(o);
        err = o = NULL;
        st = lp_run_full(a, NULL, &o, &err);
    }
    if (!status) {
        st = 1;
        g_free(err);
        err = g_strdup(T("The sound service is not answering", "소리 서비스가 답하지 않습니다"));
    }
    if (st != 0 && why) *why = lp_first_line(err, o);
    /* Every app's own volume, for the ones playing now. */
    if (status) {
        for (guint i = 0; i < tmp.streams->len; i++) {
            char ids[16];
            g_snprintf(ids, sizeof ids, "%d", ((node_t *)g_ptr_array_index(tmp.streams, i))->id);
            const char *a[] = { "wpctl", "set-volume", ids, "1.00", NULL };
            char *x = lp_run(a);
            g_free(x);
        }
        g_free(status);
    }
    g_ptr_array_free(tmp.sinks, TRUE);
    g_ptr_array_free(tmp.sources, TRUE);
    g_ptr_array_free(tmp.streams, TRUE);
    g_free(err); g_free(o);
    return st == 0;
}
