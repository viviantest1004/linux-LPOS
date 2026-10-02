/*
 * bluetooth.c - Bluetooth: the adapter on or off, being visible, finding
 * devices, pairing, connecting, disconnecting, forgetting, and the battery
 * level of the devices that report one.
 *
 * ── Why bluetoothctl and not the org.bluez D-Bus API ──
 *
 * The D-Bus API is what GNOME uses, and it is the better interface in the
 * abstract: property-change signals instead of polling, no text to parse.
 * It loses here on three counts.
 *
 *   - Pairing needs an agent (org.bluez.Agent1) to answer "confirm this
 *     passkey". bluetoothctl registers its own default agent, so `pair`
 *     works for the Just Works and confirm-only devices - headphones,
 *     mice, most keyboards made this decade - without this app exporting
 *     an object on the system bus as the desktop user.
 *   - Every other track's contract is a command (lp-net, lp-tune), and
 *     the verification of this app is a log of the commands it ran. A
 *     fake bluetoothctl is a twenty-line script; a fake bluetoothd is a
 *     D-Bus service the base has nothing to write in.
 *   - bluetoothctl comes with the bluez package that is being added, so
 *     it costs nothing.
 *
 * The price is that the screen is not live: it re-reads after each action
 * and while a search is running, rather than being told. For a settings
 * screen that people open, use and close, that is the right trade.
 *
 * ── bluetoothctl can wait forever ──
 *
 * Without bluetoothd running it prints "Waiting to connect to bluetoothd"
 * and never exits, and the page said "Reading the adapter…" for as long
 * as it was open. So the adapter is looked for in /sys/class/bluetooth
 * first - no hci device, no question to ask - and every bluetoothctl runs
 * with a limit (lp_run_async_timeout): a few seconds for what answers at
 * once, longer for pairing, which waits for a passkey to be typed.
 *
 * Output is parsed with LC_ALL=C (sys.c sets it for every child) and only
 * from the tab-indented "Key: value" lines of `show` and `info` and the
 * "Device <mac> <name>" lines of `devices`, which have kept their shape
 * across BlueZ 5.x; colour codes are stripped first because bluetoothctl
 * prints them even into a pipe.
 */
#include "core.h"

#include <stdlib.h>
#include <string.h>

#define BT_QUICK_MS  (8 * 1000)     /* show, info, devices, power, trust… */
#define BT_PAIR_MS   (90 * 1000)    /* pair and connect: a passkey is typed */
#define BT_SCAN_MS   (25 * 1000)    /* `--timeout 10 scan on` */

typedef struct {
    char *mac, *name, *icon;
    gboolean paired, connected, trusted;
    int battery;                 /* -1: the device does not report one */
} bdev_t;

typedef struct {
    GtkWidget *page;
    GtkWidget *power_sw, *vis_sw;
    GtkWidget *mine, *others;
    GtkWidget *search_btn;
    GPtrArray *devs;             /* bdev_t */
    GPtrArray *pending;          /* MACs still to ask `info` about */
    gboolean   searching;
    gboolean   powered;
} bt_t;

static bt_t *B;

static void bdev_free(gpointer p)
{
    bdev_t *d = p;
    g_free(d->mac); g_free(d->name); g_free(d->icon);
    g_free(d);
}

static void bt_free(gpointer p)
{
    bt_t *b = p;
    if (b->devs) g_ptr_array_free(b->devs, TRUE);
    if (b->pending) g_ptr_array_free(b->pending, TRUE);
    if (B == b) B = NULL;
    g_free(b);
}

/* bluetoothctl colours its output even when it is not a terminal. */
static char *strip_ansi(const char *s)
{
    GString *o = g_string_new(NULL);
    for (const char *p = s; *p; p++) {
        if (*p == '\x1b') {
            while (*p && !g_ascii_isalpha(*p)) p++;
            if (!*p) break;
            continue;
        }
        if (*p == '\001' || *p == '\002') continue;
        g_string_append_c(o, *p);
    }
    return g_string_free(o, FALSE);
}

