/* wifi - where the wireless connection stopped, and why.
 *
 *   wifi                    every step of the last connection, and the
 *                           one it stopped at, with the reason
 *   wifi log                the wpa daemon's journal: what it did, when
 *   wifi trace on|off       describe every EAPOL frame in that journal
 *   wifi fallback [on|off]  run wpa_supplicant instead of our stack
 *
 * ── Why this exists ──
 * On the Raspberry Pi the association worked and the WPA2 four-way
 * handshake never happened, inside wpa_supplicant, with nothing on the
 * screen but a timeout. This command used to try seven wpa_supplicant
 * configurations in turn to find one that worked (git show ae78156);
 * none did, and the owner asked for our own stack instead (the `wpa`
 * daemon, userland/wpa). That daemon records every step of a
 * connection, so this program no longer guesses: it asks the daemon
 * what happened and draws it as the ladder a connection climbs,
 *
 *   interface -> radio -> scan -> join -> 1/4 -> 2/4 -> 3/4 -> 4/4
 *             -> address
 *
 * with the rung it stopped on and the daemon's sentence for why. `lp-net
 * status` says the same thing in one line for the desktop; this is the
 * long form for a person at a terminal who wants to know what to fix.
 *
 * ── The fallback ──
 * wpa_supplicant is kept installed as the way back (wpa-proto.h, "The
 * fallback switch"). `wifi fallback on` makes the switch file and
 * restarts the service, which then execs wpa_supplicant; `off` removes
 * it. While it is on, `wifi` reports from wpa_cli and `wifi log` from
 * wpa_supplicant's own lines, the way this command did before.
 *
 * English by default, Korean when LANG says so, like lp-net: the
 * sentences are the catalog in wpa-proto.h, shared with the daemon.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"
#include "../wpa/wpa-proto.h"

static bool KO;
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

static char reply[64 * 1024];

static bool reply_ok(void)
{
    return strncmp(reply, "ok", 2) == 0 &&
           (reply[2] == '\n' || reply[2] == '\0');
}

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

/* The daemon's "err code a1 a2" as a sentence on stderr. */
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
        strlcpy(msg, T("the Wi-Fi service gave an answer wifi does not understand",
                       "Wi-Fi 서비스의 답을 wifi 가 이해하지 못했습니다"), sizeof msg);
    dprintf(STDERR_FILENO, "wifi: %s\n", msg);
    return 1;
}

static int no_daemon(void)
{
    char msg[400];
    wpa_msg_format(msg, sizeof msg, "no_daemon", "", "", "", KO);
    dprintf(STDERR_FILENO, "wifi: %s\n", msg);
    return 1;
}

/* ── Running things (the fallback's wpa_cli, and `service`) ────────── */

static int run_capture(const char *path, char *const argv[],
                       char *out, size_t size)
{
    if (out && size) out[0] = '\0';
    int fds[2];
    if (lp_pipe(fds) < 0)
        return -1;
    pid_t pid = lp_fork();
    if (pid < 0) {
        lp_close(fds[0]); lp_close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        lp_close(fds[0]);
        lp_dup2(fds[1], STDOUT_FILENO);
        lp_dup2(fds[1], STDERR_FILENO);
        lp_close(fds[1]);
        lp_execve(path, argv, environ);
        lp_exit(127);
    }
    lp_close(fds[1]);
    /* Read before waiting: a child that fills the pipe blocks on write,
     * and a parent that waits first would then never read. */
    size_t got = 0;
    for (;;) {
        if (!out || got + 1 >= size) {
            char sink[256];
            if (lp_read(fds[0], sink, sizeof sink) <= 0) break;
            continue;
        }
        long n = lp_read(fds[0], out + got, size - got - 1);
        if (n <= 0) break;
        got += (size_t)n;
    }
    if (out && size) out[got] = '\0';
    lp_close(fds[0]);
    int status = 0;
    lp_waitpid(pid, &status, 0);
    return (status & 0x7f) ? -1 : ((status >> 8) & 0xff);
}

/* ── The ladder ────────────────────────────────────────────────────── */

enum { S_IFACE, S_RADIO, S_SCAN, S_JOIN, S_M1, S_M2, S_M3, S_M4,
       S_ADDR, S_N };

