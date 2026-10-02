/* lp-net - the network, for people and for the desktop.
 *
 *   lp-net [status] [--json]            Wi-Fi, wired and the address
 *   lp-net scan [--json]                the Wi-Fi networks around
 *   lp-net connect NAME [--password X]  join one (X of "-": read it)
 *                       [--hidden]      it does not announce its name
 *   lp-net forget NAME                  remove a saved network
 *   lp-net disconnect                   leave, and stay off until connect
 *   lp-net radio [on|off]               the Wi-Fi radio
 *   lp-net wired [--json]               each wired port in detail
 *   lp-net dns                          which name servers are in use
 *
 * ── The contract (COMMON.md; the desktop is built against exactly this) ──
 *
 *   lp-net status --json ->
 *     {"radio":"on|off",
 *      "wifi":{"state":"disconnected|scanning|connecting|handshake|dhcp|connected|failed",
 *              "ssid":"","signal":-54,"quality":78,"ip":"","error":""},
 *      "wired":[{"iface":"eth0","carrier":true,"ip":""}]}
 *   lp-net scan --json ->
 *     [{"ssid":"","bssid":"","signal":-54,"quality":78,
 *       "security":"wpa2|wpa3|open","freq":2437,"saved":true,"connected":false}]
 *   lp-net connect <ssid> [--password X] | forget <ssid> | radio on|off
 *
 * Only keys are ever ADDED to those objects, never renamed or removed:
 * wifi also carries "iface", "bssid" and "freq", and a scan entry
 * carries "supported" (false for a network this system cannot join -
 * WEP, 802.1X, WPA3-only) with "note", the sentence saying why. A
 * WEP or enterprise network is reported with security "wpa2", because
 * the contract has three values and what the desktop does with "wpa2"
 * - ask for a password - is the nearest thing; "supported" is what says
 * it will not work.
 *
 * "error" is a sentence in the language of whoever asked (LANG), made
 * here from the code the daemon sends (wpa-proto.h). It is "" whenever
 * there is nothing to explain.
 *
 * Exit status: 0 done (for connect: HANDED OVER - the association, the
 * handshake and DHCP follow, and `lp-net status` shows them move), 1 it
 * could not be done and the reason is on stderr, 2 a usage mistake.
 *
 * ── Why it is a separate program from the daemon ──
 * The daemon (wpa) runs as root and owns the radio; this runs as
 * whoever typed it, and asks. That split is the permission model: the
 * daemon sees the asker's uid through the socket and decides, so this
 * program needs no privilege of its own and no setuid bit.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"
#include "wpa4way.h"                    /* wpa_wipe, for the password */
#include "../wpa/wpa-proto.h"

#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

static bool KO;                         /* Korean, from the locale */
#define T(en, ko) (KO ? (ko) : (en))

