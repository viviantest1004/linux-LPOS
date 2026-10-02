/* wpa - the daemon that owns the wireless: our own WPA2-PSK supplicant.
 *
 *   wpa -d [-i IFACE] [-v]     run (init starts it from /etc/services)
 *   wpa                        say what this is and how to talk to it
 *
 * People do not talk to this program; they talk to `lp-net` (and the
 * desktop talks to lp-net), which talks to this over /run/lp-net.sock.
 * wpa-proto.h is that conversation.
 *
 * ── Why it exists ──
 * On the Raspberry Pi the association succeeded and the WPA2 four-way
 * handshake then never happened, inside wpa_supplicant, with every cause
 * outside it ruled out. The owner asked for networking of our own, and
 * the rule that came with it is the design of this file: whenever the
 * wireless is not working, say at which step it stopped and why, in a
 * sentence. So the whole life of a connection is one visible state
 *
 *    scan -> choose -> CONNECT -> message 1/4 -> 2/4 -> 3/4 -> 4/4
 *         -> keys installed -> address (DHCP) -> connected
 *
 * and every way out of it has its own message code (wpa-proto.h) with
 * the evidence in its arguments: the 802.11 status the access point
 * refused us with, the number of seconds we waited for message 1, the
 * fact that the AP resent message 1 after our message 2 - which is what
 * a wrong password looks like from this side, and the only way to tell
 * it apart from an AP that simply stopped answering.
 *
 * ── The parts, and whose they are ──
 *   nl80211.c   generic netlink by hand: scan, CONNECT, keys, events.
 *               cfg80211 turns CONNECT into authentication+association
 *               itself on a softmac card (ath10k in the XPS, hwsim in
 *               the test VM) and the firmware does it on a fullmac one
 *               (brcmfmac, Pi and DW1830), so one path serves both.
 *   eapol.c     the packet socket for ethertype 0x888E. It is opened
 *               when the interface is taken and kept open for good, so
 *               it exists BEFORE every CONNECT: message 1 is sent the
 *               instant association completes, and a socket opened
 *               after that would never see it.
 *   wpa4way.c   the handshake as pure functions; this file only feeds
 *               it frames and carries out what it says (send, install).
 *   dhcp        the address. See "The address" below.
 *
 * ── The address ──
 * `dhcp -d` already runs as a service and already manages every
 * interface: it notices carrier, asks, renews, owns the default route
 * and writes /etc/resolv.conf (so DNS comes from there too). Running a
 * second DHCP client here would mean two programs owning one route and
 * one resolv.conf. What was wrong was only WHEN it asked: carrier comes
 * on at association, before the handshake, so its first DISCOVERs went
 * out unencrypted and were dropped by the AP. The fix is the kernel's
 * own mechanism for exactly this (RFC 2863 operational state, the one
 * wpa_supplicant uses): this daemon sets the link mode to DORMANT when
 * it takes the interface, so an associated link reads "dormant", and
 * sets it UP when the keys are in. dhcp treats a dormant link as not
 * yet there. Nothing else changes in dhcp, and wpa_supplicant - the
 * fallback - does the same dance, so dhcp works under either.
 *
 * ── The saved networks ──
 * <dir>/networks, where <dir> is lp_setting_path("lp-net"): /etc/lp-net
 * on the disk-rooted desktop, /data/lp-net on a RAM-rooted board. Root,
 * 0600, in a 0700 directory, rewritten atomically (temp file, fsync,
 * rename, fsync of the directory - a power cut leaves the old file or
 * the new one, never half of either). The format is wpa_supplicant's
 * network={} blocks, so the fallback can read it unchanged, and what it
 * holds is the PMK the passphrase maps to - not the passphrase, which
 * people reuse elsewhere. A network is saved only after its handshake
 * succeeded: a mistyped password is never remembered.
 *
 * /etc/wpa.conf, which /etc/rc builds from wpa_supplicant.conf on the
 * boot card, is read too, so a Pi set up the old way keeps connecting
 * after this daemon replaces wpa_supplicant - it has no other way in.
 * Those entries are not copied into our file; ours are tried first.
 *
 * ── Who may ask for what ──
 * Anyone may read the state. Scanning, connecting, disconnecting and
 * the radio switch are for root and real accounts (uid >= 1000): the
 * quick settings of whoever sits at the machine, as on any laptop.
 * Forgetting a saved network is for root, group sudo or netdev, or the
 * account that saved it - on a shared machine one person's click should
 * not delete the password everybody else relies on
 * (design/account-permissions.md). An administrator at a terminal gets
 * there through `sudo`, which asks for their password.
 *
 * ── The fallback ──
 * wpa_supplicant is kept as the way back (wpa-proto.h, "The fallback
 * switch"). When the switch is on, `wpa -d` waits for a wireless
 * interface and execs wpa_supplicant on it with the saved networks -
 * this file's format is its format for exactly this reason - so init's
 * line, the service name and `service restart wpa` stay the same either
 * way. Waiting here rather than in wpa_supplicant matters: it exits at
 * once without an interface, and init gives up on a service after
 * twenty quick deaths, which on a card whose firmware loads late would
 * be the fallback switching itself off.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"
#include "net.h"
#include "nl80211.h"
#include "eapol.h"
#include "wpa4way.h"
#include "wpa-proto.h"

/* ── Sizes and times ──────────────────────────────────────────────── */

#define MAX_NETS        64
#define MAX_BSS         64
#define MAX_CLIENTS     16
#define JOURNAL_LINES   160
#define JOURNAL_LEN     200

#define TICK_MS              1000
#define SCAN_TIMEOUT_MS     12000   /* both bands, passive DFS included */
#define CONNECT_TIMEOUT_MS  15000   /* cfg80211's own SME gives up sooner */
#define MSG1_TIMEOUT_MS      5000   /* joined -> message 1 of 4          */
#define MSG3_TIMEOUT_MS      5000   /* our 2/4 -> message 3 of 4         */
#define DHCP_SLOW_MS        20000   /* keys in, no address: say so      */
#define SIGNAL_EVERY_MS     10000
#define ADDR_EVERY_MS        1000
#define IDLE_SCAN_MIN_MS    10000   /* nothing saved is in range        */
#define IDLE_SCAN_MAX_MS   120000
#define RETRY_MIN_MS         2000   /* an attempt failed                */
#define RETRY_MAX_MS       120000
#define CLIENT_TIMEOUT_MS    3000   /* to send the request line         */

/* rtnetlink, for the link mode and operational state (see "The
 * address"). From include/uapi/linux/rtnetlink.h and if_link.h. */
#define NETLINK_ROUTE        0
#define RTM_SETLINK         19
#define IFLA_OPERSTATE      16
#define IFLA_LINKMODE       17
#define IF_OPER_DORMANT      5
#define IF_OPER_UP           6

#define AF_UNIX_             1
#define SO_PEERCRED_        17
#define MSG_DONTWAIT_     0x40
#define MSG_NOSIGNAL_   0x4000
#define DIRENT_RECLEN       16
#define DIRENT_NAME         19
#define SIGINT_              2
#define SIGPIPE_            13
#define SIGTERM_            15

/* 802.11 reason codes we send. */
#define REASON_UNSPECIFIED     1
#define REASON_DEAUTH_LEAVING  3
#define REASON_4WAY_TIMEOUT   15
#define REASON_IE_DIFFERENT   17

/* ══ The journal ═════════════════════════════════════════════════════
 *
 * Every step, kept in memory so `wifi log` can show the last attempt
 * without anybody having turned anything on first, printed to the
 * console the way every service here is, and the lines that matter sent
 * to the kernel log (lp_log), which logd keeps across a reboot. */

static char journal[JOURNAL_LINES][JOURNAL_LEN];
static int  journal_next, journal_count;
static bool trace;                      /* every EAPOL frame described   */

static void jlog_raw(const char *msg, bool important)
{
    s64 t = lp_time();
    lp_tm_t tm;
    lp_localtime(t, &tm);
    snprintf(journal[journal_next], JOURNAL_LEN, "%02d:%02d:%02d %s",
             tm.hour, tm.min, tm.sec, msg);
    journal_next = (journal_next + 1) % JOURNAL_LINES;
    if (journal_count < JOURNAL_LINES)
        journal_count++;
    printf("wpa: %s\n", msg);
    if (important)
        lp_log("wpa", msg);
}

/* No vsnprintf in our libc, so the formatting happens at the call. */
#define JLOG(...)  do { char _m[JOURNAL_LEN]; snprintf(_m, sizeof _m, __VA_ARGS__); jlog_raw(_m, false); } while (0)
#define JNOTE(...) do { char _m[JOURNAL_LEN]; snprintf(_m, sizeof _m, __VA_ARGS__); jlog_raw(_m, true); } while (0)
#define JTRACE(...) do { if (trace) JLOG(__VA_ARGS__); } while (0)

/* ══ Small helpers ═══════════════════════════════════════════════════ */

static void mac_str(const u8 m[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* An SSID for a log line or a message argument: printable bytes as they
 * are (UTF-8 included), anything else as \xNN. */
static void ssid_str(const u8 *s, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 5 < cap; i++) {
        u8 c = s[i];
        if (c >= 0x20 && c != 0x7f && c != '\t') {
            out[o++] = (char)c;
        } else {
            snprintf(out + o, cap - o, "\\x%02x", c);
            o += 4;
        }
    }
    out[o] = '\0';
}

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

/* Walk a directory's entries. fn returns false to stop. */
static void each_entry(const char *dir, bool (*fn)(const char *, void *),
                       void *arg)
{
    long fd = lp_open(dir, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;
    char buf[2048];
    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0)
            break;
        for (long off = 0; off < got; ) {
            u16 len;
            memcpy(&len, buf + off + DIRENT_RECLEN, 2);
            const char *name = buf + off + DIRENT_NAME;
            if (len == 0)
                goto out;
            off += len;
            if (name[0] == '.')
                continue;
            if (!fn(name, arg))
                goto out;
        }
    }
out:
    lp_close((int)fd);
}

/* ══ rtnetlink: the link mode and the operational state ══════════════ */

static bool rtnl_setlink(int ifindex, int linkmode, int operstate)
{
    long fd = lp_socket(16 /* AF_NETLINK */, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0)
        return false;

    u8 m[64];
    memset(m, 0, sizeof m);
    u32 len = 16 + 16;                      /* nlmsghdr + ifinfomsg */
    u16 type = RTM_SETLINK, flags = 0x01 | 0x04;   /* REQUEST | ACK */
    u32 seq = 1;
    s32 idx = ifindex;
    memcpy(m + 4, &type, 2);
    memcpy(m + 6, &flags, 2);
    memcpy(m + 8, &seq, 4);
    memcpy(m + 16 + 4, &idx, 4);            /* ifi_index */
    if (linkmode >= 0) {
        u16 alen = 5, atype = IFLA_LINKMODE;
        memcpy(m + len, &alen, 2);
        memcpy(m + len + 2, &atype, 2);
        m[len + 4] = (u8)linkmode;
        len += 8;
    }
    if (operstate >= 0) {
        u16 alen = 5, atype = IFLA_OPERSTATE;
        memcpy(m + len, &alen, 2);
        memcpy(m + len + 2, &atype, 2);
        m[len + 4] = (u8)operstate;
        len += 8;
    }
    memcpy(m, &len, 4);

    u8 sa[12];
    memset(sa, 0, sizeof sa);
    u16 fam = 16;
    memcpy(sa, &fam, 2);
    bool ok = false;
    if (lp_sendto((int)fd, m, len, 0, sa, sizeof sa) == (long)len) {
        lp_pollfd_t p = { (int)fd, LP_POLLIN, 0 };
        u8 r[256];
        if (lp_poll(&p, 1, 1000) > 0) {
            long n = lp_recvfrom((int)fd, r, sizeof r, 0, NULL, NULL);
            s32 err = -1;
            if (n >= 20)
                memcpy(&err, r + 16, 4);
            ok = (err == 0);
        }
    }
    lp_close((int)fd);
    return ok;
}

/* ══ The radio switch (rfkill) ═══════════════════════════════════════
 *
 * Through sysfs rather than /dev/rfkill: each rfkill device has a "soft"
 * file that reads and writes the software block, and a "hard" one for
 * the switch or the Fn key, which nothing can override. A laptop has
 * more than one wlan rfkill (the card's own phy, and on a Dell the
 * platform's dell-rbtn) and all of them have to be unblocked for the
 * radio to work, which is also what `rfkill unblock wifi` does. */

typedef enum { RADIO_ON = 0, RADIO_SOFT, RADIO_HARD } radio_t;

typedef struct { int soft, hard, count; int set; } rfk_t;

static bool rfkill_one(const char *name, void *arg)
{
    rfk_t *r = arg;
    char path[96], v[16];
    snprintf(path, sizeof path, "/sys/class/rfkill/%s/type", name);
    if (!sys_line(path, v, sizeof v) || strcmp(v, "wlan") != 0)
        return true;
    r->count++;
    if (r->set >= 0) {
        snprintf(path, sizeof path, "/sys/class/rfkill/%s/soft", name);
        long fd = lp_open(path, O_WRONLY, 0);
        if (fd >= 0) {
            lp_write((int)fd, r->set ? "1" : "0", 1);
            lp_close((int)fd);
        }
    }
    snprintf(path, sizeof path, "/sys/class/rfkill/%s/soft", name);
    if (sys_line(path, v, sizeof v) && strcmp(v, "1") == 0)
        r->soft++;
    snprintf(path, sizeof path, "/sys/class/rfkill/%s/hard", name);
    if (sys_line(path, v, sizeof v) && strcmp(v, "1") == 0)
        r->hard++;
    return true;
}

static radio_t rfkill(int set)
{
    rfk_t r = { 0, 0, 0, set };
    each_entry("/sys/class/rfkill", rfkill_one, &r);
    return r.hard ? RADIO_HARD : r.soft ? RADIO_SOFT : RADIO_ON;
}