static char *field(const char *text, const char *key)
{
    char **l = g_strsplit(text, "\n", -1);
    char *r = NULL;
    size_t kl = strlen(key);
    for (int i = 0; l[i] && !r; i++) {
        const char *t = l[i];
        while (*t == '\t' || *t == ' ') t++;
        if (!strncmp(t, key, kl) && t[kl] == ':')
            r = g_strstrip(g_strdup(t + kl + 1));
    }
    g_strfreev(l);
    return r;
}

static gboolean field_yes(const char *text, const char *key)
{
    char *v = field(text, key);
    gboolean y = v && !strcmp(v, "yes");
    g_free(v);
    return y;
}

static const char *device_icon(const char *icon)
{
    if (!icon) return "bluetooth-symbolic";
    if (g_str_has_prefix(icon, "audio-headset") || g_str_has_prefix(icon, "audio-headphones"))
        return "audio-headphones-symbolic";
    if (g_str_has_prefix(icon, "audio")) return "audio-speakers-symbolic";
    if (g_str_has_prefix(icon, "input-mouse")) return "input-mouse-symbolic";
    if (g_str_has_prefix(icon, "input-keyboard")) return "input-keyboard-symbolic";
    if (g_str_has_prefix(icon, "input-gaming")) return "input-gaming-symbolic";
    if (g_str_has_prefix(icon, "input-tablet")) return "input-tablet-symbolic";
    if (g_str_has_prefix(icon, "phone")) return "phone-symbolic";
    if (g_str_has_prefix(icon, "computer")) return "computer-symbolic";
    return "bluetooth-symbolic";
}

static void clear(GtkWidget *list)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(list)))
        gtk_list_box_remove(GTK_LIST_BOX(list), c);
}

/* ── actions ────────────────────────────────────────────────────────── */

static void reload(bt_t *b);

typedef struct { char *mac; char *name; const char *verb; } act_t;

static void act_free(act_t *a)
{
    g_free(a->mac); g_free(a->name);
    g_free(a);
}

static void on_acted(int st, const char *out, const char *err, gpointer p)
{
    act_t *a = p;
    char *clean = strip_ansi(out);
    /* bluetoothctl exits 0 even when the device said no; the verdict is
     * in the text ("Failed to connect: org.bluez.Error..."). */
    gboolean failed = st != 0 || strstr(clean, "Failed") || strstr(clean, "not available");
    if (!failed) {
        const char *done = !strcmp(a->verb, "connect") ? T("Connected to %s", "%s 에 연결했습니다")
                         : !strcmp(a->verb, "disconnect") ? T("Disconnected %s", "%s 연결을 끊었습니다")
                         : !strcmp(a->verb, "remove") ? T("Forgot %s", "%s 을(를) 지웠습니다")
                         : T("Paired with %s", "%s 와(과) 연결을 맺었습니다");
        lp_toast(FALSE, done, a->name);
    } else {
        char *why = lp_first_line(err, clean);
        lp_toast(TRUE, T("%s: %s", "%s: %s"), a->name, why);
        g_free(why);
    }
    g_free(clean);
    if (B) reload(B);
    act_free(a);
}

static void run_verb(const char *verb, const char *mac, const char *name)
{
    act_t *a = g_new0(act_t, 1);
    a->mac = g_strdup(mac);
    a->name = g_strdup(name);
    a->verb = verb;
    const char *v[] = { "bluetoothctl", verb, mac, NULL };
    lp_run_async_timeout(v, NULL, !strcmp(verb, "connect") ? BT_PAIR_MS : BT_QUICK_MS,
                         NULL, on_acted, a);
}

/* Pairing is three steps: pair, trust (so the device may reconnect by
 * itself tomorrow without this screen), connect. Chained, each waiting for
 * the one before, because a connect sent before the pairing has finished
 * is refused and reports as a failure of the whole thing. */
static void on_paired(int st, const char *out, const char *err, gpointer p);
static void on_trusted(int st, const char *out, const char *err, gpointer p)
{
    (void)st; (void)out; (void)err;
    act_t *a = p;
    a->verb = "connect";
    const char *v[] = { "bluetoothctl", "connect", a->mac, NULL };
    lp_run_async_timeout(v, NULL, BT_PAIR_MS, NULL, on_acted, a);
}