static void pick_language(void)
{
    const char *vars[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
    for (int i = 0; i < 3; i++) {
        const char *v = getenv(vars[i]);
        if (v && v[0]) {
            KO = v[0] == 'k' && v[1] == 'o';
            return;
        }
    }
}

/* ── Talking to the daemon ────────────────────────────────────────── */

static char   reply[64 * 1024];
static size_t reply_len;

/* Send one request, collect the whole answer (wpa-proto.h). false: the
 * daemon is not there (reply is then empty). */
static bool ask(const char *line, int timeout_ms)
{
    bool ok = wpa_ask(line, reply, sizeof reply, timeout_ms);
    reply_len = strlen(reply);
    return ok;
}

/* Split one line of the reply into TAB fields, in place. */
static int fields(char *line, char **f, int max)
{
    int n = 0;
    for (char *p = line; n < max; ) {
        f[n++] = p;
        char *t = strchr(p, '\t');
        if (!t)
            break;
        *t = '\0';
        p = t + 1;
    }
    return n;
}

/* Walk the reply's lines. The first ("ok" / "err ...") is skipped. */
static char *next_line(char **cursor)
{
    char *p = *cursor;
    if (!p || !*p)
        return NULL;
    char *nl = strchr(p, '\n');
    if (nl) {
        *nl = '\0';
        *cursor = nl + 1;
    } else {
        *cursor = p + strlen(p);
    }
    return p;
}

static bool reply_ok(void)
{
    return strncmp(reply, "ok", 2) == 0 &&
           (reply[2] == '\n' || reply[2] == '\0');
}

/* The daemon said no: turn its code into a sentence on stderr. */
static int reply_fail(void)
{
    char buf[1024];
    strlcpy(buf, reply, sizeof buf);
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    char *f[4] = { "", "", "", "" };
    int n = fields(buf, f, 4);
    char msg[600];
    if (n >= 2 && strcmp(f[0], "err") == 0)
        wpa_msg_format(msg, sizeof msg, f[1], n > 2 ? f[2] : "",
                       n > 3 ? f[3] : "", "", KO);
    else
        strlcpy(msg, T("the Wi-Fi service gave an answer lp-net does not understand",
                       "Wi-Fi 서비스의 답을 lp-net 이 이해하지 못했습니다"), sizeof msg);
    dprintf(STDERR_FILENO, "lp-net: %s\n", msg);
    return 1;
}

/* Why the daemon is not there: the fallback switch is on (and `wpa`
 * became wpa_supplicant), or the service is not running. */
static const char *no_daemon_code(void)
{
    if (wpa_fallback_running(NULL, 0))
        return "fallback";
    return "no_daemon";
}

static int no_daemon(void)
{
    char msg[400];
    wpa_msg_format(msg, sizeof msg, no_daemon_code(), "", "", "", KO);
    dprintf(STDERR_FILENO, "lp-net: %s\n", msg);
    return 1;
}

/* ── JSON ─────────────────────────────────────────────────────────── */

/* A JSON string from bytes that are usually UTF-8 and are allowed not
 * to be (an SSID is any 32 bytes). Valid UTF-8 passes through; a byte
 * that is not part of a valid sequence becomes \u00XX, so the output
 * is always valid JSON even for a network named in Latin-1. */
static void json_str(const u8 *s, size_t n)
{
    putchar('"');
    for (size_t i = 0; i < n; ) {
        u8 c = s[i];
        if (c == '"' || c == '\\') {
            printf("\\%c", c);
            i++;
        } else if (c < 0x20 || c == 0x7f) {
            printf("\\u%04x", c);
            i++;
        } else if (c < 0x80) {
            putchar(c);
            i++;
        } else {
            size_t len = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3
                       : (c & 0xf8) == 0xf0 ? 4 : 0;
            bool ok = len && i + len <= n && !(len == 2 && c < 0xc2);
            for (size_t k = 1; ok && k < len; k++)
                if ((s[i + k] & 0xc0) != 0x80)
                    ok = false;
            if (ok) {
                for (size_t k = 0; k < len; k++)
                    putchar(s[i + k]);
                i += len;
            } else {
                printf("\\u%04x", c);
                i++;
            }
        }
    }
    putchar('"');
}

static void json_cstr(const char *s) { json_str((const u8 *)s, strlen(s)); }

/* ── Wired ports ──────────────────────────────────────────────────────
 *
 * Every Ethernet-type interface (sysfs type 1) that is a real device
 * (it has a "device" link - bridges, veth, tun and docker0 do not) and
 * is not wireless. Named by what the kernel called it, whatever that is:
 * eth0 on a Pi, enp0s31f6 or a USB adapter's enx... on a PC. */

#define MAX_WIRED 8
typedef struct { char name[16]; bool carrier; u32 ip; } wired_t;
static wired_t WIRED[MAX_WIRED];
static int     NWIRED;

static bool sys_line(const char *path, char *buf, size_t n)
{
    long r = proc_read(path, buf, n);
    if (r <= 0) {
        if (n) buf[0] = '\0';
        return false;
    }
    for (size_t i = 0; buf[i]; i++)
        if (buf[i] == '\n') { buf[i] = '\0'; break; }
    return true;
}

static void wired_scan(void)
{
    NWIRED = 0;
    long fd = lp_open("/sys/class/net", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;
    char buf[2048];
    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0)
            break;
        for (long off = 0; off < got && NWIRED < MAX_WIRED; ) {
            u16 len;
            memcpy(&len, buf + off + DIRENT_RECLEN, 2);
            const char *name = buf + off + DIRENT_NAME;
            if (len == 0)
                break;
            off += len;
            if (name[0] == '.')
                continue;
            char path[96], v[16];
            snprintf(path, sizeof path, "/sys/class/net/%s/type", name);
            if (!sys_line(path, v, sizeof v) || strcmp(v, "1") != 0)
                continue;
            snprintf(path, sizeof path, "/sys/class/net/%s/phy80211", name);
            if (lp_exists(path))
                continue;
            snprintf(path, sizeof path, "/sys/class/net/%s/device", name);
            if (!lp_exists(path))
                continue;
            wired_t *w = &WIRED[NWIRED++];
            memset(w, 0, sizeof *w);
            strlcpy(w->name, name, sizeof w->name);
            snprintf(path, sizeof path, "/sys/class/net/%s/carrier", name);
            w->carrier = sys_line(path, v, sizeof v) && strcmp(v, "1") == 0;
            if (net_get_addr(name, &w->ip) < 0)
                w->ip = 0;
        }
    }
    lp_close((int)fd);
    /* Sorted by name, so the order does not move between two calls. */
    for (int i = 1; i < NWIRED; i++)
        for (int j = i; j > 0 && strcmp(WIRED[j - 1].name, WIRED[j].name) > 0; j--) {
            wired_t t = WIRED[j]; WIRED[j] = WIRED[j - 1]; WIRED[j - 1] = t;
        }
}