/* ══ Saved networks ══════════════════════════════════════════════════ */

typedef struct {
    u8   ssid[NL_SSID_MAX];
    u8   ssid_len;
    bool open;
    bool have_pmk;
    u8   pmk[WPA_PMK_LEN];
    /* The passphrase itself, kept only because WPA3 needs it: SAE proves
     * knowledge of the password, not of a PMK, so a WPA3-only network
     * cannot be joined from the hash alone (see "WPA3" below). */
    bool have_pass;
    char pass[64];
    int  priority;
    bool hidden;
    bool disabled;          /* a forgotten boot-card network: skip it   */
    bool provisioned;       /* from /etc/wpa.conf; never written back   */
    u32  saved_by;
    s64  last_used;
    /* this run only */
    bool pw_rejected;       /* the AP refused this PMK: do not retry it */
} net_t;

static net_t NETS[MAX_NETS];
static int   NN;
static char  confdir[64], netfile[96], radiofile[96];

static bool ssid_eq(const u8 *a, size_t an, const u8 *b, size_t bn)
{
    return an == bn && memcmp(a, b, an) == 0;
}

static net_t *net_find(const u8 *ssid, size_t n, bool provisioned)
{
    for (int i = 0; i < NN; i++)
        if (NETS[i].provisioned == provisioned &&
            ssid_eq(NETS[i].ssid, NETS[i].ssid_len, ssid, n))
            return &NETS[i];
    return NULL;
}

/* A value after "key=", either "quoted text" (between the first and the
 * LAST quote, as wpa_supplicant reads it) or bare hex. */
static bool conf_bytes(const char *v, u8 *out, size_t cap, size_t *n,
                       bool *was_quoted)
{
    *was_quoted = false;
    if (v[0] == '"') {
        const char *end = strrchr(v + 1, '"');
        if (!end)
            return false;
        size_t len = (size_t)(end - (v + 1));
        if (len > cap)
            return false;
        memcpy(out, v + 1, len);
        *n = len;
        *was_quoted = true;
        return true;
    }
    char hex[2 * 64 + 1];
    size_t i = 0;
    for (; v[i] && v[i] != ' ' && v[i] != '\t' && i < sizeof hex - 1; i++)
        hex[i] = v[i];
    hex[i] = '\0';
    return wpa_hex_decode(hex, out, cap, n);
}

/* Parse network={...} blocks out of a wpa_supplicant-style file into
 * NETS. Our own comment line "# lp-net saved_by=N last_used=N" carries
 * what wpa_supplicant has no field for. Returns how many were added. */
static int conf_load(const char *path, bool provisioned)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return 0;

    int added = 0;
    bool in = false, bad = false, key_none = false, have_psk_text = false;
    char psk_text[80];
    net_t cur;
    char line[512];

    while (readline((int)fd, line, sizeof line) >= 0) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!in) {
            if (strncmp(p, "network={", 9) == 0) {
                in = true;
                bad = key_none = have_psk_text = false;
                memset(&cur, 0, sizeof cur);
                cur.provisioned = provisioned;
            }
            continue;
        }
        if (p[0] == '}') {
            in = false;
            if (bad || cur.ssid_len == 0 || NN >= MAX_NETS)
                continue;
            /* The shipped example, which is not a network anybody has. */
            if (ssid_eq(cur.ssid, cur.ssid_len,
                        (const u8 *)"YOUR_NETWORK_NAME", 17))
                continue;
            if (key_none) {
                cur.open = true;
            } else if (have_psk_text) {
                if (wpa_pmk_from_psk(psk_text, cur.ssid, cur.ssid_len,
                                     cur.pmk) != WPA_PSK_OK)
                    continue;
                cur.have_pmk = true;
                if (!cur.have_pass) {
                    strlcpy(cur.pass, psk_text, sizeof cur.pass);
                    cur.have_pass = true;
                }
            }
            wpa_wipe(psk_text, sizeof psk_text);
            if (!cur.open && !cur.have_pmk && !cur.disabled)
                continue;
            /* A later block for the same SSID replaces an earlier one,
             * which is how wpa_supplicant's own files end up read too. */
            net_t *old = net_find(cur.ssid, cur.ssid_len, provisioned);
            if (old)
                *old = cur;
            else
                NETS[NN++] = cur, added++;
            continue;
        }
        if (strncmp(p, "# lp-net ", 9) == 0) {
            char *s = strstr(p, "saved_by=");
            if (s) cur.saved_by = (u32)atoi(s + 9);
            s = strstr(p, "last_used=");
            if (s) cur.last_used = strtoll(s + 10, NULL, 10);
            continue;
        }
        if (p[0] == '#')
            continue;

        bool q;
        size_t n;
        if (strncmp(p, "ssid=", 5) == 0) {
            if (!conf_bytes(p + 5, cur.ssid, NL_SSID_MAX, &n, &q))
                bad = true;
            else
                cur.ssid_len = (u8)n;
        } else if (strncmp(p, "psk=", 4) == 0) {
            if (p[4] == '"') {
                const char *end = strrchr(p + 5, '"');
                size_t len = end ? (size_t)(end - (p + 5)) : 0;
                if (!end || len >= sizeof psk_text) { bad = true; continue; }
                memcpy(psk_text, p + 5, len);
                psk_text[len] = '\0';
                have_psk_text = true;
            } else if (conf_bytes(p + 4, cur.pmk, WPA_PMK_LEN, &n, &q) &&
                       n == WPA_PMK_LEN) {
                cur.have_pmk = true;
            } else {
                bad = true;
            }
        } else if (strncmp(p, "sae_password=\"", 14) == 0) {
            const char *end = strrchr(p + 14, '"');
            size_t len = end ? (size_t)(end - (p + 14)) : 0;
            if (end && len > 0 && len < sizeof cur.pass) {
                memcpy(cur.pass, p + 14, len);
                cur.pass[len] = '\0';
                cur.have_pass = true;
            }
        } else if (strncmp(p, "key_mgmt=", 9) == 0) {
            key_none = strncmp(p + 9, "NONE", 4) == 0;
        } else if (strncmp(p, "priority=", 9) == 0) {
            cur.priority = atoi(p + 9);
        } else if (strncmp(p, "scan_ssid=", 10) == 0) {
            cur.hidden = atoi(p + 10) != 0;
        } else if (strncmp(p, "disabled=", 9) == 0) {
            cur.disabled = atoi(p + 9) != 0;
        }
    }
    wpa_wipe(line, sizeof line);
    lp_close((int)fd);
    return added;
}

/* Write our networks (not the provisioned ones) atomically: a temp file
 * created 0600 from the start - never a window where the keys are
 * readable - fsync, rename over the old, fsync the directory. */
static bool conf_save(char *why, size_t whyn)
{
    lp_mkdir(confdir, 0700);
    lp_chmod(confdir, 0700);

    static char text[MAX_NETS * 400 + 1024];
    size_t o = 0;
    o += (size_t)snprintf(text + o, sizeof text - o,
        "# Saved Wi-Fi networks. Written by the wpa daemon when a connection\n"
        "# succeeds (lp-net connect) and by lp-net forget; read by wpa at\n"
        "# start, and by wpa_supplicant as it is when the fallback is on.\n"
        "# psk is the 64-hex key the passphrase maps to, not the passphrase.\n"
        "# A hand-written psk=\"passphrase\" is accepted too. sae_password is\n"
        "# the passphrase itself, which WPA3 (SAE) cannot do without; it is\n"
        "# kept for every network joined with a typed password, so a router\n"
        "# switched to WPA3 later is still joined.\n");

    for (int i = 0; i < NN && o + 400 < sizeof text; i++) {
        const net_t *n = &NETS[i];
        if (n->provisioned)
            continue;
        o += (size_t)snprintf(text + o, sizeof text - o, "\nnetwork={\n");

        bool quotable = n->ssid_len > 0;
        for (int k = 0; k < n->ssid_len; k++)
            if (n->ssid[k] < 0x20 || n->ssid[k] == 0x7f)
                quotable = false;
        if (quotable) {
            char s[NL_SSID_MAX + 1];
            memcpy(s, n->ssid, n->ssid_len);
            s[n->ssid_len] = '\0';
            o += (size_t)snprintf(text + o, sizeof text - o,
                                  "\tssid=\"%s\"\n", s);
        } else {
            char h[2 * NL_SSID_MAX + 1];
            wpa_hex_encode(n->ssid, n->ssid_len, h);
            o += (size_t)snprintf(text + o, sizeof text - o, "\tssid=%s\n", h);
        }
        if (n->open) {
            o += (size_t)snprintf(text + o, sizeof text - o,
                                  "\tkey_mgmt=NONE\n");
        } else if (n->have_pmk) {
            char h[2 * WPA_PMK_LEN + 1];
            wpa_hex_encode(n->pmk, WPA_PMK_LEN, h);
            o += (size_t)snprintf(text + o, sizeof text - o,
                                  "\tkey_mgmt=WPA-PSK\n\tpsk=%s\n", h);
            wpa_wipe(h, sizeof h);
        }
        if (!n->open && n->have_pass)
            o += (size_t)snprintf(text + o, sizeof text - o,
                                  "\tsae_password=\"%s\"\n", n->pass);
        if (n->priority)
            o += (size_t)snprintf(text + o, sizeof text - o,
                                  "\tpriority=%d\n", n->priority);
        if (n->hidden)
            o += (size_t)snprintf(text + o, sizeof text - o, "\tscan_ssid=1\n");
        if (n->disabled)
            o += (size_t)snprintf(text + o, sizeof text - o, "\tdisabled=1\n");
        o += (size_t)snprintf(text + o, sizeof text - o,
                              "\t# lp-net saved_by=%u last_used=%lld\n}\n",
                              (unsigned)n->saved_by, (long long)n->last_used);
    }

    char tmp[112];
    snprintf(tmp, sizeof tmp, "%s.tmp", netfile);
    lp_unlink(tmp);
    long fd = lp_open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL, 0600);
    bool ok = fd >= 0;
    if (ok) {
        size_t done = 0;
        while (ok && done < o) {
            long w = lp_write((int)fd, text + done, o - done);
            if (w <= 0) ok = false; else done += (size_t)w;
        }
        if (ok && lp_fsync((int)fd) < 0)
            ok = false;
        lp_close((int)fd);
    }
    wpa_wipe(text, o);
    if (ok && lp_rename(tmp, netfile) < 0)
        ok = false;
    if (!ok) {
        snprintf(why, whyn, "cannot write %s (%ld)", netfile,
                 fd < 0 ? -fd : 0L);
        lp_unlink(tmp);
        return false;
    }
    long dfd = lp_open(confdir, O_RDONLY | O_DIRECTORY, 0);
    if (dfd >= 0) {
        lp_fsync((int)dfd);
        lp_close((int)dfd);
    }
    return true;
}

static void conf_load_all(void)
{
    char buf[64];
    lp_setting_path("lp-net", buf, sizeof buf);
    strlcpy(confdir, buf, sizeof confdir);
    snprintf(netfile, sizeof netfile, "%s/networks", confdir);
    snprintf(radiofile, sizeof radiofile, "%s/radio", confdir);

    NN = 0;
    int ours = conf_load(netfile, false);
    int boot = conf_load("/etc/wpa.conf", true);

    /* A boot-card network that was forgotten here stays forgotten. */
    for (int i = 0; i < NN; i++) {
        if (!NETS[i].provisioned)
            continue;
        net_t *t = net_find(NETS[i].ssid, NETS[i].ssid_len, false);
        if (t && t->disabled) {
            NETS[i] = NETS[--NN];
            i--;
            boot--;
        }
    }
    JLOG("%d saved network%s in %s%s", ours, ours == 1 ? "" : "s", netfile,
         boot > 0 ? ", and from /etc/wpa.conf:" : "");
    if (boot > 0)
        JLOG("  %d more from the boot card's wpa_supplicant.conf", boot);
}

static bool radio_saved_on(void)
{
    char v[8];
    if (!sys_line(radiofile, v, sizeof v))
        return true;
    return strcmp(v, "off") != 0;
}

static void radio_save(bool on)
{
    lp_mkdir(confdir, 0700);
    char tmp[112];
    snprintf(tmp, sizeof tmp, "%s.tmp", radiofile);
    long fd = lp_open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    lp_write((int)fd, on ? "on\n" : "off\n", on ? 3 : 4);
    lp_fsync((int)fd);
    lp_close((int)fd);
    lp_rename(tmp, radiofile);
    long dfd = lp_open(confdir, O_RDONLY | O_DIRECTORY, 0);
    if (dfd >= 0) { lp_fsync((int)dfd); lp_close((int)dfd); }
}

/* ══ Scan results ════════════════════════════════════════════════════ */

typedef enum { SEC_OPEN, SEC_WPA2, SEC_WPA3, SEC_WEP, SEC_8021X, SEC_WPA1 } sec_t;
static const char *SEC_NAME[] = { "open", "wpa2", "wpa3", "wep", "8021x", "wpa1" };

typedef struct {
    nl_bss_t    b;
    sec_t       sec;
    const char *refuse;         /* NULL: we can join it; else a msg code */
    char        detail[64];     /* the %2 of "cipher"                     */
    u32         group;          /* the group cipher our IE must name      */
} bss_t;

static nl_t     NL;                  /* ~9 KB: static, never on the stack */
static nl_bss_t RAW[MAX_BSS];
static bss_t    BSS[MAX_BSS];
static int      NB;

