/*
 * network.c - Wi-Fi & Network: the list of networks around, connecting to
 * one, forgetting one, the radio, airplane mode, wired status, and what
 * address the machine ended up with.
 *
 * Everything that changes something goes through lp-net (the networking
 * track's command), in the shapes COMMON.md fixes:
 *
 *     lp-net status --json      radio, wifi {state ssid signal quality ip error}, wired[]
 *     lp-net scan --json        [{ssid bssid signal quality security freq saved connected}]
 *     lp-net connect SSID [--password X] | forget SSID | radio on|off
 *
 * The addresses under "Connection" are read from the kernel and from
 * /etc/resolv.conf rather than from lp-net, because they are the answer
 * to "what is this machine actually using", and a daemon's idea of that
 * and the routing table can disagree - which is exactly when somebody
 * opens this screen.
 *
 * ── Connecting takes a while, and says so ──
 *
 * `lp-net connect` returns when it has handed the job to the daemon, and
 * the association, the WPA handshake and DHCP follow. So after it returns
 * this panel polls `lp-net status` once a second and shows the state as
 * it moves - "handshake", "getting an address" - and the password dialog
 * stays open, busy, until the answer is connected or failed. A dialog that
 * closed at once and left the failure to a banner would make somebody who
 * mistyped the password open the network and type it all again; left
 * open, the on-screen keyboard is still up and the field is still there.
 *
 * ── No Wi-Fi adapter ──
 *
 * A virtual machine, or a laptop whose Wi-Fi driver or firmware is
 * missing, has no wireless interface at all. `lp-net radio on` still
 * answers 0 there, so the switch used to say "Wi-Fi is on" and flip back
 * off a moment later with no reason given. The kernel is asked first
 * (/sys/class/ieee80211, or a wireless/ directory under a network
 * interface): with nothing there the switch is locked and says why, and
 * no scan is started.
 *
 * ── The list changes under the finger, gently ──
 *
 * A rescan does not clear the list and draw it again: that makes every row
 * blink and moves the one somebody was about to tap. Rows are matched by
 * network name - one that is still there is updated in place, one that has
 * gone closes (154ms), a new one opens (220ms) at its place in the order
 * (ui.c, row_add_animated). The order is kept by the list's sort function,
 * so a network whose signal changed moves without being rebuilt.
 */
#include "core.h"

#include <glib/gstdio.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>

typedef struct {
    GtkWidget   *page;
    GtkWidget   *radio_sw;
    GtkWidget   *air_sw;
    GtkWidget   *list;           /* Wi-Fi networks */
    GtkWidget   *wired;
    GtkWidget   *details;
    GtkWidget   *scan_btn;
    jnode_t     *status;
    guint        poll;
    int          polls;
    char        *want;           /* the SSID being connected to */
    GtkWidget   *pw_dialog;      /* the password dialog's window, while open */
    gboolean     wifi;           /* the kernel has a Wi-Fi device */
} net_t;

static net_t *N;                 /* the page on screen, or NULL */

static void net_free(gpointer p)
{
    net_t *n = p;
    if (n->poll) g_source_remove(n->poll);
    json_free(n->status);
    g_free(n->want);
    if (N == n) N = NULL;
    g_free(n);
}

/* ── reading the kernel ─────────────────────────────────────────────── */

