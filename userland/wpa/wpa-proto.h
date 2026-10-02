/* wpa-proto.h - what the wpa daemon and the programs that talk to it
 * (lp-net, wifi) agree on: where the socket is, what a request and an
 * answer look like, and every sentence the daemon can say about why
 * the wireless is not working, in English and in Korean.
 *
 * ── Why the daemon does not speak JSON ──
 * `lp-net status --json` is a contract with the desktop (COMMON.md), and
 * its "error" field is read out to a person in the language they chose.
 * The daemon runs as root with no locale; the person's language is the
 * environment of the lp-net that asked. So the daemon answers in plain
 * TAB-separated records with a message CODE and its arguments, and
 * lp-net - which knows LANG - turns the code into a sentence and the
 * records into the contract's JSON. One catalog, here, used by both
 * sides; the daemon prints the English form into its own log.
 *
 * ── The conversation ──
 * A client connects to SOCK_PATH (a unix stream socket, mode 0666 - who
 * may do what is decided per verb from SO_PEERCRED, not by the file
 * mode), writes ONE line of TAB-separated fields ending in '\n', and
 * reads until the daemon closes. The answer's first line is
 *
 *      ok
 *      err <TAB> code <TAB> arg1 <TAB> arg2
 *
 * and any records follow, one per line, first field naming the record.
 * SSIDs travel as hex everywhere in this protocol: an SSID is 0..32
 * arbitrary bytes, TAB and newline included, and hex is the one
 * spelling of it that cannot break a line-based format.
 *
 * Requests (fields after the verb):
 *      status                      the state, one "key<TAB>value" per line
 *      scan                        a fresh scan (waits for it, <= 12 s),
 *                                  then one "bss" record per network
 *      connect  ssid-hex [password] [hidden]
 *      forget   ssid-hex
 *      disconnect                  stay off until the next connect
 *      radio    on|off
 *      log                         the daemon's recent journal
 *      trace    on|off             every EAPOL frame described in the log
 *
 * The password crosses this socket and nothing else: it is never in the
 * daemon's argv, never in a file in the clear once the network is saved
 * (the file holds the PMK it maps to), and never in the log.
 */
#ifndef LP_WPA_PROTO_H
#define LP_WPA_PROTO_H

#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "net.h"

#define WPA_SOCK_PATH     "/run/lp-net.sock"
/* The same state as `status`, rewritten on every change, for anything
 * that would rather read a file than open a socket (a shell script, a
 * status bar). Same records as the status answer, minus the "ok". */
#define WPA_STATUS_PATH   "/run/lp-net.status"
#define WPA_REQ_MAX       512

/* ── The fallback switch ──────────────────────────────────────────────
 *
 * wpa_supplicant stays installed as the way back if this stack ever
 * fails on some card. The switch is the existence of a file, because a
 * file can be made by anyone who can reach the machine in any way:
 *
 *   /boot/wpa-supplicant     on the Pi's FAT partition, from any PC -
 *                            the only switch that works when the
 *                            wireless is the only way in
 *   <settings>/fallback      `wifi fallback on`, where <settings> is
 *                            lp_setting_path("lp-net"): /etc/lp-net on
 *                            the desktop, /data/lp-net on a board
 *
 * `wpa -d` looks at it once, at start, and when it is on it becomes
 * wpa_supplicant (exec), so the service keeps its name and init keeps
 * supervising the same line. lp-net and wifi look at it too, to say
 * "the fallback is running" instead of "the service is not running". */
#define WPA_FALLBACK_CARD "/boot/wpa-supplicant"

/* Both settings places are looked at, not lp_setting_path()'s choice:
 * that choice depends on whether the CALLER can write /data, so an
 * lp-net run by uid 1000 on a board would look in /etc and miss the
 * switch root made in /data. */
static const char *const WPA_FALLBACK_FILES[] = {
    WPA_FALLBACK_CARD, "/data/lp-net/fallback", "/etc/lp-net/fallback", NULL
};

static inline bool wpa_fallback_on(char *which, size_t n)
{
    const char *c = NULL;
    for (int i = 0; WPA_FALLBACK_FILES[i] && !c; i++)
        if (lp_exists(WPA_FALLBACK_FILES[i]))
            c = WPA_FALLBACK_FILES[i];
    if (which && n)
        snprintf(which, n, "%s", c ? c : "");
    return c != NULL;
}

/* Is wpa_supplicant running in our place right now? `wpa -d` writes
 * WPA_FALLBACK_RUN (0644, in /run) just before it execs wpa_supplicant,
 * and removes it when it starts as itself. A client asks this rather
 * than wpa_fallback_on(), because the settings switch lives in a 0700
 * root directory that uid 1000 cannot look into - and because what is
 * running is the thing worth reporting. `which` gets the switch. */