/* ── The radio, when the daemon cannot be asked ───────────────────── */

static const char *radio_from_rfkill(void)
{
    long fd = lp_open("/sys/class/rfkill", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return "off";
    char buf[2048];
    int wlan = 0, blocked = 0;
    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0)
            break;
        for (long off = 0; off < got; ) {
            u16 len;
            memcpy(&len, buf + off + DIRENT_RECLEN, 2);
            const char *name = buf + off + DIRENT_NAME;
            if (len == 0)
                break;
            off += len;
            if (name[0] == '.')
                continue;
            char path[96], v[16];
            snprintf(path, sizeof path, "/sys/class/rfkill/%s/type", name);
            if (!sys_line(path, v, sizeof v) || strcmp(v, "wlan") != 0)
                continue;
            wlan++;
            snprintf(path, sizeof path, "/sys/class/rfkill/%s/soft", name);
            if (sys_line(path, v, sizeof v) && strcmp(v, "1") == 0) blocked++;
            snprintf(path, sizeof path, "/sys/class/rfkill/%s/hard", name);
            if (sys_line(path, v, sizeof v) && strcmp(v, "1") == 0) blocked++;
        }
    }
    lp_close((int)fd);
    return wlan && !blocked ? "on" : "off";
}

/* ── status ───────────────────────────────────────────────────────── */

typedef struct {
    bool have;                  /* the daemon answered */
    char radio[8];              /* on off hard none */
    char iface[16], driver[32], state[16];
    u8   ssid[32]; size_t ssid_len;
    char bssid[18];
    int  freq, signal;
    char ip[16], security[8];
    char err_code[24], err_a1[96], err_a2[200];
    int  hs[5];
    bool have_hs;
    int  saved;
    char config[96];
    bool trace;
    long since;
} status_t;

static void status_get(status_t *s)
{
    memset(s, 0, sizeof *s);
    strlcpy(s->state, "disconnected", sizeof s->state);
    if (!ask("status", 3000) || !reply_ok()) {
        strlcpy(s->radio, radio_from_rfkill(), sizeof s->radio);
        strlcpy(s->err_code, no_daemon_code(), sizeof s->err_code);
        return;
    }
    s->have = true;
    char *cur = reply, *line;
    next_line(&cur);
    while ((line = next_line(&cur))) {
        char *f[5] = { "", "", "", "", "" };
        int n = fields(line, f, 6);
        if (n < 2) continue;
        const char *k = f[0], *v = f[1];
        if      (!strcmp(k, "radio"))    strlcpy(s->radio, v, sizeof s->radio);
        else if (!strcmp(k, "iface"))    strlcpy(s->iface, v, sizeof s->iface);
        else if (!strcmp(k, "driver"))   strlcpy(s->driver, v, sizeof s->driver);
        else if (!strcmp(k, "state"))    strlcpy(s->state, v, sizeof s->state);
        else if (!strcmp(k, "ssid"))     wpa_hex_decode(v, s->ssid, sizeof s->ssid, &s->ssid_len);
        else if (!strcmp(k, "bssid"))    strlcpy(s->bssid, v, sizeof s->bssid);
        else if (!strcmp(k, "freq"))     s->freq = atoi(v);
        else if (!strcmp(k, "signal"))   s->signal = atoi(v);
        else if (!strcmp(k, "ip"))       strlcpy(s->ip, v, sizeof s->ip);
        else if (!strcmp(k, "security")) strlcpy(s->security, v, sizeof s->security);
        else if (!strcmp(k, "saved"))    s->saved = atoi(v);
        else if (!strcmp(k, "config"))   strlcpy(s->config, v, sizeof s->config);
        else if (!strcmp(k, "trace"))    s->trace = atoi(v) != 0;
        else if (!strcmp(k, "since"))    s->since = atoi(v);
        else if (!strcmp(k, "error")) {
            strlcpy(s->err_code, v, sizeof s->err_code);
            if (n > 2) strlcpy(s->err_a1, f[2], sizeof s->err_a1);
            if (n > 3) strlcpy(s->err_a2, f[3], sizeof s->err_a2);
        } else if (!strcmp(k, "handshake") && n >= 6) {
            s->have_hs = true;
            for (int i = 0; i < 5; i++) s->hs[i] = atoi(f[1 + i]);
        }
    }
}

