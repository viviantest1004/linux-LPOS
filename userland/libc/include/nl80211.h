/* nl80211.h - talking to the kernel's wireless subsystem directly.
 *
 * This is the bottom half of our own wireless stack: generic netlink
 * and nl80211, written by hand, with no libnl and no kernel headers.
 * The top half - the WPA2 four-way handshake - lives in wpa4way.h and
 * only ever sees this file's types.
 *
 * ── Why this exists ──
 * The board associates with the access point and the four-way handshake
 * then never happens. Every explanation outside the host was checked
 * and none of them held: the firmware, the NVRAM, the CLM blob, the
 * kernel's wireless and crypto configuration, the wpa_supplicant build.
 * What was left was a third-party supplicant we could not see inside,
 * on a system where we wrote everything else. So we stopped debugging
 * it and wrote this, for the same reason this system has its own libc:
 * when it fails, it has to be able to say exactly where.
 *
 * ── What a fullmac chip changes ──
 * The BCM43438 on a Pi Zero W is *fullmac*: the firmware runs the whole
 * MLME itself. It scans, it authenticates, it associates. The host does
 * not send an authentication frame and does not build an association
 * request. It says "connect to this SSID with these ciphers", the
 * firmware comes back later with an event saying whether it worked,
 * and from then on EAPOL is just ordinary traffic arriving on wlan0
 * like any other ethernet frame.
 *
 * That is why this header is so short: resolve the family, scan,
 * connect, install two keys, listen for events. Five things.
 *
 * ── And a softmac card needs nothing more ──
 * The same five work on a softmac card - ath10k in a Dell XPS 15, any
 * mac80211 driver, mac80211_hwsim in a test VM - because a driver that
 * has no .connect of its own gets cfg80211's in-kernel SME, which turns
 * one NL80211_CMD_CONNECT into the authentication and association
 * frames itself and reports back with the same CONNECT event. What
 * differs is who writes the association request: on softmac it is the
 * kernel, from the elements in nl_conn_t.ie and nothing else, which is
 * why that element is not optional (see nl_conn_t).
 *
 * ── The shape of the conversation ──
 * Generic netlink is a multiplexer that lives on one socket. Every
 * message is
 *
 *      struct nlmsghdr        16 bytes, the netlink envelope
 *      struct genlmsghdr       4 bytes, cmd + version
 *      attributes             TLVs, each padded to 4 bytes
 *
 * and the nlmsghdr's type field is not a constant: it is the *family
 * id*, which the kernel assigns at boot and which you have to ask for
 * by name first. That request is itself a generic netlink message, to
 * the one family whose id is fixed (nlctrl, id 16). So the first thing
 * nl_open does is ask nlctrl for "nl80211", and the answer carries both
 * the family id and the ids of the multicast groups - "mlme", "scan",
 * "config" - which you then have to subscribe to with setsockopt before
 * any event will ever reach you.
 *
 * Everything is in host byte order. Nothing here is network order, with
 * exactly one exception: cipher and AKM suite selectors, which are the
 * IEEE 802.11 OUI-and-type quadruples packed big-endian-looking into a
 * u32 (00-0F-AC:4 is 0x000FAC04). nl_suite() builds one from the four
 * bytes as they appear in an RSN information element.
 *
 * ── The one rule about the buffer ──
 * A netlink message is a byte array. The Pi Zero W is 32-bit ARMv6 and
 * will fault, or silently read the wrong thing, if you cast a pointer
 * into the middle of that array to a u16 * or u32 *. Everything in the
 * implementation goes through memcpy for that reason, and nothing in
 * this header hands the caller a pointer into a receive buffer.
 */
#ifndef _LP_NL80211_H
#define _LP_NL80211_H

#include "types.h"

/* ── Constants copied from the kernel ──────────────────────────────
 *
 * Taken by hand from linux-6.12.107, include/uapi/linux/nl80211.h,
 * include/uapi/linux/netlink.h, include/uapi/linux/genetlink.h and
 * include/linux/socket.h. These are ABI: they are fixed forever once
 * shipped, which is why copying them is safe and why including the
 * kernel's headers - which drag in linux/types.h and a different idea
 * of what u32 is - is not.
 *
 * Only the values this file actually uses are here. The full enums run
 * to several hundred members and pasting them would hide the dozen that
 * matter.
 */