static char *default_route(char **gw_out)
{
    char *r = lp_slurp("/proc/net/route");
    if (!r) return NULL;
    char **lines = g_strsplit(r, "\n", -1);
    char *iface = NULL;
    for (int i = 1; lines[i] && !iface; i++) {
        char **f = g_strsplit_set(lines[i], "\t ", -1);
        int n = g_strv_length(f);
        if (n >= 3 && !g_strcmp0(f[1], "00000000")) {
            iface = g_strdup(f[0]);
            /* Little-endian hex: the bytes have to be reversed to be read. */
            guint32 v = (guint32)g_ascii_strtoull(f[2], NULL, 16);
            if (gw_out)
                *gw_out = g_strdup_printf("%u.%u.%u.%u", v & 0xff, (v >> 8) & 0xff,
                                          (v >> 16) & 0xff, (v >> 24) & 0xff);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(r);
    return iface;
}

static char *address_of(const char *iface)
{
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) return NULL;
    char *out = NULL;
    for (struct ifaddrs *p = ifa; p && !out; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (iface && g_strcmp0(p->ifa_name, iface)) continue;
        if (!g_strcmp0(p->ifa_name, "lo")) continue;
        char buf[INET_ADDRSTRLEN] = "";
        struct sockaddr_in *s = (struct sockaddr_in *)p->ifa_addr;
        if (inet_ntop(AF_INET, &s->sin_addr, buf, sizeof buf))
            out = g_strdup(buf);
    }
    freeifaddrs(ifa);
    return out;
}

static char *dns_servers(void)
{
    char *dns = lp_slurp("/etc/resolv.conf");
    if (!dns) return NULL;
    char **l = g_strsplit(dns, "\n", -1);
    GString *acc = g_string_new(NULL);
    for (int i = 0; l[i]; i++) {
        if (!g_str_has_prefix(l[i], "nameserver")) continue;
        char *v = g_strstrip(g_strdup(l[i] + 10));
        if (acc->len) g_string_append(acc, ", ");
        g_string_append(acc, v);
        g_free(v);
    }
    g_strfreev(l);
    g_free(dns);
    return g_string_free(acc, acc->len == 0);
}

/* ── the pieces of the page ─────────────────────────────────────────── */

static const char *signal_icon(int quality)
{
    if (quality >= 75) return "network-wireless-signal-excellent-symbolic";
    if (quality >= 50) return "network-wireless-signal-good-symbolic";
    if (quality >= 25) return "network-wireless-signal-ok-symbolic";
    return "network-wireless-signal-weak-symbolic";
}

static const char *state_words(const char *st)
{
    if (!g_strcmp0(st, "scanning"))   return T("looking for the network", "네트워크를 찾는 중");
    if (!g_strcmp0(st, "connecting")) return T("connecting", "연결하는 중");
    if (!g_strcmp0(st, "handshake"))  return T("checking the password", "비밀번호를 확인하는 중");
    if (!g_strcmp0(st, "dhcp"))       return T("getting an address", "주소를 받는 중");
    if (!g_strcmp0(st, "connected"))  return T("connected", "연결됨");
    if (!g_strcmp0(st, "failed"))     return T("failed", "실패");
    return T("not connected", "연결 안 됨");
}

static void clear_list(GtkWidget *list)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(list)))
        gtk_list_box_remove(GTK_LIST_BOX(list), c);
}

static void subtitle_from_status(net_t *n)
{
    const jnode_t *s = n->status;
    const char *radio = json_str(s, "radio", "on");
    const char *st = json_str(s, "wifi.state", "disconnected");
    const char *ssid = json_str(s, "wifi.ssid", "");
    char *sub = NULL;

    if (!s) {
        sub = g_strdup(T("lp-net did not answer - the networking service may not be running",
                         "lp-net 이 답하지 않습니다 - 네트워크 서비스가 돌고 있지 않을 수 있습니다"));
    } else if (!g_strcmp0(st, "connected") && *ssid) {
        sub = g_strdup_printf(T("Connected to %s · signal %d%%", "%s 에 연결됨 · 신호 %d%%"),
                              ssid, (int)json_num(s, "wifi.quality", 0));
    } else if (n->want && g_strcmp0(st, "disconnected") && g_strcmp0(st, "failed")) {
        sub = g_strdup_printf("%s: %s…", n->want, state_words(st));
    } else {
        const jnode_t *w = json_get(s, "wired");
        for (const jnode_t *e = w ? w->child : NULL; e && !sub; e = e->next)
            if (json_bool(e, "carrier", FALSE) && *json_str(e, "ip", ""))
                sub = g_strdup_printf(T("Wired · %s", "유선 · %s"), json_str(e, "ip", ""));
        if (!sub)
            sub = g_strdup(!g_strcmp0(radio, "off") ? T("Wi-Fi is off", "Wi-Fi 가 꺼져 있습니다")
                                                   : T("Not connected", "연결되어 있지 않습니다"));
    }
    page_set_subtitle(n->page, sub);
    g_free(sub);
}

static void fill_wired(net_t *n)
{
    clear_list(n->wired);
    const jnode_t *w = json_get(n->status, "wired");
    int count = 0;
    for (const jnode_t *e = w ? w->child : NULL; e; e = e->next) {
        const char *iface = json_str(e, "iface", "?");
        const char *ip = json_str(e, "ip", "");
        gboolean carrier = json_bool(e, "carrier", FALSE);
        char *title = g_strdup_printf(T("Wired (%s)", "유선 (%s)"), iface);
        char *val = carrier
            ? (*ip ? g_strdup_printf(T("Connected · %s", "연결됨 · %s"), ip)
                   : g_strdup(T("Cable in, no address yet", "케이블 연결됨, 주소 없음")))
            : g_strdup(T("Cable unplugged", "케이블이 빠져 있음"));
        row_value(n->wired, title, NULL, val);
        g_free(title); g_free(val);
        count++;
    }
    if (!count)
        row_value(n->wired, T("Wired", "유선"), T("This machine has no wired port, or lp-net reported none",
                                                  "유선 포트가 없거나 lp-net 이 알려 주지 않았습니다"),
                  T("None", "없음"));
}