static void status_error(const status_t *s, char *out, size_t n)
{
    out[0] = '\0';
    if (s->err_code[0])
        wpa_msg_format(out, n, s->err_code, s->err_a1, s->err_a2, "", KO);
}

static int cmd_status(bool json)
{
    status_t s;
    status_get(&s);
    wired_scan();
    char err[600];
    status_error(&s, err, sizeof err);
    bool radio_on = strcmp(s.radio, "on") == 0;

    if (json) {
        printf("{\"radio\":\"%s\",\"wifi\":{\"state\":\"%s\",\"ssid\":",
               radio_on ? "on" : "off",
               s.have ? s.state : (s.err_code[0] ? "failed" : "disconnected"));
        json_str(s.ssid, s.ssid_len);
        printf(",\"signal\":%d,\"quality\":%d,\"ip\":", s.signal,
               wpa_quality(s.signal));
        json_cstr(s.ip);
        printf(",\"error\":");
        json_cstr(err);
        printf(",\"iface\":");
        json_cstr(s.iface);
        printf(",\"bssid\":");
        json_cstr(s.bssid);
        printf(",\"freq\":%d},\"wired\":[", s.freq);
        for (int i = 0; i < NWIRED; i++) {
            char ip[16] = "";
            if (WIRED[i].ip) ipv4_format(WIRED[i].ip, ip);
            printf("%s{\"iface\":", i ? "," : "");
            json_cstr(WIRED[i].name);
            printf(",\"carrier\":%s,\"ip\":", WIRED[i].carrier ? "true" : "false");
            json_cstr(ip);
            printf("}");
        }
        printf("]}\n");
        return 0;
    }

    char ssid[96] = "";
    size_t o = 0;
    for (size_t i = 0; i < s.ssid_len && o + 1 < sizeof ssid; i++)
        ssid[o++] = s.ssid[i] >= 0x20 ? (char)s.ssid[i] : '?';
    ssid[o] = '\0';

    printf("\n");
    if (!s.have) {
        printf("  %-10s %s\n", "Wi-Fi", err);
    } else if (!strcmp(s.radio, "none")) {
        printf("  %-10s %s\n", "Wi-Fi", err[0] ? err : T("no wireless interface",
                                                         "무선 장치 없음"));
    } else if (!radio_on) {
        printf("  %-10s %s\n", "Wi-Fi", err[0] ? err : T("off", "꺼짐"));
    } else {
        const char *st = s.state;
        const char *what =
            !strcmp(st, "connected")  ? T("connected to", "연결됨:") :
            !strcmp(st, "dhcp")       ? T("getting an address from", "주소를 받는 중:") :
            !strcmp(st, "handshake")  ? T("checking the password with", "비밀번호 확인 중:") :
            !strcmp(st, "connecting") ? T("joining", "연결하는 중:") :
            !strcmp(st, "scanning")   ? T("looking for networks", "네트워크를 찾는 중") :
            !strcmp(st, "failed")     ? T("not connected", "연결 안 됨") :
                                        T("not connected", "연결 안 됨");
        if (ssid[0] && strcmp(st, "failed") && strcmp(st, "disconnected"))
            printf("  %-10s %s \"%s\"", "Wi-Fi", what, ssid);
        else
            printf("  %-10s %s", "Wi-Fi", what);
        if (s.signal && (!strcmp(st, "connected") || !strcmp(st, "dhcp")))
            printf("  (%d dBm, %d%%)", s.signal, wpa_quality(s.signal));
        printf("\n");
        if (s.ip[0])
            printf("  %-10s %s  (%s)\n", T("address", "주소"), s.ip, s.iface);
        if (err[0])
            printf("  %-10s %s\n", strcmp(st, "failed") ? T("note", "참고")
                                                        : T("why", "이유"), err);
    }
    for (int i = 0; i < NWIRED; i++) {
        char ip[16] = "";
        if (WIRED[i].ip) ipv4_format(WIRED[i].ip, ip);
        printf("  %-10s %s: %s%s%s\n", T("wired", "유선"), WIRED[i].name,
               WIRED[i].carrier ? T("cable in", "케이블 연결됨")
                                : T("no cable", "케이블 없음"),
               ip[0] ? ", " : "", ip);
    }
    printf("\n");
    return 0;
}

/* ── scan ─────────────────────────────────────────────────────────── */

typedef struct {
    char bssid[18];
    u8   ssid[32]; size_t ssid_len;
    int  signal, freq;
    char sec[8];
    bool saved, connected;
    char refuse[24], detail[64];
} net_row_t;

static net_row_t ROWS[64];
static int       NROWS;