#define WPA_FALLBACK_RUN "/run/lp-net.fallback"

static inline bool wpa_fallback_running(char *which, size_t n)
{
    char buf[128];
    long r = proc_read(WPA_FALLBACK_RUN, buf, sizeof buf - 1);
    if (r <= 0)
        return wpa_fallback_on(which, n);
    buf[r] = '\0';
    for (long i = 0; i < r; i++)
        if (buf[i] == '\n') { buf[i] = '\0'; break; }
    if (which && n)
        snprintf(which, n, "%s", buf);
    return true;
}

/* ── The catalog ──────────────────────────────────────────────────────
 *
 * %1 %2 %3 are replaced by the arguments that travel with the code. In
 * the Korean sentences a network name is always followed by "네트워크"
 * or put in quotes before a particle-free phrase, so that no particle
 * has to agree with a name we cannot see (을/를 depends on the last
 * syllable). Technical details that come from deep inside the stack -
 * the kernel's own error text, an 802.11 status name - stay English
 * inside the Korean sentence; they are what a person pastes into a
 * search engine.
 */
typedef struct {
    const char *code;
    const char *en;
    const char *ko;
} wpa_msg_t;

static const wpa_msg_t WPA_MSGS[] = {
    { "no_iface",
      "this machine has no wireless interface",
      "이 컴퓨터에는 무선 장치가 없습니다" },
    { "no_cfg80211",
      "the kernel has no wireless support (cfg80211): %1",
      "커널에 무선 지원(cfg80211)이 없습니다: %1" },
    { "iface",
      "the wireless interface %1 cannot be used: %2",
      "무선 장치 %1 을(를) 쓸 수 없습니다: %2" },
    { "radio_off",
      "Wi-Fi is turned off",
      "Wi-Fi 가 꺼져 있습니다" },
    { "radio_hard",
      "Wi-Fi is switched off by a hardware switch or key",
      "Wi-Fi 가 하드웨어 스위치나 키로 꺼져 있습니다" },
    { "no_networks",
      "no network is saved yet - choose one to connect to",
      "저장된 네트워크가 아직 없습니다 - 연결할 네트워크를 고르십시오" },
    { "none_in_range",
      "none of the saved networks is in range",
      "저장된 네트워크가 하나도 범위 안에 없습니다" },
    { "not_found",
      "\"%1\" was not found - it is out of range, or the name is not exactly right",
      "\"%1\" 네트워크를 찾지 못했습니다 - 범위 밖이거나 이름이 정확히 맞지 않습니다" },
    { "scan_failed",
      "scanning for networks failed: %1",
      "네트워크를 검색하지 못했습니다: %1" },
    { "need_password",
      "\"%1\" needs a password",
      "\"%1\" 네트워크에 연결하려면 비밀번호가 필요합니다" },
    { "pw_short",
      "a Wi-Fi password is at least 8 characters long",
      "Wi-Fi 비밀번호는 8자 이상입니다" },
    { "pw_long",
      "a Wi-Fi password is at most 63 characters long (or exactly 64 hex digits)",
      "Wi-Fi 비밀번호는 63자 이하입니다 (또는 정확히 16진수 64자리)" },
    { "pw_chars",
      "a Wi-Fi password can only contain letters, digits, spaces and ASCII symbols",
      "Wi-Fi 비밀번호에는 영문자, 숫자, 공백, ASCII 기호만 쓸 수 있습니다" },
    { "pw_hex",
      "a 64-character Wi-Fi key must be all hex digits (0-9, a-f)",
      "64자짜리 Wi-Fi 키는 전부 16진수(0-9, a-f)여야 합니다" },
    { "wpa3_only",
      "\"%1\" accepts only WPA3 (SAE), and this system has no wpa_supplicant that does WPA3 (/usr/sbin/wpa_supplicant, Debian's wpasupplicant) - install it, or switch the router to WPA2/WPA3 mixed mode",
      "\"%1\" 네트워크는 WPA3(SAE)만 받는데, 이 시스템에 WPA3 를 하는 wpa_supplicant(/usr/sbin/wpa_supplicant, Debian 의 wpasupplicant)가 없습니다 - 설치하거나 공유기를 WPA2/WPA3 혼합 모드로 바꾸십시오" },
    { "wpa3_failed",
      "joining \"%1\" through wpa_supplicant (WPA3) did not finish: %2 - check the password",
      "wpa_supplicant 로 \"%1\" 네트워크(WPA3)에 연결하지 못했습니다: %2 - 비밀번호를 확인하십시오" },
    { "enterprise",
      "\"%1\" uses 802.1X (enterprise) sign-in, which this system does not support",
      "\"%1\" 네트워크는 802.1X(기업용) 로그인을 쓰는데, 이 시스템은 지원하지 않습니다" },
    { "wep",
      "\"%1\" uses WEP, which is broken and not supported",
      "\"%1\" 네트워크는 WEP 를 쓰는데, WEP 는 깨진 방식이라 지원하지 않습니다" },
    { "wpa1",
      "\"%1\" offers only WPA1 (TKIP), which is not supported - switch the router to WPA2",
      "\"%1\" 네트워크는 WPA1(TKIP)만 제공하는데 지원하지 않습니다 - 공유기를 WPA2 로 바꾸십시오" },
    { "mfp_required",
      "\"%1\" requires protected management frames (802.11w), and this system has no wpa_supplicant that does them (/usr/sbin/wpa_supplicant)",
      "\"%1\" 네트워크는 관리 프레임 보호(802.11w)를 요구하는데, 이를 하는 wpa_supplicant(/usr/sbin/wpa_supplicant)가 이 시스템에 없습니다" },
    { "cipher",
      "\"%1\" offers no cipher this system supports (%2)",
      "\"%1\" 네트워크가 이 시스템이 지원하는 암호 방식을 제공하지 않습니다 (%2)" },
    { "connect_refused",
      "the kernel would not start joining \"%1\": %2",
      "커널이 \"%1\" 네트워크 연결을 시작하지 않았습니다: %2" },
    { "assoc_rejected",
      "\"%1\" refused to let this machine join: %2",
      "\"%1\" 네트워크가 연결을 거절했습니다: %2" },
    { "assoc_timeout",
      "\"%1\" did not answer while joining (%2)",
      "\"%1\" 네트워크가 연결하는 동안 응답하지 않았습니다 (%2)" },
    { "no_msg1",
      "joined \"%1\", but it never began the WPA handshake (no message 1 of 4 in %2 s)",
      "\"%1\" 네트워크에 붙었지만 WPA 핸드셰이크가 시작되지 않았습니다 (%2초 동안 4개 중 1번째 메시지가 오지 않음)" },
    { "wrong_password",
      "\"%1\" did not accept the password - it is almost certainly wrong",
      "\"%1\" 네트워크가 비밀번호를 받아들이지 않았습니다 - 비밀번호가 틀렸을 가능성이 큽니다" },
    { "no_msg3",
      "\"%1\" stopped answering in the middle of the WPA handshake (no message 3 of 4 in %2 s)",
      "\"%1\" 네트워크가 WPA 핸드셰이크 도중 응답을 멈췄습니다 (%2초 동안 4개 중 3번째 메시지가 오지 않음)" },
    { "handshake",
      "the WPA handshake with \"%1\" failed: %2",
      "\"%1\" 네트워크와의 WPA 핸드셰이크가 실패했습니다: %2" },
    { "set_key",
      "could not install the %1 key: %2",
      "%1 키를 설치하지 못했습니다: %2" },
    { "authorize",
      "the handshake with \"%1\" worked, but the kernel would not open the connection for data: %2",
      "\"%1\" 네트워크와의 핸드셰이크는 성공했지만 커널이 데이터 연결을 열지 않았습니다: %2" },
    { "ap_left",
      "\"%1\" ended the connection: %2",
      "\"%1\" 네트워크가 연결을 끊었습니다: %2" },
    { "lost",
      "the connection to \"%1\" was lost: %2",
      "\"%1\" 네트워크와의 연결이 끊겼습니다: %2" },
    { "no_address",
      "connected to \"%1\", but no DHCP server has given this machine an address yet",
      "\"%1\" 네트워크에 연결했지만 아직 DHCP 서버에서 주소를 받지 못했습니다" },
    { "denied",
      "only an administrator (group sudo) or whoever saved \"%1\" can %2 it - try `sudo lp-net %2 ...`",
      "\"%1\" 네트워크를 %2 하는 것은 관리자(sudo 그룹)나 그 네트워크를 저장한 사람만 할 수 있습니다 - `sudo lp-net %2 ...` 로 해 보십시오" },
    { "denied_account",
      "this account (uid %1) may not change the wireless",
      "이 계정(uid %1)은 무선 설정을 바꿀 수 없습니다" },
    { "not_saved",
      "\"%1\" is not a saved network",
      "\"%1\" 은(는) 저장된 네트워크가 아닙니다" },
    { "bad_request",
      "the request was not understood: %1",
      "요청을 이해하지 못했습니다: %1" },
    { "no_daemon",
      "the Wi-Fi service is not running (`sudo service start wpa`)",
      "Wi-Fi 서비스가 돌고 있지 않습니다 (`sudo service start wpa`)" },
    { "fallback",
      "Wi-Fi is being run by wpa_supplicant (the fallback); lp-net can only report - use wpa_cli, or `wifi fallback off`",
      "Wi-Fi 를 wpa_supplicant(대체 경로)가 맡고 있습니다; lp-net 은 상태만 알려 줍니다 - wpa_cli 를 쓰거나 `wifi fallback off` 를 실행하십시오" },
    { "save_failed",
      "connected, but the network could not be saved: %1",
      "연결했지만 네트워크를 저장하지 못했습니다: %1" },
    { "internal",
      "%1",
      "%1" },
    { NULL, NULL, NULL }
};