/* What a network is, and whether this stack can join it. */
static void classify(bss_t *x)
{
    x->refuse = NULL;
    x->detail[0] = '\0';
    x->group = WLAN_CIPHER_SUITE_CCMP;
    const nl_bss_t *b = &x->b;

    if (b->rsn_ie_len >= 2) {
        nl_rsn_t r;
        x->sec = SEC_WPA2;
        if (b->rsn_ie_total > b->rsn_ie_len ||
            !nl_rsn_parse(b->rsn_ie, b->rsn_ie_len, &r)) {
            x->refuse = "cipher";
            strlcpy(x->detail, "its RSN element is malformed", sizeof x->detail);
            return;
        }
        bool psk = false, sae = false, dot1x = false;
        for (int i = 0; i < r.n_akm; i++) {
            if (r.akm[i] == WLAN_AKM_SUITE_PSK) psk = true;
            else if (r.akm[i] == WLAN_AKM_SUITE_SAE ||
                     r.akm[i] == 0x000FAC09 /* FT-SAE */) sae = true;
            else if (r.akm[i] == WLAN_AKM_SUITE_8021X ||
                     r.akm[i] == WLAN_AKM_SUITE_FT_8021X ||
                     r.akm[i] == WLAN_AKM_SUITE_8021X_SHA256) dot1x = true;
        }
        if (!psk) {
            if (sae)        { x->sec = SEC_WPA3;  x->refuse = "wpa3_only"; }
            else if (dot1x) { x->sec = SEC_8021X; x->refuse = "enterprise"; }
            else {
                x->refuse = "cipher";
                snprintf(x->detail, sizeof x->detail, "key management %s",
                         r.n_akm ? nl_akm_name(r.akm[0]) : "none");
            }
            return;
        }
        bool ccmp = false;
        for (int i = 0; i < r.n_pairwise; i++)
            if (r.pairwise[i] == WLAN_CIPHER_SUITE_CCMP)
                ccmp = true;
        if (!ccmp) {
            x->refuse = "cipher";
            snprintf(x->detail, sizeof x->detail, "pairwise %s only",
                     r.n_pairwise ? nl_cipher_name(r.pairwise[0]) : "none");
            return;
        }
        if (r.group_cipher != WLAN_CIPHER_SUITE_CCMP &&
            r.group_cipher != WLAN_CIPHER_SUITE_TKIP) {
            x->refuse = "cipher";
            snprintf(x->detail, sizeof x->detail, "group %s",
                     nl_cipher_name(r.group_cipher));
            return;
        }
        if (r.mfp_required) {
            x->refuse = "mfp_required";
            return;
        }
        x->group = r.group_cipher;
        return;
    }
    if (b->wpa_ie_len) {
        x->sec = SEC_WPA1;
        x->refuse = "wpa1";
        return;
    }
    if (b->capability & 0x0010) {
        x->sec = SEC_WEP;
        x->refuse = "wep";
        return;
    }
    x->sec = SEC_OPEN;
}

static int bss_signal(const nl_bss_t *b)
{
    if (b->signal_mbm)
        return (int)(b->signal_mbm / 100);
    if (b->signal_unspec)
        return -100 + b->signal_unspec / 2;
    return 0;
}

static void results_load(void)
{
    int n = nl_scan_results(&NL, RAW, MAX_BSS);
    if (n < 0) {
        JLOG("reading the scan results failed: %s", nl_error(&NL));
        return;
    }
    NB = 0;
    for (int i = 0; i < n; i++) {
        BSS[NB].b = RAW[i];
        classify(&BSS[NB]);
        NB++;
    }
}

/* Was this BSSID ever seen with an empty (or all-zero) SSID? Then it is
 * a hidden network, found by asking for it by name. */
static bool bssid_hidden(const u8 bssid[6])
{
    for (int i = 0; i < NB; i++) {
        if (memcmp(BSS[i].b.bssid, bssid, 6) != 0)
            continue;
        bool zero = true;
        for (int k = 0; k < BSS[i].b.ssid_len; k++)
            if (BSS[i].b.ssid[k]) zero = false;
        if (zero)
            return true;
    }
    return false;
}

/* ══ The state ═══════════════════════════════════════════════════════ */

static eapol_t EP;
static char    IFN[16];
static int     IFX;
static u8      MAC[6];
static char    DRIVER[32];
static char    PIN_IF[16];            /* -i */
static radio_t RADIO = RADIO_ON;
static bool    radio_want = true;

typedef enum {
    ST_IDLE, ST_SCANNING, ST_CONNECTING, ST_HANDSHAKE, ST_DHCP, ST_CONNECTED
} st_t;
static const char *ST_NAME[] = {
    "disconnected", "scanning", "connecting", "handshake", "dhcp", "connected"
};
static st_t ST = ST_IDLE;
static s64  st_since;

/* Why we are not connected. err_failed says it was an attempt that
 * failed (state "failed"); otherwise it is only an explanation of
 * "disconnected" (the radio is off, nothing saved is in range). */
static bool err_failed;
static char err_code[24], err_a1[96], err_a2[200];

static bool user_off;                 /* `disconnect`: until `connect`  */
static bool expect_disconnect;        /* our own DISCONNECT is coming   */

/* The attempt in progress. */
static struct {
    net_t net;                        /* a copy; for a trial, the only one */
    bool  trial;                      /* save it when the handshake works */
    u32   uid;
    bss_t bss;
    u8    bssid[6];
    u8    own_ie[WPA_IE_MAX];
    size_t own_ie_len;
    s64   deadline;
    s64   keys_at;
    bool  said_slow;
    u32   addr;
    int   signal;
    wpa_sm_t sm;
    char  ssid[80];                   /* for messages */
} C;

/* A connection somebody asked for by name. */
static struct {
    bool  on;
    net_t net;
    bool  trial;
    bool  hidden;
    u32   uid;
    int   scans;
} REQ;

static bool scanning, scan_for_connect;
static s64  scan_deadline, scan_retry_at;
static char scan_ssid[NL_SSID_MAX + 1];
static s64  next_scan_at;
static u32  idle_scan_ms = IDLE_SCAN_MIN_MS;
static s64  retry_at;
static u32  retry_ms;
static int  hidden_rr;
static s64  next_iface_check, next_signal, next_addr;

static void set_state(st_t s)
{
    if (ST != s)
        st_since = lp_monotonic_ms();
    ST = s;
}

static void set_err(const char *code, const char *a1, const char *a2,
                    bool failed)
{
    err_failed = failed;
    strlcpy(err_code, code ? code : "", sizeof err_code);
    strlcpy(err_a1, a1 ? a1 : "", sizeof err_a1);
    strlcpy(err_a2, a2 ? a2 : "", sizeof err_a2);
    if (code) {
        char m[400];
        wpa_msg_format(m, sizeof m, code, err_a1, err_a2, "", false);
        if (failed)
            JNOTE("stopped: %s", m);
        else
            JLOG("%s", m);
    }
}

static void clear_err(void) { set_err(NULL, NULL, NULL, false); }

static void attempt_wipe(void)
{
    wpa_wipe(&C.sm, sizeof C.sm);
    wpa_wipe(&C.net, sizeof C.net);
}

/* ══ WPA3: one connection handed to wpa_supplicant ═══════════════════
 *
 * Our handshake is WPA2-PSK's four messages. A network that takes only
 * WPA3 (SAE: the password proved by an elliptic-curve exchange before
 * the association, no PSK at all) or that requires protected management
 * frames is joined instead by Debian's wpa_supplicant, which does both -
 * for that one connection, with a one-network configuration written for
 * it. Everything else stays here: the scan and the list the desktop
 * shows, the request, the state and its messages, saving the network,
 * the address (dhcp waits for the link to go "up", which wpa_supplicant
 * does the same way we do). While it holds the link our handshake keeps
 * out of the way: CONNECT and DISCONNECT events are only watched, EAPOL
 * frames are left to it. It stops when the link is left for any reason
 * (drop_link) - another network, Disconnect, the radio, the daemon
 * stopping - and ends its own connection on the way out.
 *
 * Around routers, WPA3-only is now common (phones' hotspots, recent
 * ISP routers' 5 GHz networks); this was the reason Wi-Fi "did not
 * work" next to them. */
#define SAE_SUPPLICANT "/usr/sbin/wpa_supplicant"
#define SAE_CONF       "/run/lp-net-sae.conf"
#define SAE_CTRL       "/run/lp-net-sae"
#define SAE_LOG        "/run/lp-net-sae.log"
#define SAE_JOIN_MS    30000          /* SAE's exchange, then the four messages */

static struct {
    bool on;
    long pid;
    int  rejects;                     /* refusals seen meanwhile */
} DG;

static bool sae_capable(void) { return lp_exists(SAE_SUPPLICANT); }

/* A network our own handshake cannot do, and the delegate can. */
static bool delegatable(const bss_t *b)
{
    return b->refuse && (strcmp(b->refuse, "wpa3_only") == 0 ||
                         strcmp(b->refuse, "mfp_required") == 0) &&
           sae_capable();
}

/* ...with the key it needs: SAE the passphrase, PSK with 802.11w the PMK. */
static bool delegate_has_key(const bss_t *b, const net_t *n)
{
    return strcmp(b->refuse, "wpa3_only") == 0 ? n->have_pass : n->have_pmk;
}

static void stop_delegate(const char *why)
{
    if (!DG.on)
        return;
    DG.on = false;
    JLOG("stopping wpa_supplicant (%s)", why);
    /* it leaves the access point on its way out: that DISCONNECT is ours */
    expect_disconnect = true;
    lp_kill((pid_t)DG.pid, SIGTERM_);
    int st = 0;
    for (int i = 0; i < 40 && DG.pid > 0; i++) {
        if (lp_waitpid((pid_t)DG.pid, &st, WNOHANG) == (pid_t)DG.pid)
            DG.pid = 0;
        else
            lp_sleep_ms(50);
    }
    if (DG.pid > 0) {
        lp_kill((pid_t)DG.pid, 9);
        lp_waitpid((pid_t)DG.pid, &st, 0);
        DG.pid = 0;
    }
    lp_unlink(SAE_CONF);
    /* it put the link mode back to the default as it left */
    if (IFX)
        rtnl_setlink(IFX, 1, -1);
}

/* How many times wpa_supplicant's log says this. Two verdicts there are
 * worth reading before the clock runs out, both a wrong password said as
 * such: SAE's confirm refused by the access point (CTRL-EVENT-AUTH-REJECT
 * with auth_type=3, SAE, auth_transaction=2, the confirm - the commit
 * went through, so the AP is there and speaks SAE, and what it did not
 * accept is the proof of the password), and "WRONG_KEY" from a four-way
 * handshake that failed the same way. */
static int sae_log_count(const char *what)
{
    static char buf[16384];
    long n = proc_read(SAE_LOG, buf, sizeof buf - 1);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    int k = 0;
    for (const char *p = buf; (p = strstr(p, what)) != NULL; p++)
        k++;
    return k;
}

/* Leave the current association, if there is one, and say that the
 * DISCONNECT event this causes is ours and not news. */
static void drop_link(u16 reason)
{
    if (!IFN[0])
        return;
    stop_delegate("leaving the network");
    if (nl_disconnect(&NL, reason))
        expect_disconnect = true;
}

static void schedule_retry(void)
{
    retry_ms = retry_ms ? retry_ms * 2 : RETRY_MIN_MS;
    if (retry_ms > RETRY_MAX_MS)
        retry_ms = RETRY_MAX_MS;
    retry_at = lp_monotonic_ms() + retry_ms;
}

/* The attempt ended badly: record why, tidy up, and decide when to try
 * again. The message code IS the "at which step, and why". */
static void fail(const char *code, const char *a1, const char *a2)
{
    bool linked = ST == ST_CONNECTING || ST == ST_HANDSHAKE ||
                  ST == ST_DHCP || ST == ST_CONNECTED;
    set_err(code, a1, a2, true);

    if (strcmp(code, "wrong_password") == 0) {
        /* Do not keep offering the AP a key it refused: some lock a
         * station out after a few, and the answer will not change. A
         * new password from `lp-net connect` clears this. */
        for (int i = 0; i < NN; i++)
            if (ssid_eq(NETS[i].ssid, NETS[i].ssid_len,
                        C.net.ssid, C.net.ssid_len) &&
                memcmp(NETS[i].pmk, C.net.pmk, WPA_PMK_LEN) == 0)
                NETS[i].pw_rejected = true;
    }
    if (linked)
        drop_link(REASON_UNSPECIFIED);

    REQ.on = false;
    attempt_wipe();
    set_state(ST_IDLE);
    schedule_retry();
}

/* ══ The interface ═══════════════════════════════════════════════════ */

/* Wireless interfaces that are not stations - hostapd's access point,
 * a monitor interface - found when attach() asked. sysfs cannot tell a
 * station from an AP (both are type 1 with a phy80211 link), so without
 * this list the preferred name would be picked, refused and picked
 * again every two seconds while a station interface next to it waited.
 * Forgotten after a minute, because a mode is not for ever. */
#define NOT_STA_MAX 4
static struct { char name[16]; s64 until; } NOT_STA[NOT_STA_MAX];

static void not_sta_add(const char *name)
{
    int k = 0;
    for (int i = 0; i < NOT_STA_MAX; i++)
        if (NOT_STA[i].until < NOT_STA[k].until)
            k = i;
    strlcpy(NOT_STA[k].name, name, sizeof NOT_STA[k].name);
    NOT_STA[k].until = lp_monotonic_ms() + 60000;
}

static bool not_sta(const char *name)
{
    s64 now = lp_monotonic_ms();
    for (int i = 0; i < NOT_STA_MAX; i++)
        if (NOT_STA[i].until > now && strcmp(NOT_STA[i].name, name) == 0)
            return true;
    return false;
}

static bool pick_iface(const char *name, void *arg)
{
    char *out = arg;
    char path[96];
    snprintf(path, sizeof path, "/sys/class/net/%s/phy80211", name);
    if (!lp_exists(path) || not_sta(name))
        return true;
    /* wlan0 before anything else; otherwise the first in order. */
    if (!out[0] || strcmp(name, "wlan0") == 0 ||
        (strcmp(out, "wlan0") != 0 && strcmp(name, out) < 0))
        strlcpy(out, name, 16);
    return true;
}