static void fill_details(net_t *n)
{
    clear_list(n->details);
    char *gw = NULL;
    char *iface = default_route(&gw);
    char *ip = iface ? address_of(iface) : NULL;
    if (!ip && *json_str(n->status, "wifi.ip", ""))
        ip = g_strdup(json_str(n->status, "wifi.ip", ""));
    char *dns = dns_servers();

    row_value(n->details, T("IP address", "IP 주소"), iface, ip ? ip : T("None", "없음"));
    row_value(n->details, T("Gateway", "게이트웨이"), NULL, gw ? gw : T("None", "없음"));
    row_value(n->details, "DNS", T("from /etc/resolv.conf", "/etc/resolv.conf 에서"),
              dns ? dns : T("None", "없음"));
    if (iface) {
        char *p = g_strdup_printf("/sys/class/net/%s/address", iface);
        char *mac = lp_slurp(p);
        row_value(n->details, T("MAC address", "MAC 주소"), NULL, mac ? mac : "?");
        g_free(mac); g_free(p);
    }
    if (!g_strcmp0(json_str(n->status, "wifi.state", ""), "connected")) {
        char *v = g_strdup_printf("%d dBm · %d%%", (int)json_num(n->status, "wifi.signal", 0),
                                  (int)json_num(n->status, "wifi.quality", 0));
        row_value(n->details, T("Wi-Fi signal", "Wi-Fi 신호"), NULL, v);
        g_free(v);
    }
    g_free(gw); g_free(iface); g_free(ip); g_free(dns);
}

/* ── status and polling ─────────────────────────────────────────────── */

static void scan(net_t *n);
static void refresh_all(net_t *n);
static void connect_to(net_t *n, const char *ssid, const char *password);

static void on_status(int st, const char *out, const char *err, gpointer p)
{
    net_t *n = p;
    (void)err;
    json_free(n->status);
    n->status = st == 0 ? json_parse(out) : NULL;

    LP_QUIET(gtk_switch_set_active(GTK_SWITCH(n->radio_sw),
                                   g_strcmp0(json_str(n->status, "radio", "on"), "off")));
    subtitle_from_status(n);
    fill_wired(n);
    fill_details(n);
}

static void finish_connect(net_t *n, gboolean ok, const char *why)
{
    if (n->poll) { g_source_remove(n->poll); n->poll = 0; }
    GtkWidget *dw = n->pw_dialog;
    lp_dialog_t *d = dw ? g_object_get_data(G_OBJECT(dw), "lp-dialog") : NULL;
    if (ok) {
        lp_toast(FALSE, T("Connected to %s", "%s 에 연결했습니다"), n->want);
        if (d) lp_dialog_close(d);
    } else {
        char *msg = g_strdup_printf(T("Could not connect to %s: %s", "%s 에 연결하지 못했습니다: %s"),
                                    n->want, why && *why ? why : T("no reason given", "이유 없음"));
        if (d) {
            lp_dialog_busy(d, FALSE);
            lp_dialog_error(d, msg);
        }
        lp_toast(TRUE, "%s", msg);
        g_free(msg);
    }
    g_clear_pointer(&n->want, g_free);
    refresh_all(n);
}

static void on_poll_status(int st, const char *out, const char *err, gpointer p)
{
    net_t *n = p;
    on_status(st, out, err, p);
    if (!n->want) return;
    const char *state = json_str(n->status, "wifi.state", "");
    const char *ssid = json_str(n->status, "wifi.ssid", "");
    if (!g_strcmp0(state, "connected") && !g_strcmp0(ssid, n->want))
        finish_connect(n, TRUE, NULL);
    else if (!g_strcmp0(state, "failed"))
        finish_connect(n, FALSE, json_str(n->status, "wifi.error", ""));
    else if (++n->polls > 45)
        finish_connect(n, FALSE, T("it took longer than 45 seconds", "45초 안에 끝나지 않았습니다"));
}

static gboolean poll_tick(gpointer p)
{
    net_t *n = p;
    static const char *const v[] = { "lp-net", "status", "--json", NULL };
    lp_run_async(v, NULL, n->page, on_poll_status, n);
    return G_SOURCE_CONTINUE;
}

static void on_connect_started(int st, const char *out, const char *err, gpointer p)
{
    net_t *n = p;
    if (st != 0) {
        char *why = lp_first_line(err, out);
        finish_connect(n, FALSE, why);
        g_free(why);
        return;
    }
    n->polls = 0;
    if (!n->poll)
        n->poll = g_timeout_add(1000, poll_tick, n);
    poll_tick(n);
}

static void connect_to(net_t *n, const char *ssid, const char *password)
{
    g_free(n->want);
    n->want = g_strdup(ssid);
    char *sub = g_strdup_printf(T("Connecting to %s…", "%s 에 연결하는 중…"), ssid);
    page_set_subtitle(n->page, sub);
    g_free(sub);
    const char *v[] = { "lp-net", "connect", ssid, password ? "--password" : NULL, password, NULL };
    lp_run_async(v, NULL, n->page, on_connect_started, n);
}