static const char *step_name(int s)
{
    switch (s) {
    case S_IFACE: return T("interface", "장치");
    case S_RADIO: return T("radio", "무선");
    case S_SCAN:  return T("scan", "검색");
    case S_JOIN:  return T("join", "연결");
    case S_M1:    return T("message 1/4", "메시지 1/4");
    case S_M2:    return T("message 2/4", "메시지 2/4");
    case S_M3:    return T("message 3/4", "메시지 3/4");
    case S_M4:    return T("message 4/4", "메시지 4/4");
    default:      return T("address", "주소");
    }
}

/* The step an error code says the attempt stopped at; -1 for a code
 * that is not about the connection's progress (a refused request, a
 * file that could not be written). S_N means it was connected and then
 * the connection ended. */
static int step_of(const char *code)
{
    static const struct { const char *code; int step; } MAP[] = {
        { "no_iface", S_IFACE }, { "no_cfg80211", S_IFACE }, { "iface", S_IFACE },
        { "radio_off", S_RADIO }, { "radio_hard", S_RADIO },
        { "no_networks", S_SCAN }, { "none_in_range", S_SCAN },
        { "not_found", S_SCAN }, { "scan_failed", S_SCAN },
        { "need_password", S_SCAN }, { "pw_short", S_SCAN },
        { "pw_long", S_SCAN }, { "pw_chars", S_SCAN }, { "pw_hex", S_SCAN },
        { "wpa3_only", S_SCAN }, { "enterprise", S_SCAN }, { "wep", S_SCAN },
        { "wpa1", S_SCAN }, { "mfp_required", S_SCAN }, { "cipher", S_SCAN },
        { "connect_refused", S_JOIN }, { "assoc_rejected", S_JOIN },
        { "assoc_timeout", S_JOIN }, { "wpa3_failed", S_JOIN },
        { "no_msg1", S_M1 }, { "handshake", S_M1 },
        { "wrong_password", S_M3 }, { "no_msg3", S_M3 },
        { "set_key", S_M4 }, { "authorize", S_M4 },
        { "no_address", S_ADDR },
        { "ap_left", S_N }, { "lost", S_N },
        { NULL, 0 }
    };
    for (int i = 0; MAP[i].code; i++)
        if (strcmp(MAP[i].code, code) == 0)
            return MAP[i].step;
    return -1;
}

typedef struct {
    char radio[8], iface[16], driver[32], state[16], security[8];
    u8   ssid[32]; size_t ssid_len;
    char bssid[18], ip[16];
    int  freq, signal, saved;
    long since;
    char code[24], a1[96], a2[200];
    int  hs[5];                 /* msg1, msg1 after our 2, msg3, group rekeys, ptk rekeys */
    bool have_hs, trace;
    char config[96];
} st_t;

static bool status_get(st_t *s)
{
    memset(s, 0, sizeof *s);
    if (!wpa_ask("status", reply, sizeof reply, 3000) || !reply_ok())
        return false;
    char *cur = reply, *line;
    next_line(&cur);
    while ((line = next_line(&cur))) {
        char *f[6] = { "", "", "", "", "", "" };
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
        else if (!strcmp(k, "since"))    s->since = atoi(v);
        else if (!strcmp(k, "trace"))    s->trace = atoi(v) != 0;
        else if (!strcmp(k, "config"))   strlcpy(s->config, v, sizeof s->config);
        else if (!strcmp(k, "error")) {
            strlcpy(s->code, v, sizeof s->code);
            if (n > 2) strlcpy(s->a1, f[2], sizeof s->a1);
            if (n > 3) strlcpy(s->a2, f[3], sizeof s->a2);
        } else if (!strcmp(k, "handshake") && n >= 6) {
            s->have_hs = true;
            for (int i = 0; i < 5; i++) s->hs[i] = atoi(f[1 + i]);
        }
    }
    return true;
}

static void printable(const u8 *b, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++)
        out[o++] = b[i] >= 0x20 && b[i] != 0x7f ? (char)b[i] : '?';
    out[o] = '\0';
}