static void on_paired(int st, const char *out, const char *err, gpointer p)
{
    act_t *a = p;
    char *clean = strip_ansi(out);
    if (st != 0 || strstr(clean, "Failed")) {
        g_free(clean);
        a->verb = "pair";
        on_acted(1, out, err, a);
        return;
    }
    /* A keyboard answers pairing with a passkey to type on it; the text is
     * bluetoothctl's agent speaking, and the person needs to see it. */
    char *pk = strstr(clean, "Passkey:");
    if (pk) {
        char *line = g_strndup(pk, strcspn(pk, "\n"));
        lp_toast(FALSE, T("Type this on the keyboard, then Enter: %s",
                          "키보드에서 이것을 입력하고 Enter: %s"), line);
        g_free(line);
    }
    g_free(clean);
    const char *v[] = { "bluetoothctl", "trust", a->mac, NULL };
    lp_run_async_timeout(v, NULL, BT_QUICK_MS, NULL, on_trusted, a);
}

static void on_device_tapped(GtkWidget *row, gpointer p)
{
    (void)p;
    bdev_t *d = g_object_get_data(G_OBJECT(row), "lp-dev");
    if (!d) return;
    row_set_detail(row, T("Working…", "처리하는 중…"));
    if (!d->paired) {
        act_t *a = g_new0(act_t, 1);
        a->mac = g_strdup(d->mac);
        a->name = g_strdup(d->name);
        const char *v[] = { "bluetoothctl", "pair", d->mac, NULL };
        lp_run_async_timeout(v, NULL, BT_PAIR_MS, NULL, on_paired, a);
    } else {
        run_verb(d->connected ? "disconnect" : "connect", d->mac, d->name);
    }
}

static void forget_ok(lp_dialog_t *dl, gpointer p)
{
    (void)p;
    run_verb("remove", lp_dialog_get_data(dl, "mac"), lp_dialog_get_data(dl, "name"));
    lp_dialog_close(dl);
}

static void on_device_more(GtkButton *btn, gpointer p)
{
    (void)p;
    bdev_t *d = g_object_get_data(G_OBJECT(btn), "lp-dev");
    lp_dialog_t *dl = lp_dialog_new(d->name, T("Forget", "지우기"), TRUE, forget_ok, NULL);
    lp_dialog_set_data(dl, "mac", g_strdup(d->mac), g_free);
    lp_dialog_set_data(dl, "name", g_strdup(d->name), g_free);
    char *t = g_strdup_printf("%s\n%s", d->mac,
                              d->connected ? T("Connected", "연결됨") : T("Not connected", "연결 안 됨"));
    lp_dialog_text(dl, t, NULL);
    g_free(t);
    if (d->battery >= 0) {
        char *bt = g_strdup_printf(T("Battery %d%%", "배터리 %d%%"), d->battery);
        lp_dialog_text(dl, bt, NULL);
        g_free(bt);
    }
    lp_dialog_text(dl, T("Forgetting removes the pairing. To use it again it has to be paired "
                         "again, which usually means putting the device in pairing mode.",
                         "지우면 연결 정보가 사라집니다. 다시 쓰려면 기기를 페어링 모드로 두고 "
                         "다시 연결해야 합니다."), "lp-note");
    lp_dialog_present(dl);
}

/* ── drawing ────────────────────────────────────────────────────────── */