/* netlink protocols and address family (uapi/linux/netlink.h, socket.h) */
#define NL_AF_NETLINK           16
#define NL_SOCK_RAW              3
#define NETLINK_GENERIC         16
#define SOL_NETLINK            270      /* include/linux/socket.h        */
#define NETLINK_ADD_MEMBERSHIP   1
#define NETLINK_EXT_ACK         11      /* ask for a reason, not just a
                                           number, when a request is
                                           rejected                      */

/* nlmsghdr.nlmsg_type, the reserved control values (netlink.h) */
#define NLMSG_NOOP              0x1
#define NLMSG_ERROR             0x2
#define NLMSG_DONE              0x3
#define NLMSG_OVERRUN           0x4
#define NLMSG_MIN_TYPE          0x10    /* below this: control messages   */
#define GENL_ID_CTRL            NLMSG_MIN_TYPE  /* nlctrl's fixed id      */

/* nlmsghdr.nlmsg_flags (netlink.h) */
#define NLM_F_REQUEST           0x01
#define NLM_F_MULTI             0x02    /* one of several; DONE ends it   */
#define NLM_F_ACK               0x04    /* answer even when it worked     */
#define NLM_F_ROOT              0x100
#define NLM_F_MATCH             0x200
#define NLM_F_DUMP              (NLM_F_ROOT | NLM_F_MATCH)
#define NLM_F_CAPPED            0x100   /* on an ACK: the echoed request
                                           was truncated                  */
#define NLM_F_ACK_TLVS          0x200   /* on an ACK: ext-ack TLVs follow */

/* extended-ack attributes inside an NLMSG_ERROR (netlink.h) */
#define NLMSGERR_ATTR_MSG        1      /* NUL-terminated string          */
#define NLMSGERR_ATTR_OFFS       2      /* u32, byte offset of the
                                           attribute the kernel disliked  */

/* nlattr flags (netlink.h) */
#define NLA_F_NESTED            (1 << 15)
#define NLA_F_NET_BYTEORDER     (1 << 14)
#define NLA_TYPE_MASK           (u16)(~(NLA_F_NESTED | NLA_F_NET_BYTEORDER))

/* the generic netlink controller (genetlink.h) */
#define CTRL_CMD_GETFAMILY       3
#define CTRL_ATTR_FAMILY_ID      1
#define CTRL_ATTR_FAMILY_NAME    2
#define CTRL_ATTR_MCAST_GROUPS   7
#define CTRL_ATTR_MCAST_GRP_NAME 1
#define CTRL_ATTR_MCAST_GRP_ID   2

/* nl80211 commands (uapi/linux/nl80211.h, enum nl80211_commands) */
#define NL80211_CMD_GET_INTERFACE       5
#define NL80211_CMD_NEW_INTERFACE       7
#define NL80211_CMD_NEW_KEY            11
#define NL80211_CMD_SET_STATION        18
#define NL80211_CMD_GET_REG            31
#define NL80211_CMD_GET_SCAN           32
#define NL80211_CMD_TRIGGER_SCAN       33
#define NL80211_CMD_NEW_SCAN_RESULTS   34
#define NL80211_CMD_SCAN_ABORTED       35
#define NL80211_CMD_AUTHENTICATE       37
#define NL80211_CMD_ASSOCIATE          38
#define NL80211_CMD_DEAUTHENTICATE     39
#define NL80211_CMD_DISASSOCIATE       40
#define NL80211_CMD_CONNECT            46
#define NL80211_CMD_ROAM               47
#define NL80211_CMD_DISCONNECT         48
#define NL80211_CMD_PORT_AUTHORIZED   125

/* nl80211 attributes (enum nl80211_attrs). The number in the comment is
 * the payload the kernel's policy demands - getting one of these wrong
 * is the single most common way to earn a bare EINVAL. */