static void show_ladder(const st_t *s)
{
    /* How far it got (steps before `done` are done), where it is now
     * (`here`, in progress) and where it stopped (`bad`). */
    int done = 0, here = -1, bad = -1;
    const char *st = s->state;
    bool open = !strcmp(s->security, "open");

    if (!strcmp(st, "connected"))       done = S_N;
    else if (!strcmp(st, "dhcp"))       { done = S_ADDR; here = S_ADDR; }
    else if (!strcmp(st, "handshake")) {
        done = S_M1;
        if (s->have_hs && s->hs[0] > 0)
            done = S_M3;                /* 1/4 in, our 2/4 out          */
        here = done;
    } else if (!strcmp(st, "connecting")) { done = S_JOIN; here = S_JOIN; }
    else if (!strcmp(st, "scanning"))   { done = S_SCAN; here = S_SCAN; }
    else {
        done = s->iface[0] ? S_SCAN : S_IFACE;
        if (!strcmp(s->radio, "off") || !strcmp(s->radio, "hard"))
            done = S_RADIO;
    }
    int at = s->code[0] ? step_of(s->code) : -1;
    if (at >= 0 && at < S_N && here < 0) {
        bad = at;
        done = at;
    } else if (at == S_ADDR) {
        bad = S_ADDR;                   /* dhcp is slow: a warning     */
    }
    if (!strcmp(s->code, "wrong_password") || !strcmp(s->code, "no_msg3"))
        done = S_M3;                    /* 2/4 did go out              */

    printf("  %s\n", T("steps", "단계"));
    for (int i = 0; i < S_N; i++) {
        const char *mark, *what = "";
        char buf[160] = "";
        bool skip = open && i >= S_M1 && i <= S_M4;
        if (skip && (done > i || here > S_M4 || done >= S_ADDR)) {
            mark = "[--]";
            what = T("not needed (open network)", "필요 없음 (개방형 네트워크)");
        } else if (i == bad) {
            mark = "[!!]";
            what = T("stopped here", "여기서 멈춤");
        } else if (i == here) {
            mark = "[..]";
            what = T("in progress", "진행 중");
        } else if (i < done) {
            mark = "[ok]";
            switch (i) {
            case S_IFACE:
                snprintf(buf, sizeof buf, "%s%s%s%s", s->iface,
                         s->driver[0] ? " (" : "", s->driver,
                         s->driver[0] ? ")" : "");
                what = buf;
                break;
            case S_RADIO: what = T("on", "켜짐"); break;
            case S_SCAN:  what = T("the network was found", "네트워크를 찾음"); break;
            case S_JOIN:  what = T("associated", "연결됨 (association)"); break;
            case S_M1:    what = T("received from the access point", "공유기에서 받음"); break;
            case S_M2:    what = T("sent", "보냄"); break;
            case S_M3:    what = T("received and verified", "받아서 확인함"); break;
            case S_M4:    what = T("sent, keys installed", "보내고 키를 설치함"); break;
            case S_ADDR:  what = s->ip[0] ? s->ip : T("received", "받음"); break;
            }
        } else {
            mark = "[  ]";
        }
        printf("    %s  %-12s %s\n", mark, step_name(i), what);
    }
}

static int show_fallback_status(const char *sw)
{
    printf("\n  %s\n", T("Wi-Fi is run by wpa_supplicant (the fallback).",
                         "Wi-Fi 를 wpa_supplicant(대체 경로)가 맡고 있습니다."));
    printf("  %-12s %s\n", T("switch", "스위치"), sw);
    char out[4096];
    char *argv[] = { (char *)"wpa_cli", (char *)"status", NULL };
    const char *cli = lp_exists("/bin/wpa_cli") ? "/bin/wpa_cli"
                    : lp_exists("/sbin/wpa_cli") ? "/sbin/wpa_cli"
                    : "/usr/sbin/wpa_cli";
    if (run_capture(cli, argv, out, sizeof out) == 0) {
        for (char *p = out; p && *p; ) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            if (!strncmp(p, "wpa_state=", 10) || !strncmp(p, "ssid=", 5) ||
                !strncmp(p, "bssid=", 6) || !strncmp(p, "ip_address=", 11) ||
                !strncmp(p, "freq=", 5) || !strncmp(p, "key_mgmt=", 9))
                printf("  %s\n", p);
            p = nl ? nl + 1 : NULL;
        }
    } else {
        printf("  %s\n", T("wpa_cli got no answer - `service status wpa`",
                           "wpa_cli 가 답을 받지 못했습니다 - `service status wpa`"));
    }
    printf("\n  %s\n\n", T("`wifi fallback off` goes back to LP's own Wi-Fi.",
                           "`wifi fallback off` 로 LP 자체 Wi-Fi 로 돌아갑니다."));
    return 0;
}