static int cmd_scan(bool json)
{
    if (!ask("scan", 16000)) {
        if (json) printf("[]\n");
        return no_daemon();
    }
    if (!reply_ok()) {
        if (json) printf("[]\n");
        return reply_fail();
    }
    NROWS = 0;
    char *cur = reply, *line;
    next_line(&cur);
    while ((line = next_line(&cur)) && NROWS < 64) {
        char *f[10];
        if (fields(line, f, 10) < 10 || strcmp(f[0], "bss") != 0)
            continue;
        net_row_t r;
        memset(&r, 0, sizeof r);
        strlcpy(r.bssid, f[1], sizeof r.bssid);
        if (!wpa_hex_decode(f[2], r.ssid, sizeof r.ssid, &r.ssid_len))
            continue;
        r.signal = atoi(f[3]);
        r.freq = atoi(f[4]);
        strlcpy(r.sec, f[5], sizeof r.sec);
        r.saved = atoi(f[6]) != 0;
        r.connected = atoi(f[7]) != 0;
        if (strcmp(f[8], "-") != 0) strlcpy(r.refuse, f[8], sizeof r.refuse);
        if (strcmp(f[9], "-") != 0) strlcpy(r.detail, f[9], sizeof r.detail);

        /* One row per network name: the access point to show is the one
         * we are on, else the strongest. The desktop lists networks,
         * not radios, and three rows of the same name are three chances
         * to pick the weak one. */
        int same = -1;
        for (int i = 0; i < NROWS; i++)
            if (ROWS[i].ssid_len == r.ssid_len &&
                memcmp(ROWS[i].ssid, r.ssid, r.ssid_len) == 0)
                same = i;
        if (same < 0)
            ROWS[NROWS++] = r;
        else if (!ROWS[same].connected &&
                 (r.connected || r.signal > ROWS[same].signal))
            ROWS[same] = r;
    }
    /* Connected first, then saved, then by signal. */
    for (int i = 1; i < NROWS; i++)
        for (int j = i; j > 0; j--) {
            net_row_t *a = &ROWS[j - 1], *b = &ROWS[j];
            int ka = a->connected * 2 + a->saved, kb = b->connected * 2 + b->saved;
            if (kb > ka || (kb == ka && b->signal > a->signal)) {
                net_row_t t = *a; *a = *b; *b = t;
            } else {
                break;
            }
        }

    if (json) {
        printf("[");
        for (int i = 0; i < NROWS; i++) {
            net_row_t *r = &ROWS[i];
            const char *sec = !strcmp(r->sec, "open") ? "open"
                            : !strcmp(r->sec, "wpa3") ? "wpa3" : "wpa2";
            printf("%s{\"ssid\":", i ? "," : "");
            json_str(r->ssid, r->ssid_len);
            printf(",\"bssid\":\"%s\",\"signal\":%d,\"quality\":%d,"
                   "\"security\":\"%s\",\"freq\":%d,\"saved\":%s,"
                   "\"connected\":%s,\"supported\":%s,\"note\":",
                   r->bssid, r->signal, wpa_quality(r->signal), sec, r->freq,
                   r->saved ? "true" : "false", r->connected ? "true" : "false",
                   r->refuse[0] ? "false" : "true");
            char note[400] = "";
            if (r->refuse[0]) {
                char name[80];
                size_t o = 0;
                for (size_t k = 0; k < r->ssid_len && o + 1 < sizeof name; k++)
                    name[o++] = (char)r->ssid[k];
                name[o] = '\0';
                wpa_msg_format(note, sizeof note, r->refuse, name, r->detail, "", KO);
            }
            json_cstr(note);
            printf("}");
        }
        printf("]\n");
        return 0;
    }

    if (NROWS == 0) {
        printf("\n  %s\n\n", T("No networks found.", "찾은 네트워크가 없습니다."));
        return 0;
    }
    printf("\n  %-6s %-8s %-6s %-9s %s\n", T("SIGNAL", "신호"), "dBm",
           T("BAND", "대역"), T("SECURITY", "보안"), T("NAME", "이름"));
    for (int i = 0; i < NROWS; i++) {
        net_row_t *r = &ROWS[i];
        char name[80];
        size_t o = 0;
        for (size_t k = 0; k < r->ssid_len && o + 1 < sizeof name; k++)
            name[o++] = r->ssid[k] >= 0x20 ? (char)r->ssid[k] : '?';
        name[o] = '\0';
        char q[8];
        snprintf(q, sizeof q, "%d%%", wpa_quality(r->signal));
        printf("  %-6s %-8d %-6s %-9s %s", q, r->signal,
               r->freq >= 5900 ? "6GHz" : r->freq >= 4900 ? "5GHz" : "2.4GHz",
               !strcmp(r->sec, "8021x") ? "802.1X" : r->sec, name);
        if (r->connected) printf("  [%s]", T("connected", "연결됨"));
        else if (r->saved) printf("  [%s]", T("saved", "저장됨"));
        if (r->refuse[0]) printf("  [%s]", T("not supported", "지원 안 함"));
        printf("\n");
    }
    printf("\n");
    return 0;
}