#define NL80211_ATTR_WIPHY                     1  /* u32                 */
#define NL80211_ATTR_IFINDEX                   3  /* u32                 */
#define NL80211_ATTR_IFNAME                    4  /* NUL-terminated      */
#define NL80211_ATTR_IFTYPE                    5  /* u32                 */
#define NL80211_ATTR_MAC                       6  /* exactly 6 bytes     */
#define NL80211_ATTR_KEY_DATA                  7  /* binary              */
#define NL80211_ATTR_KEY_IDX                   8  /* u8!  not u32        */
#define NL80211_ATTR_KEY_CIPHER                9  /* u32                 */
#define NL80211_ATTR_KEY_SEQ                  10  /* binary, <= 16       */
#define NL80211_ATTR_KEY_DEFAULT              11  /* flag                */
#define NL80211_ATTR_REG_ALPHA2               33  /* string, 2 + NUL     */
#define NL80211_ATTR_WIPHY_FREQ               38  /* u32, MHz            */
#define NL80211_ATTR_IE                       42  /* binary              */
#define NL80211_ATTR_SCAN_SSIDS               45  /* nested              */
#define NL80211_ATTR_BSS                      47  /* nested              */
#define NL80211_ATTR_SSID                     52  /* binary, <= 32       */
#define NL80211_ATTR_AUTH_TYPE                53  /* u32                 */
#define NL80211_ATTR_REASON_CODE              54  /* u16!  not u32       */
#define NL80211_ATTR_KEY_TYPE                 55  /* u32                 */
#define NL80211_ATTR_TIMED_OUT                65  /* flag                */
#define NL80211_ATTR_STA_FLAGS2               67  /* u32 mask, u32 set   */
#define NL80211_ATTR_CONTROL_PORT             68  /* flag                */
#define NL80211_STA_FLAG_AUTHORIZED            1  /* bit in STA_FLAGS2   */
#define NL80211_ATTR_PRIVACY                  70  /* flag                */
#define NL80211_ATTR_DISCONNECTED_BY_AP       71  /* flag                */
#define NL80211_ATTR_STATUS_CODE              72  /* u16                 */
#define NL80211_ATTR_CIPHER_SUITES_PAIRWISE   73  /* array of u32, raw   */
#define NL80211_ATTR_CIPHER_SUITE_GROUP       74  /* u32                 */
#define NL80211_ATTR_WPA_VERSIONS             75  /* u32, bitmask        */
#define NL80211_ATTR_AKM_SUITES               76  /* array of u32, raw   */
#define NL80211_ATTR_REQ_IE                   77  /* binary              */
#define NL80211_ATTR_RESP_IE                  78  /* binary              */
#define NL80211_ATTR_WDEV                    153  /* u64                 */
#define NL80211_ATTR_TIMEOUT_REASON          248  /* u32                 */

/* nested inside NL80211_ATTR_BSS (enum nl80211_bss) */
#define NL80211_BSS_BSSID                      1
#define NL80211_BSS_FREQUENCY                  2
#define NL80211_BSS_CAPABILITY                 5
#define NL80211_BSS_INFORMATION_ELEMENTS       6
#define NL80211_BSS_SIGNAL_MBM                 7  /* s32, mBm            */
#define NL80211_BSS_SIGNAL_UNSPEC              8  /* u8, 0..100          */
#define NL80211_BSS_STATUS                     9
#define NL80211_BSS_SEEN_MS_AGO               10
#define NL80211_BSS_BEACON_IES                11
#define NL80211_BSS_STATUS_ASSOCIATED          1

/* enum nl80211_iftype */
#define NL80211_IFTYPE_STATION                 2
#define NL80211_IFTYPE_AP                      3
#define NL80211_IFTYPE_MONITOR                 6

/* enum nl80211_auth_type */
#define NL80211_AUTHTYPE_OPEN_SYSTEM           0
#define NL80211_AUTHTYPE_SHARED_KEY            1
#define NL80211_AUTHTYPE_SAE                   4
#define NL80211_AUTHTYPE_AUTOMATIC             9

/* enum nl80211_wpa_versions */
#define NL80211_WPA_VERSION_1                  1
#define NL80211_WPA_VERSION_2                  2
#define NL80211_WPA_VERSION_3                  4

/* enum nl80211_key_type */
#define NL80211_KEYTYPE_GROUP                  0
#define NL80211_KEYTYPE_PAIRWISE               1

/* The multicast group names, verbatim from nl80211.h. These are looked
 * up by name at run time; the ids are not fixed. */
#define NL80211_GROUP_CONFIG    "config"
#define NL80211_GROUP_SCAN      "scan"
#define NL80211_GROUP_REG       "regulatory"
#define NL80211_GROUP_MLME      "mlme"