static int cmd_status(void)
{
    char sw[96];
    if (wpa_fallback_running(sw, sizeof sw))
        return show_fallback_status(sw);

    st_t s;
    if (!status_get(&s))
        return no_daemon();

    char ssid[96];
    printable(s.ssid, s.ssid_len, ssid, sizeof ssid);
    printf("\n");
    if (ssid[0]) {
        printf("  %-12s \"%s\"", T("network", "네트워크"), ssid);
        if (s.bssid[0] && strcmp(s.bssid, "00:00:00:00:00:00"))
            printf("  %s", s.bssid);
        if (s.freq)
            printf("  %d MHz", s.freq);
        if (s.signal)
            printf("  %d dBm (%d%%)", s.signal, wpa_quality(s.signal));
        if (s.security[0])
            printf("  %s", !strcmp(s.security, "open") ? T("open", "개방형") : "WPA2-PSK");
        printf("\n");
    }
    const char *st = s.state;
    const char *said =
        !strcmp(st, "connected")  ? T("connected", "연결됨") :
        !strcmp(st, "dhcp")       ? T("keys installed, waiting for an address",
                                      "키 설치됨, 주소를 기다리는 중") :
        !strcmp(st, "handshake")  ? T("in the WPA handshake", "WPA 핸드셰이크 중") :
        !strcmp(st, "connecting") ? T("joining", "연결하는 중") :
        !strcmp(st, "scanning")   ? T("looking for networks", "네트워크를 찾는 중") :
        !strcmp(st, "failed")     ? T("not connected - the last attempt failed",
                                      "연결 안 됨 - 마지막 시도가 실패함") :
                                    T("not connected", "연결 안 됨");
    printf("  %-12s %s", T("state", "상태"), said);
    if (s.since > 0)
        printf(T("  (for %ld s)", "  (%ld초째)"), s.since);
    printf("\n\n");

    show_ladder(&s);

    if (s.code[0]) {
        char msg[600];
        wpa_msg_format(msg, sizeof msg, s.code, s.a1, s.a2, "", KO);
        int at = step_of(s.code);
        printf("\n  %s %s\n",
               at == S_N ? T("then:", "그 뒤:")
               : !strcmp(st, "failed") || at >= 0 ? T("why:", "이유:")
               : T("note:", "참고:"), msg);
    }
    if (s.have_hs && (s.hs[0] > 1 || s.hs[3] || s.hs[4])) {
        printf("\n  %s", T("handshake", "핸드셰이크"));
        printf(T(": message 1 seen %d times", ": 메시지 1을 %d번 받음"), s.hs[0]);
        if (s.hs[1])
            printf(T(", %d of them after our message 2", ", 그중 %d번은 우리 메시지 2 뒤"), s.hs[1]);
        if (s.hs[3])
            printf(T(", group key renewed %d times", ", 그룹 키 %d번 갱신"), s.hs[3]);
        if (s.hs[4])
            printf(T(", pairwise key renewed %d times", ", 쌍 키 %d번 갱신"), s.hs[4]);
        printf("\n");
    }
    printf("\n  %s %d, %s\n", T("saved networks:", "저장된 네트워크:"), s.saved,
           s.config);
    printf("  %s\n\n", T("`wifi log` shows every step with its time;"
                         " `lp-net scan` the networks around.",
                         "`wifi log` 는 단계마다 시각을, `lp-net scan` 은"
                         " 주변 네트워크를 보여 줍니다."));
    return 0;
}

/* ── log, trace ────────────────────────────────────────────────────── */