static void draw(bt_t *b)
{
    clear(b->mine);
    clear(b->others);
    int mine = 0, others = 0;
    for (guint i = 0; i < b->devs->len; i++) {
        bdev_t *d = g_ptr_array_index(b->devs, i);
        char *detail;
        if (d->paired) {
            detail = d->connected
                ? (d->battery >= 0 ? g_strdup_printf(T("Connected · battery %d%%", "연결됨 · 배터리 %d%%"), d->battery)
                                   : g_strdup(T("Connected", "연결됨")))
                : g_strdup(T("Not connected - tap to connect", "연결 안 됨 - 누르면 연결합니다"));
        } else {
            detail = g_strdup(T("Tap to pair", "누르면 연결을 맺습니다"));
        }
        GtkWidget *row = row_shell(d->name, detail);
        g_free(detail);
        GtkWidget *icon = gtk_image_new_from_icon_name(device_icon(d->icon));
        gtk_box_prepend(GTK_BOX(row_box(row)), icon);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        g_object_set_data(G_OBJECT(row), "lp-activate", (gpointer)on_device_tapped);
        g_object_set_data(G_OBJECT(row), "lp-dev", d);
        if (d->paired) {
            GtkWidget *more = gtk_button_new_from_icon_name("view-more-symbolic");
            gtk_widget_set_tooltip_text(more, T("Details", "자세히"));
            gtk_widget_set_valign(more, GTK_ALIGN_CENTER);
            g_object_set_data(G_OBJECT(more), "lp-dev", d);
            g_object_set_data_full(G_OBJECT(more), "lp-title", g_strdup_printf("more:%s", d->name), g_free);
            g_signal_connect(more, "clicked", G_CALLBACK(on_device_more), NULL);
            gtk_box_append(GTK_BOX(row_box(row)), more);
            row_add(b->mine, row);
            mine++;
        } else {
            row_add(b->others, row);
            others++;
        }
    }
    if (!mine)
        row_value(b->mine, T("No paired devices", "연결을 맺은 기기가 없습니다"), NULL, NULL);
    if (!others)
        row_value(b->others, b->searching ? T("Searching…", "찾는 중…")
                                          : T("Nothing new nearby", "주변에 새 기기가 없습니다"),
                  b->searching ? NULL
                               : T("Put the device in pairing mode, then search.",
                                   "기기를 페어링 모드로 두고 찾으십시오."), NULL);
}

static void next_info(bt_t *b);

static void on_info(int st, const char *out, const char *err, gpointer p)
{
    (void)err;
    bt_t *b = p;
    char *mac = g_ptr_array_steal_index(b->pending, 0);
    for (guint i = 0; i < b->devs->len; i++) {
        bdev_t *x = g_ptr_array_index(b->devs, i);
        if (strcmp(x->mac, mac)) continue;
        if (st == 0) {
            char *t = strip_ansi(out);
            char *alias = field(t, "Alias");
            if (alias && *alias) { g_free(x->name); x->name = alias; } else g_free(alias);
            x->icon = field(t, "Icon");
            x->paired = field_yes(t, "Paired");
            x->connected = field_yes(t, "Connected");
            x->trusted = field_yes(t, "Trusted");
            char *bat = field(t, "Battery Percentage");
            /* "0x55 (85)" - the decimal in brackets. */
            if (bat) {
                char *br = strchr(bat, '(');
                x->battery = br ? atoi(br + 1) : (int)g_ascii_strtoll(bat, NULL, 0);
            }
            g_free(bat);
            g_free(t);
        }
    }
    g_free(mac);
    next_info(b);
}

static void next_info(bt_t *b)
{
    if (!b->pending->len) {
        draw(b);
        return;
    }
    const char *v[] = { "bluetoothctl", "info", g_ptr_array_index(b->pending, 0), NULL };
    lp_run_async_timeout(v, NULL, BT_QUICK_MS, b->page, on_info, b);
}

static void on_devices(int st, const char *out, const char *err, gpointer p)
{
    (void)err;
    bt_t *b = p;
    g_ptr_array_set_size(b->devs, 0);
    g_ptr_array_set_size(b->pending, 0);
    if (st == 0) {
        char *t = strip_ansi(out);
        char **l = g_strsplit(t, "\n", -1);
        for (int i = 0; l[i]; i++) {
            char *line = g_strstrip(l[i]);
            if (!g_str_has_prefix(line, "Device ")) continue;
            char **f = g_strsplit(line + 7, " ", 2);
            if (f[0] && strlen(f[0]) == 17) {
                bdev_t *d = g_new0(bdev_t, 1);
                d->mac = g_strdup(f[0]);
                d->name = g_strdup(f[1] && *f[1] ? f[1] : f[0]);
                d->battery = -1;
                g_ptr_array_add(b->devs, d);
                g_ptr_array_add(b->pending, g_strdup(d->mac));
            }
            g_strfreev(f);
        }
        g_strfreev(l);
        g_free(t);
    }
    next_info(b);
}