/* ── dialogs ────────────────────────────────────────────────────────── */

static void pw_ok(lp_dialog_t *d, gpointer p)
{
    net_t *n = N;
    (void)p;
    if (!n) { lp_dialog_close(d); return; }
    GtkWidget *e = lp_dialog_get_data(d, "pw");
    GtkWidget *se = lp_dialog_get_data(d, "ssid");
    const char *ssid = se ? gtk_editable_get_text(GTK_EDITABLE(se))
                          : lp_dialog_get_data(d, "ssid-name");
    const char *pw = gtk_editable_get_text(GTK_EDITABLE(e));
    if (!ssid || !*ssid) {
        lp_dialog_error(d, T("Type the network name.", "네트워크 이름을 입력하십시오."));
        return;
    }
    gboolean need = GPOINTER_TO_INT(lp_dialog_get_data(d, "secure"));
    /* WPA's own rule: 8 to 63 characters. Said here, before the four
     * seconds a handshake takes to say it less clearly. */
    if (need && (strlen(pw) < 8 || strlen(pw) > 63)) {
        lp_dialog_error(d, T("A Wi-Fi password is 8 to 63 characters long.",
                             "Wi-Fi 비밀번호는 8자에서 63자 사이입니다."));
        return;
    }
    lp_dialog_busy(d, TRUE);
    n->pw_dialog = lp_dialog_window(d);
    g_object_add_weak_pointer(G_OBJECT(n->pw_dialog), (gpointer *)&n->pw_dialog);
    connect_to(n, ssid, *pw ? pw : NULL);
}

static void ask_password(const char *ssid, gboolean hidden)
{
    lp_dialog_t *d = lp_dialog_new(hidden ? T("Hidden network", "숨겨진 네트워크")
                                          : T("Wi-Fi password", "Wi-Fi 비밀번호"),
                                   T("Connect", "연결"), FALSE, pw_ok, NULL);
    if (hidden) {
        lp_dialog_text(d, T("Type the name of a network that does not announce itself.",
                            "이름을 알리지 않는 네트워크의 이름을 입력하십시오."), NULL);
        lp_dialog_set_data(d, "ssid", lp_dialog_entry(d, T("Network name", "네트워크 이름"),
                                                      NULL, FALSE), NULL);
        lp_dialog_set_data(d, "pw", lp_dialog_entry(d, T("Password (empty for an open network)",
                                                         "비밀번호 (열린 네트워크면 비워 두십시오)"),
                                                    NULL, TRUE), NULL);
    } else {
        char *t = g_strdup_printf(T("“%s” needs a password.", "“%s” 에 연결하려면 비밀번호가 필요합니다."),
                                  ssid);
        lp_dialog_text(d, t, NULL);
        g_free(t);
        lp_dialog_set_data(d, "ssid-name", g_strdup(ssid), g_free);
        lp_dialog_set_data(d, "secure", GINT_TO_POINTER(1), NULL);
        lp_dialog_set_data(d, "pw", lp_dialog_entry(d, T("Password", "비밀번호"), NULL, TRUE), NULL);
    }
    lp_dialog_present(d);
}

static void on_forgot(int st, const char *out, const char *err, gpointer p)
{
    char *ssid = p;
    if (st == 0) {
        lp_toast(FALSE, T("Forgot %s", "%s 을(를) 지웠습니다"), ssid);
        if (N) refresh_all(N);
    } else {
        char *why = lp_first_line(err, out);
        lp_toast(TRUE, T("Could not forget %s: %s", "%s 을(를) 지우지 못했습니다: %s"), ssid, why);
        g_free(why);
    }
    g_free(ssid);
}

static void forget_ok(lp_dialog_t *d, gpointer p)
{
    (void)p;
    const char *ssid = lp_dialog_get_data(d, "ssid-name");
    const char *v[] = { "lp-net", "forget", ssid, NULL };
    lp_run_async(v, NULL, NULL, on_forgot, g_strdup(ssid));
    lp_dialog_close(d);
}

static void details_dialog(const jnode_t *net)
{
    const char *ssid = json_str(net, "ssid", "");
    gboolean saved = json_bool(net, "saved", FALSE);
    lp_dialog_t *d = lp_dialog_new(ssid, saved ? T("Forget", "지우기") : NULL, TRUE,
                                   forget_ok, NULL);
    lp_dialog_set_data(d, "ssid-name", g_strdup(ssid), g_free);
    int freq = (int)json_num(net, "freq", 0);
    char *t = g_strdup_printf(
        T("Signal %d%% (%d dBm) · %s · %s\nBSSID %s",
          "신호 %d%% (%d dBm) · %s · %s\nBSSID %s"),
        (int)json_num(net, "quality", 0), (int)json_num(net, "signal", 0),
        json_str(net, "security", "?"),
        freq >= 5900 ? "6 GHz" : freq >= 4900 ? "5 GHz" : freq ? "2.4 GHz" : "?",
        json_str(net, "bssid", "?"));
    lp_dialog_text(d, t, NULL);
    g_free(t);
    if (saved)
        lp_dialog_text(d, T("Forgetting removes the saved password. To connect again the "
                            "password has to be typed again.",
                            "지우면 저장된 비밀번호도 지워집니다. 다시 연결하려면 비밀번호를 "
                            "다시 입력해야 합니다."), "lp-note");
    lp_dialog_present(d);
}