static int fallback_log(void)
{
    const char *files[] = { "/data/log/wpa.log", "/var/log/wpa.log",
                            "/data/log/messages", NULL };
    static const char *KEY[] = {
        "Trying to associate", "Associated with", "RX message 1 of 4-Way",
        "Key negotiation completed", "CTRL-EVENT-CONNECTED",
        "timed out", "CTRL-EVENT-DISCONNECTED", "CTRL-EVENT-ASSOC-REJECT",
        "WRONG_KEY", "pre-shared key may be incorrect", NULL
    };
    int shown = 0;
    for (int f = 0; files[f]; f++) {
        long fd = lp_open(files[f], O_RDONLY, 0);
        if (fd < 0) continue;
        char line[512];
        while (readline((int)fd, line, sizeof line) >= 0)
            for (int k = 0; KEY[k]; k++)
                if (strstr(line, KEY[k])) {
                    printf("  %s\n", line);
                    shown++;
                    break;
                }
        lp_close((int)fd);
    }
    if (!shown)
        printf("  %s\n", T("wpa_supplicant has recorded nothing yet. `touch /boot/wpa-debug`"
                           " and a restart write every frame to the log.",
                           "wpa_supplicant 가 아직 아무것도 기록하지 않았습니다."
                           " `touch /boot/wpa-debug` 후 재시작하면 모든 프레임을 기록합니다."));
    return 0;
}

static int cmd_log(void)
{
    if (wpa_fallback_running(NULL, 0))
        return fallback_log();
    if (!wpa_ask("log", reply, sizeof reply, 3000))
        return no_daemon();
    if (!reply_ok())
        return reply_fail();
    char *cur = reply, *line;
    next_line(&cur);
    int n = 0;
    while ((line = next_line(&cur))) {
        if (strncmp(line, "line\t", 5) == 0) {
            printf("%s\n", line + 5);
            n++;
        }
    }
    if (!n)
        printf("%s\n", T("(nothing yet)", "(아직 없음)"));
    return 0;
}

static int cmd_trace(int argc, char **argv)
{
    if (argc != 1 || (strcmp(argv[0], "on") && strcmp(argv[0], "off"))) {
        dprintf(STDERR_FILENO, "%s\n", T("usage: wifi trace on|off", "사용법: wifi trace on|off"));
        return 2;
    }
    char line[32];
    snprintf(line, sizeof line, "trace\t%s", argv[0]);
    if (!wpa_ask(line, reply, sizeof reply, 3000))
        return no_daemon();
    if (!reply_ok())
        return reply_fail();
    printf("%s\n", !strcmp(argv[0], "on")
           ? T("every EAPOL frame is now described in `wifi log`",
               "이제 모든 EAPOL 프레임이 `wifi log` 에 기록됩니다")
           : T("frame tracing is off", "프레임 기록을 껐습니다"));
    return 0;
}

/* ── fallback ──────────────────────────────────────────────────────── */

static void restart_service(void)
{
    char *argv[] = { (char *)"service", (char *)"restart", (char *)"wpa", NULL };
    char out[512];
    if (run_capture("/bin/service", argv, out, sizeof out) != 0)
        printf("%s\n", T("could not restart the service - `sudo service restart wpa`, or reboot",
                         "서비스를 다시 시작하지 못했습니다 - `sudo service restart wpa` 또는 재부팅하십시오"));
}