static void detach(const char *why)
{
    if (!IFN[0])
        return;
    JNOTE("letting go of %s: %s", IFN, why);
    stop_delegate(why);
    rtnl_setlink(IFX, 0, -1);
    eapol_close(&EP);
    nl_close(&NL);
    IFN[0] = '\0';
    IFX = 0;
    attempt_wipe();
    set_state(ST_IDLE);
    scanning = false;
}

static bool attach(const char *name)
{
    if (!nl_open(&NL, name)) {
        int e = nl_errno(&NL);
        if (e == 2)
            set_err("no_cfg80211", nl_error(&NL), NULL, false);
        else
            set_err("iface", name, nl_error(&NL), false);
        return false;
    }
    nl_iface_t info;
    if (nl_interface_info(&NL, &info) && info.iftype != NL80211_IFTYPE_STATION) {
        /* An access point (hostapd's radio in the test VM) or a monitor
         * interface is somebody else's. */
        JLOG("%s is in %s mode, not a station - leaving it alone", name,
             nl_iftype_name(info.iftype));
        nl_close(&NL);
        not_sta_add(name);
        return false;
    }
    int err = 0;
    if (!eapol_open(&EP, name, &err)) {
        char e[48];
        snprintf(e, sizeof e, "the EAPOL socket: %s", lp_strerror(err));
        set_err("iface", name, e, false);
        nl_close(&NL);
        return false;
    }
    strlcpy(IFN, name, sizeof IFN);
    IFX = (int)EP.ifindex;
    net_if_hwaddr(IFN, MAC);

    char path[96], link[160];
    snprintf(path, sizeof path, "/sys/class/net/%s/device/driver", IFN);
    long n = lp_readlink(path, link, sizeof link - 1);
    DRIVER[0] = '\0';
    if (n > 0) {
        link[n] = '\0';
        const char *s = strrchr(link, '/');
        strlcpy(DRIVER, s ? s + 1 : link, sizeof DRIVER);
    }

    /* Associated but not yet keyed reads as "dormant" from now on; see
     * "The address" at the top. */
    if (!rtnl_setlink(IFX, 1, -1))
        JLOG("could not set %s's link mode to dormant - dhcp may ask"
             " before the keys are in", IFN);

    char m[18];
    mac_str(MAC, m);
    JNOTE("using %s (driver %s, address %s)", IFN,
          DRIVER[0] ? DRIVER : "unknown", m);

    /* Start from nothing: a connection left by an earlier run of this
     * daemon (or by wpa_supplicant) has keys we do not know. */
    net_if_up(IFN);
    drop_link(REASON_DEAUTH_LEAVING);
    set_state(ST_IDLE);
    clear_err();
    next_scan_at = lp_monotonic_ms() + 300;
    idle_scan_ms = IDLE_SCAN_MIN_MS;
    return true;
}

static void find_iface(void)
{
    /* Each refusal puts that interface on the NOT_STA list, so the next
     * round picks the one after it; a station next to an AP is found in
     * this call rather than a sleep later. */
    for (int round = 0; round <= NOT_STA_MAX; round++) {
        char name[16] = "";
        if (PIN_IF[0]) {
            char path[64];
            snprintf(path, sizeof path, "/sys/class/net/%s", PIN_IF);
            if (lp_exists(path))
                strlcpy(name, PIN_IF, sizeof name);
        } else {
            each_entry("/sys/class/net", pick_iface, name);
        }
        if (!name[0]) {
            if (strcmp(err_code, "no_iface") != 0)
                set_err("no_iface", NULL, NULL, false);
            return;
        }
        if (attach(name) || PIN_IF[0] || !not_sta(name))
            return;
    }
}

/* ══ Scanning ════════════════════════════════════════════════════════ */

static void start_scan(const u8 *ssid, size_t n, bool for_connect)
{
    if (scanning)
        return;
    scan_ssid[0] = '\0';
    if (ssid && n) {
        memcpy(scan_ssid, ssid, n);
        scan_ssid[n] = '\0';
    }
    if (!net_if_is_up(IFN))
        net_if_up(IFN);
    if (!nl_scan_trigger(&NL, scan_ssid[0] ? scan_ssid : NULL)) {
        /* EBUSY: the kernel is already scanning (its own SME does
         * before a connect, and so does a scan somebody else asked
         * for). Its results will arrive as an event all the same. */
        if (nl_errno(&NL) == 16) {
            scanning = true;
            scan_for_connect = for_connect;
            scan_deadline = lp_monotonic_ms() + SCAN_TIMEOUT_MS;
            return;
        }
        set_err("scan_failed", nl_error(&NL), NULL, for_connect);
        scan_retry_at = lp_monotonic_ms() + 2000;
        if (for_connect) {
            set_state(ST_IDLE);
            schedule_retry();
        }
        return;
    }
    scanning = true;
    scan_for_connect = for_connect;
    scan_deadline = lp_monotonic_ms() + SCAN_TIMEOUT_MS;
    if (for_connect)
        set_state(ST_SCANNING);
    if (scan_ssid[0]) {
        char s[80];
        ssid_str(ssid, n, s, sizeof s);
        JLOG("scanning, and asking for \"%s\" by name", s);
    } else {
        JTRACE("scanning");
    }
}

/* ══ Connecting ══════════════════════════════════════════════════════ */

static void begin_connect(const bss_t *b, const net_t *n, bool trial, u32 uid)
{
    memset(&C, 0, sizeof C);
    C.net = *n;
    C.trial = trial;
    C.uid = uid;
    C.bss = *b;
    memcpy(C.bssid, b->b.bssid, 6);
    ssid_str(n->ssid, n->ssid_len, C.ssid, sizeof C.ssid);
    C.signal = bss_signal(&b->b);

    eapol_flush(&EP);
    expect_disconnect = false;

    nl_conn_t req;
    if (n->open) {
        memset(&req, 0, sizeof req);
        req.ssid = C.net.ssid;
        req.ssid_len = C.net.ssid_len;
        req.bssid = C.bssid;
        req.freq = b->b.freq;
        req.auth_type = NL80211_AUTHTYPE_OPEN_SYSTEM;
    } else {
        nl_conn_wpa2_psk(&req, C.net.ssid, C.net.ssid_len, C.bssid, b->b.freq);
        req.cipher_group = b->group;
        C.own_ie_len = wpa_rsn_ie_build(C.own_ie, sizeof C.own_ie, b->group);
        req.ie = C.own_ie;
        req.ie_len = (u16)C.own_ie_len;
    }

    char m[18];
    mac_str(C.bssid, m);
    JNOTE("joining \"%s\" at %s, %u MHz, %d dBm (%s)", C.ssid, m,
          (unsigned)b->b.freq, C.signal,
          n->open ? "open" : b->group == WLAN_CIPHER_SUITE_TKIP
                             ? "WPA2-PSK, CCMP with a TKIP group key"
                             : "WPA2-PSK, CCMP");

    set_state(ST_CONNECTING);
    C.deadline = lp_monotonic_ms() + CONNECT_TIMEOUT_MS;
    if (!net_if_is_up(IFN))
        net_if_up(IFN);
    if (!nl_connect(&NL, &req)) {
        /* EALREADY: still joined to something - leave it, try again. */
        if (nl_errno(&NL) == 114) {
            drop_link(REASON_DEAUTH_LEAVING);
            set_state(ST_IDLE);
            retry_at = lp_monotonic_ms() + 500;
            return;
        }
        char e[160];
        strlcpy(e, nl_error(&NL), sizeof e);
        fail("connect_refused", C.ssid, e);
    }
}