/* ── the list ───────────────────────────────────────────────────────── */

static void on_net_tapped(GtkWidget *row, gpointer p)
{
    (void)p;
    net_t *n = N;
    const jnode_t *net = g_object_get_data(G_OBJECT(row), "lp-net");
    if (!n || !net) return;
    const char *ssid = json_str(net, "ssid", "");
    if (json_bool(net, "connected", FALSE)) {
        details_dialog(net);
    } else if (json_bool(net, "saved", FALSE) || !g_strcmp0(json_str(net, "security", "open"), "open")) {
        connect_to(n, ssid, NULL);
    } else {
        ask_password(ssid, FALSE);
    }
}

static void on_more(GtkButton *b, gpointer p)
{
    (void)p;
    GtkWidget *row = g_object_get_data(G_OBJECT(b), "lp-row");
    const jnode_t *net = g_object_get_data(G_OBJECT(row), "lp-net");
    if (net) details_dialog(net);
}

static void on_hidden(GtkWidget *row, gpointer p)
{
    (void)row; (void)p;
    ask_password(NULL, TRUE);
}

static int rank(const jnode_t *n)
{
    return (json_bool(n, "connected", FALSE) ? 2000 : 0) +
           (json_bool(n, "saved", FALSE) ? 1000 : 0) + (int)json_num(n, "quality", 0);
}

/* Status rows ("Looking for networks…") above everything, networks by
 * rank, "Other network…" last. */
static int row_rank(GtkListBoxRow *r)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "lp-rank"));
}

static int sort_rows(GtkListBoxRow *a, GtkListBoxRow *b, gpointer d)
{
    (void)d;
    int ra = row_rank(a), rb = row_rank(b);
    if (ra != rb) return rb - ra;
    const char *ta = g_object_get_data(G_OBJECT(a), "lp-title");
    const char *tb = g_object_get_data(G_OBJECT(b), "lp-title");
    return g_strcmp0(ta, tb);
}

#define RANK_STATUS 100000
#define RANK_OTHER  (-100000)

/* The one status row, replaced (both animated) when the text changes. */
static void set_status_row(net_t *n, const char *title, const char *detail, gboolean spin)
{
    GtkWidget *old = g_object_get_data(G_OBJECT(n->list), "lp-status-row");
    if (old && title) {
        const char *t = g_object_get_data(G_OBJECT(old), "lp-title");
        if (!g_strcmp0(t, title)) {
            row_set_detail(old, detail);
            return;
        }
    }
    if (old) {
        g_object_set_data(G_OBJECT(n->list), "lp-status-row", NULL);
        row_remove_animated(old);
    }
    if (!title) return;
    GtkWidget *row = row_shell(title, detail);
    if (spin) {
        GtkWidget *sp = gtk_spinner_new();
        gtk_spinner_start(GTK_SPINNER(sp));
        gtk_box_prepend(GTK_BOX(row_box(row)), sp);
    }
    g_object_set_data(G_OBJECT(row), "lp-rank", GINT_TO_POINTER(RANK_STATUS));
    g_object_set_data(G_OBJECT(n->list), "lp-status-row", row);
    row_add_animated(n->list, row);
}

static void ensure_other_row(net_t *n)
{
    if (g_object_get_data(G_OBJECT(n->list), "lp-other-row"))
        return;
    GtkWidget *row = row_chevron(NULL, T("Other network…", "다른 네트워크…"),
                                 T("A network that hides its name", "이름을 숨긴 네트워크"), NULL,
                                 G_CALLBACK(on_hidden), NULL);
    g_object_set_data(G_OBJECT(row), "lp-rank", GINT_TO_POINTER(RANK_OTHER));
    g_object_set_data(G_OBJECT(n->list), "lp-other-row", row);
    row_add(n->list, row);
}

/* Everything on a network's row but its name: the signal icon, the lock,
 * the details button, the second line. Rebuilt on every scan, inside a row
 * that stays where it is. */