static int cmd_fallback(int argc, char **argv)
{
    char sw[96];
    bool on = wpa_fallback_running(sw, sizeof sw);
    if (argc == 0) {
        if (on)
            printf("%s (%s)\n", T("on: wpa_supplicant runs the Wi-Fi",
                                  "켜짐: wpa_supplicant 가 Wi-Fi 를 맡음"), sw);
        else
            printf("%s\n", T("off: LP's own Wi-Fi (the wpa daemon) runs it",
                             "꺼짐: LP 자체 Wi-Fi(wpa 데몬)가 맡음"));
        return 0;
    }
    bool want;
    if (argc == 1 && !strcmp(argv[0], "on")) want = true;
    else if (argc == 1 && !strcmp(argv[0], "off")) want = false;
    else {
        dprintf(STDERR_FILENO, "%s\n", T("usage: wifi fallback [on|off]",
                                         "사용법: wifi fallback [on|off]"));
        return 2;
    }
    if (lp_getuid() != 0) {
        dprintf(STDERR_FILENO, "wifi: %s\n", T("this changes a system file - `sudo wifi fallback ...`",
                                               "시스템 파일을 바꾸는 일입니다 - `sudo wifi fallback ...`"));
        return 1;
    }
    char dir[64], file[96];
    lp_setting_path("lp-net", dir, sizeof dir);
    snprintf(file, sizeof file, "%s/fallback", dir);
    if (want) {
        lp_mkdir(dir, 0700);
        long fd = lp_open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "wifi: %s %s (%s)\n", T("cannot write", "쓸 수 없음:"),
                    file, lp_strerror((int)-fd));
            return 1;
        }
        dprintf((int)fd, "# While this file exists, `wpa -d` runs wpa_supplicant"
                         " instead of LP's own Wi-Fi.\n# `wifi fallback off` removes it.\n");
        lp_fsync((int)fd);
        lp_close((int)fd);
        long dfd = lp_open(dir, O_RDONLY | O_DIRECTORY, 0);
        if (dfd >= 0) { lp_fsync((int)dfd); lp_close((int)dfd); }
    } else {
        lp_unlink("/data/lp-net/fallback");
        lp_unlink("/etc/lp-net/fallback");
        if (lp_exists(WPA_FALLBACK_CARD) && lp_unlink(WPA_FALLBACK_CARD) < 0)
            printf("%s %s\n", T("the switch on the card stays on - delete it from any PC:",
                                "카드의 스위치가 그대로 켜져 있습니다 - 아무 PC 에서나 지우십시오:"),
                   WPA_FALLBACK_CARD);
    }
    restart_service();
    printf("%s\n", want ? T("wpa_supplicant now runs the Wi-Fi (`wifi fallback off` to go back)",
                            "이제 wpa_supplicant 가 Wi-Fi 를 맡습니다 (되돌리려면 `wifi fallback off`)")
                        : T("LP's own Wi-Fi runs it again", "다시 LP 자체 Wi-Fi 가 맡습니다"));
    return 0;
}

static void usage(void)
{
    printf("%s", T(
        "usage: wifi [log | trace on|off | fallback [on|off]]\n"
        "\n"
        "  wifi                    every step of the last connection, and why it stopped\n"
        "  wifi log                the Wi-Fi service's journal, step by step\n"
        "  wifi trace on|off       describe every handshake frame in that journal\n"
        "  wifi fallback [on|off]  run wpa_supplicant instead of LP's own Wi-Fi\n"
        "\n"
        "  To join a network: lp-net connect NAME --password -\n",
        "사용법: wifi [log | trace on|off | fallback [on|off]]\n"
        "\n"
        "  wifi                    마지막 연결의 모든 단계와 멈춘 이유\n"
        "  wifi log                Wi-Fi 서비스의 단계별 기록\n"
        "  wifi trace on|off       그 기록에 핸드셰이크 프레임을 모두 남김\n"
        "  wifi fallback [on|off]  LP 자체 Wi-Fi 대신 wpa_supplicant 사용\n"
        "\n"
        "  네트워크에 연결하려면: lp-net connect 이름 --password -\n"));
}

int main(int argc, char **argv)
{
    pick_language();
    if (argc == 1)
        return cmd_status();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
        usage();
        return 0;
    }
    if (!strcmp(cmd, "status"))   return cmd_status();
    if (!strcmp(cmd, "log"))      return cmd_log();
    if (!strcmp(cmd, "trace"))    return cmd_trace(argc - 2, argv + 2);
    if (!strcmp(cmd, "fallback")) return cmd_fallback(argc - 2, argv + 2);
    if (!strcmp(cmd, "fix")) {
        /* The old seven-variant bisect of wpa_supplicant (ae78156). */
        dprintf(STDERR_FILENO, "wifi: %s\n",
                T("`wifi fix` is gone: LP runs its own Wi-Fi now, and `wifi` says where it stopped",
                  "`wifi fix` 는 없어졌습니다: 이제 LP 가 자체 Wi-Fi 를 쓰고, `wifi` 가 멈춘 곳을 알려 줍니다"));
        return 2;
    }
    dprintf(STDERR_FILENO, "wifi: %s: %s\n", T("not a command", "없는 명령"), cmd);
    usage();
    return 2;
}