/* The sentence for a code, in the language asked for. An unknown code
 * is shown as itself with its arguments - a newer daemon talking to an
 * older lp-net must still say something true. */
static inline void wpa_msg_format(char *out, size_t n, const char *code,
                                  const char *a1, const char *a2,
                                  const char *a3, bool korean)
{
    const char *tpl = NULL;
    for (int i = 0; WPA_MSGS[i].code; i++)
        if (strcmp(WPA_MSGS[i].code, code) == 0) {
            tpl = korean ? WPA_MSGS[i].ko : WPA_MSGS[i].en;
            break;
        }
    if (!n)
        return;
    if (!tpl) {
        snprintf(out, n, "%s%s%s%s%s", code,
                 a1 && *a1 ? ": " : "", a1 ? a1 : "",
                 a2 && *a2 ? " / " : "", a2 ? a2 : "");
        return;
    }
    size_t o = 0;
    for (const char *p = tpl; *p && o + 1 < n; p++) {
        if (p[0] == '%' && p[1] >= '1' && p[1] <= '3') {
            const char *a = p[1] == '1' ? a1 : p[1] == '2' ? a2 : a3;
            for (; a && *a && o + 1 < n; a++)
                out[o++] = *a;
            p++;
            continue;
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

/* ── SSIDs as hex ─────────────────────────────────────────────────── */

static inline void wpa_hex_encode(const u8 *b, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = H[b[i] >> 4];
        out[2 * i + 1] = H[b[i] & 15];
    }
    out[2 * n] = '\0';
}

static inline int wpa_hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode exactly strlen(s)/2 bytes; false on an odd length, a bad digit
 * or more than cap bytes. */
static inline bool wpa_hex_decode(const char *s, u8 *out, size_t cap,
                                  size_t *outlen)
{
    size_t n = strlen(s);
    if (n % 2 || n / 2 > cap)
        return false;
    for (size_t i = 0; i < n / 2; i++) {
        int h = wpa_hex_val(s[2 * i]), l = wpa_hex_val(s[2 * i + 1]);
        if (h < 0 || l < 0)
            return false;
        out[i] = (u8)(h << 4 | l);
    }
    *outlen = n / 2;
    return true;
}

/* ── Asking the daemon (the client side, for lp-net and wifi) ────────
 *
 * One request line out, the whole answer in, NUL-terminated. false when
 * the daemon is not there to ask (reply is then ""); a timeout returns
 * what arrived, which the caller then finds incomplete or not "ok". */
static inline bool wpa_ask(const char *line, char *reply, size_t cap,
                           int timeout_ms)
{
    size_t len = 0;
    reply[0] = '\0';

    struct { u16 family; char path[108]; } sa;
    memset(&sa, 0, sizeof sa);
    sa.family = 1;                              /* AF_UNIX */
    strlcpy(sa.path, WPA_SOCK_PATH, sizeof sa.path);

    long fd = lp_socket(1, SOCK_STREAM | LP_SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    if (lp_connect((int)fd, &sa, sizeof sa) < 0) {
        lp_close((int)fd);
        return false;
    }
    size_t n = strlen(line);
    if (lp_sendto((int)fd, line, n, 0x4000 /* NOSIGNAL */, NULL, 0) != (long)n ||
        lp_sendto((int)fd, "\n", 1, 0x4000, NULL, 0) != 1) {
        lp_close((int)fd);
        return false;
    }
    s64 deadline = lp_monotonic_ms() + timeout_ms;
    while (len + 1 < cap) {
        int left = (int)(deadline - lp_monotonic_ms());
        if (left <= 0)
            break;
        lp_pollfd_t p = { (int)fd, LP_POLLIN, 0 };
        if (lp_poll(&p, 1, left) <= 0)
            break;
        long got = lp_read((int)fd, reply + len, cap - 1 - len);
        if (got <= 0)
            break;
        len += (size_t)got;
    }
    reply[len] = '\0';
    lp_close((int)fd);
    return len > 0;
}

/* Signal strength as a percentage, the mapping NetworkManager uses:
 * -100 dBm is 0 %, -50 dBm and better is 100 %, linear between. It is
 * what a person compares against the bars on their phone, which is the
 * only use this number has. */
static inline int wpa_quality(int dbm)
{
    if (dbm == 0)
        return 0;
    int q = 2 * (dbm + 100);
    return q < 0 ? 0 : q > 100 ? 100 : q;
}

#endif /* LP_WPA_PROTO_H */