/* ── connect, forget, disconnect, radio ───────────────────────────── */

static bool ssid_arg(const char *name, char *hex)
{
    size_t n = strlen(name);
    if (n == 0 || n > 32) {
        dprintf(STDERR_FILENO, "lp-net: %s\n",
                T("a network name is 1 to 32 bytes long",
                  "네트워크 이름은 1바이트에서 32바이트 사이입니다"));
        return false;
    }
    wpa_hex_encode((const u8 *)name, n, hex);
    return true;
}

/* A password typed at the terminal, not echoed; or one line of stdin. */
static bool read_password(char *out, size_t n)
{
    bool tty = lp_isatty(STDIN_FILENO);
    lp_termios_t saved;
    bool quiet = false;
    if (tty) {
        dprintf(STDERR_FILENO, "%s", T("Wi-Fi password: ", "Wi-Fi 비밀번호: "));
        quiet = lp_term_cbreak(STDIN_FILENO, &saved) == 0;
    }
    size_t o = 0;
    for (;;) {
        char c;
        if (lp_read(STDIN_FILENO, &c, 1) != 1)
            break;
        if (c == '\n' || c == '\r')
            break;
        if (tty && (c == 0x7f || c == 8)) {
            if (o) o--;
            continue;
        }
        if (o + 1 < n)
            out[o++] = c;
    }
    out[o] = '\0';
    if (quiet) {
        lp_term_restore(STDIN_FILENO, &saved);
        dprintf(STDERR_FILENO, "\n");
    }
    return true;
}

static int cmd_connect(int argc, char **argv)
{
    const char *name = NULL, *pw = NULL;
    bool hidden = false;
    char typed[128];
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--password") && i + 1 < argc)
            pw = argv[++i];
        else if (!strncmp(argv[i], "--password=", 11))
            pw = argv[i] + 11;
        else if (!strcmp(argv[i], "--hidden"))
            hidden = true;
        else if (!name)
            name = argv[i];
        else {
            dprintf(STDERR_FILENO, "lp-net: %s: %s\n",
                    T("unexpected", "알 수 없는 인자"), argv[i]);
            return 2;
        }
    }
    if (!name) {
        dprintf(STDERR_FILENO, "%s\n", T("usage: lp-net connect NAME [--password X] [--hidden]",
                                         "사용법: lp-net connect 이름 [--password X] [--hidden]"));
        return 2;
    }
    if (pw && !strcmp(pw, "-")) {
        read_password(typed, sizeof typed);
        pw = typed;
    }
    char hex[65];
    if (!ssid_arg(name, hex))
        return 2;
    /* The password may not contain the protocol's separators; WPA's own
     * rule (printable ASCII) already excludes both, so this refuses
     * nothing a router would accept. */
    if (pw && (strchr(pw, '\t') || strchr(pw, '\n'))) {
        char m[200];
        wpa_msg_format(m, sizeof m, "pw_chars", "", "", "", KO);
        dprintf(STDERR_FILENO, "lp-net: %s\n", m);
        return 1;
    }
    char line[WPA_REQ_MAX];
    snprintf(line, sizeof line, "connect\t%s\t%s%s", hex, pw ? pw : "",
             hidden ? "\thidden" : "");
    bool ok = ask(line, 8000);
    wpa_wipe(line, sizeof line);
    wpa_wipe(typed, sizeof typed);
    if (!ok)
        return no_daemon();
    if (!reply_ok())
        return reply_fail();
    printf("%s \"%s\" - %s\n", T("connecting to", "연결을 시작했습니다:"), name,
           T("`lp-net status` shows how it goes", "`lp-net status` 로 진행을 볼 수 있습니다"));
    return 0;
}

static int simple(const char *line, const char *done)
{
    if (!ask(line, 5000))
        return no_daemon();
    if (!reply_ok())
        return reply_fail();
    if (done)
        printf("%s\n", done);
    return 0;
}

static int cmd_forget(int argc, char **argv)
{
    if (argc != 1) {
        dprintf(STDERR_FILENO, "%s\n", T("usage: lp-net forget NAME", "사용법: lp-net forget 이름"));
        return 2;
    }
    char hex[65], line[128];
    if (!ssid_arg(argv[0], hex))
        return 2;
    snprintf(line, sizeof line, "forget\t%s", hex);
    return simple(line, NULL);
}