/* ── Cipher and AKM suite selectors ────────────────────────────────
 *
 * IEEE 802.11-2020 table 9-149 (ciphers) and 9-151 (AKMs). The value is
 * the OUI followed by the suite type, read as one 32-bit number in the
 * order the bytes appear in an RSN information element - so 00-0F-AC
 * type 4, CCMP, is 0x000FAC04. cfg80211 compares against exactly these
 * constants, so they go on the wire as a plain host-order u32. */
#define WLAN_CIPHER_SUITE_WEP40      0x000FAC01
#define WLAN_CIPHER_SUITE_TKIP       0x000FAC02
#define WLAN_CIPHER_SUITE_CCMP       0x000FAC04
#define WLAN_CIPHER_SUITE_WEP104     0x000FAC05
#define WLAN_CIPHER_SUITE_AES_CMAC   0x000FAC06   /* BIP, management */
#define WLAN_CIPHER_SUITE_GCMP       0x000FAC08
#define WLAN_CIPHER_SUITE_GCMP_256   0x000FAC09
#define WLAN_CIPHER_SUITE_CCMP_256   0x000FAC0A

#define WLAN_AKM_SUITE_8021X         0x000FAC01
#define WLAN_AKM_SUITE_PSK           0x000FAC02
#define WLAN_AKM_SUITE_FT_8021X      0x000FAC03
#define WLAN_AKM_SUITE_FT_PSK        0x000FAC04
#define WLAN_AKM_SUITE_8021X_SHA256  0x000FAC05
#define WLAN_AKM_SUITE_PSK_SHA256    0x000FAC06
#define WLAN_AKM_SUITE_SAE           0x000FAC08

/* An information element id we care about. 48 is RSN; 221 is the
 * vendor-specific element that carries the older WPA1 blob. */
#define WLAN_EID_RSN                48
#define WLAN_EID_VENDOR_SPECIFIC   221

/* Build a suite selector from the four bytes as they sit in an RSN IE.
 * The IE holds 00 0F AC 04 and cfg80211 wants 0x000FAC04, which is the
 * same bytes read as one big-endian number - not a host-order load, and
 * on a little-endian machine those are different. */
static inline u32 nl_suite(const u8 s[4])
{
    return ((u32)s[0] << 24) | ((u32)s[1] << 16) |
           ((u32)s[2] <<  8) |  (u32)s[3];
}

/* ── Sizes ─────────────────────────────────────────────────────────── */

/* The receive buffer, and it is load-bearing rather than arbitrary. The
 * kernel remembers the largest length ever passed to recvmsg on this
 * socket and will not build a dump message larger than that, so what we
 * choose here is also the cap on what the kernel will send us. 8 KB
 * holds a dozen scan results per read, which is several reads for a
 * busy band and no reads wasted on a quiet one. */
#define NL_RXBUF        8192
/* The largest request we build. A CONNECT with an SSID, a BSSID, the
 * cipher lists and an RSN IE is about 140 bytes; 1 KB is room for an
 * extra IE blob and still small enough to be a local. */
#define NL_TXBUF        1024

#define NL_SSID_MAX       32
#define NL_RSN_IE_MAX    128    /* a real RSN IE is under 40 bytes  */
#define NL_WPA_IE_MAX     40
#define NL_MAX_CIPHERS     5    /* cfg80211's NL80211_MAX_NR_CIPHER_SUITES */
#define NL_MAX_AKMS        4

/* How many events can pile up between two calls into this library.
 *
 * This matters more than it looks. The caller polls the netlink socket
 * next to the EAPOL socket, but it also makes blocking requests - every
 * nl_set_ptk waits for its own acknowledgement. If a DISCONNECT arrives
 * while we are waiting for that acknowledgement and we throw it away
 * because we were not looking for it, the caller sits forever waiting
 * for message 3 of a handshake whose association is already gone. So
 * events found on the way to something else are queued, never dropped.
 * When the queue does overflow the *oldest* goes, because the newest
 * event is the one that describes the current state. */
#define NL_EVQ_LEN        16

/* ── Events ────────────────────────────────────────────────────────── */

typedef enum {
    NL_EV_NONE = 0,
    NL_EV_CONNECT,          /* the firmware finished (or failed) joining */
    NL_EV_DISCONNECT,
    NL_EV_ROAM,
    NL_EV_AUTHENTICATE,     /* softmac only; a fullmac chip never sends  */
    NL_EV_ASSOCIATE,        /* these two, and their absence is a fact    */
    NL_EV_DEAUTHENTICATE,   /* worth reporting rather than hiding        */
    NL_EV_DISASSOCIATE,
    NL_EV_NEW_SCAN_RESULTS,
    NL_EV_SCAN_ABORTED,
    NL_EV_PORT_AUTHORIZED,  /* the driver did the handshake itself       */
    NL_EV_OTHER             /* an nl80211 command we do not model        */
} nl_ev_kind_t;