static void reload_devices(bt_t *b)
{
    static const char *const v[] = { "bluetoothctl", "devices", NULL };
    lp_run_async_timeout(v, NULL, BT_QUICK_MS, b->page, on_devices, b);
}

static void show_unavailable(bt_t *b, const char *why)
{
    page_set_subtitle(b->page, T("No Bluetooth adapter is available", "쓸 수 있는 블루투스 장치가 없습니다"));
    gtk_widget_set_sensitive(b->power_sw, FALSE);
    gtk_widget_set_sensitive(b->vis_sw, FALSE);
    gtk_widget_set_sensitive(b->search_btn, FALSE);
    clear(b->mine);
    clear(b->others);
    row_value(b->mine, T("Bluetooth is not available", "블루투스를 쓸 수 없습니다"), why, NULL);
}

/* Is there an adapter for bluetoothd to drive at all? The kernel lists
 * each one as /sys/class/bluetooth/hciN. */
static gboolean have_adapter(void)
{
    GDir *d = g_dir_open("/sys/class/bluetooth", 0, NULL);
    gboolean yes = FALSE;
    const char *n;
    while (d && !yes && (n = g_dir_read_name(d)))
        yes = g_str_has_prefix(n, "hci") && !strchr(n, ':');
    if (d) g_dir_close(d);
    return yes;
}

static void on_show(int st, const char *out, const char *err, gpointer p)
{
    bt_t *b = p;
    char *t = strip_ansi(out);
    if (st != 0 || !strstr(t, "Controller")) {
        char *why = st == -1 ? g_strdup(T("bluetoothctl is not installed (package bluez)",
                                          "bluetoothctl 이 설치되어 있지 않습니다 (bluez 패키지)"))
                  : st == LP_RUN_TIMEOUT
                  ? g_strdup(T("The Bluetooth service (bluetoothd) is not answering - it may not be running",
                               "블루투스 서비스(bluetoothd)가 답하지 않습니다 - 돌고 있지 않을 수 있습니다"))
                  : lp_first_line(err, t);
        show_unavailable(b, why);
        g_free(why);
        g_free(t);
        return;
    }
    b->powered = field_yes(t, "Powered");
    gboolean vis = field_yes(t, "Discoverable");
    char *name = field(t, "Alias");
    if (!name) name = field(t, "Name");
    LP_QUIET(gtk_switch_set_active(GTK_SWITCH(b->power_sw), b->powered));
    LP_QUIET(gtk_switch_set_active(GTK_SWITCH(b->vis_sw), vis));
    gtk_widget_set_sensitive(b->vis_sw, b->powered);
    gtk_widget_set_sensitive(b->search_btn, b->powered && !b->searching);
    GtkWidget *vrow = g_object_get_data(G_OBJECT(b->vis_sw), "lp-row");
    char *d = g_strdup_printf(T("Other devices see this computer as “%s”", "다른 기기에 “%s” 로 보입니다"),
                              name ? name : "?");
    row_set_detail(vrow, d);
    g_free(d);
    page_set_subtitle(b->page, b->powered ? T("On", "켜짐") : T("Off", "꺼짐"));
    g_free(name);
    g_free(t);
    if (b->powered)
        reload_devices(b);
    else {
        clear(b->mine);
        clear(b->others);
        row_value(b->mine, T("Bluetooth is off", "블루투스가 꺼져 있습니다"), NULL, NULL);
    }
}

static void reload(bt_t *b)
{
    if (!have_adapter()) {
        show_unavailable(b, T("This machine has no Bluetooth adapter, or its driver is not loaded",
                              "이 기계에는 블루투스 장치가 없거나 드라이버가 올라와 있지 않습니다"));
        return;
    }
    static const char *const v[] = { "bluetoothctl", "show", NULL };
    lp_run_async_timeout(v, NULL, BT_QUICK_MS, b->page, on_show, b);
}

/* ── switches and search ────────────────────────────────────────────── */

static void on_toggled(int st, const char *out, const char *err, gpointer p)
{
    char *what = p;
    char *clean = strip_ansi(out);
    if (st != 0 || strstr(clean, "Failed")) {
        char *why = lp_first_line(err, clean);
        lp_toast(TRUE, "%s: %s", what, why);
        g_free(why);
    } else {
        lp_toast(FALSE, "%s", what);
    }
    g_free(clean);
    g_free(what);
    if (B) reload(B);
}