static void fill_net_row(GtkWidget *row, jnode_t *net)
{
    const char *ssid = json_str(net, "ssid", "");
    gboolean conn = json_bool(net, "connected", FALSE);
    gboolean saved = json_bool(net, "saved", FALSE);
    const char *sec = json_str(net, "security", "open");
    gboolean open = !g_strcmp0(sec, "open");

    const char *what = conn ? T("Connected", "연결됨")
                     : saved ? T("Saved", "저장됨")
                     : open ? T("Open - anyone nearby can see the traffic",
                                "열린 네트워크 - 주변에서 내용을 볼 수 있습니다")
                     : NULL;
    char *detail = what ? g_strdup(what) : g_ascii_strup(sec, -1);
    row_set_detail(row, detail);
    g_free(detail);

    GtkWidget *h = row_box(row);
    GtkWidget *keep = gtk_widget_get_parent(g_object_get_data(G_OBJECT(row), "lp-detail"));
    GtkWidget *c = gtk_widget_get_first_child(h);
    while (c) {
        GtkWidget *next = gtk_widget_get_next_sibling(c);
        if (c != keep) gtk_box_remove(GTK_BOX(h), c);
        c = next;
    }
    GtkWidget *icon = gtk_image_new_from_icon_name(signal_icon((int)json_num(net, "quality", 0)));
    gtk_widget_add_css_class(icon, "lp-signal");
    gtk_box_prepend(GTK_BOX(h), icon);
    if (!open) {
        GtkWidget *lock = gtk_image_new_from_icon_name("changes-prevent-symbolic");
        gtk_widget_add_css_class(lock, "lp-value");
        gtk_box_append(GTK_BOX(h), lock);
    }
    if (saved || conn) {
        GtkWidget *more = gtk_button_new_from_icon_name("view-more-symbolic");
        gtk_widget_set_tooltip_text(more, T("Details", "자세히"));
        gtk_widget_set_valign(more, GTK_ALIGN_CENTER);
        g_object_set_data(G_OBJECT(more), "lp-row", row);
        g_object_set_data_full(G_OBJECT(more), "lp-title", g_strdup_printf("more:%s", ssid), g_free);
        g_signal_connect(more, "clicked", G_CALLBACK(on_more), NULL);
        gtk_box_append(GTK_BOX(h), more);
    }
    g_object_set_data(G_OBJECT(row), "lp-net", net);
    g_object_set_data(G_OBJECT(row), "lp-rank", GINT_TO_POINTER(rank(net)));
}

static void drop_net_rows(net_t *n, GHashTable *keep)
{
    GtkWidget *c = gtk_widget_get_first_child(n->list);
    while (c) {
        GtkWidget *next = gtk_widget_get_next_sibling(c);
        const char *ssid = g_object_get_data(G_OBJECT(c), "lp-ssid");
        if (ssid && !g_object_get_data(G_OBJECT(c), "lp-leaving") &&
            (!keep || !g_hash_table_contains(keep, ssid))) {
            /* Its JSON is about to be freed with the old scan. */
            g_object_set_data(G_OBJECT(c), "lp-net", NULL);
            g_object_set_data(G_OBJECT(c), "lp-leaving", GINT_TO_POINTER(1));
            row_remove_animated(c);
        }
        c = next;
    }
}

static void on_scan(int st, const char *out, const char *err, gpointer p)
{
    net_t *n = p;
    gtk_widget_set_sensitive(n->scan_btn, TRUE);

    jnode_t *arr = st == 0 ? json_parse(out) : NULL;
    if (!arr || arr->type != J_ARR) {
        char *why = st == 0 ? g_strdup(T("lp-net answered something that is not a list",
                                         "lp-net 의 답이 목록이 아닙니다"))
                            : lp_first_line(err, out);
        drop_net_rows(n, NULL);
        set_status_row(n, T("Could not scan", "주변 네트워크를 찾지 못했습니다"), why, FALSE);
        g_free(why);
        json_free(arr);
        ensure_other_row(n);
        return;
    }

    /* Which rows are there now, by name. */
    GHashTable *have = g_hash_table_new(g_str_hash, g_str_equal);
    for (GtkWidget *c = gtk_widget_get_first_child(n->list); c; c = gtk_widget_get_next_sibling(c)) {
        const char *ssid = g_object_get_data(G_OBJECT(c), "lp-ssid");
        if (ssid && !g_object_get_data(G_OBJECT(c), "lp-leaving"))
            g_hash_table_insert(have, (gpointer)ssid, c);
    }
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    int count = 0;
    for (jnode_t *net = arr->child; net; net = net->next) {
        const char *ssid = json_str(net, "ssid", "");
        if (!*ssid) continue;                    /* hidden: "Other network…" handles those */
        if (g_hash_table_contains(seen, ssid)) continue;   /* a second access point */
        g_hash_table_add(seen, g_strdup(ssid));
        count++;
        GtkWidget *row = g_hash_table_lookup(have, ssid);
        if (row) {
            fill_net_row(row, net);
            continue;
        }
        row = row_shell(ssid, NULL);
        g_object_set_data_full(G_OBJECT(row), "lp-ssid", g_strdup(ssid), g_free);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        g_object_set_data(G_OBJECT(row), "lp-activate", (gpointer)on_net_tapped);
        fill_net_row(row, net);
        row_add_animated(n->list, row);
    }
    drop_net_rows(n, seen);
    g_hash_table_destroy(have);
    g_hash_table_destroy(seen);

    /* Kept on the list so the rows can point into it; the previous scan's
     * array goes now, and every row that pointed into it was refilled or
     * told to forget it above. */
    g_object_set_data_full(G_OBJECT(n->list), "lp-scan", arr, (GDestroyNotify)json_free);

    if (!count)
        set_status_row(n, T("No networks found", "찾은 네트워크가 없습니다"),
                       T("Move closer to the access point, or scan again.",
                         "공유기 가까이 가거나 다시 찾아 보십시오."), FALSE);
    else
        set_status_row(n, NULL, NULL, FALSE);
    ensure_other_row(n);
    gtk_list_box_invalidate_sort(GTK_LIST_BOX(n->list));
}