typedef struct {
    nl_ev_kind_t kind;
    u8   cmd;               /* the raw nl80211 command, for NL_EV_OTHER  */
    u32  ifindex;           /* 0 when the event did not name one         */
    u8   bssid[6];
    bool have_bssid;
    u16  status;            /* CONNECT: 802.11 status code, 0 = joined   */
    u16  reason;            /* DISCONNECT: 802.11 reason code            */
    bool by_ap;             /* DISCONNECT: the AP sent it, we did not    */
    bool timed_out;         /* CONNECT: no answer at all, not a refusal  */
    u32  timeout_reason;

    /* CONNECT: the RSN element the association request actually carried,
     * from NL80211_ATTR_REQ_IE. Message 2 of the handshake has to repeat
     * that element byte for byte or the AP drops it, and on a fullmac
     * chip it is the firmware that wrote the request - so what we asked
     * for and what went out are not guaranteed to be the same bytes.
     * wpa_supplicant takes its own element from here for that reason.
     * Length 0 when the driver did not report the request. */
    u8   req_rsn_ie[NL_RSN_IE_MAX];
    u8   req_rsn_ie_len;
} nl_event_t;

/* ── A scan result ─────────────────────────────────────────────────── */

typedef struct {
    u8   bssid[6];
    u8   ssid[NL_SSID_MAX];
    u8   ssid_len;          /* an SSID is bytes, not a string; a hidden
                               network has length 0 and is not "" */
    u32  freq;              /* MHz */
    s32  signal_mbm;        /* mBm: -4250 is -42.5 dBm. 0 = not reported */
    u8   signal_unspec;     /* 0..100, only when mBm is missing          */
    u16  capability;        /* the 802.11 capability field; bit 4 is
                               Privacy, i.e. "encrypted at all"          */
    bool associated;        /* this is the BSS we are joined to          */
    u32  seen_ms_ago;

    /* The raw elements, exactly as the access point sent them, so the
     * caller can see what is really on offer rather than what we thought
     * worth keeping. rsn_ie_total is what the AP sent; when it is larger
     * than rsn_ie_len the element did not fit and was clipped, and the
     * caller is being told so rather than handed a short IE that looks
     * whole. */
    u8   rsn_ie[NL_RSN_IE_MAX];
    u8   rsn_ie_len;
    u16  rsn_ie_total;
    u8   wpa_ie[NL_WPA_IE_MAX];
    u8   wpa_ie_len;
    u16  wpa_ie_total;
} nl_bss_t;

/* What an RSN information element says, decoded. */
typedef struct {
    u16  version;                       /* 1 for everything that exists  */
    u32  group_cipher;
    u32  pairwise[NL_MAX_CIPHERS];
    int  n_pairwise;
    u32  akm[NL_MAX_AKMS];
    int  n_akm;
    u16  caps;
    bool mfp_capable;
    bool mfp_required;
} nl_rsn_t;

/* ── A connection request ──────────────────────────────────────────── */

typedef struct {
    const u8 *ssid;
    u8        ssid_len;
    const u8 *bssid;        /* NULL: let the firmware pick a BSS         */
    u32       freq;         /* MHz; 0: let the firmware find the channel */

    u32  auth_type;         /* NL80211_AUTHTYPE_OPEN_SYSTEM for WPA2     */
    u32  wpa_versions;      /* NL80211_WPA_VERSION_2                     */
    u32  ciphers_pairwise[NL_MAX_CIPHERS];
    int  n_ciphers_pairwise;
    u32  cipher_group;
    u32  akm_suites[NL_MAX_AKMS];
    int  n_akm_suites;
    bool privacy;           /* the network is encrypted at all           */

    /* Extra elements for the association request - in practice the RSN
     * IE, and it is not optional. On a softmac card (ath10k, anything
     * under mac80211) the kernel's own SME builds the association
     * request, and it puts in exactly these elements: the cipher lists
     * above configure the kernel, they do not become an RSN element. An
     * association request without one is refused by any WPA2 access
     * point (status 40, "invalid element"). brcmfmac reads the element
     * from here too, into the firmware's "wpaie". So always pass it -
     * wpa_rsn_ie_build() makes the one to send. */
    const u8 *ie;
    u16       ie_len;

    /* NL80211_ATTR_CONTROL_PORT: keep the port closed to everything but
     * EAPOL until nl_authorize() opens it, after the keys are in.
     * Without it mac80211 opens the port at association, and whatever
     * the machine sends during the handshake - a DHCP renewal, a TCP
     * retransmission from before a roam - goes out in the clear before
     * there is a key to encrypt it with. wpa_supplicant sets it for the
     * same reason; nl_conn_wpa2_psk() sets it. */
    bool      control_port;
} nl_conn_t;