/* Hand this one connection to wpa_supplicant (see "WPA3" above). */
static void begin_delegate(const bss_t *b, const net_t *n, bool trial, u32 uid)
{
    memset(&C, 0, sizeof C);
    C.net = *n;
    C.trial = trial;
    C.uid = uid;
    C.bss = *b;
    memcpy(C.bssid, b->b.bssid, 6);
    ssid_str(n->ssid, n->ssid_len, C.ssid, sizeof C.ssid);
    C.signal = bss_signal(&b->b);
    eapol_flush(&EP);
    expect_disconnect = false;
    DG.rejects = 0;
    bool sae = strcmp(b->refuse, "wpa3_only") == 0;

    lp_mkdir(SAE_CTRL, 0700);
    lp_unlink(SAE_LOG);
    long fd = lp_open(SAE_CONF, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fail("internal", "could not write " SAE_CONF, NULL);
        return;
    }
    char h[2 * NL_SSID_MAX + 1];
    wpa_hex_encode(n->ssid, n->ssid_len, h);
    /* sae_pwe=2: either way of deriving the password element, the old
     * looping one and hash-to-element, whichever the router does. */
    dprintf((int)fd, "ctrl_interface=%s\nupdate_config=0\nsae_pwe=2\n\n"
                     "network={\n\tssid=%s\n", SAE_CTRL, h);
    if (n->hidden)
        dprintf((int)fd, "\tscan_ssid=1\n");
    if (sae) {
        dprintf((int)fd, "\tkey_mgmt=SAE\n\tieee80211w=2\n\tsae_password=\"%s\"\n",
                n->pass);
    } else {
        char k[2 * WPA_PMK_LEN + 1];
        wpa_hex_encode(n->pmk, WPA_PMK_LEN, k);
        dprintf((int)fd, "\tkey_mgmt=WPA-PSK WPA-PSK-SHA256\n\tieee80211w=2\n\tpsk=%s\n", k);
        wpa_wipe(k, sizeof k);
    }
    dprintf((int)fd, "}\n");
    lp_close((int)fd);

    if (!net_if_is_up(IFN))
        net_if_up(IFN);
    long pid = lp_fork();
    if (pid == 0) {
        for (int f = 3; f < 256; f++)
            lp_close(f);
        long nul = lp_open("/dev/null", O_RDWR, 0);
        if (nul >= 0) {
            lp_dup2((int)nul, 0);
            lp_dup2((int)nul, 1);
            lp_dup2((int)nul, 2);
        }
        char *argv[12];
        int a = 0;
        argv[a++] = (char *)"wpa_supplicant";
        argv[a++] = (char *)"-i";
        argv[a++] = IFN;
        argv[a++] = (char *)"-D";
        argv[a++] = (char *)"nl80211";
        argv[a++] = (char *)"-c";
        argv[a++] = (char *)SAE_CONF;
        argv[a++] = (char *)"-f";
        argv[a++] = (char *)SAE_LOG;
        if (trace)
            argv[a++] = (char *)"-dd";
        argv[a] = NULL;
        char *envp[] = { (char *)"PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL };
        lp_execve(SAE_SUPPLICANT, argv, envp);
        lp_exit(127);
    }
    if (pid < 0) {
        fail("internal", "could not start wpa_supplicant", NULL);
        return;
    }
    DG.on = true;
    DG.pid = pid;

    char m[18];
    mac_str(C.bssid, m);
    JNOTE("joining \"%s\" at %s, %u MHz, %d dBm (%s) - through wpa_supplicant",
          C.ssid, m, (unsigned)b->b.freq, C.signal,
          sae ? "WPA3-SAE" : "WPA2-PSK with protected management frames");
    set_state(ST_CONNECTING);
    C.deadline = lp_monotonic_ms() + SAE_JOIN_MS;
}

static void joined(void);

/* While wpa_supplicant holds the link: has it finished, given up, or
 * gone? The link going "up" is its keys being in (the same signal dhcp
 * waits for). */
static void delegate_tick(s64 now)
{
    int st = 0;
    if (lp_waitpid((pid_t)DG.pid, &st, WNOHANG) == (pid_t)DG.pid) {
        DG.on = false;
        DG.pid = 0;
        lp_unlink(SAE_CONF);
        if (IFX)
            rtnl_setlink(IFX, 1, -1);
        JNOTE("wpa_supplicant stopped by itself");
        if (ST == ST_DHCP || ST == ST_CONNECTED) {
            char s[80];
            strlcpy(s, C.ssid, sizeof s);
            set_err("lost", s, "wpa_supplicant stopped", true);
            attempt_wipe();
            set_state(ST_IDLE);
            retry_at = now + 2000;
        } else {
            fail("wpa3_failed", C.ssid, "wpa_supplicant stopped");
        }
        return;
    }
    if (ST != ST_CONNECTING && ST != ST_HANDSHAKE)
        return;
    char path[64], op[16];
    snprintf(path, sizeof path, "/sys/class/net/%s/operstate", IFN);
    if (sys_line(path, op, sizeof op) && strcmp(op, "up") == 0) {
        JNOTE("joined \"%s\" - the keys are in (wpa_supplicant)", C.ssid);
        joined();
        return;
    }
    /* Twice, not once: one refused confirm can be a frame lost in the air. */
    if (sae_log_count("auth_type=3 auth_transaction=2 status_code=1") >= 2 ||
        sae_log_count("reason=WRONG_KEY") >= 1) {
        fail("wrong_password", C.ssid, NULL);
        return;
    }
    if (now > C.deadline) {
        char s[32];
        snprintf(s, sizeof s, "no answer in %d s", SAE_JOIN_MS / 1000);
        fail("wpa3_failed", C.ssid, s);
    }
}

/* Choose what to join from the scan cache and start. */
static void choose_and_connect(void)
{
    if (REQ.on) {
        char s[80];
        ssid_str(REQ.net.ssid, REQ.net.ssid_len, s, sizeof s);
        const bss_t *best = NULL;
        for (int i = 0; i < NB; i++) {
            const bss_t *b = &BSS[i];
            if (!ssid_eq(b->b.ssid, b->b.ssid_len, REQ.net.ssid,
                         REQ.net.ssid_len))
                continue;
            if (!best || (best->refuse && !b->refuse) ||
                (!!best->refuse == !!b->refuse &&
                 bss_signal(&b->b) > bss_signal(&best->b)))
                best = b;
        }
        if (!best) {
            /* One scan can miss an AP - a busy channel, a 5 GHz channel
             * where probing is not allowed - so ask twice. */
            if (++REQ.scans < 2) {
                start_scan(REQ.net.ssid, REQ.net.ssid_len, true);
                return;
            }
            REQ.on = false;
            set_err("not_found", s, NULL, true);
            set_state(ST_IDLE);
            schedule_retry();
            return;
        }
        if (best->refuse && !delegatable(best)) {
            REQ.on = false;
            set_err(best->refuse, s, best->detail, true);
            set_state(ST_IDLE);
            return;
        }
        net_t n = REQ.net;
        if (best->sec == SEC_OPEN) {
            n.open = true;
            n.have_pmk = false;
        } else if (n.open || !n.have_pmk) {
            REQ.on = false;
            set_err("need_password", s, NULL, true);
            set_state(ST_IDLE);
            return;
        }
        if (REQ.hidden || bssid_hidden(best->b.bssid))
            n.hidden = true;
        REQ.on = false;
        if (best->refuse) {
            /* WPA3 needs the password itself; a network saved before
             * this kept only the hash. Asking again is the way on. */
            if (!delegate_has_key(best, &n)) {
                set_err("need_password", s, NULL, true);
                set_state(ST_IDLE);
                return;
            }
            begin_delegate(best, &n, REQ.trial, REQ.uid);
            return;
        }
        begin_connect(best, &n, REQ.trial, REQ.uid);
        return;
    }

    /* Automatic: the best saved network in range. Priority first, then
     * the strongest signal - the same order wpa_supplicant uses. */
    const bss_t *best = NULL;
    const net_t *bestn = NULL;
    const bss_t *refused = NULL;
    int seen_saved = 0;
    for (int i = 0; i < NB; i++) {
        const bss_t *b = &BSS[i];
        for (int k = 0; k < NN; k++) {
            const net_t *n = &NETS[k];
            if (n->disabled || !ssid_eq(b->b.ssid, b->b.ssid_len,
                                        n->ssid, n->ssid_len))
                continue;
            seen_saved++;
            if (n->pw_rejected)
                continue;
            if (b->refuse && !(delegatable(b) && delegate_has_key(b, n))) {
                refused = b;
                continue;
            }
            if (n->open != (b->sec == SEC_OPEN))
                continue;
            if (!best || n->priority > bestn->priority ||
                (n->priority == bestn->priority &&
                 (bestn->provisioned && !n->provisioned)) ||
                (n->priority == bestn->priority &&
                 bestn->provisioned == n->provisioned &&
                 bss_signal(&b->b) > bss_signal(&best->b))) {
                best = b;
                bestn = n;
            }
        }
    }
    if (!best) {
        set_state(ST_IDLE);
        if (refused) {
            char s[80];
            ssid_str(refused->b.ssid, refused->b.ssid_len, s, sizeof s);
            if (strcmp(err_code, refused->refuse) != 0)
                set_err(refused->refuse, s, refused->detail, false);
        } else if (!err_failed && strcmp(err_code, "none_in_range") != 0 &&
                   seen_saved == 0) {
            set_err("none_in_range", NULL, NULL, false);
        }
        next_scan_at = lp_monotonic_ms() + idle_scan_ms;
        idle_scan_ms = idle_scan_ms * 2 > IDLE_SCAN_MAX_MS
                       ? IDLE_SCAN_MAX_MS : idle_scan_ms * 2;
        return;
    }
    if (best->refuse)
        begin_delegate(best, bestn, false, 0);
    else
        begin_connect(best, bestn, false, 0);
}

/* The handshake finished (or, for an open network, the association
 * did): remember the network, and start waiting for the address. */
static void joined(void)
{
    set_state(ST_DHCP);
    C.keys_at = lp_monotonic_ms();
    retry_ms = 0;
    idle_scan_ms = IDLE_SCAN_MIN_MS;
    clear_err();

    if (!rtnl_setlink(IFX, -1, IF_OPER_UP))
        JLOG("could not mark %s up - dhcp will wait for it", IFN);

    /* Save it, or refresh it. A trial becomes a saved network here, and
     * only here: the password is now known to be right. */
    net_t *n = net_find(C.net.ssid, C.net.ssid_len, false);
    bool changed = false;
    if (!n && (C.trial || !C.net.provisioned)) {
        if (NN < MAX_NETS) {
            n = &NETS[NN++];
            *n = C.net;
            n->provisioned = false;
            n->saved_by = C.uid;
            changed = true;
        }
    } else if (n && C.trial) {
        n->open = C.net.open;
        n->have_pmk = C.net.have_pmk;
        memcpy(n->pmk, C.net.pmk, WPA_PMK_LEN);
        if (C.net.have_pass) {
            n->have_pass = true;
            memcpy(n->pass, C.net.pass, sizeof n->pass);
        }
        n->disabled = false;
        changed = true;
    }
    if (n) {
        n->pw_rejected = false;
        if (C.net.hidden && !n->hidden) { n->hidden = true; changed = true; }
        s64 now = lp_time();
        /* last_used is written at most once a day unless something else
         * changed: it is a convenience, not a reason to write a file on
         * every reconnect. */
        if (changed || now - n->last_used > 86400) {
            n->last_used = now;
            changed = true;
        }
    }
    if (changed) {
        char why[160];
        if (!conf_save(why, sizeof why))
            set_err("save_failed", why, NULL, false);
        else
            JLOG("saved \"%s\" in %s", C.ssid, netfile);
    }
}

/* ══ Events from the kernel ══════════════════════════════════════════ */

static void on_connect_event(const nl_event_t *ev)
{
    if (DG.on) {
        /* wpa_supplicant's association: watched, not acted on. */
        if (ev->status != 0 || ev->timed_out) {
            DG.rejects++;
            JLOG("\"%s\": the access point refused the association (%s) -"
                 " wpa_supplicant tries again", C.ssid, nl_status_name(ev->status));
            return;
        }
        if (ev->have_bssid)
            memcpy(C.bssid, ev->bssid, 6);
        if (ST == ST_CONNECTING) {
            set_state(ST_HANDSHAKE);
            C.deadline = lp_monotonic_ms() + SAE_JOIN_MS;
            JNOTE("associated with \"%s\" - wpa_supplicant is doing the handshake", C.ssid);
        }
        return;
    }
    if (ST != ST_CONNECTING) {
        JTRACE("a CONNECT event while %s - not ours, ignored", ST_NAME[ST]);
        return;
    }
    if (ev->status != 0 || ev->timed_out) {
        char why[96];
        if (ev->timed_out) {
            snprintf(why, sizeof why, "timeout reason %u",
                     (unsigned)ev->timeout_reason);
            fail("assoc_timeout", C.ssid, why);
        } else {
            snprintf(why, sizeof why, "%s (802.11 status %u)",
                     nl_status_name(ev->status), (unsigned)ev->status);
            fail("assoc_rejected", C.ssid, why);
        }
        return;
    }
    if (ev->have_bssid)
        memcpy(C.bssid, ev->bssid, 6);

    if (C.net.open) {
        JNOTE("joined \"%s\" (open network, no handshake)", C.ssid);
        joined();
        return;
    }

    wpa_sm_reset(&C.sm);
    memcpy(C.sm.pmk, C.net.pmk, WPA_PMK_LEN);
    memcpy(C.sm.aa, C.bssid, 6);
    memcpy(C.sm.spa, MAC, 6);
    /* Message 2 repeats the RSN element our association request really
     * carried, which on a fullmac chip the firmware wrote. */
    if (ev->req_rsn_ie_len) {
        memcpy(C.sm.own_ie, ev->req_rsn_ie, ev->req_rsn_ie_len);
        C.sm.own_ie_len = ev->req_rsn_ie_len;
        if (ev->req_rsn_ie_len != C.own_ie_len ||
            memcmp(ev->req_rsn_ie, C.own_ie, C.own_ie_len) != 0)
            JLOG("the driver sent a different RSN element from ours;"
                 " using the one it sent");
    } else {
        memcpy(C.sm.own_ie, C.own_ie, C.own_ie_len);
        C.sm.own_ie_len = C.own_ie_len;
    }
    if (memcmp(C.bss.b.bssid, C.bssid, 6) == 0 && C.bss.b.rsn_ie_len <= WPA_IE_MAX) {
        memcpy(C.sm.ap_ie, C.bss.b.rsn_ie, C.bss.b.rsn_ie_len);
        C.sm.ap_ie_len = C.bss.b.rsn_ie_len;
    }
    C.sm.group_cipher = C.bss.group;

    set_state(ST_HANDSHAKE);
    C.deadline = lp_monotonic_ms() + MSG1_TIMEOUT_MS;
    JNOTE("joined \"%s\" - waiting for message 1 of 4", C.ssid);
}

static void on_disconnect_event(const nl_event_t *ev)
{
    if (expect_disconnect && !ev->by_ap) {
        expect_disconnect = false;
        JTRACE("our own disconnect went through");
        return;
    }
    expect_disconnect = false;

    char why[96];
    snprintf(why, sizeof why, "%s (reason %u)", nl_reason_name(ev->reason),
             (unsigned)ev->reason);

    if (DG.on) {
        /* wpa_supplicant joins again by itself; we only follow. */
        if (ST == ST_DHCP || ST == ST_CONNECTED)
            JNOTE("lost \"%s\": %s - wpa_supplicant is joining again", C.ssid, why);
        else
            DG.rejects++;
        set_state(ST_CONNECTING);
        C.deadline = lp_monotonic_ms() + SAE_JOIN_MS;
        return;
    }

    switch (ST) {
    case ST_HANDSHAKE:
        /* The AP sent message 1 again after our message 2 and then gave
         * up on us: it could not verify 2's MIC, and the only key that
         * goes into that MIC is the one the password makes. */
        if (C.sm.msg1_after_2 > 0 ||
            (C.sm.state == WPA_SM_WAIT_3 &&
             (ev->reason == REASON_4WAY_TIMEOUT || ev->reason == 2 ||
              ev->reason == 23)))
            fail("wrong_password", C.ssid, NULL);
        else if (C.sm.msg1_seen == 0) {
            char a2[140];
            snprintf(a2, sizeof a2, "the access point left first: %s", why);
            fail("handshake", C.ssid, a2);
        } else {
            char a2[140];
            snprintf(a2, sizeof a2, "the access point left: %s", why);
            fail("handshake", C.ssid, a2);
        }
        return;
    case ST_CONNECTING:
        fail("assoc_rejected", C.ssid, why);
        return;
    case ST_DHCP:
    case ST_CONNECTED: {
        char s[80];
        strlcpy(s, C.ssid, sizeof s);
        set_err(ev->by_ap ? "ap_left" : "lost", s, why, true);
        attempt_wipe();
        set_state(ST_IDLE);
        /* Straight back: a dropped link is usually a moment of noise,
         * and the next attempt is the diagnosis if it is not. */
        retry_ms = 0;
        retry_at = lp_monotonic_ms() + 1000;
        next_scan_at = 0;
        idle_scan_ms = IDLE_SCAN_MIN_MS;
        return;
    }
    default:
        JTRACE("disconnected while %s: %s", ST_NAME[ST], why);
        return;
    }
}

static void answer_parked_scans(void);

static void on_scan_done(bool ok)
{
    bool was_for_connect = scan_for_connect;
    scanning = false;
    scan_for_connect = false;
    if (ok)
        results_load();
    else
        JLOG("the scan was aborted");
    answer_parked_scans();
    if (was_for_connect && ST == ST_SCANNING) {
        if (!ok) {
            set_state(ST_IDLE);
            retry_at = lp_monotonic_ms() + 2000;
            return;
        }
        choose_and_connect();
    }
}

static void on_event(const nl_event_t *ev)
{
    if (ev->ifindex && (int)ev->ifindex != IFX)
        return;
    switch (ev->kind) {
    case NL_EV_CONNECT:          on_connect_event(ev);    break;
    case NL_EV_DISCONNECT:       on_disconnect_event(ev); break;
    case NL_EV_NEW_SCAN_RESULTS: on_scan_done(true);      break;
    case NL_EV_SCAN_ABORTED:     on_scan_done(false);     break;
    case NL_EV_ROAM: {
        char m[18];
        if (ev->have_bssid) {
            memcpy(C.bssid, ev->bssid, 6);
            mac_str(ev->bssid, m);
            JLOG("the firmware roamed to %s", m);
        }
        break;
    }
    default:
        if (trace && ev->kind != NL_EV_OTHER)
            JLOG("event %s", nl_cmd_name(ev->cmd));
        break;
    }
}

/* ══ EAPOL ═══════════════════════════════════════════════════════════ */

static void describe_frame(const char *dir, const u8 *f, size_t n)
{
    if (!trace)
        return;
    wpa_key_t k;
    wpa_kp_err_t e = wpa_key_parse(f, n, &k);
    if (e != WPA_KP_OK) {
        JLOG("%s EAPOL %u bytes: %s", dir, (unsigned)n, wpa_kp_error(e));
        return;
    }
    JLOG("%s EAPOL-Key %u bytes, info 0x%04x%s%s%s%s%s, replay %02x%02x%02x%02x%02x%02x%02x%02x, data %u",
         dir, (unsigned)n, k.info,
         (k.info & WPA_KI_PAIRWISE) ? " pairwise" : " group",
         (k.info & WPA_KI_ACK) ? " ack" : "",
         (k.info & WPA_KI_MIC) ? " mic" : "",
         (k.info & WPA_KI_INSTALL) ? " install" : "",
         (k.info & WPA_KI_SECURE) ? " secure" : "",
         k.replay[0], k.replay[1], k.replay[2], k.replay[3],
         k.replay[4], k.replay[5], k.replay[6], k.replay[7],
         (unsigned)k.data_len);
}

static void on_eapol(const u8 *frame, size_t len, const u8 from[6])
{
    if (DG.on)
        return;                     /* wpa_supplicant's handshake, not ours */
    if (memcmp(from, C.bssid, 6) != 0) {
        char m[18];
        mac_str(from, m);
        JTRACE("an EAPOL frame from %s, which is not our access point - ignored", m);
        return;
    }
    describe_frame("rx", frame, len);

    u8 nonce[WPA_NONCE_LEN];
    if (lp_getrandom(nonce, sizeof nonce, 0) != (long)sizeof nonce) {
        fail("internal", "no random numbers from the kernel for the nonce", NULL);
        return;
    }
    static wpa_out_t out;
    wpa_rx_t r = wpa_sm_rx(&C.sm, frame, len, nonce, &out);
    wpa_wipe(nonce, sizeof nonce);

    if (r == WPA_RX_IGNORED) {
        JTRACE("frame ignored: %s", C.sm.why);
        return;
    }
    if (r == WPA_RX_FATAL) {
        char why[200];
        strlcpy(why, C.sm.why, sizeof why);
        if (ST == ST_HANDSHAKE)
            fail("handshake", C.ssid, why);
        else
            fail("lost", C.ssid, why);
        return;
    }

    if (ST == ST_HANDSHAKE && out.msg == 1) {
        JLOG("message 1 of 4 - sending 2 of 4%s",
             C.sm.msg1_seen > 1 ? " (again: the AP did not take the last one)" : "");
        C.deadline = lp_monotonic_ms() + MSG3_TIMEOUT_MS;
    } else if (ST == ST_HANDSHAKE && out.msg == 3) {
        JLOG("message 3 of 4 verified - sending 4 of 4 and installing the keys");
    }

    if (out.flags & WPA_DO_SEND) {
        describe_frame("tx", out.tx, out.tx_len);
        if (!eapol_send(&EP, C.bssid, out.tx, out.tx_len)) {
            fail("handshake", C.ssid, "the EAPOL frame could not be sent");
            goto done;
        }
    }
    if (out.flags & WPA_DO_SET_PTK) {
        if (!nl_set_ptk(&NL, C.bssid, out.tk, WPA_TK_LEN)) {
            char e[160];
            strlcpy(e, nl_error(&NL), sizeof e);
            fail("set_key", "pairwise", e);
            goto done;
        }
        JTRACE("pairwise key installed");
    }
    if (out.flags & WPA_DO_SET_GTK) {
        if (!nl_set_gtk(&NL, out.gtk_idx, out.gtk, out.gtk_len, out.gtk_rsc)) {
            char e[160];
            strlcpy(e, nl_error(&NL), sizeof e);
            fail("set_key", "group", e);
            goto done;
        }
        JTRACE("group key %d installed", out.gtk_idx);
    }
    if (out.flags & WPA_DO_4WAY_DONE) {
        if (ST == ST_HANDSHAKE) {
            /* The CONNECT asked for a closed port (nl_conn_t's
             * control_port): open it now that the keys are in. A fullmac
             * firmware that keeps the port itself says EOPNOTSUPP. */
            if (!nl_authorize(&NL, C.bssid)) {
                if (nl_errno(&NL) == 95) {
                    JTRACE("the driver keeps the port itself (%s)", nl_error(&NL));
                } else {
                    char e[160];
                    strlcpy(e, nl_error(&NL), sizeof e);
                    fail("authorize", C.ssid, e);
                    goto done;
                }
            }
            JNOTE("handshake with \"%s\" complete, keys installed%s", C.ssid,
                  C.bss.group == WLAN_CIPHER_SUITE_TKIP ? " (TKIP group key)" : "");
            joined();
        } else {
            JLOG("the access point renewed the pairwise key");
        }
    }
    if (out.flags & WPA_DO_GROUP_DONE)
        JLOG("the access point renewed the group key (index %d)", out.gtk_idx);
done:
    wpa_wipe(&out, sizeof out);
}

/* ══ Clients ═════════════════════════════════════════════════════════ */

typedef struct {
    int  fd;
    char buf[WPA_REQ_MAX];
    size_t len;
    s64  deadline;
    u32  uid, gid;
    bool parked;                      /* waiting for a scan to finish  */
} client_t;

static client_t CL[MAX_CLIENTS];
static int      listen_fd = -1;

/* The answer is built here and sent in one go. */
static char   ob[24 * 1024];
static size_t obn;

static void ob_reset(void) { obn = 0; ob[0] = '\0'; }
static void ob_put(const char *s)
{
    size_t n = strlen(s);
    if (obn + n >= sizeof ob)
        n = sizeof ob - obn - 1;
    memcpy(ob + obn, s, n);
    obn += n;
    ob[obn] = '\0';
}
#define OB(...) do { char _l[512]; snprintf(_l, sizeof _l, __VA_ARGS__); ob_put(_l); } while (0)

static void cl_close(client_t *c)
{
    if (c->fd >= 0)
        lp_close(c->fd);
    c->fd = -1;
    c->parked = false;
}

static void cl_send_ob(client_t *c)
{
    size_t done = 0;
    while (done < obn) {
        long w = lp_sendto(c->fd, ob + done, obn - done, MSG_NOSIGNAL_, NULL, 0);
        if (w <= 0)
            break;
        done += (size_t)w;
    }
    cl_close(c);
}

static void cl_err(client_t *c, const char *code, const char *a1, const char *a2)
{
    ob_reset();
    OB("err\t%s\t%s\t%s\n", code, a1 ? a1 : "", a2 ? a2 : "");
    cl_send_ob(c);
}

static bool peer_cred(int fd, u32 *uid, u32 *gid)
{
    u32 cred[3];
    u32 len = sizeof cred;
    long r = sys_call5(SYS_getsockopt, fd, SOL_SOCKET, SO_PEERCRED_,
                       (long)cred, (long)&len);
    if (r < 0 || len < sizeof cred)
        return false;
    *uid = cred[1];
    *gid = cred[2];
    return true;
}

/* Root, or group sudo or netdev - by name in /etc/group, or as the
 * primary group. The account database rather than the process's own
 * supplementary groups: the desktop session is started without them
 * (see lp-privd), and removing somebody from sudo takes effect now. */
static bool is_admin(u32 uid, u32 gid)
{
    if (uid == 0)
        return true;
    lp_user_t u;
    if (!lp_user_by_uid(uid, &u))
        return false;
    static char grp[16384];
    long got = proc_read("/etc/group", grp, sizeof grp - 1);
    if (got <= 0)
        return false;
    grp[got] = '\0';
    for (char *line = grp; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, "sudo:", 5) == 0 || strncmp(line, "netdev:", 7) == 0) {
            char *f2 = strchr(line, ':');
            f2 = f2 ? strchr(f2 + 1, ':') : NULL;        /* after "x" */
            char *f3 = f2 ? strchr(f2 + 1, ':') : NULL;
            u32 g = f2 ? (u32)atoi(f2 + 1) : 0xffffffffu;
            if (f2 && (g == gid || g == u.gid))
                return true;
            for (char *m = f3 ? f3 + 1 : NULL; m && *m; ) {
                char *comma = strchr(m, ',');
                if (comma) *comma = '\0';
                if (strcmp(m, u.name) == 0)
                    return true;
                m = comma ? comma + 1 : NULL;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    return false;
}

static bool may_use(u32 uid) { return uid == 0 || uid >= 1000; }

static const char *wifi_state_name(void)
{
    if (ST == ST_IDLE && err_failed)
        return "failed";
    return ST_NAME[ST];
}

/* The status records - for `status` and for WPA_STATUS_PATH. */
static void status_records(void)
{
    const char *radio = !IFN[0] ? "none"
                      : RADIO == RADIO_HARD ? "hard"
                      : RADIO == RADIO_SOFT ? "off" : "on";
    OB("radio\t%s\n", radio);
    OB("iface\t%s\n", IFN);
    OB("driver\t%s\n", DRIVER);
    OB("state\t%s\n", wifi_state_name());
    OB("since\t%lld\n", (long long)((lp_monotonic_ms() - st_since) / 1000));
    if (ST >= ST_CONNECTING) {
        char h[2 * NL_SSID_MAX + 1], m[18];
        wpa_hex_encode(C.net.ssid, C.net.ssid_len, h);
        mac_str(C.bssid, m);
        OB("ssid\t%s\n", h);
        OB("bssid\t%s\n", m);
        OB("freq\t%u\n", (unsigned)C.bss.b.freq);
        OB("signal\t%d\n", C.signal);
        OB("security\t%s\n", C.net.open ? "open" : C.bss.sec == SEC_WPA3 ? "wpa3" : "wpa2");
    } else if (REQ.on) {
        char h[2 * NL_SSID_MAX + 1];
        wpa_hex_encode(REQ.net.ssid, REQ.net.ssid_len, h);
        OB("ssid\t%s\n", h);
    }
    if (ST == ST_CONNECTED || ST == ST_DHCP) {
        u32 a = 0;
        if (net_get_addr(IFN, &a) >= 0 && a) {
            char ip[16];
            ipv4_format(a, ip);
            OB("ip\t%s\n", ip);
        }
    }
    if (err_code[0])
        OB("error\t%s\t%s\t%s\n", err_code, err_a1, err_a2);
    if (ST == ST_HANDSHAKE || ST == ST_DHCP || ST == ST_CONNECTED ||
        C.sm.msg1_seen)
        OB("handshake\t%d\t%d\t%d\t%d\t%d\n", C.sm.msg1_seen,
           C.sm.msg1_after_2, C.sm.msg3_seen, C.sm.group_rekeys,
           C.sm.ptk_rekeys);
    int saved = 0;
    for (int i = 0; i < NN; i++)
        if (!NETS[i].disabled) saved++;
    OB("saved\t%d\n", saved);
    OB("config\t%s\n", netfile);
    OB("trace\t%d\n", trace ? 1 : 0);
}

static char last_status[4096];

static void publish_status(void)
{
    ob_reset();
    status_records();
    /* "since" changes every second; compare without it. */
    char cmp[4096];
    strlcpy(cmp, ob, sizeof cmp);
    char *s = strstr(cmp, "since\t");
    if (s) { char *e = strchr(s, '\n'); if (e) memmove(s, e + 1, strlen(e + 1) + 1); }
    if (strcmp(cmp, last_status) == 0)
        return;
    strlcpy(last_status, cmp, sizeof last_status);
    long fd = lp_open(WPA_STATUS_PATH ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    lp_write((int)fd, ob, obn);
    lp_close((int)fd);
    lp_rename(WPA_STATUS_PATH ".tmp", WPA_STATUS_PATH);
}

static void scan_records(void)
{
    for (int i = 0; i < NB; i++) {
        const bss_t *b = &BSS[i];
        if (b->b.ssid_len == 0 || b->b.ssid[0] == 0)
            continue;                         /* hidden, unnamed */
        char h[2 * NL_SSID_MAX + 1], m[18];
        wpa_hex_encode(b->b.ssid, b->b.ssid_len, h);
        mac_str(b->b.bssid, m);
        bool saved = false;
        for (int k = 0; k < NN; k++)
            if (!NETS[k].disabled && ssid_eq(NETS[k].ssid, NETS[k].ssid_len,
                                             b->b.ssid, b->b.ssid_len))
                saved = true;
        bool conn = (ST == ST_CONNECTED || ST == ST_DHCP) &&
                    memcmp(C.bssid, b->b.bssid, 6) == 0;
        OB("bss\t%s\t%s\t%d\t%u\t%s\t%d\t%d\t%s\t%s\n", m, h,
           bss_signal(&b->b), (unsigned)b->b.freq, SEC_NAME[b->sec],
           saved ? 1 : 0, conn ? 1 : 0,
           b->refuse && !delegatable(b) ? b->refuse : "-",
           b->detail[0] ? b->detail : "-");
    }
}

static void answer_parked_scans(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (CL[i].fd < 0 || !CL[i].parked)
            continue;
        ob_reset();
        ob_put("ok\n");
        scan_records();
        cl_send_ob(&CL[i]);
    }
}

/* Somebody asked for this SSID by name. */
static void request_connect(const u8 *ssid, size_t n, const char *password,
                            bool hidden, u32 uid, client_t *c)
{
    net_t want;
    memset(&want, 0, sizeof want);
    memcpy(want.ssid, ssid, n);
    want.ssid_len = (u8)n;
    bool newpw = false;

    if (password && password[0]) {
        wpa_psk_err_t e = wpa_pmk_from_psk(password, ssid, n, want.pmk);
        if (e != WPA_PSK_OK) {
            const char *code = e == WPA_PSK_SHORT ? "pw_short"
                             : e == WPA_PSK_LONG ? "pw_long"
                             : e == WPA_PSK_BAD_HEX ? "pw_hex" : "pw_chars";
            cl_err(c, code, NULL, NULL);
            return;
        }
        want.have_pmk = true;
        strlcpy(want.pass, password, sizeof want.pass);
        want.have_pass = true;
        newpw = true;
    } else {
        net_t *s = net_find(ssid, n, false);
        if (!s || s->disabled)
            s = net_find(ssid, n, true);
        if (s && !s->disabled) {
            want = *s;
            s->pw_rejected = false;       /* asked for by name: try it */
        }
    }
    want.hidden = want.hidden || hidden;

    /* Whatever was going on stops: the person has said what they want. */
    if (ST != ST_IDLE && ST != ST_SCANNING)
        drop_link(REASON_DEAUTH_LEAVING);
    attempt_wipe();
    memset(&REQ, 0, sizeof REQ);
    REQ.on = true;
    REQ.net = want;
    /* Whatever somebody asked for by name is saved once it works - a
     * new password, an open network, or one forgotten here earlier and
     * now chosen again - so it comes back by itself after a reboot. */
    REQ.trial = true;
    REQ.hidden = want.hidden;
    REQ.uid = uid;
    wpa_wipe(&want, sizeof want);
    user_off = false;
    retry_ms = 0;
    retry_at = 0;
    clear_err();
    set_state(ST_SCANNING);

    char s[80];
    ssid_str(ssid, n, s, sizeof s);
    JNOTE("uid %u asked to connect to \"%s\"%s", (unsigned)uid, s,
          newpw ? " with a new password" : "");

    ob_reset();
    ob_put("ok\n");
    cl_send_ob(c);

    scanning = false;               /* a scan of our own, for this name */
    start_scan(REQ.net.ssid, REQ.net.ssid_len, true);
}

static void handle(client_t *c, char *line)
{
    char *f[6];
    int nf = 0;
    for (char *p = line; nf < 6; ) {
        f[nf++] = p;
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = '\0';
        p = t + 1;
    }
    const char *verb = f[0];

    if (strcmp(verb, "status") == 0) {
        ob_reset();
        ob_put("ok\n");
        status_records();
        cl_send_ob(c);
        return;
    }
    if (strcmp(verb, "log") == 0) {
        ob_reset();
        ob_put("ok\n");
        int start = (journal_next - journal_count + JOURNAL_LINES) % JOURNAL_LINES;
        for (int i = 0; i < journal_count; i++)
            OB("line\t%s\n", journal[(start + i) % JOURNAL_LINES]);
        cl_send_ob(c);
        return;
    }

    char uidstr[16];
    snprintf(uidstr, sizeof uidstr, "%u", (unsigned)c->uid);
    if (!may_use(c->uid)) {
        cl_err(c, "denied_account", uidstr, NULL);
        return;
    }
    bool need_iface = strcmp(verb, "scan") == 0 || strcmp(verb, "connect") == 0;
    if (need_iface && !IFN[0]) {
        cl_err(c, err_code[0] ? err_code : "no_iface", err_a1, err_a2);
        return;
    }
    if (need_iface && RADIO != RADIO_ON) {
        cl_err(c, RADIO == RADIO_HARD ? "radio_hard" : "radio_off", NULL, NULL);
        return;
    }

    if (strcmp(verb, "scan") == 0) {
        /* In the middle of joining, a scan would get in the way (and on
         * some chips is refused): answer from what we have. */
        if (ST == ST_CONNECTING || ST == ST_HANDSHAKE) {
            ob_reset();
            ob_put("ok\n");
            scan_records();
            cl_send_ob(c);
            return;
        }
        c->parked = true;
        c->deadline = lp_monotonic_ms() + SCAN_TIMEOUT_MS + 1000;
        if (!scanning)
            start_scan(NULL, 0, false);
        if (!scanning) {                    /* it could not start */
            c->parked = false;
            cl_err(c, "scan_failed", nl_error(&NL), NULL);
        }
        return;
    }

    u8 ssid[NL_SSID_MAX];
    size_t sn = 0;
    bool have_ssid = nf >= 2 && wpa_hex_decode(f[1], ssid, sizeof ssid, &sn) && sn > 0;
    char ss[80] = "";
    if (have_ssid)
        ssid_str(ssid, sn, ss, sizeof ss);

    if (strcmp(verb, "connect") == 0) {
        if (!have_ssid) { cl_err(c, "bad_request", "connect needs a network name", NULL); return; }
        const char *pw = nf >= 3 ? f[2] : NULL;
        bool hidden = nf >= 4 && strcmp(f[3], "hidden") == 0;
        request_connect(ssid, sn, pw, hidden, c->uid, c);
        return;
    }
    if (strcmp(verb, "forget") == 0) {
        if (!have_ssid) { cl_err(c, "bad_request", "forget needs a network name", NULL); return; }
        net_t *ours = net_find(ssid, sn, false);
        net_t *boot = net_find(ssid, sn, true);
        if ((!ours || ours->disabled) && !boot) {
            cl_err(c, "not_saved", ss, NULL);
            return;
        }
        u32 owner = ours && !ours->disabled ? ours->saved_by : 0;
        if (!is_admin(c->uid, c->gid) && !(owner && owner == c->uid)) {
            cl_err(c, "denied", ss, "forget");
            return;
        }
        if (ours)
            *ours = NETS[--NN];     /* the last one moves into its place */
        if (boot) {
            /* It would come back from the boot card at the next start:
             * leave a tombstone that says it was forgotten here. */
            boot = net_find(ssid, sn, true);
            if (boot && NN < MAX_NETS) {
                net_t t;
                memset(&t, 0, sizeof t);
                memcpy(t.ssid, ssid, sn);
                t.ssid_len = (u8)sn;
                t.disabled = true;
                t.saved_by = c->uid;
                *boot = t;
            }
        }
        char why[160];
        if (!conf_save(why, sizeof why)) {
            cl_err(c, "internal", why, NULL);
            return;
        }
        JNOTE("uid %u forgot \"%s\"", (unsigned)c->uid, ss);
        if ((ST >= ST_CONNECTING) &&
            ssid_eq(C.net.ssid, C.net.ssid_len, ssid, sn)) {
            drop_link(REASON_DEAUTH_LEAVING);
            attempt_wipe();
            set_state(ST_IDLE);
            clear_err();
        }
        ob_reset();
        ob_put("ok\n");
        cl_send_ob(c);
        return;
    }
    if (strcmp(verb, "disconnect") == 0) {
        user_off = true;
        REQ.on = false;
        if (ST != ST_IDLE)
            drop_link(REASON_DEAUTH_LEAVING);
        attempt_wipe();
        set_state(ST_IDLE);
        clear_err();
        JNOTE("uid %u disconnected the wireless", (unsigned)c->uid);
        ob_reset();
        ob_put("ok\n");
        cl_send_ob(c);
        return;
    }
    if (strcmp(verb, "radio") == 0) {
        bool on;
        if (nf >= 2 && strcmp(f[1], "on") == 0) on = true;
        else if (nf >= 2 && strcmp(f[1], "off") == 0) on = false;
        else { cl_err(c, "bad_request", "radio on or radio off", NULL); return; }
        radio_want = on;
        radio_save(on);
        if (!on && IFN[0] && ST != ST_IDLE)
            drop_link(REASON_DEAUTH_LEAVING);
        if (!on) {
            attempt_wipe();
            set_state(ST_IDLE);
            REQ.on = false;
        }
        RADIO = rfkill(on ? 0 : 1);
        JNOTE("uid %u turned the radio %s", (unsigned)c->uid, on ? "on" : "off");
        if (on) {
            user_off = false;
            clear_err();
            retry_ms = 0;
            retry_at = 0;
            idle_scan_ms = IDLE_SCAN_MIN_MS;
            next_scan_at = lp_monotonic_ms() + 1000;  /* the card needs a moment */
            if (RADIO == RADIO_HARD) {
                cl_err(c, "radio_hard", NULL, NULL);
                return;
            }
        }
        ob_reset();
        ob_put("ok\n");
        cl_send_ob(c);
        return;
    }
    if (strcmp(verb, "trace") == 0) {
        if (!is_admin(c->uid, c->gid)) { cl_err(c, "denied", "trace", "trace"); return; }
        trace = nf >= 2 && strcmp(f[1], "on") == 0;
        JNOTE("frame tracing %s", trace ? "on" : "off");
        ob_reset();
        ob_put("ok\n");
        cl_send_ob(c);
        return;
    }
    cl_err(c, "bad_request", verb, NULL);
}

static void on_listen(void)
{
    for (;;) {
        long fd = lp_accept(listen_fd, NULL, NULL, LP_SOCK_NONBLOCK | LP_SOCK_CLOEXEC);
        if (fd < 0)
            return;
        client_t *c = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (CL[i].fd < 0) { c = &CL[i]; break; }
        if (!c) {
            lp_close((int)fd);          /* busy; the client says so */
            continue;
        }
        memset(c, 0, sizeof *c);
        c->fd = (int)fd;
        c->deadline = lp_monotonic_ms() + CLIENT_TIMEOUT_MS;
        if (!peer_cred(c->fd, &c->uid, &c->gid)) {
            cl_close(c);
            continue;
        }
    }
}

static void on_client(client_t *c)
{
    long n = lp_read(c->fd, c->buf + c->len, sizeof c->buf - 1 - c->len);
    if (n <= 0) {
        cl_close(c);
        return;
    }
    c->len += (size_t)n;
    c->buf[c->len] = '\0';
    char *nl = strchr(c->buf, '\n');
    if (!nl) {
        if (c->len >= sizeof c->buf - 1)
            cl_err(c, "bad_request", "request too long", NULL);
        return;
    }
    *nl = '\0';
    handle(c, c->buf);
    wpa_wipe(c->buf, sizeof c->buf);     /* it may have held a password */
}

static bool open_socket(void)
{
    struct { u16 family; char path[108]; } sa;
    memset(&sa, 0, sizeof sa);
    sa.family = AF_UNIX_;
    strlcpy(sa.path, WPA_SOCK_PATH, sizeof sa.path);

    /* Another of us answering means this one is not needed. */
    long probe = lp_socket(AF_UNIX_, SOCK_STREAM, 0);
    if (probe >= 0) {
        if (lp_connect((int)probe, &sa, sizeof sa) == 0) {
            lp_close((int)probe);
            dprintf(STDERR_FILENO, "wpa: another wpa is already running"
                    " (%s answers)\n", WPA_SOCK_PATH);
            return false;
        }
        lp_close((int)probe);
    }
    lp_unlink(WPA_SOCK_PATH);
    long fd = lp_socket(AF_UNIX_, SOCK_STREAM | LP_SOCK_NONBLOCK | LP_SOCK_CLOEXEC, 0);
    if (fd < 0 || lp_bind((int)fd, &sa, sizeof sa) < 0 ||
        lp_listen((int)fd, 16) < 0) {
        dprintf(STDERR_FILENO, "wpa: cannot listen on %s\n", WPA_SOCK_PATH);
        if (fd >= 0) lp_close((int)fd);
        return false;
    }
    /* Anyone may connect; what they may do is decided per request. */
    lp_chmod(WPA_SOCK_PATH, 0666);
    listen_fd = (int)fd;
    return true;
}

/* ══ Noticing a new interface ════════════════════════════════════════
 *
 * With no wireless interface (a VM, a desktop PC, a card whose firmware
 * has not loaded yet) there is nothing to do but wait for one, and
 * waking every second to look would be the only thing on an idle
 * machine that never sleeps. rtnetlink's link group says when an
 * interface appears, so the wait is a poll on that with a long timeout;
 * the timeout stays as the guarantee (a message lost to a full buffer
 * costs one timeout, nothing more). */

#define RTMGRP_LINK          1
#define IDLE_NO_IFACE_MS 30000

static int link_fd = -1;
static volatile int stop_sig;
static void on_signal(int sig) { stop_sig = sig; }

static void link_watch_open(void)
{
    long fd = lp_socket(16 /* AF_NETLINK */, SOCK_DGRAM | LP_SOCK_NONBLOCK |
                        LP_SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return;
    u8 sa[12];
    memset(sa, 0, sizeof sa);
    u16 fam = 16;
    u32 groups = RTMGRP_LINK;
    memcpy(sa, &fam, 2);
    memcpy(sa + 8, &groups, 4);
    if (lp_bind((int)fd, sa, sizeof sa) < 0) {
        lp_close((int)fd);
        return;
    }
    link_fd = (int)fd;
}

static void link_watch_drain(void)
{
    u8 buf[4096];
    while (link_fd >= 0 &&
           lp_recvfrom(link_fd, buf, sizeof buf, MSG_DONTWAIT_, NULL, NULL) > 0)
        ;
}

/* ══ The fallback: become wpa_supplicant ═════════════════════════════ */

static const char *find_supplicant(void)
{
    static const char *const where[] = {
        "/bin/wpa_supplicant", "/sbin/wpa_supplicant",
        "/usr/sbin/wpa_supplicant", "/usr/bin/wpa_supplicant", NULL
    };
    for (int i = 0; where[i]; i++)
        if (lp_exists(where[i]))
            return where[i];
    return NULL;
}

/* Returns only when there is no wpa_supplicant to hand over to; the
 * caller then runs this daemon after all, because a switch that points
 * at nothing must not leave the machine off the network. */
static void run_fallback(const char *sw)
{
    const char *bin = find_supplicant();
    if (!bin) {
        JNOTE("the fallback switch (%s) is on, but there is no"
              " wpa_supplicant on this machine - running wpa instead", sw);
        return;
    }
    JNOTE("the fallback switch (%s) is on: handing the wireless to %s",
          sw, bin);

    char name[16] = "";
    bool said = false;
    for (;;) {
        if (PIN_IF[0]) {
            char path[64];
            snprintf(path, sizeof path, "/sys/class/net/%s", PIN_IF);
            if (lp_exists(path))
                strlcpy(name, PIN_IF, sizeof name);
        } else {
            each_entry("/sys/class/net", pick_iface, name);
        }
        if (name[0] || stop_sig)
            break;
        if (!said)
            JLOG("waiting for a wireless interface to appear");
        said = true;
        lp_pollfd_t p = { link_fd, LP_POLLIN, 0 };
        lp_poll(&p, link_fd >= 0 ? 1 : 0, 5000);
        link_watch_drain();
    }
    if (stop_sig)
        lp_exit(0);

    /* The global part: /etc/wpa.conf where the Pi's rc built one from
     * the card (it has ctrl_interface and the card's networks), else a
     * two-line file of our own so that wpa_cli can reach it. Our saved
     * networks come in through -I either way. */
    /* wpa_supplicant exits at once when it cannot make its control
     * socket's directory, and nothing else here would make /var/run on
     * a root that lacks it. */
    lp_mkdir("/var", 0755);
    lp_mkdir("/var/run", 0755);
    const char *conf = "/etc/wpa.conf";
    if (!lp_exists(conf)) {
        conf = "/run/lp-net-wpa_supplicant.conf";
        long fd = lp_open(conf, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            dprintf((int)fd, "ctrl_interface=/var/run/wpa_supplicant\n"
                             "update_config=0\n");
            lp_close((int)fd);
        }
    }
    char logf[64];
    snprintf(logf, sizeof logf, "%s/wpa.log",
             lp_is_dir("/data/log") ? "/data/log" : "/var/log");

    char *argv[16];
    int a = 0;
    argv[a++] = (char *)"wpa_supplicant";
    if (trace) {
        /* The card's /boot/wpa-debug, as it always was: every frame of
         * the association and the handshake, to a file. */
        argv[a++] = (char *)"-dd";
        argv[a++] = (char *)"-t";
        argv[a++] = (char *)"-f";
        argv[a++] = logf;
    } else {
        argv[a++] = (char *)"-s";
    }
    argv[a++] = (char *)"-i";
    argv[a++] = name;
    argv[a++] = (char *)"-c";
    argv[a++] = (char *)conf;
    if (lp_exists(netfile)) {
        argv[a++] = (char *)"-I";
        argv[a++] = netfile;
    }
    argv[a] = NULL;

    lp_close(listen_fd);
    lp_unlink(WPA_SOCK_PATH);
    long ff = lp_open(WPA_FALLBACK_RUN, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ff >= 0) {
        dprintf((int)ff, "%s\n%s\n", sw, bin);
        lp_close((int)ff);
    }
    char *envp[] = {
        (char *)"PATH=/bin:/sbin:/usr/bin:/usr/sbin", NULL
    };
    lp_execve(bin, argv, envp);
    lp_unlink(WPA_FALLBACK_RUN);
    JNOTE("could not run %s - running wpa instead", bin);
}

/* ══ Time ════════════════════════════════════════════════════════════ */

static void tick(void)
{
    s64 now = lp_monotonic_ms();

    if (!IFN[0]) {
        if (now >= next_iface_check) {
            next_iface_check = now + 2000;
            find_iface();
        }
        return;
    }
    char path[64];
    snprintf(path, sizeof path, "/sys/class/net/%s", IFN);
    if (!lp_exists(path)) {
        detach("the interface has gone");
        next_iface_check = now + 500;
        return;
    }

    radio_t r = rfkill(-1);
    if (r != RADIO) {
        JNOTE("the radio is %s", r == RADIO_ON ? "on" : r == RADIO_HARD
              ? "off (hardware switch)" : "off");
        if (r == RADIO_ON) {
            net_if_up(IFN);
            next_scan_at = now + 1000;
            idle_scan_ms = IDLE_SCAN_MIN_MS;
        }
    }
    RADIO = r;
    if (RADIO != RADIO_ON) {
        stop_delegate("the radio is off");
        if (ST != ST_IDLE) {
            attempt_wipe();
            set_state(ST_IDLE);
        }
        scanning = false;
        const char *code = RADIO == RADIO_HARD ? "radio_hard" : "radio_off";
        if (strcmp(err_code, code) != 0)
            set_err(code, NULL, NULL, false);
        return;
    }
    if (!strcmp(err_code, "radio_off") || !strcmp(err_code, "radio_hard") ||
        !strcmp(err_code, "no_iface"))
        clear_err();

    if (DG.on) {
        delegate_tick(now);
        now = lp_monotonic_ms();
    }

    if (scanning && now > scan_deadline) {
        scanning = false;
        JLOG("the scan did not finish in %d s", SCAN_TIMEOUT_MS / 1000);
        answer_parked_scans();
        if (ST == ST_SCANNING) {
            set_err("scan_failed", "it did not finish in time", NULL, true);
            set_state(ST_IDLE);
            schedule_retry();
        }
    }

    switch (ST) {
    case ST_CONNECTING:
        if (DG.on)
            break;                  /* delegate_tick has the clock */
        if (now > C.deadline) {
            char s[16];
            snprintf(s, sizeof s, "no answer in %d s", CONNECT_TIMEOUT_MS / 1000);
            fail("assoc_timeout", C.ssid, s);
        }
        break;
    case ST_HANDSHAKE:
        if (DG.on)
            break;
        if (now > C.deadline) {
            char s[8];
            if (C.sm.msg1_seen == 0) {
                snprintf(s, sizeof s, "%d", MSG1_TIMEOUT_MS / 1000);
                fail("no_msg1", C.ssid, s);
            } else if (C.sm.msg1_after_2 > 0) {
                fail("wrong_password", C.ssid, NULL);
            } else {
                snprintf(s, sizeof s, "%d", MSG3_TIMEOUT_MS / 1000);
                fail("no_msg3", C.ssid, s);
            }
        }
        break;
    case ST_DHCP:
    case ST_CONNECTED:
        if (now >= next_addr) {
            next_addr = now + ADDR_EVERY_MS;
            u32 a = 0;
            bool have = net_get_addr(IFN, &a) >= 0 && a != 0;
            if (have && (ST == ST_DHCP || a != C.addr)) {
                char ip[16];
                ipv4_format(a, ip);
                JNOTE("connected to \"%s\", address %s", C.ssid, ip);
                set_state(ST_CONNECTED);
                clear_err();
            } else if (!have && ST == ST_CONNECTED) {
                JLOG("the address went away - waiting for dhcp");
                set_state(ST_DHCP);
                C.keys_at = now;
                C.said_slow = false;
            }
            C.addr = have ? a : 0;
            if (ST == ST_DHCP && !C.said_slow && now - C.keys_at > DHCP_SLOW_MS) {
                C.said_slow = true;
                set_err("no_address", C.ssid, NULL, false);
            }
        }
        if (now >= next_signal) {
            next_signal = now + SIGNAL_EVERY_MS;
            if (!scanning) {
                results_load();
                for (int i = 0; i < NB; i++)
                    if (memcmp(BSS[i].b.bssid, C.bssid, 6) == 0 &&
                        bss_signal(&BSS[i].b))
                        C.signal = bss_signal(&BSS[i].b);
            }
        }
        break;
    case ST_SCANNING:
        if (!scanning) {
            /* A scan that could not start; try the request again. */
            if (now >= scan_retry_at && REQ.on)
                start_scan(REQ.net.ssid, REQ.net.ssid_len, true);
        }
        break;
    case ST_IDLE:
        if (user_off || scanning || now < retry_at || now < next_scan_at)
            break;
        if (REQ.on) {
            start_scan(REQ.net.ssid, REQ.net.ssid_len, true);
            break;
        }
        {
            int usable = 0, hidden = 0;
            for (int i = 0; i < NN; i++)
                if (!NETS[i].disabled && !NETS[i].pw_rejected) {
                    usable++;
                    if (NETS[i].hidden) hidden++;
                }
            if (!usable) {
                if (!err_code[0])
                    set_err(NN ? "none_in_range" : "no_networks", NULL, NULL, false);
                next_scan_at = now + IDLE_SCAN_MAX_MS;
                break;
            }
            /* Hidden networks answer only when asked by name: every
             * other scan asks for one of them, in turn. */
            const net_t *h = NULL;
            if (hidden && (hidden_rr++ % 2) == 1) {
                int want = (hidden_rr / 2) % hidden, k = 0;
                for (int i = 0; i < NN; i++)
                    if (!NETS[i].disabled && NETS[i].hidden && k++ == want)
                        h = &NETS[i];
            }
            start_scan(h ? h->ssid : NULL, h ? h->ssid_len : 0, true);
        }
        break;
    default:
        break;
    }
}

/* ══ Start and stop ══════════════════════════════════════════════════ */

static void usage(void)
{
    printf("usage: wpa -d [-i interface] [-v]\n"
           "\n"
           "  The daemon that owns the wireless: scanning, joining, the WPA2\n"
           "  handshake, and the saved networks. init starts it from\n"
           "  /etc/services; you talk to it with lp-net:\n"
           "\n"
           "    lp-net status          what the wireless is doing\n"
           "    lp-net scan            the networks around\n"
           "    lp-net connect NAME    join one (--password X)\n"
           "    wifi                   where it stopped, and why\n"
           "\n"
           "  -d  run (in the foreground, as init wants)\n"
           "  -i  use this interface rather than the first wireless one\n"
           "  -v  describe every EAPOL frame in the log (also `wifi trace on`)\n");
}

int main(int argc, char **argv)
{
    bool daemon = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) daemon = true;
        else if (strcmp(argv[i], "-v") == 0) trace = true;
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
            strlcpy(PIN_IF, argv[++i], sizeof PIN_IF);
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else {
            dprintf(STDERR_FILENO, "wpa: unknown option %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (!daemon) {
        usage();
        return 0;
    }
    if (lp_getuid() != 0) {
        dprintf(STDERR_FILENO, "wpa: the wireless needs root\n");
        return 1;
    }
    /* The debugging switch on the boot card, kept from wpa_supplicant's
     * days: any PC can create it, which matters when the wireless is the
     * only way in. */
    if (lp_exists("/boot/wpa-debug"))
        trace = true;

    for (int i = 0; i < MAX_CLIENTS; i++)
        CL[i].fd = -1;
    lp_signal_ignore(SIGPIPE_);
    lp_signal_handler(SIGTERM_, on_signal);
    lp_signal_handler(SIGINT_, on_signal);

    if (!open_socket())
        return 1;

    JNOTE("starting%s", trace ? " (frame tracing on)" : "");
    conf_load_all();
    radio_want = radio_saved_on();
    RADIO = rfkill(radio_want ? 0 : 1);
    if (!radio_want)
        JLOG("the radio stays off, as it was left (%s)", radiofile);
    st_since = lp_monotonic_ms();
    link_watch_open();

    lp_unlink(WPA_FALLBACK_RUN);    /* whatever ran before, this is us */
    char sw[96];
    if (wpa_fallback_on(sw, sizeof sw))
        run_fallback(sw);           /* returns only if it could not */

    static u8 frame[EAPOL_MAX];
    while (!stop_sig) {
        lp_pollfd_t p[3 + MAX_CLIENTS];
        int np = 0, i_nl = -1, i_ep = -1;
        p[np].fd = listen_fd; p[np].events = LP_POLLIN; p[np++].revents = 0;
        if (IFN[0]) {
            i_nl = np;
            p[np].fd = nl_fd(&NL); p[np].events = LP_POLLIN; p[np++].revents = 0;
            /* EAPOL is read only once the CONNECT event has been seen:
             * a message 1 that races ahead of it waits in the socket,
             * where it is safe, rather than being handled before we
             * know which AP we are talking to. */
            if (ST >= ST_HANDSHAKE) {
                i_ep = np;
                p[np].fd = EP.fd; p[np].events = LP_POLLIN; p[np++].revents = 0;
            }
        }
        int i_lk = -1;
        if (!IFN[0] && link_fd >= 0) {
            i_lk = np;
            p[np].fd = link_fd; p[np].events = LP_POLLIN; p[np++].revents = 0;
        }
        int cmap[MAX_CLIENTS], nc = 0, cbase = np;
        bool any_client = false;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (CL[i].fd >= 0)
                any_client = true;
            if (CL[i].fd >= 0 && !CL[i].parked) {
                cmap[nc++] = i;
                p[np].fd = CL[i].fd; p[np].events = LP_POLLIN; p[np++].revents = 0;
            }
        }

        /* A second's tick while there is a radio to look after; with no
         * interface, sleep until one appears (see "Noticing a new
         * interface"), unless a client's deadline needs the tick. */
        lp_poll(p, (unsigned)np,
                IFN[0] || any_client || link_fd < 0 ? TICK_MS : IDLE_NO_IFACE_MS);

        if (i_lk >= 0 && (p[i_lk].revents & LP_POLLIN)) {
            link_watch_drain();
            next_iface_check = 0;
        }

        if (i_nl >= 0 && IFN[0]) {
            nl_event_t ev;
            while (IFN[0] && nl_drain(&NL, &ev))
                on_event(&ev);
        }
        if (i_ep >= 0 && IFN[0] && (p[i_ep].revents & LP_POLLIN)) {
            for (int k = 0; k < 8 && ST >= ST_HANDSHAKE; k++) {
                u8 from[6];
                long n = eapol_recv(&EP, frame, sizeof frame, from, 0);
                if (n <= 0)
                    break;
                on_eapol(frame, (size_t)n, from);
                /* Keys installed on the way may have queued events. */
                nl_event_t ev;
                while (IFN[0] && nl_drain(&NL, &ev))
                    on_event(&ev);
            }
        }
        if (p[0].revents & LP_POLLIN)
            on_listen();
        for (int k = 0; k < nc; k++) {
            client_t *c = &CL[cmap[k]];
            if (c->fd >= 0 && (p[cbase + k].revents &
                               (LP_POLLIN | LP_POLLHUP | LP_POLLERR)))
                on_client(c);
        }
        s64 now = lp_monotonic_ms();
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (CL[i].fd >= 0 && now > CL[i].deadline) {
                if (CL[i].parked) {
                    ob_reset();
                    ob_put("ok\n");
                    scan_records();
                    cl_send_ob(&CL[i]);
                } else {
                    cl_close(&CL[i]);
                }
            }

        tick();
        publish_status();
    }

    JNOTE("stopping (signal %d)", stop_sig);
    stop_delegate("stopping");
    if (IFN[0]) {
        if (ST != ST_IDLE)
            nl_disconnect(&NL, REASON_DEAUTH_LEAVING);
        rtnl_setlink(IFX, 0, -1);
    }
    lp_unlink(WPA_SOCK_PATH);
    lp_unlink(WPA_STATUS_PATH);
    return 0;
}