static void on_power(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    const char *v[] = { "bluetoothctl", "power", on ? "on" : "off", NULL };
    lp_run_async_timeout(v, NULL, BT_QUICK_MS, NULL, on_toggled,
                 g_strdup(on ? T("Bluetooth is on", "블루투스를 켰습니다")
                             : T("Bluetooth is off", "블루투스를 껐습니다")));
}

static void on_visible(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    const char *v[] = { "bluetoothctl", "discoverable", on ? "on" : "off", NULL };
    lp_run_async_timeout(v, NULL, BT_QUICK_MS, NULL, on_toggled,
                 g_strdup(on ? T("This computer is visible to other devices",
                                 "다른 기기에 이 컴퓨터가 보입니다")
                             : T("This computer is hidden", "이 컴퓨터를 숨겼습니다")));
}

static void on_searched(int st, const char *out, const char *err, gpointer p)
{
    (void)st; (void)out; (void)err;
    bt_t *b = p;
    b->searching = FALSE;
    gtk_button_set_label(GTK_BUTTON(b->search_btn), T("Search for devices", "기기 찾기"));
    gtk_widget_set_sensitive(b->search_btn, TRUE);
    reload(b);
}

static gboolean search_tick(gpointer p)
{
    bt_t *b = B;
    (void)p;
    if (!b || !b->searching) return G_SOURCE_REMOVE;
    reload_devices(b);
    return G_SOURCE_CONTINUE;
}

static void on_search(GtkButton *btn, gpointer p)
{
    (void)p;
    bt_t *b = B;
    if (!b || b->searching) return;
    b->searching = TRUE;
    gtk_button_set_label(btn, T("Searching…", "찾는 중…"));
    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
    draw(b);
    /* Ten seconds of discovery; the list is re-read every three seconds
     * meanwhile so devices appear as they are found. */
    static const char *const v[] = { "bluetoothctl", "--timeout", "10", "scan", "on", NULL };
    lp_run_async_timeout(v, NULL, BT_SCAN_MS, b->page, on_searched, b);
    g_timeout_add(3000, search_tick, NULL);
}

static GtkWidget *build(void)
{
    bt_t *b = g_new0(bt_t, 1);
    B = b;
    b->devs = g_ptr_array_new_with_free_func(bdev_free);
    b->pending = g_ptr_array_new_with_free_func(g_free);
    b->page = page_new("Bluetooth", T("Reading the adapter…", "블루투스 장치를 읽는 중…"));
    g_object_set_data_full(G_OBJECT(b->page), "lp-bt", b, bt_free);

    GtkWidget *g = group_new(b->page, NULL);
    b->power_sw = row_control(row_switch(g, "Bluetooth", NULL, FALSE, G_CALLBACK(on_power), NULL));
    b->vis_sw = row_control(row_switch(g, T("Visible to other devices", "다른 기기에 보이기"),
                                       NULL, FALSE, G_CALLBACK(on_visible), NULL));

    b->mine = group_new(b->page, T("My devices", "내 기기"));
    b->others = group_new(b->page, T("Other devices", "다른 기기"));
    b->search_btn = gtk_button_new_with_label(T("Search for devices", "기기 찾기"));
    gtk_widget_set_halign(b->search_btn, GTK_ALIGN_END);
    gtk_widget_set_margin_top(b->search_btn, 8);
    g_signal_connect(b->search_btn, "clicked", G_CALLBACK(on_search), NULL);
    gtk_box_append(GTK_BOX(b->page), b->search_btn);

    reload(b);
    return b->page;
}

static const char *const KEYS[] = {
    "Bluetooth", "블루투스",
    "Visible to other devices", "다른 기기에 보이기",
    "My devices", "내 기기",
    "Search for devices", "기기 찾기",
    "Headphones", "헤드폰",
    "Pair", "페어링",
    NULL
};

const lp_panel_t lp_panel_bluetooth = {
    "bluetooth", "Bluetooth", "블루투스", "bluetooth-symbolic", build, KEYS, NULL
};