/* ── What an interface is ──────────────────────────────────────────── */

typedef struct {
    char name[16];
    u32  ifindex;
    u32  wiphy;
    u32  iftype;            /* NL80211_IFTYPE_STATION and friends        */
    u8   mac[6];
    bool have_mac;
} nl_iface_t;

/* ── The handle ────────────────────────────────────────────────────── */

/* Roughly 9 KB, nearly all of it the receive buffer. That is too much
 * for a deep stack on a board with 512 MB and an 8 KB kernel stack, so
 * put it in a static or on the heap, not in a recursive function. */
typedef struct {
    int  fd;
    u32  seq;               /* our request sequence; 0 is never used, so
                               a message with seq 0 is an event          */
    u16  family;            /* the nl80211 generic netlink family id     */
    u32  grp_config, grp_scan, grp_mlme, grp_reg;

    char ifname[16];
    u32  ifindex;
    u32  wiphy;

    /* The last failure, kept rather than printed, so the caller decides
     * whether it is worth a line on the console. err is a positive
     * errno; msg names the command and what the kernel said about it,
     * including the kernel's own extended-ack text when there was one. */
    int  err;
    char msg[160];

    nl_event_t evq[NL_EVQ_LEN];
    int  ev_head, ev_tail;
    u32  ev_dropped;        /* events lost to a full queue, ever         */

    u8   rx[NL_RXBUF];
} nl_t;

/* ── Lifecycle ─────────────────────────────────────────────────────── */

/* Open the socket, resolve the nl80211 family and its multicast groups,
 * find the interface, and subscribe to mlme, scan and config.
 *
 * Subscribing happens here, before anything is asked for, on purpose: a
 * scan triggered before the scan group is joined completes into a void.
 *
 * false on failure, with nl_error() saying which of those steps it was.
 * On a machine with no wireless at all it fails at family resolution
 * with ENOENT, which is the truth and not a bug. */
bool nl_open(nl_t *nl, const char *ifname);
void nl_close(nl_t *nl);

/* The last failure, as a sentence naming the command and the errno.
 * Never NULL; "" when nothing has failed. */
const char *nl_error(const nl_t *nl);
int         nl_errno(const nl_t *nl);

/* ── Scanning ──────────────────────────────────────────────────────── */

/* Ask the firmware to scan. ssid NULL scans for everything; a name asks
 * for a directed probe as well, which is the only way to find a network
 * that does not broadcast its SSID.
 *
 * This returns as soon as the kernel accepts the request. EBUSY means
 * something else - or this program a moment ago - is already scanning. */
bool nl_scan_trigger(nl_t *nl, const char *ssid_or_null);

/* Trigger, then wait for NEW_SCAN_RESULTS or SCAN_ABORTED. Events that
 * are not about the scan are queued for nl_wait as usual, so a
 * disconnect that lands during a scan is not lost.
 *
 * timeout_ms below zero uses a sensible default (10 s); a full passive
 * scan of both bands can take eight. */
bool nl_scan(nl_t *nl, const char *ssid_or_null, int timeout_ms);

/* Dump what the kernel currently holds. This is the cache, not a fresh
 * scan: it answers immediately and may be minutes old. Returns how many
 * entries were written, or -1 on failure. */
int  nl_scan_results(nl_t *nl, nl_bss_t *out, int max);

/* Decode an RSN element - the bytes in nl_bss_t.rsn_ie, id and length
 * included. false if it is malformed or truncated. */
bool nl_rsn_parse(const u8 *ie, size_t len, nl_rsn_t *out);

/* ── Connecting ────────────────────────────────────────────────────── */