/* Does the kernel know any Wi-Fi device? */
static gboolean have_wifi(void)
{
    gboolean yes = FALSE;
    const char *e;
    GDir *d = g_dir_open("/sys/class/ieee80211", 0, NULL);
    if (d) {
        yes = g_dir_read_name(d) != NULL;
        g_dir_close(d);
    }
    d = yes ? NULL : g_dir_open("/sys/class/net", 0, NULL);
    while (d && !yes && (e = g_dir_read_name(d))) {
        char *w = g_build_filename("/sys/class/net", e, "wireless", NULL);
        yes = g_file_test(w, G_FILE_TEST_IS_DIR);
        g_free(w);
    }
    if (d) g_dir_close(d);
    return yes;
}

static void scan(net_t *n)
{
    if (!n->wifi) {
        drop_net_rows(n, NULL);
        set_status_row(n, T("No Wi-Fi adapter", "Wi-Fi 장치가 없습니다"),
                       T("This machine has no Wi-Fi hardware, or its driver is not loaded. "
                         "A network cable still works.",
                         "이 기계에는 Wi-Fi 장치가 없거나 드라이버가 올라와 있지 않습니다. "
                         "유선 연결은 그대로 쓸 수 있습니다."), FALSE);
        gtk_widget_set_sensitive(n->scan_btn, FALSE);
        return;
    }
    if (!gtk_switch_get_active(GTK_SWITCH(n->radio_sw))) {
        drop_net_rows(n, NULL);
        set_status_row(n, T("Wi-Fi is off", "Wi-Fi 가 꺼져 있습니다"),
                       T("Turn it on above to see networks.", "위에서 켜면 네트워크가 보입니다."), FALSE);
        return;
    }
    gtk_widget_set_sensitive(n->scan_btn, FALSE);
    /* Only when there is nothing to look at yet: with networks listed, the
     * scan button's own state says a scan is running, and the list stays
     * put rather than growing a row at the top. */
    gboolean empty = TRUE;
    for (GtkWidget *c = gtk_widget_get_first_child(n->list); c; c = gtk_widget_get_next_sibling(c))
        if (g_object_get_data(G_OBJECT(c), "lp-ssid") && !g_object_get_data(G_OBJECT(c), "lp-leaving"))
            empty = FALSE;
    if (empty)
        set_status_row(n, T("Looking for networks…", "네트워크를 찾는 중…"), NULL, TRUE);
    static const char *const v[] = { "lp-net", "scan", "--json", NULL };
    lp_run_async(v, NULL, n->page, on_scan, n);
}

/* The status answer sets the radio switch, and whether to scan at all
 * depends on it, so the scan waits for the status. */
static void on_status_then_scan(int st, const char *out, const char *err, gpointer p)
{
    on_status(st, out, err, p);
    scan(p);
}

static void refresh_all(net_t *n)
{
    static const char *const v[] = { "lp-net", "status", "--json", NULL };
    lp_run_async(v, NULL, n->page, on_status_then_scan, n);
}

static void on_scan_clicked(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    if (N) refresh_all(N);
}

/* ── the switches ───────────────────────────────────────────────────── */

typedef struct { net_t *n; GtkWidget *sw; gboolean want; } flip_t;

static void on_radio_done(int st, const char *out, const char *err, gpointer p)
{
    flip_t *f = p;
    if (st == 0) {
        lp_toast(FALSE, f->want ? T("Wi-Fi is on", "Wi-Fi 를 켰습니다")
                                : T("Wi-Fi is off", "Wi-Fi 를 껐습니다"));
    } else {
        char *why = lp_first_line(err, out);
        lp_toast(TRUE, T("Could not switch Wi-Fi: %s", "Wi-Fi 를 바꾸지 못했습니다: %s"), why);
        g_free(why);
        LP_QUIET(gtk_switch_set_active(GTK_SWITCH(f->sw), !f->want));
    }
    refresh_all(f->n);
    g_free(f);
}