static int cmd_radio(int argc, char **argv)
{
    if (argc == 0) {
        status_t s;
        status_get(&s);
        printf("%s\n", !strcmp(s.radio, "on") ? "on" : "off");
        return 0;
    }
    if (argc != 1 || (strcmp(argv[0], "on") && strcmp(argv[0], "off"))) {
        dprintf(STDERR_FILENO, "%s\n", T("usage: lp-net radio [on|off]", "사용법: lp-net radio [on|off]"));
        return 2;
    }
    char line[32];
    snprintf(line, sizeof line, "radio\t%s", argv[0]);
    return simple(line, NULL);
}

/* ── wired, dns ───────────────────────────────────────────────────── */

/* The interface that holds the default route, from /proc/net/route. */
static bool default_route(char *iface, size_t n, u32 *gw)
{
    static char buf[8192];
    long got = proc_read("/proc/net/route", buf, sizeof buf - 1);
    if (got <= 0)
        return false;
    buf[got] = '\0';
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *f[4];
        int k = 0;
        for (char *p = line; *p && k < 4; ) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            f[k++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
        if (k >= 3 && strcmp(f[1], "00000000") == 0) {
            strlcpy(iface, f[0], n);
            *gw = (u32)strtoll(f[2], NULL, 16);   /* already network order */
            return true;
        }
        line = nl ? nl + 1 : NULL;
    }
    return false;
}

static int cmd_wired(bool json)
{
    wired_scan();
    char rif[16] = "";
    u32 gw = 0;
    default_route(rif, sizeof rif, &gw);
    if (json) {
        printf("[");
        for (int i = 0; i < NWIRED; i++) {
            wired_t *w = &WIRED[i];
            char path[96], v[64], ip[16] = "", g[16] = "";
            if (w->ip) ipv4_format(w->ip, ip);
            if (gw && !strcmp(rif, w->name)) ipv4_format(gw, g);
            printf("%s{\"iface\":", i ? "," : "");
            json_cstr(w->name);
            printf(",\"carrier\":%s,\"ip\":", w->carrier ? "true" : "false");
            json_cstr(ip);
            snprintf(path, sizeof path, "/sys/class/net/%s/address", w->name);
            sys_line(path, v, sizeof v);
            printf(",\"mac\":");
            json_cstr(v);
            snprintf(path, sizeof path, "/sys/class/net/%s/speed", w->name);
            int speed = w->carrier && sys_line(path, v, sizeof v) ? atoi(v) : 0;
            printf(",\"speed\":%d,\"gateway\":", speed > 0 ? speed : 0);
            json_cstr(g);
            printf("}");
        }
        printf("]\n");
        return 0;
    }
    printf("\n");
    if (NWIRED == 0)
        printf("  %s\n", T("This machine has no wired port (a USB adapter shows up here when plugged in).",
                           "이 컴퓨터에는 유선 포트가 없습니다 (USB 어댑터를 꽂으면 여기에 나옵니다)."));
    for (int i = 0; i < NWIRED; i++) {
        wired_t *w = &WIRED[i];
        char path[96], v[64], drv[64] = "";
        printf("  %s\n", w->name);
        snprintf(path, sizeof path, "/sys/class/net/%s/device/driver", w->name);
        long n = lp_readlink(path, v, sizeof v - 1);
        if (n > 0) {
            v[n] = '\0';
            const char *s = strrchr(v, '/');
            strlcpy(drv, s ? s + 1 : v, sizeof drv);
        }
        if (drv[0]) printf("    %-10s %s\n", T("driver", "드라이버"), drv);
        snprintf(path, sizeof path, "/sys/class/net/%s/address", w->name);
        if (sys_line(path, v, sizeof v)) printf("    %-10s %s\n", "MAC", v);
        if (!w->carrier) {
            printf("    %-10s %s\n", T("link", "링크"), T("no cable", "케이블 없음"));
            continue;
        }
        snprintf(path, sizeof path, "/sys/class/net/%s/speed", w->name);
        int speed = sys_line(path, v, sizeof v) ? atoi(v) : 0;
        snprintf(path, sizeof path, "/sys/class/net/%s/duplex", w->name);
        char dup[16] = "";
        sys_line(path, dup, sizeof dup);
        if (speed > 0)
            printf("    %-10s %d Mb/s%s%s\n", T("link", "링크"), speed,
                   dup[0] ? ", " : "", dup);
        else
            printf("    %-10s %s\n", T("link", "링크"), T("cable in", "케이블 연결됨"));
        char ip[16];
        if (w->ip) {
            ipv4_format(w->ip, ip);
            printf("    %-10s %s\n", T("address", "주소"), ip);
        } else {
            printf("    %-10s %s\n", T("address", "주소"),
                   T("none yet (dhcp is asking)", "아직 없음 (dhcp 가 요청 중)"));
        }
        if (gw && !strcmp(rif, w->name)) {
            ipv4_format(gw, ip);
            printf("    %-10s %s\n", T("gateway", "게이트웨이"), ip);
        }
    }
    printf("\n");
    return 0;
}