/* Hand the request to the firmware. On a fullmac chip this returns once
 * the request has been *accepted*; whether it worked arrives later as an
 * NL_EV_CONNECT with a status code. Status 0 means joined.
 *
 * The interface has to be up first: cfg80211 marks CONNECT
 * NEED_NETDEV_UP and answers ENETDOWN otherwise, which is a confusing
 * thing to read when the real problem is that nobody called
 * net_if_up(). This checks, and says so. */
bool nl_connect(nl_t *nl, const nl_conn_t *req);

/* Fill in a request for the ordinary case: WPA2-PSK with CCMP, open
 * authentication, privacy on. The caller can then override anything.
 * Nothing is allocated and nothing is copied - ssid and bssid are
 * borrowed and must outlive the nl_connect call. */
void nl_conn_wpa2_psk(nl_conn_t *req, const u8 *ssid, u8 ssid_len,
                      const u8 *bssid_or_null, u32 freq_mhz);

bool nl_disconnect(nl_t *nl, u16 reason);

/* ── Events ────────────────────────────────────────────────────────── */

/* Wait for the next event. Returns false on timeout - which is not an
 * error - or on a socket failure, which sets nl_error(). */
bool nl_wait(nl_t *nl, int timeout_ms, nl_event_t *ev);

/* The socket, so the caller can poll it next to the EAPOL socket. Do
 * not read from it: the queueing in this file depends on every byte
 * going through nl_drain or nl_wait. */
int  nl_fd(const nl_t *nl);

/* Take one event without blocking. Call it in a loop after poll() says
 * the descriptor is readable: one read can carry several messages, and
 * poll will not fire again for the ones still in the queue. */
bool nl_drain(nl_t *nl, nl_event_t *ev);

/* ── Keys ──────────────────────────────────────────────────────────── */

/* Install the pairwise key, once the four-way handshake has derived it.
 *
 * The cipher is taken from the length, because that is the only thing
 * that distinguishes them in what the handshake produces: 16 bytes is
 * CCMP, 32 is TKIP (16 of key and two 8-byte MIC keys). A caller doing
 * anything else - GCMP-256 is also 32 - should use nl_set_key and say
 * which cipher it means. */
bool nl_set_ptk(nl_t *nl, const u8 bssid[6], const u8 *tk, size_t tklen);

/* Install a group key. keyidx is the one from the key data encapsulation
 * in message 3; rsc is the six-byte receive sequence counter, and
 * leaving it out means the first broadcast frames after the handshake
 * are thrown away as replays. rsc may be NULL when there is none. */
bool nl_set_gtk(nl_t *nl, int keyidx, const u8 *gtk, size_t len,
                const u8 rsc[6]);

/* Open the port to the access point for data (NL80211_CMD_SET_STATION,
 * the AUTHORIZED flag), after a CONNECT with control_port and a
 * completed handshake. A fullmac driver that keeps the port in its
 * firmware may answer EOPNOTSUPP, which the caller treats as done. */
bool nl_authorize(nl_t *nl, const u8 peer[6]);

/* The general form. addr NULL installs a group key, an address installs
 * a pairwise one. Note that no key is ever made the default transmit
 * key here: under WPA the station transmits with the pairwise key and
 * marking a group key default is what breaks it. */
bool nl_set_key(nl_t *nl, const u8 *addr_or_null, int keyidx, u32 cipher,
                const u8 *key, size_t keylen, const u8 *rsc, size_t rsclen);

/* ── Diagnosis ─────────────────────────────────────────────────────── */

/* The two-letter regulatory domain, NUL terminated, so country[4]. "00"
 * is the world-roaming default, which forbids active scanning on most
 * channels and is a real cause of "the network is simply not there". */
bool nl_get_reg(nl_t *nl, char country[4]);

/* iftype, wiphy and MAC for the interface this handle was opened on. */
bool nl_interface_info(nl_t *nl, nl_iface_t *out);

/* Names for numbers, for messages meant to be read by a person. Always
 * a string, never NULL. */
const char *nl_cmd_name(u8 cmd);
const char *nl_iftype_name(u32 iftype);
const char *nl_status_name(u16 status);
const char *nl_reason_name(u16 reason);
const char *nl_cipher_name(u32 suite);
const char *nl_akm_name(u32 suite);

#endif /* _LP_NL80211_H */