static void on_radio(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    if (!N) return;
    flip_t *f = g_new0(flip_t, 1);
    f->n = N;
    f->sw = GTK_WIDGET(sw);
    f->want = gtk_switch_get_active(GTK_SWITCH(sw));
    const char *v[] = { "lp-net", "radio", f->want ? "on" : "off", NULL };
    lp_run_async(v, NULL, N->page, on_radio_done, f);
}

/* Airplane mode is the Wi-Fi radio and the Bluetooth radio together. The
 * flag file is shared with the quick menu (desktop/quick), which draws the
 * same state; the radios are the truth and the flag is only what was asked. */
static void on_air(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    char *flag = lp_config_path("airplane");
    if (on) lp_write_file(flag, "on\n");
    else    g_remove(flag);
    g_free(flag);

    const char *v[] = { "lp-net", "radio", on ? "off" : "on", NULL };
    char *err = NULL;
    int st = lp_run_full(v, NULL, NULL, &err);
    /* rfkill is optional: a machine without Bluetooth has nothing to block. */
    const char *b[] = { "rfkill", on ? "block" : "unblock", "bluetooth", NULL };
    lp_run_full(b, NULL, NULL, NULL);
    if (st == 0)
        lp_toast(FALSE, on ? T("Airplane mode is on: Wi-Fi and Bluetooth are off",
                               "비행기 모드를 켰습니다: Wi-Fi 와 블루투스가 꺼졌습니다")
                           : T("Airplane mode is off", "비행기 모드를 껐습니다"));
    else
        lp_toast(TRUE, T("Could not switch the Wi-Fi radio: %s", "Wi-Fi 를 바꾸지 못했습니다: %s"),
                 err ? err : "");
    g_free(err);
    if (N) {
        LP_QUIET(gtk_switch_set_active(GTK_SWITCH(N->radio_sw), !on));
        refresh_all(N);
    }
}

static GtkWidget *build(void)
{
    net_t *n = g_new0(net_t, 1);
    N = n;
    n->page = page_new(T("Network", "네트워크"), T("Reading the network state…", "네트워크 상태를 읽는 중…"));
    g_object_set_data_full(G_OBJECT(n->page), "lp-net", n, net_free);

    GtkWidget *g0 = group_new(n->page, NULL);
    n->wifi = have_wifi();
    GtkWidget *r = row_switch(g0, "Wi-Fi", NULL, n->wifi, G_CALLBACK(on_radio), NULL);
    n->radio_sw = row_control(r);
    if (!n->wifi) {
        LP_QUIET(gtk_switch_set_active(GTK_SWITCH(n->radio_sw), FALSE));
        gtk_widget_set_sensitive(n->radio_sw, FALSE);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r), FALSE);
        row_set_detail(r, T("This machine has no Wi-Fi adapter", "이 기계에는 Wi-Fi 장치가 없습니다"));
    }
    char *flag = lp_config_path("airplane");
    gboolean air = g_file_test(flag, G_FILE_TEST_EXISTS);
    g_free(flag);
    r = row_switch(g0, T("Airplane mode", "비행기 모드"),
                   T("Turns off Wi-Fi and Bluetooth", "Wi-Fi 와 블루투스를 끕니다"),
                   air, G_CALLBACK(on_air), NULL);
    n->air_sw = row_control(r);

    n->list = group_new(n->page, T("Wi-Fi networks", "Wi-Fi 네트워크"));
    gtk_list_box_set_sort_func(GTK_LIST_BOX(n->list), sort_rows, NULL, NULL);
    n->scan_btn = gtk_button_new_with_label(T("Scan again", "다시 찾기"));
    gtk_widget_set_halign(n->scan_btn, GTK_ALIGN_END);
    gtk_widget_set_margin_top(n->scan_btn, 8);
    g_signal_connect(n->scan_btn, "clicked", G_CALLBACK(on_scan_clicked), NULL);
    gtk_box_append(GTK_BOX(n->page), n->scan_btn);

    n->wired = group_new(n->page, T("Wired", "유선"));
    n->details = group_new(n->page, T("Connection", "연결 정보"));

    refresh_all(n);
    return n->page;
}

static const char *const KEYS[] = {
    "Wi-Fi", "와이파이",
    "Airplane mode", "비행기 모드",
    "Other network…", "다른 네트워크…",
    "IP address", "IP 주소",
    "Gateway", "게이트웨이",
    "DNS", "DNS",
    "MAC address", "MAC 주소",
    "Wired", "유선",
    "Password", "비밀번호",
    NULL
};

const lp_panel_t lp_panel_network = {
    "network", "Network", "네트워크", "network-wireless-symbolic", build, KEYS, NULL
};