static int cmd_dns(void)
{
    static char buf[4096];
    long got = proc_read("/etc/resolv.conf", buf, sizeof buf - 1);
    int shown = 0;
    printf("\n");
    if (got > 0) {
        buf[got] = '\0';
        for (char *line = buf; line && *line; ) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            while (*line == ' ' || *line == '\t') line++;
            if (!strncmp(line, "nameserver", 10) && (line[10] == ' ' || line[10] == '\t')) {
                char *v = line + 10;
                while (*v == ' ' || *v == '\t') v++;
                printf("  %-12s %s\n", T("nameserver", "이름 서버"), v);
                shown++;
            } else if (!strncmp(line, "search", 6) || !strncmp(line, "domain", 6)) {
                char *v = line + 6;
                while (*v == ' ' || *v == '\t') v++;
                printf("  %-12s %s\n", T("search", "검색 도메인"), v);
            }
            line = nl ? nl + 1 : NULL;
        }
    }
    if (!shown)
        printf("  %s\n", T("No name server is set: /etc/resolv.conf is written by dhcp when a network gives an address.",
                           "이름 서버가 없습니다: /etc/resolv.conf 는 네트워크에서 주소를 받을 때 dhcp 가 씁니다."));
    printf("\n");
    return 0;
}

static void usage(void)
{
    printf("%s",
        T("usage: lp-net [command] [--json]\n"
          "\n"
          "  status [--json]             Wi-Fi, wired and the address (the default)\n"
          "  scan [--json]               the Wi-Fi networks around\n"
          "  connect NAME [--password X] join a network; X of \"-\" reads it\n"
          "               [--hidden]     from the terminal without echo\n"
          "  forget NAME                 remove a saved network\n"
          "  disconnect                  leave, and stay off until the next connect\n"
          "  radio [on|off]              the Wi-Fi radio\n"
          "  wired [--json]              each wired port in detail\n"
          "  dns                         the name servers in use\n"
          "\n"
          "  `wifi` explains where a connection stopped; `wifi log` shows the steps.\n",
          "사용법: lp-net [명령] [--json]\n"
          "\n"
          "  status [--json]             Wi-Fi, 유선, 주소 (기본)\n"
          "  scan [--json]               주변의 Wi-Fi 네트워크\n"
          "  connect 이름 [--password X] 네트워크에 연결; X 가 \"-\" 이면\n"
          "               [--hidden]     터미널에서 화면에 보이지 않게 읽음\n"
          "  forget 이름                 저장된 네트워크 지우기\n"
          "  disconnect                  연결을 끊고 다음 connect 까지 그대로 둠\n"
          "  radio [on|off]              Wi-Fi 무선\n"
          "  wired [--json]              유선 포트 자세히\n"
          "  dns                         쓰고 있는 이름 서버\n"
          "\n"
          "  `wifi` 는 연결이 어디서 멈췄는지, `wifi log` 는 단계별 기록을 보여 줍니다.\n"));
}

int main(int argc, char **argv)
{
    pick_language();
    bool json = false;
    int n = 0;
    char *args[32];
    for (int i = 1; i < argc && n < 32; i++) {
        if (!strcmp(argv[i], "--json"))
            json = true;
        else
            args[n++] = argv[i];
    }
    const char *cmd = n ? args[0] : "status";
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
        usage();
        return 0;
    }
    if (!strcmp(cmd, "status"))     return cmd_status(json);
    if (!strcmp(cmd, "scan"))       return cmd_scan(json);
    if (!strcmp(cmd, "connect"))    return cmd_connect(n - 1, args + 1);
    if (!strcmp(cmd, "forget"))     return cmd_forget(n - 1, args + 1);
    if (!strcmp(cmd, "disconnect")) return simple("disconnect", NULL);
    if (!strcmp(cmd, "radio"))      return cmd_radio(n - 1, args + 1);
    if (!strcmp(cmd, "wired"))      return cmd_wired(json);
    if (!strcmp(cmd, "dns"))        return cmd_dns();
    dprintf(STDERR_FILENO, "lp-net: %s: %s\n",
            T("not a command", "없는 명령"), cmd);
    usage();
    return 2;
}
