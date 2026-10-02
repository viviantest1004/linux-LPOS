/* nl80211.c - generic netlink and nl80211, by hand.
 *
 * The header says why this exists at all. This file is the how, and it
 * is written to be read by somebody who has never built a netlink
 * message, because the alternative - a wrapper around libnl - is how we
 * ended up unable to say why the four-way handshake never started.
 *
 * ── A generic netlink request, byte by byte ──
 * One message is three things laid end to end in one datagram:
 *
 *   struct nlmsghdr {           16 bytes
 *       u32 nlmsg_len;          the whole message, header included
 *       u16 nlmsg_type;         WHICH FAMILY - not a command
 *       u16 nlmsg_flags;        REQUEST, ACK, DUMP...
 *       u32 nlmsg_seq;          ours; the answer carries it back
 *       u32 nlmsg_pid;          0 from us, the kernel fills it in
 *   }
 *   struct genlmsghdr {          4 bytes
 *       u8  cmd;                NL80211_CMD_CONNECT and friends
 *       u8  version;            1
 *       u16 reserved;           must be zero
 *   }
 *   attributes...               TLVs
 *
 * and each attribute is
 *
 *   struct nlattr { u16 nla_len; u16 nla_type; } then nla_len - 4 bytes
 *   of payload, then zero padding up to a multiple of 4.
 *
 * nla_len counts the four header bytes and the payload but NOT the
 * padding. The next attribute starts at NLA_ALIGN(nla_len) past this
 * one. Get that backwards - count the padding in nla_len, or forget to
 * skip it - and the kernel's parser walks off into the middle of your
 * payload, reads a length of nonsense and answers EINVAL, with no hint
 * about which attribute or why. Every netlink bug looks exactly the
 * same from outside, which is why there is a test harness that decodes
 * these messages back byte by byte instead of trusting them.
 *
 * A nested attribute is an attribute whose payload is more attributes.
 * Nothing else about it is special; the NLA_F_NESTED bit in nla_type is
 * advisory and the kernel's older parsers ignore it. We set it anyway.
 *
 * ── Why memcpy everywhere ──
 * The message is a byte array and offsets inside it are only guaranteed
 * to be 4-aligned. On the Pi Zero W - ARM1176, 32-bit, ARMv6 - loading
 * a u32 through a misaligned pointer does not fault, it silently
 * rotates the word, which is worse. And a u64 attribute (WDEV) is only
 * 4-aligned even in a well-formed message. So nothing in this file
 * casts a pointer into the buffer; everything goes through memcpy and
 * the compiler turns the aligned cases back into single loads.
 *
 * ── The connect flow on a fullmac chip ──
 *      nl_open          resolve "nl80211" -> family id and group ids,
 *                       join mlme + scan + config
 *      nl_scan          TRIGGER_SCAN, then wait for the NEW_SCAN_RESULTS
 *                       event on the scan group
 *      nl_scan_results  GET_SCAN as a dump; each message is one BSS
 *      nl_connect       CONNECT with SSID, BSSID, auth OPEN, WPA2, the
 *                       cipher lists, AKM PSK and our RSN element
 *                       (which a softmac card's association request is
 *                       built from). Returns when the kernel has
 *                       ACCEPTED it.
 *      nl_wait          the CONNECT event, carrying the 802.11 status
 *                       code. 0 means the firmware authenticated and
 *                       associated for us.
 *      ... the four-way handshake now runs as ordinary EAPOL frames on
 *          wlan0, which is somebody else's file ...
 *      nl_set_ptk       NEW_KEY, pairwise, addressed to the AP
 *      nl_set_gtk       NEW_KEY, group, with the RSC from message 3
 *
 * There is no AUTHENTICATE and no ASSOCIATE here, and their absence is
 * the point: on this chip the firmware does both and the host never
 * sees the frames.
 */
#include "nl80211.h"
#include "net.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

/* Socket flags we need and net.h does not have. Same values on arm,
 * arm64 and x86-64 - these come from include/linux/socket.h, which is
 * not one of the headers that was renumbered per architecture. */
#define MSG_TRUNC       0x20
#define MSG_DONTWAIT    0x40
#define SO_RCVBUF        8      /* asm-generic/socket.h */

/* A netlink socket buffer big enough that a burst of events during a
 * scan does not overflow it. An overflow is not silent - recvfrom
 * answers ENOBUFS - but a lost DISCONNECT is still a supplicant waiting
 * forever, so it is cheaper to make it not happen. */
#define NL_SO_RCVBUF    (256 * 1024)

#define NLMSG_HDRLEN    16
#define GENL_HDRLEN     4
#define NLA_HDRLEN      4

#define NL_DEFAULT_TIMEOUT_MS   3000    /* a request the kernel answers
                                           locally; if this expires
                                           something is badly wrong  */
#define NL_SCAN_TIMEOUT_MS     10000    /* a real radio scan of both
                                           bands takes several seconds */

/* struct sockaddr_nl. Twelve bytes, identical on all three machines. */
typedef struct {
    u16 nl_family;
    u16 nl_pad;
    u32 nl_pid;
    u32 nl_groups;
} sockaddr_nl_t;

/* ── Little-endian-agnostic loads and stores ───────────────────────────
 *
 * Host order, through memcpy, always. See the note at the top. */
static void put_u16_at(u8 *p, u16 v) { memcpy(p, &v, 2); }
static void put_u32_at(u8 *p, u32 v) { memcpy(p, &v, 4); }
static u16  get_u16_at(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }
static u32  get_u32_at(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }

static size_t nla_align(size_t n) { return (n + 3u) & ~(size_t)3u; }

/* ══ Building a message ══════════════════════════════════════════════
 *
 * A tiny append-only writer over a caller-supplied buffer. Every put
 * returns false if it would not fit, and the failure is sticky, so a
 * caller can build a whole message and check once at the end rather
 * than after every attribute. A half-written message is never sent.
 *
 * Every command below is split into a build_xxx() that produces bytes
 * and a public function that sends them. That split is not tidiness:
 * this build host has no radio and no cfg80211, so the only way to know
 * the messages are right is to build them here and decode them back
 * with an independent parser. If building only happened on the way to a
 * socket, none of it could be checked until the board was in front of
 * us - which is the position this file was written to get out of. */

typedef struct {
    u8    *buf;
    size_t cap;
    size_t len;
    bool   ok;
} nlbuild_t;

static void msg_begin(nlbuild_t *m, u8 *buf, size_t cap,
                      u16 family, u8 cmd, u16 flags, u32 seq)
{
    m->buf = buf;
    m->cap = cap;
    m->len = 0;
    m->ok  = true;

    if (cap < NLMSG_HDRLEN + GENL_HDRLEN) {
        m->ok = false;
        return;
    }
    memset(buf, 0, NLMSG_HDRLEN + GENL_HDRLEN);

    /* nlmsg_len is written last, by msg_finish, because we do not know
     * it yet. Leaving it zero in the meantime is deliberate: a message
     * that escapes unfinished is rejected by the kernel rather than
     * being interpreted as something shorter than it is. */
    put_u32_at(buf + 0,  0);            /* nlmsg_len, later     */
    put_u16_at(buf + 4,  family);       /* nlmsg_type           */
    put_u16_at(buf + 6,  flags);        /* nlmsg_flags          */
    put_u32_at(buf + 8,  seq);          /* nlmsg_seq            */
    put_u32_at(buf + 12, 0);            /* nlmsg_pid: the kernel
                                           fills in ours        */
    buf[NLMSG_HDRLEN + 0] = cmd;        /* genlmsghdr.cmd       */
    buf[NLMSG_HDRLEN + 1] = 1;          /* genlmsghdr.version   */
    put_u16_at(buf + NLMSG_HDRLEN + 2, 0);  /* reserved         */

    m->len = NLMSG_HDRLEN + GENL_HDRLEN;
}

/* Reserve room for one attribute and return where its payload goes, or
 * NULL when it does not fit. The padding bytes are zeroed: the kernel
 * does not look at them, but a message whose padding is stack rubbish
 * is a message that hexdumps differently every run, and this file is
 * meant to be verified by hexdump. */
static u8 *attr_reserve(nlbuild_t *m, u16 type, size_t paylen)
{
    if (!m->ok)
        return NULL;

    size_t total = NLA_HDRLEN + paylen;
    size_t step  = nla_align(total);

    if (paylen > 0xFFFFu - NLA_HDRLEN || m->len + step > m->cap) {
        m->ok = false;
        return NULL;
    }

    u8 *at = m->buf + m->len;
    put_u16_at(at + 0, (u16)total);
    put_u16_at(at + 2, type);
    memset(at + NLA_HDRLEN, 0, step - NLA_HDRLEN);
    m->len += step;
    return at + NLA_HDRLEN;
}

static bool put_bytes(nlbuild_t *m, u16 type, const void *data, size_t n)
{
    u8 *p = attr_reserve(m, type, n);
    if (!p)
        return false;
    if (n)
        memcpy(p, data, n);
    return true;
}

static bool put_u8(nlbuild_t *m, u16 type, u8 v)
{
    return put_bytes(m, type, &v, 1);
}

static bool put_u16(nlbuild_t *m, u16 type, u16 v)
{
    u8 t[2];
    put_u16_at(t, v);
    return put_bytes(m, type, t, 2);
}

static bool put_u32(nlbuild_t *m, u16 type, u32 v)
{
    u8 t[4];
    put_u32_at(t, v);
    return put_bytes(m, type, t, 4);
}

/* A flag attribute carries nothing at all: four bytes of header, a
 * length of exactly 4, no payload. Its presence is the value. Sending
 * a zero byte instead - which looks harmless - makes cfg80211 read it
 * as present anyway, but the policy check rejects the length first. */
static bool put_flag(nlbuild_t *m, u16 type)
{
    return attr_reserve(m, type, 0) != NULL;
}

/* An array of suite selectors is NOT nested. cfg80211 reads
 * NL80211_ATTR_AKM_SUITES with nla_data()/nla_len() and divides by four
 * - it is one attribute whose payload is n * 4 raw bytes. Writing it as
 * a nest of u32 attributes is the other classic way to earn EINVAL. */
static bool put_u32_array(nlbuild_t *m, u16 type, const u32 *v, int n)
{
    if (n <= 0)
        return true;
    u8 *p = attr_reserve(m, type, (size_t)n * 4);
    if (!p)
        return false;
    for (int i = 0; i < n; i++)
        put_u32_at(p + i * 4, v[i]);
    return true;
}

/* Start a nest and remember where its header is. The length is written
 * when the nest closes, because until then we do not know it. */
static size_t nest_start(nlbuild_t *m, u16 type)
{
    size_t at = m->len;
    if (!attr_reserve(m, (u16)(type | NLA_F_NESTED), 0))
        return 0;
    return at + 1;              /* +1 so that 0 can mean "failed" */
}

static void nest_end(nlbuild_t *m, size_t mark)
{
    if (!m->ok || mark == 0)
        return;
    size_t at = mark - 1;
    if (m->len - at > 0xFFFFu) {         /* cannot happen with a 1 KB
                                            buffer, and a silent wrap
                                            here would be invisible */
        m->ok = false;
        return;
    }
    put_u16_at(m->buf + at, (u16)(m->len - at));
}

/* Stamp nlmsg_len. Nothing may be appended afterwards. */
static bool msg_finish(nlbuild_t *m)
{
    if (!m->ok)
        return false;
    put_u32_at(m->buf + 0, (u32)m->len);
    return true;
}

/* ══ Reading a message ═══════════════════════════════════════════════
 *
 * Everything the kernel sends is checked before it is believed. Not out
 * of paranoia about the kernel - out of paranoia about us: a message
 * that was truncated because the receive buffer was too small looks
 * exactly like a well-formed one until you check the lengths, and
 * reading two bytes past the end of an 8 KB buffer is the kind of bug
 * that shows up as a wrong SSID six months later.
 *
 * The rule for both walkers below: an element must have room for its
 * header, must declare a length that is at least its header, and must
 * not claim to extend past the end of the buffer. Anything else stops
 * the walk and sets *bad. */

/* Step to the next attribute. false ends the walk; *bad distinguishes
 * "ran out cleanly" from "the buffer is malformed". */
static bool nla_next(const u8 *buf, size_t len, size_t *off,
                     u16 *type, const u8 **pay, u16 *paylen, bool *bad)
{
    if (*off > len || len - *off < NLA_HDRLEN)
        return false;                   /* clean end, or trailing pad */

    u16 alen  = get_u16_at(buf + *off);
    u16 atype = get_u16_at(buf + *off + 2);

    if (alen < NLA_HDRLEN || (size_t)alen > len - *off) {
        *bad = true;
        return false;
    }

    *type   = (u16)(atype & NLA_TYPE_MASK);
    *pay    = buf + *off + NLA_HDRLEN;
    *paylen = (u16)(alen - NLA_HDRLEN);
    *off   += nla_align(alen);
    return true;
}

/* Find one attribute by type. Returns false when it is absent, which is
 * not an error - most nl80211 attributes are optional. */
static bool nla_find(const u8 *buf, size_t len, u16 want,
                     const u8 **pay, u16 *paylen)
{
    size_t off = 0;
    u16 type, plen;
    const u8 *p;
    bool bad = false;

    while (nla_next(buf, len, &off, &type, &p, &plen, &bad)) {
        if (type == want) {
            *pay    = p;
            *paylen = plen;
            return true;
        }
    }
    return false;
}

static bool nla_get_u32v(const u8 *buf, size_t len, u16 want, u32 *out)
{
    const u8 *p; u16 n;
    if (!nla_find(buf, len, want, &p, &n) || n < 4)
        return false;
    *out = get_u32_at(p);
    return true;
}

static bool nla_get_u16v(const u8 *buf, size_t len, u16 want, u16 *out)
{
    const u8 *p; u16 n;
    if (!nla_find(buf, len, want, &p, &n) || n < 2)
        return false;
    *out = get_u16_at(p);
    return true;
}

static bool nla_has(const u8 *buf, size_t len, u16 want)
{
    const u8 *p; u16 n;
    return nla_find(buf, len, want, &p, &n);
}

/* Step to the next netlink message in a datagram. One read can carry
 * several - that is how a dump arrives - so this is a loop, not a
 * single parse. */
typedef struct {
    u16       type;
    u16       flags;
    u32       seq;
    u32       pid;
    u8        cmd;          /* genlmsghdr.cmd, when there is one */
    const u8 *attrs;        /* payload past both headers         */
    size_t    attrlen;
    const u8 *raw;          /* payload past the nlmsghdr only    */
    size_t    rawlen;
} nlmsg_t;

static bool nlmsg_next(const u8 *buf, size_t len, size_t *off,
                       nlmsg_t *out, bool *bad)
{
    if (*off > len || len - *off < NLMSG_HDRLEN)
        return false;

    u32 mlen = get_u32_at(buf + *off);
    if (mlen < NLMSG_HDRLEN || (size_t)mlen > len - *off) {
        *bad = true;
        return false;
    }

    out->type  = get_u16_at(buf + *off + 4);
    out->flags = get_u16_at(buf + *off + 6);
    out->seq   = get_u32_at(buf + *off + 8);
    out->pid   = get_u32_at(buf + *off + 12);

    out->raw    = buf + *off + NLMSG_HDRLEN;
    out->rawlen = (size_t)mlen - NLMSG_HDRLEN;

    /* Only a family message has a genlmsghdr; NLMSG_ERROR and
     * NLMSG_DONE are netlink's own and have none. */
    if (out->type >= NLMSG_MIN_TYPE && out->rawlen >= GENL_HDRLEN) {
        out->cmd     = out->raw[0];
        out->attrs   = out->raw + GENL_HDRLEN;
        out->attrlen = out->rawlen - GENL_HDRLEN;
    } else {
        out->cmd     = 0;
        out->attrs   = NULL;
        out->attrlen = 0;
    }

    *off += nla_align(mlen);    /* NLMSG_ALIGN is the same 4-byte rule */
    return true;
}

/* ══ Errors ══════════════════════════════════════════════════════════ */

static void nl_fail(nl_t *nl, int err, const char *what)
{
    nl->err = err < 0 ? -err : err;
    snprintf(nl->msg, sizeof nl->msg, "%s: %s (%d)",
             what, lp_strerror(nl->err), nl->err);
}

static void nl_fail_msg(nl_t *nl, int err, const char *what, const char *extra)
{
    nl->err = err < 0 ? -err : err;
    snprintf(nl->msg, sizeof nl->msg, "%s: %s (%d): %s",
             what, lp_strerror(nl->err), nl->err, extra);
}

const char *nl_error(const nl_t *nl) { return nl->msg; }
int         nl_errno(const nl_t *nl) { return nl->err; }
int         nl_fd(const nl_t *nl)    { return nl->fd; }

/* The kernel's own words for why it said no.
 *
 * With NETLINK_EXT_ACK turned on, an NLMSG_ERROR can carry a string
 * saying which attribute was wrong - "invalid cipher suite", say -
 * instead of only the number 22. That string is the whole reason this
 * library is worth writing, so it is worth the fiddly offset
 * arithmetic below.
 *
 * The error payload is { s32 error; struct nlmsghdr failed_request; }.
 * If NLM_F_CAPPED is set, the echoed request stops at its header and
 * the TLVs begin right after; if not, the whole failed request was
 * copied in and the TLVs begin after it. NLM_F_ACK_TLVS says whether
 * there are any at all. */
static bool ext_ack_text(const nlmsg_t *m, char *out, size_t outn)
{
    out[0] = '\0';

    if (!(m->flags & NLM_F_ACK_TLVS) || m->rawlen < 4 + NLMSG_HDRLEN)
        return false;

    size_t tlv_off;
    if (m->flags & NLM_F_CAPPED) {
        tlv_off = nla_align(4 + NLMSG_HDRLEN);
    } else {
        u32 inner = get_u32_at(m->raw + 4);      /* failed_request.len */
        if (inner < NLMSG_HDRLEN)
            return false;
        tlv_off = nla_align((size_t)4 + inner);
    }
    if (tlv_off >= m->rawlen)
        return false;

    const u8 *p; u16 n;
    if (!nla_find(m->raw + tlv_off, m->rawlen - tlv_off,
                  NLMSGERR_ATTR_MSG, &p, &n) || n == 0)
        return false;

    size_t copy = n;
    if (copy >= outn)
        copy = outn - 1;
    /* The kernel's string is NUL terminated inside the attribute; trust
     * the attribute length, not the NUL. */
    for (size_t i = 0; i < copy; i++)
        out[i] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : ' ';
    out[copy] = '\0';
    /* Drop the trailing spaces a NUL turned into. */
    while (copy > 0 && out[copy - 1] == ' ')
        out[--copy] = '\0';
    return copy > 0;
}

/* ══ The event queue ═════════════════════════════════════════════════ */

static void ev_push(nl_t *nl, const nl_event_t *ev)
{
    int next = (nl->ev_tail + 1) % NL_EVQ_LEN;
    if (next == nl->ev_head) {
        /* Full. Drop the oldest, not the newest: the newest event is
         * the one that describes where the radio is now. */
        nl->ev_head = (nl->ev_head + 1) % NL_EVQ_LEN;
        nl->ev_dropped++;
    }
    nl->evq[nl->ev_tail] = *ev;
    nl->ev_tail = next;
}

static bool ev_pop(nl_t *nl, nl_event_t *out)
{
    if (nl->ev_head == nl->ev_tail)
        return false;
    *out = nl->evq[nl->ev_head];
    nl->ev_head = (nl->ev_head + 1) % NL_EVQ_LEN;
    return true;
}

/* Take the first queued event of one of two kinds and leave everything
 * else in the queue, in order.
 *
 * This exists because nl_scan waits for one particular event while
 * other events are still arriving for the caller. The obvious way to
 * write that - pop, and push back anything uninteresting - reorders the
 * queue and, worse, spins: popping is instant when the queue is not
 * empty, so a scan waiting eight seconds with one unrelated event
 * already queued would push and pop it a few million times on a board
 * that has one slow core to spare. */
static bool ev_take_kind(nl_t *nl, nl_ev_kind_t a, nl_ev_kind_t b,
                         nl_event_t *out)
{
    for (int i = nl->ev_head; i != nl->ev_tail; i = (i + 1) % NL_EVQ_LEN) {
        if (nl->evq[i].kind != a && nl->evq[i].kind != b)
            continue;
        *out = nl->evq[i];
        /* Close the gap by sliding everything in front of it one place
         * later and advancing the head, so the events the caller has
         * not seen yet keep the order they arrived in. */
        for (int j = i; j != nl->ev_head; ) {
            int prev = (j + NL_EVQ_LEN - 1) % NL_EVQ_LEN;
            nl->evq[j] = nl->evq[prev];
            j = prev;
        }
        nl->ev_head = (nl->ev_head + 1) % NL_EVQ_LEN;
        return true;
    }
    return false;
}

static nl_ev_kind_t ev_kind_of(u8 cmd)
{
    switch (cmd) {
    case NL80211_CMD_CONNECT:           return NL_EV_CONNECT;
    case NL80211_CMD_DISCONNECT:        return NL_EV_DISCONNECT;
    case NL80211_CMD_ROAM:              return NL_EV_ROAM;
    case NL80211_CMD_AUTHENTICATE:      return NL_EV_AUTHENTICATE;
    case NL80211_CMD_ASSOCIATE:         return NL_EV_ASSOCIATE;
    case NL80211_CMD_DEAUTHENTICATE:    return NL_EV_DEAUTHENTICATE;
    case NL80211_CMD_DISASSOCIATE:      return NL_EV_DISASSOCIATE;
    case NL80211_CMD_NEW_SCAN_RESULTS:  return NL_EV_NEW_SCAN_RESULTS;
    case NL80211_CMD_SCAN_ABORTED:      return NL_EV_SCAN_ABORTED;
    case NL80211_CMD_PORT_AUTHORIZED:   return NL_EV_PORT_AUTHORIZED;
    default:                            return NL_EV_OTHER;
    }
}

static void ie_copy(const u8 *ies, u16 ielen, u8 want_id,
                    const u8 *want_oui, u8 want_oui_type,
                    u8 *out, size_t outcap, u8 *out_len, u16 *out_total);

/* Turn one broadcast nl80211 message into an event. */
static void ev_parse(const nlmsg_t *m, nl_event_t *ev)
{
    const u8 *p; u16 n;

    memset(ev, 0, sizeof *ev);
    ev->cmd  = m->cmd;
    ev->kind = ev_kind_of(m->cmd);

    if (!m->attrs)
        return;

    (void)nla_get_u32v(m->attrs, m->attrlen, NL80211_ATTR_IFINDEX,
                       &ev->ifindex);

    if (nla_find(m->attrs, m->attrlen, NL80211_ATTR_MAC, &p, &n) && n >= 6) {
        memcpy(ev->bssid, p, 6);
        ev->have_bssid = true;
    }

    (void)nla_get_u16v(m->attrs, m->attrlen, NL80211_ATTR_STATUS_CODE,
                       &ev->status);
    (void)nla_get_u16v(m->attrs, m->attrlen, NL80211_ATTR_REASON_CODE,
                       &ev->reason);
    (void)nla_get_u32v(m->attrs, m->attrlen, NL80211_ATTR_TIMEOUT_REASON,
                       &ev->timeout_reason);

    ev->by_ap     = nla_has(m->attrs, m->attrlen,
                            NL80211_ATTR_DISCONNECTED_BY_AP);
    ev->timed_out = nla_has(m->attrs, m->attrlen, NL80211_ATTR_TIMED_OUT);

    /* The RSN element our association request really carried. The
     * handshake repeats it in message 2, and an AP that sees a different
     * one there drops the message without a word - so it is taken from
     * what the driver says it sent, not from what we asked it to send.
     * A clipped element is worse than none: it would be repeated wrong. */
    if ((ev->kind == NL_EV_CONNECT || ev->kind == NL_EV_ROAM) &&
        nla_find(m->attrs, m->attrlen, NL80211_ATTR_REQ_IE, &p, &n)) {
        u16 total = 0;
        ie_copy(p, n, WLAN_EID_RSN, NULL, 0, ev->req_rsn_ie,
                sizeof ev->req_rsn_ie, &ev->req_rsn_ie_len, &total);
        if (total > sizeof ev->req_rsn_ie)
            ev->req_rsn_ie_len = 0;
    }
}

/* ══ The socket ══════════════════════════════════════════════════════ */

/* Read one datagram, with a deadline. Returns the number of bytes in
 * nl->rx, 0 on timeout, or -errno.
 *
 * MSG_TRUNC is not a mistake. Without it a message longer than the
 * buffer is silently cut and recvfrom reports the buffer size, so a
 * truncated scan result looks like a short one. With it recvfrom
 * reports the real length, and a value larger than the buffer tells us
 * we lost something - which we then say out loud instead of parsing
 * rubbish. */
static long nl_recv_once(nl_t *nl, int timeout_ms)
{
    lp_pollfd_t pfd;
    pfd.fd      = nl->fd;
    pfd.events  = LP_POLLIN;
    pfd.revents = 0;

    long pr = lp_poll(&pfd, 1, timeout_ms);
    if (pr == 0)
        return 0;
    if (pr < 0)
        return pr == -4 /* EINTR */ ? 0 : pr;
    if (!(pfd.revents & LP_POLLIN))
        return 0;

    sockaddr_nl_t from;
    u32 fromlen = sizeof from;
    memset(&from, 0, sizeof from);

    long got = lp_recvfrom(nl->fd, nl->rx, sizeof nl->rx,
                           MSG_TRUNC | MSG_DONTWAIT, &from, &fromlen);
    if (got == -11 /* EAGAIN */)
        return 0;                       /* poll said readable and it was
                                           not; treat as a quiet moment
                                           rather than as a failure */
    if (got < 0)
        return got;
    if (got == 0)
        return 0;

    /* Who sent it. Netlink stamps the sender's port id in the source
     * address, and a userspace socket can never have port id 0 - the
     * kernel autobinds everybody else to something nonzero, and reserves
     * 0 for itself. So this one comparison is what stops another process
     * on this machine from feeding us a fabricated DISCONNECT.
     *
     * A short address is refused rather than trusted: if we cannot see
     * who sent it, the answer is not "probably the kernel". */
    if (fromlen < sizeof from || from.nl_pid != 0)
        return 0;

    if ((size_t)got > sizeof nl->rx) {
        /* The kernel had more to say than we had room for. Say so; do
         * not parse the fragment we got. */
        nl_fail(nl, 75 /* EOVERFLOW */, "netlink message longer than the "
                                        "receive buffer");
        return 0;
    }
    return got;
}

/* ══ Request and response ════════════════════════════════════════════
 *
 * Send one request and read until its acknowledgement, its error, or
 * the end of its dump. Messages that are not the answer to this request
 * are events, and they go on the queue rather than in the bin - see the
 * note by NL_EVQ_LEN for why that matters so much here. */

typedef bool (*nl_reply_fn)(nl_t *nl, const nlmsg_t *m, void *arg);

/* What one datagram did to the transaction in progress. */
typedef enum { NL_STEP_MORE, NL_STEP_DONE, NL_STEP_ERROR } nl_step_t;

/* Handle every message in one datagram.
 *
 * This is a separate function so that the interleaving it exists to get
 * right - an event landing in the middle of somebody else's reply - can
 * be tested by handing it a buffer, on a machine with no radio. It is
 * the only piece of this file where a mistake is invisible: dropping
 * the event still leaves the transaction working, and the failure only
 * shows up later as a supplicant waiting for a message that will never
 * come because the association it belongs to is already gone. */
static nl_step_t nl_dispatch(nl_t *nl, const u8 *buf, size_t len, u32 seq,
                             const char *what, nl_reply_fn *cb, void *arg,
                             int *err)
{
    size_t off = 0;
    bool bad = false;
    nlmsg_t m;

    while (nlmsg_next(buf, len, &off, &m, &bad)) {
        if (m.seq != seq) {
            /* Not the answer to this request. Every nl80211 broadcast
             * carries seq 0, so this is where events are picked up. */
            if (m.type == nl->family && m.attrs) {
                nl_event_t ev;
                ev_parse(&m, &ev);
                ev_push(nl, &ev);
            }
            continue;
        }

        if (m.type == NLMSG_ERROR) {
            if (m.rawlen < 4) {
                nl_fail(nl, 74 /* EBADMSG */, what);
                *err = -74;
                return NL_STEP_ERROR;
            }
            s32 e;
            memcpy(&e, m.raw, 4);
            if (e == 0)
                return NL_STEP_DONE;    /* an acknowledgement, not an error */

            char extra[96];
            if (ext_ack_text(&m, extra, sizeof extra))
                nl_fail_msg(nl, (int)e, what, extra);
            else
                nl_fail(nl, (int)e, what);
            *err = e < 0 ? (int)e : -(int)e;
            return NL_STEP_ERROR;
        }

        if (m.type == NLMSG_DONE)
            return NL_STEP_DONE;

        if (m.type == NLMSG_NOOP)
            continue;

        if (m.type == NLMSG_OVERRUN) {
            nl_fail(nl, 105 /* ENOBUFS */, what);
            *err = -105;
            return NL_STEP_ERROR;
        }

        if (*cb && !(*cb)(nl, &m, arg)) {
            /* The callback has what it needs and does not want the rest
             * of a dump. Keep draining to the DONE anyway, or the
             * leftovers turn up as the answer to the next request and
             * nothing makes sense after that. */
            *cb = NULL;
        }

        /* Do NOT stop here, even for a single reply. Every request this
         * file sends sets NLM_F_ACK, so the kernel sends the reply and
         * then an acknowledgement, sometimes in two datagrams. Stopping
         * at the reply leaves the ACK in the socket for the next
         * transaction to trip over. The loop ends at the NLMSG_ERROR
         * above, which for a request that worked carries error 0. */
    }

    if (bad) {
        nl_fail(nl, 74 /* EBADMSG */,
                "malformed netlink message from the kernel");
        *err = -74;
        return NL_STEP_ERROR;
    }
    return NL_STEP_MORE;
}

static int nl_transact(nl_t *nl, const u8 *req, size_t reqlen, u32 seq,
                       const char *what, nl_reply_fn cb, void *arg,
                       int timeout_ms)
{
    sockaddr_nl_t to;
    memset(&to, 0, sizeof to);
    to.nl_family = NL_AF_NETLINK;       /* pid 0, groups 0: the kernel */

    long sent = lp_sendto(nl->fd, req, reqlen, 0, &to, sizeof to);
    if (sent < 0) {
        nl_fail(nl, (int)sent, what);
        return (int)sent;
    }

    s64 deadline = lp_monotonic_ms() + timeout_ms;

    for (;;) {
        s64 left = deadline - lp_monotonic_ms();
        if (left <= 0) {
            nl_fail(nl, 110 /* ETIMEDOUT */, what);
            return -110;
        }

        long got = nl_recv_once(nl, (int)left);
        if (got < 0) {
            /* ENOBUFS means the socket buffer overran and the kernel
             * dropped broadcast messages. Nothing is wrong with this
             * request, but events were lost, and the caller deserves to
             * know that rather than to wonder later. */
            if (got == -105 /* ENOBUFS */) {
                nl->ev_dropped++;
                continue;
            }
            nl_fail(nl, (int)got, what);
            return (int)got;
        }
        if (got == 0)
            continue;

        int rc = 0;
        nl_step_t st = nl_dispatch(nl, nl->rx, (size_t)got, seq, what,
                                   &cb, arg, &rc);
        if (st == NL_STEP_DONE)
            return 0;
        if (st == NL_STEP_ERROR)
            return rc;
    }
}

/* ══ Family resolution ═══════════════════════════════════════════════
 *
 * The one bootstrapping step. nlmsg_type is a family id, and nl80211's
 * is assigned when cfg80211 registers - it is not a constant and it
 * differs between boots. So the first message goes to nlctrl, whose id
 * IS a constant (16), asking CTRL_CMD_GETFAMILY with the name.
 *
 * The reply carries CTRL_ATTR_FAMILY_ID and, nested under
 * CTRL_ATTR_MCAST_GROUPS, one sub-nest per group. The sub-nest's *type*
 * is just an array index (1, 2, 3...) with no meaning; what matters is
 * the NAME and ID attributes inside it. Those ids are what
 * NETLINK_ADD_MEMBERSHIP takes, and without joining a group no event
 * from it is ever delivered - the socket simply stays quiet, which is
 * indistinguishable from a radio that is not doing anything. */

typedef struct {
    u16  id;
    bool found;
    u32  grp_config, grp_scan, grp_mlme, grp_reg;
} getfamily_t;

static bool getfamily_cb(nl_t *nl, const nlmsg_t *m, void *arg)
{
    getfamily_t *g = arg;
    const u8 *p; u16 n;

    (void)nl;
    if (!m->attrs)
        return true;

    u32 id;
    if (nla_get_u32v(m->attrs, m->attrlen, CTRL_ATTR_FAMILY_ID, &id)) {
        g->id = (u16)id;
        g->found = true;
    } else if (nla_find(m->attrs, m->attrlen, CTRL_ATTR_FAMILY_ID, &p, &n)
               && n >= 2) {
        /* nlctrl declares FAMILY_ID as u16 in its policy and writes it
         * with nla_put_u16, so the attribute really is two bytes. The
         * u32 read above is the belt; this is the braces. */
        g->id = get_u16_at(p);
        g->found = true;
    }

    if (!nla_find(m->attrs, m->attrlen, CTRL_ATTR_MCAST_GROUPS, &p, &n))
        return true;

    size_t off = 0;
    u16 type, plen;
    const u8 *grp;
    bool bad = false;

    while (nla_next(p, n, &off, &type, &grp, &plen, &bad)) {
        const u8 *np; u16 nn;
        u32 gid;

        if (!nla_find(grp, plen, CTRL_ATTR_MCAST_GRP_NAME, &np, &nn))
            continue;
        if (!nla_get_u32v(grp, plen, CTRL_ATTR_MCAST_GRP_ID, &gid))
            continue;

        /* The name is NUL terminated inside its attribute. Compare on a
         * bounded copy rather than on the buffer, because a malicious
         * or truncated attribute need not contain the NUL at all. */
        char name[24];
        size_t cn = nn < sizeof name ? nn : sizeof name - 1;
        memcpy(name, np, cn);
        name[cn] = '\0';

        if (strcmp(name, NL80211_GROUP_CONFIG) == 0) g->grp_config = gid;
        else if (strcmp(name, NL80211_GROUP_SCAN) == 0) g->grp_scan = gid;
        else if (strcmp(name, NL80211_GROUP_MLME) == 0) g->grp_mlme = gid;
        else if (strcmp(name, NL80211_GROUP_REG) == 0) g->grp_reg = gid;
    }
    return true;
}

static bool build_getfamily(nlbuild_t *m, u8 *buf, size_t cap, u32 seq,
                            const char *name)
{
    msg_begin(m, buf, cap, GENL_ID_CTRL, CTRL_CMD_GETFAMILY,
              NLM_F_REQUEST | NLM_F_ACK, seq);
    /* The name goes in with its NUL: nlctrl's policy is NLA_NUL_STRING
     * and a name without the terminator is rejected. */
    put_bytes(m, CTRL_ATTR_FAMILY_NAME, name, strlen(name) + 1);
    return msg_finish(m);
}

static bool resolve_family(nl_t *nl, const char *name, getfamily_t *out)
{
    u8 buf[128];
    nlbuild_t m;
    u32 seq = ++nl->seq;

    memset(out, 0, sizeof *out);

    if (!build_getfamily(&m, buf, sizeof buf, seq, name)) {
        nl_fail(nl, 22 /* EINVAL */, "CTRL_CMD_GETFAMILY: request too large");
        return false;
    }

    char what[64];
    snprintf(what, sizeof what, "CTRL_CMD_GETFAMILY(%s)", name);

    int rc = nl_transact(nl, buf, m.len, seq, what, getfamily_cb, out,
                         NL_DEFAULT_TIMEOUT_MS);
    if (rc < 0)
        return false;
    if (!out->found) {
        nl_fail(nl, 2 /* ENOENT */, what);
        return false;
    }
    return true;
}

/* ══ Opening ═════════════════════════════════════════════════════════ */

static bool join_group(nl_t *nl, u32 gid, const char *name)
{
    if (gid == 0)
        return true;                    /* this kernel has no such group */

    long rc = lp_setsockopt(nl->fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP,
                            &gid, sizeof gid);
    if (rc < 0) {
        char what[64];
        snprintf(what, sizeof what, "joining the \"%s\" multicast group",
                 name);
        nl_fail(nl, (int)rc, what);
        return false;
    }
    return true;
}

/* Pull iftype, wiphy and MAC out of a GET_INTERFACE reply. */
static bool iface_cb(nl_t *nl, const nlmsg_t *m, void *arg)
{
    nl_iface_t *out = arg;
    const u8 *p; u16 n;

    (void)nl;
    if (!m->attrs)
        return true;

    (void)nla_get_u32v(m->attrs, m->attrlen, NL80211_ATTR_IFINDEX,
                       &out->ifindex);
    (void)nla_get_u32v(m->attrs, m->attrlen, NL80211_ATTR_WIPHY, &out->wiphy);
    (void)nla_get_u32v(m->attrs, m->attrlen, NL80211_ATTR_IFTYPE,
                       &out->iftype);

    if (nla_find(m->attrs, m->attrlen, NL80211_ATTR_MAC, &p, &n) && n >= 6) {
        memcpy(out->mac, p, 6);
        out->have_mac = true;
    }
    if (nla_find(m->attrs, m->attrlen, NL80211_ATTR_IFNAME, &p, &n) && n > 0) {
        size_t cn = n < sizeof out->name ? n : sizeof out->name - 1;
        memcpy(out->name, p, cn);
        out->name[cn] = '\0';
    }
    return true;
}

static bool build_get_interface(nl_t *nl, nlbuild_t *m, u8 *buf, size_t cap,
                                u32 seq)
{
    msg_begin(m, buf, cap, nl->family, NL80211_CMD_GET_INTERFACE,
              NLM_F_REQUEST | NLM_F_ACK, seq);
    put_u32(m, NL80211_ATTR_IFINDEX, nl->ifindex);
    return msg_finish(m);
}

bool nl_interface_info(nl_t *nl, nl_iface_t *out)
{
    u8 buf[64];
    nlbuild_t m;
    u32 seq = ++nl->seq;

    memset(out, 0, sizeof *out);

    if (!build_get_interface(nl, &m, buf, sizeof buf, seq)) {
        nl_fail(nl, 22, "GET_INTERFACE: request too large");
        return false;
    }
    return nl_transact(nl, buf, m.len, seq, "NL80211_CMD_GET_INTERFACE",
                       iface_cb, out, NL_DEFAULT_TIMEOUT_MS) == 0;
}

bool nl_open(nl_t *nl, const char *ifname)
{
    memset(nl, 0, sizeof *nl);
    nl->fd = -1;
    strlcpy(nl->ifname, ifname, sizeof nl->ifname);

    /* SOCK_RAW rather than SOCK_DGRAM. Netlink treats the two the same
     * - automount's uevent socket uses SOCK_DGRAM - and every generic
     * netlink tool in existence uses SOCK_RAW, so this one matches what
     * anybody comparing against `iw` will see on strace. */
    long fd = lp_socket(NL_AF_NETLINK, NL_SOCK_RAW, NETLINK_GENERIC);
    if (fd < 0) {
        nl_fail(nl, (int)fd, "opening a generic netlink socket");
        return false;
    }
    nl->fd = (int)fd;

    int rcvbuf = NL_SO_RCVBUF;
    lp_setsockopt(nl->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);

    /* Ask for the kernel's own explanation when it rejects something.
     * Old kernels do not have this; failing to set it is not fatal and
     * only costs us the text. */
    int on = 1;
    lp_setsockopt(nl->fd, SOL_NETLINK, NETLINK_EXT_ACK, &on, sizeof on);

    /* pid 0 asks the kernel to assign a port id; groups 0 because the
     * groups we want are joined by id below, not by this bitmask -
     * which only reaches the first 32 groups anyway. */
    sockaddr_nl_t sa;
    memset(&sa, 0, sizeof sa);
    sa.nl_family = NL_AF_NETLINK;

    if (lp_bind(nl->fd, &sa, sizeof sa) < 0) {
        nl_fail(nl, 22, "binding the netlink socket");
        lp_close(nl->fd);
        nl->fd = -1;
        return false;
    }

    getfamily_t g;
    if (!resolve_family(nl, "nl80211", &g)) {
        /* ENOENT here means cfg80211 is not loaded: there is no
         * wireless subsystem on this machine at all. That is a
         * different problem from a missing interface and says so. */
        lp_close(nl->fd);
        nl->fd = -1;
        return false;
    }

    nl->family     = g.id;
    nl->grp_config = g.grp_config;
    nl->grp_scan   = g.grp_scan;
    nl->grp_mlme   = g.grp_mlme;
    nl->grp_reg    = g.grp_reg;

    /* Join before anything is asked for. A scan triggered before the
     * scan group is joined finishes into silence and the caller waits
     * out its timeout for an event that was delivered to nobody. */
    if (!join_group(nl, nl->grp_mlme, NL80211_GROUP_MLME) ||
        !join_group(nl, nl->grp_scan, NL80211_GROUP_SCAN) ||
        !join_group(nl, nl->grp_config, NL80211_GROUP_CONFIG)) {
        lp_close(nl->fd);
        nl->fd = -1;
        return false;
    }

    int idx = 0;
    long rc = net_if_index(ifname, &idx);
    if (rc < 0 || idx <= 0) {
        char what[64];
        snprintf(what, sizeof what, "no interface called %s", ifname);
        nl_fail(nl, rc < 0 ? (int)rc : 19 /* ENODEV */, what);
        lp_close(nl->fd);
        nl->fd = -1;
        return false;
    }
    nl->ifindex = (u32)idx;

    nl_iface_t info;
    if (!nl_interface_info(nl, &info)) {
        /* The interface exists but nl80211 does not know it - it is not
         * a wireless device. nl_error already says which errno. */
        lp_close(nl->fd);
        nl->fd = -1;
        return false;
    }
    nl->wiphy = info.wiphy;

    nl->err = 0;
    nl->msg[0] = '\0';
    return true;
}

void nl_close(nl_t *nl)
{
    if (nl->fd >= 0)
        lp_close(nl->fd);
    nl->fd = -1;
}

/* ══ Events ══════════════════════════════════════════════════════════ */

/* Read whatever is waiting and queue every event in it. Returns false
 * only on a socket error. */
static bool pump(nl_t *nl, int timeout_ms)
{
    long got = nl_recv_once(nl, timeout_ms);
    if (got < 0) {
        if (got == -105 /* ENOBUFS */) {
            nl->ev_dropped++;
            return true;
        }
        nl_fail(nl, (int)got, "reading from the netlink socket");
        return false;
    }
    if (got == 0)
        return true;

    size_t off = 0;
    bool bad = false;
    nlmsg_t m;

    while (nlmsg_next(nl->rx, (size_t)got, &off, &m, &bad)) {
        if (m.type == nl->family && m.attrs) {
            nl_event_t ev;
            ev_parse(&m, &ev);
            ev_push(nl, &ev);
        }
        /* Anything else at this point is the tail of a transaction that
         * has already returned - an ACK we stopped waiting for. Not an
         * error, and nothing to do with it. */
    }
    if (bad)
        nl_fail(nl, 74, "malformed netlink message from the kernel");
    return true;
}

bool nl_drain(nl_t *nl, nl_event_t *ev)
{
    if (ev_pop(nl, ev))
        return true;
    if (!pump(nl, 0))
        return false;
    return ev_pop(nl, ev);
}

bool nl_wait(nl_t *nl, int timeout_ms, nl_event_t *ev)
{
    if (ev_pop(nl, ev))
        return true;

    s64 deadline = lp_monotonic_ms() + (timeout_ms < 0 ? 0 : timeout_ms);

    for (;;) {
        int left = timeout_ms < 0 ? -1 : (int)(deadline - lp_monotonic_ms());
        if (timeout_ms >= 0 && left <= 0)
            return false;

        if (!pump(nl, left))
            return false;
        if (ev_pop(nl, ev))
            return true;
    }
}

/* ══ Scanning ════════════════════════════════════════════════════════ */

static bool build_trigger_scan(nl_t *nl, nlbuild_t *m, u8 *buf, size_t cap,
                               u32 seq, const char *ssid_or_null)
{
    msg_begin(m, buf, cap, nl->family, NL80211_CMD_TRIGGER_SCAN,
              NLM_F_REQUEST | NLM_F_ACK, seq);
    put_u32(m, NL80211_ATTR_IFINDEX, nl->ifindex);

    /* NL80211_ATTR_SCAN_SSIDS is a nest of SSIDs to probe for actively,
     * and it is not optional in the way it looks. With no nest at all
     * the scan is passive: the radio listens for beacons and never
     * sends a probe request. A zero-length SSID inside the nest is the
     * wildcard - "probe for anything" - and a named one is the only way
     * to find a network that does not beacon its name.
     *
     * The type of each SSID inside the nest is an array index starting
     * at 1, not an attribute name; cfg80211 walks the nest and ignores
     * the types entirely. Using 0 would still work but would be the one
     * value nla_next() is entitled to treat as padding, so it is 1. */
    size_t nest = nest_start(m, NL80211_ATTR_SCAN_SSIDS);
    if (ssid_or_null && ssid_or_null[0]) {
        size_t n = strlen(ssid_or_null);
        if (n > NL_SSID_MAX)
            n = NL_SSID_MAX;
        put_bytes(m, 1, ssid_or_null, n);
    } else {
        put_bytes(m, 1, "", 0);
    }
    nest_end(m, nest);

    return msg_finish(m);
}

bool nl_scan_trigger(nl_t *nl, const char *ssid_or_null)
{
    u8 buf[128];
    nlbuild_t m;
    u32 seq = ++nl->seq;

    if (!build_trigger_scan(nl, &m, buf, sizeof buf, seq, ssid_or_null)) {
        nl_fail(nl, 22, "TRIGGER_SCAN: request too large");
        return false;
    }
    return nl_transact(nl, buf, m.len, seq, "NL80211_CMD_TRIGGER_SCAN",
                       NULL, NULL, NL_DEFAULT_TIMEOUT_MS) == 0;
}

bool nl_scan(nl_t *nl, const char *ssid_or_null, int timeout_ms)
{
    if (!nl_scan_trigger(nl, ssid_or_null))
        return false;

    if (timeout_ms < 0)
        timeout_ms = NL_SCAN_TIMEOUT_MS;

    s64 deadline = lp_monotonic_ms() + timeout_ms;

    for (;;) {
        int left = (int)(deadline - lp_monotonic_ms());
        if (left <= 0) {
            nl_fail(nl, 110 /* ETIMEDOUT */,
                    "waiting for the scan to finish");
            return false;
        }

        /* Only the two events this is waiting for are taken off the
         * queue. Anything else - a disconnect, say - happened while we
         * were scanning and still belongs to the caller, so it stays
         * where it is and nl_wait will hand it over afterwards. */
        nl_event_t ev;
        if (ev_take_kind(nl, NL_EV_NEW_SCAN_RESULTS, NL_EV_SCAN_ABORTED,
                         &ev)) {
            if (ev.kind == NL_EV_NEW_SCAN_RESULTS)
                return true;
            nl_fail(nl, 125 /* ECANCELED */, "the scan was aborted");
            return false;
        }

        /* pump() blocks for the rest of the deadline, so this loop
         * turns over once per datagram and not once per microsecond. */
        if (!pump(nl, left))
            return false;               /* the socket broke; reported */
    }
}

/* Copy one information element out of an IE blob, id and length byte
 * included, so the caller sees exactly what the access point sent.
 * `total` reports the real size even when it did not fit. */
static void ie_copy(const u8 *ies, u16 ielen, u8 want_id,
                    const u8 *want_oui, u8 want_oui_type,
                    u8 *out, size_t outcap, u8 *out_len, u16 *out_total)
{
    *out_len   = 0;
    *out_total = 0;

    /* An IE blob is a flat run of { id, len, len bytes }. A length that
     * runs past the end means the blob is corrupt, and the right answer
     * then is to stop, not to guess where the next element begins. */
    for (u16 i = 0; i + 2 <= ielen; ) {
        u8 id  = ies[i];
        u8 len = ies[i + 1];

        if ((size_t)i + 2 + len > ielen)
            return;

        bool match = (id == want_id);
        if (match && want_oui) {
            match = (len >= 4 &&
                     memcmp(ies + i + 2, want_oui, 3) == 0 &&
                     ies[i + 5] == want_oui_type);
        }

        if (match) {
            size_t whole = (size_t)len + 2;
            *out_total = (u16)whole;    /* at most 257; u16, so the
                                           report is never a rounded
                                           lie about a clipped element */
            size_t copy = whole < outcap ? whole : outcap;
            memcpy(out, ies + i, copy);
            *out_len = (u8)copy;
            return;
        }
        i = (u16)(i + 2 + len);
    }
}

typedef struct {
    nl_bss_t *out;
    int       max;
    int       n;
} scandump_t;

static bool scan_cb(nl_t *nl, const nlmsg_t *m, void *arg)
{
    scandump_t *s = arg;
    const u8 *bss; u16 bsslen;
    const u8 *p;  u16 n;

    (void)nl;
    if (!m->attrs || s->n >= s->max)
        return s->n < s->max;

    if (!nla_find(m->attrs, m->attrlen, NL80211_ATTR_BSS, &bss, &bsslen))
        return true;

    nl_bss_t *b = &s->out[s->n];
    memset(b, 0, sizeof *b);

    if (!nla_find(bss, bsslen, NL80211_BSS_BSSID, &p, &n) || n < 6)
        return true;                    /* a BSS with no BSSID is noise */
    memcpy(b->bssid, p, 6);

    (void)nla_get_u32v(bss, bsslen, NL80211_BSS_FREQUENCY, &b->freq);
    (void)nla_get_u16v(bss, bsslen, NL80211_BSS_CAPABILITY, &b->capability);
    (void)nla_get_u32v(bss, bsslen, NL80211_BSS_SEEN_MS_AGO, &b->seen_ms_ago);

    if (nla_find(bss, bsslen, NL80211_BSS_SIGNAL_MBM, &p, &n) && n >= 4) {
        u32 raw = get_u32_at(p);
        b->signal_mbm = (s32)raw;       /* the kernel sends s32 in mBm  */
    }
    if (nla_find(bss, bsslen, NL80211_BSS_SIGNAL_UNSPEC, &p, &n) && n >= 1)
        b->signal_unspec = p[0];

    u32 status;
    if (nla_get_u32v(bss, bsslen, NL80211_BSS_STATUS, &status))
        b->associated = (status == NL80211_BSS_STATUS_ASSOCIATED);

    /* Prefer the probe-response elements, fall back to the beacon's.
     * They usually agree; when they do not it is the probe response
     * that describes what the AP will actually accept.
     *
     * A BSS with no elements at all is still a BSS - we have its BSSID,
     * its channel and how strong it is - so it is kept rather than
     * dropped. Dropping it would make a radio that can hear an access
     * point look like one that cannot. */
    if (nla_find(bss, bsslen, NL80211_BSS_INFORMATION_ELEMENTS, &p, &n) ||
        nla_find(bss, bsslen, NL80211_BSS_BEACON_IES, &p, &n)) {

        /* The SSID is element 0. A hidden network sends it with length
         * 0, and that is a real answer rather than a missing one. */
        u8 ssid_ie[NL_SSID_MAX + 2], ssid_len = 0;
        u16 ssid_total = 0;
        ie_copy(p, n, 0, NULL, 0, ssid_ie, sizeof ssid_ie,
                &ssid_len, &ssid_total);
        if (ssid_len >= 2) {
            u8 take = ssid_ie[1];
            if (take > NL_SSID_MAX)
                take = NL_SSID_MAX;
            if ((size_t)take + 2 > ssid_len)
                take = (u8)(ssid_len - 2);
            memcpy(b->ssid, ssid_ie + 2, take);
            b->ssid_len = take;
        }

        ie_copy(p, n, WLAN_EID_RSN, NULL, 0, b->rsn_ie, sizeof b->rsn_ie,
                &b->rsn_ie_len, &b->rsn_ie_total);

        /* WPA1 has no element of its own: it rides in a vendor-specific
         * element, OUI 00-50-F2, type 1. */
        static const u8 msft_oui[3] = { 0x00, 0x50, 0xF2 };
        ie_copy(p, n, WLAN_EID_VENDOR_SPECIFIC, msft_oui, 1, b->wpa_ie,
                sizeof b->wpa_ie, &b->wpa_ie_len, &b->wpa_ie_total);
    }

    s->n++;
    return true;
}

/* GET_SCAN has no .doit in the kernel, only a .dumpit - asking for it
 * without NLM_F_DUMP gets EOPNOTSUPP. The dump's attributes are read
 * from this request, which is why the ifindex goes here. */
static bool build_get_scan(nl_t *nl, nlbuild_t *m, u8 *buf, size_t cap,
                           u32 seq)
{
    msg_begin(m, buf, cap, nl->family, NL80211_CMD_GET_SCAN,
              NLM_F_REQUEST | NLM_F_DUMP, seq);
    put_u32(m, NL80211_ATTR_IFINDEX, nl->ifindex);
    return msg_finish(m);
}

int nl_scan_results(nl_t *nl, nl_bss_t *out, int max)
{
    u8 buf[64];
    nlbuild_t m;
    u32 seq = ++nl->seq;
    scandump_t s = { out, max, 0 };

    if (max <= 0)
        return 0;

    if (!build_get_scan(nl, &m, buf, sizeof buf, seq)) {
        nl_fail(nl, 22, "GET_SCAN: request too large");
        return -1;
    }
    if (nl_transact(nl, buf, m.len, seq, "NL80211_CMD_GET_SCAN",
                    scan_cb, &s, NL_DEFAULT_TIMEOUT_MS) < 0)
        return -1;
    return s.n;
}

/* ── RSN elements ─────────────────────────────────────────────────────
 *
 *   id(48) len version(2) group(4) n_pair(2) pair[]*4 n_akm(2) akm[]*4
 *   caps(2) ...
 *
 * Everything after the version is optional and a short element means
 * "the defaults", so every read below checks that there is room first.
 * The counts are little-endian on the wire - unlike the suite
 * selectors, which are big-endian-looking byte quadruples. Mixing those
 * two up gives a group cipher of 0x04AC0F00 and a puzzled EINVAL. */
bool nl_rsn_parse(const u8 *ie, size_t len, nl_rsn_t *out)
{
    memset(out, 0, sizeof *out);

    if (!ie || len < 4 || ie[0] != WLAN_EID_RSN)
        return false;
    size_t body = ie[1];
    if (body + 2 > len)
        return false;

    const u8 *p = ie + 2;
    size_t i = 0;

    if (body < 2)
        return false;
    out->version = (u16)(p[0] | ((u16)p[1] << 8));
    i = 2;

    /* Defaults from IEEE 802.11-2020 9.4.2.24: CCMP for everything. */
    out->group_cipher     = WLAN_CIPHER_SUITE_CCMP;
    out->pairwise[0]      = WLAN_CIPHER_SUITE_CCMP;
    out->n_pairwise       = 1;
    out->akm[0]           = WLAN_AKM_SUITE_8021X;
    out->n_akm            = 1;

    if (i + 4 > body) return true;
    out->group_cipher = nl_suite(p + i);
    i += 4;

    /* A count field that promises more suites than the element holds is
     * not a short element, it is a lie, and the difference matters: a
     * short element means "use the defaults" and a lying one means the
     * bytes cannot be trusted at all. Stopping quietly at the end would
     * hand the caller a plausible-looking cipher list assembled out of
     * whatever followed. So the counts are checked before they are
     * used, and a lie fails the whole parse. */
    if (i + 2 > body) return true;
    u16 count = (u16)(p[i] | ((u16)p[i + 1] << 8));
    i += 2;
    if (i + (size_t)count * 4 > body)
        return false;
    out->n_pairwise = 0;
    for (u16 k = 0; k < count; k++) {
        if (out->n_pairwise < NL_MAX_CIPHERS)
            out->pairwise[out->n_pairwise++] = nl_suite(p + i);
        i += 4;
    }

    if (i + 2 > body) return true;
    count = (u16)(p[i] | ((u16)p[i + 1] << 8));
    i += 2;
    if (i + (size_t)count * 4 > body)
        return false;
    out->n_akm = 0;
    for (u16 k = 0; k < count; k++) {
        if (out->n_akm < NL_MAX_AKMS)
            out->akm[out->n_akm++] = nl_suite(p + i);
        i += 4;
    }

    if (i + 2 > body) return true;
    out->caps = (u16)(p[i] | ((u16)p[i + 1] << 8));
    out->mfp_capable  = (out->caps & 0x0080) != 0;   /* MFPC, bit 7 */
    out->mfp_required = (out->caps & 0x0040) != 0;   /* MFPR, bit 6 */
    return true;
}

/* ══ Connecting ══════════════════════════════════════════════════════ */

void nl_conn_wpa2_psk(nl_conn_t *req, const u8 *ssid, u8 ssid_len,
                      const u8 *bssid_or_null, u32 freq_mhz)
{
    memset(req, 0, sizeof *req);
    req->ssid                = ssid;
    req->ssid_len            = ssid_len;
    req->bssid               = bssid_or_null;
    req->freq                = freq_mhz;
    req->auth_type           = NL80211_AUTHTYPE_OPEN_SYSTEM;
    req->wpa_versions        = NL80211_WPA_VERSION_2;
    req->ciphers_pairwise[0] = WLAN_CIPHER_SUITE_CCMP;
    req->n_ciphers_pairwise  = 1;
    req->cipher_group        = WLAN_CIPHER_SUITE_CCMP;
    req->akm_suites[0]       = WLAN_AKM_SUITE_PSK;
    req->n_akm_suites        = 1;
    req->privacy             = true;
    req->control_port        = true;
}

/* The message this whole file exists to get right.
 *
 * Attribute order does not matter to the kernel - it builds a table
 * indexed by type before it looks at anything - but it is fixed here
 * so that two runs produce identical bytes and a hexdump can be
 * compared against a known-good one. */
static bool build_connect(nl_t *nl, nlbuild_t *m, u8 *buf, size_t cap,
                          u32 seq, const nl_conn_t *req)
{
    msg_begin(m, buf, cap, nl->family, NL80211_CMD_CONNECT,
              NLM_F_REQUEST | NLM_F_ACK, seq);

    put_u32(m, NL80211_ATTR_IFINDEX, nl->ifindex);
    /* The SSID is bytes, not a string, and it goes in WITHOUT a NUL.
     * cfg80211 takes nla_len() as the length, so a terminator would
     * make "home" a five-character SSID that matches nothing. */
    put_bytes(m, NL80211_ATTR_SSID, req->ssid, req->ssid_len);

    if (req->bssid)
        put_bytes(m, NL80211_ATTR_MAC, req->bssid, 6);
    if (req->freq)
        put_u32(m, NL80211_ATTR_WIPHY_FREQ, req->freq);

    put_u32(m, NL80211_ATTR_AUTH_TYPE, req->auth_type);

    if (req->privacy)
        put_flag(m, NL80211_ATTR_PRIVACY);

    if (req->wpa_versions)
        put_u32(m, NL80211_ATTR_WPA_VERSIONS, req->wpa_versions);

    put_u32_array(m, NL80211_ATTR_CIPHER_SUITES_PAIRWISE,
                  req->ciphers_pairwise, req->n_ciphers_pairwise);
    if (req->cipher_group)
        put_u32(m, NL80211_ATTR_CIPHER_SUITE_GROUP, req->cipher_group);
    put_u32_array(m, NL80211_ATTR_AKM_SUITES,
                  req->akm_suites, req->n_akm_suites);

    if (req->ie && req->ie_len)
        put_bytes(m, NL80211_ATTR_IE, req->ie, req->ie_len);

    if (req->control_port)
        put_flag(m, NL80211_ATTR_CONTROL_PORT);

    return msg_finish(m);
}

bool nl_connect(nl_t *nl, const nl_conn_t *req)
{
    u8 buf[NL_TXBUF];
    nlbuild_t m;
    u32 seq = ++nl->seq;

    if (!req->ssid || req->ssid_len == 0 || req->ssid_len > NL_SSID_MAX) {
        nl_fail(nl, 22, "CONNECT: the SSID is empty or too long");
        return false;
    }

    /* cfg80211 marks CONNECT NEED_NETDEV_UP and answers ENETDOWN when
     * the interface is not up. That errno on its own has sent people
     * looking at routing tables; say what it means. */
    if (!net_if_is_up(nl->ifname)) {
        char what[64];
        snprintf(what, sizeof what, "CONNECT: %s is not up", nl->ifname);
        nl_fail(nl, 100 /* ENETDOWN */, what);
        return false;
    }

    if (!build_connect(nl, &m, buf, sizeof buf, seq, req)) {
        nl_fail(nl, 22, "CONNECT: request too large");
        return false;
    }

    /* NL80211_ATTR_CONTROL_PORT is sent (see nl_conn_t.control_port):
     * with it the kernel holds data back until nl_authorize().
     *
     * Deliberately NOT sent:
     *
     *   NL80211_ATTR_CONTROL_PORT_OVER_NL80211 - that would ask the
     *   kernel to deliver EAPOL frames here on this socket instead of
     *   on wlan0. The four-way handshake in this system reads them from
     *   an AF_PACKET socket like any other frame, which is the path
     *   brcmfmac has always used.
     *
     *   NL80211_ATTR_CONTROL_PORT_ETHERTYPE - cfg80211 defaults it to
     *   0x888E when absent, and sending it *requires* also sending
     *   NL80211_ATTR_CONTROL_PORT or the request is rejected.
     *
     *   NL80211_ATTR_PMK - that hands the PSK to the firmware and asks
     *   it to do the handshake itself. It is exactly what we are not
     *   doing, and the chip only accepts it with an extended feature
     *   flag this one does not set. */

    /* This returns when the *request* is accepted. Success or failure
     * of the join arrives later as an NL_EV_CONNECT event, because on
     * a fullmac chip the firmware has not even started yet. */
    return nl_transact(nl, buf, m.len, seq, "NL80211_CMD_CONNECT",
                       NULL, NULL, NL_DEFAULT_TIMEOUT_MS) == 0;
}

static bool build_disconnect(nl_t *nl, nlbuild_t *m, u8 *buf, size_t cap,
                             u32 seq, u16 reason)
{
    msg_begin(m, buf, cap, nl->family, NL80211_CMD_DISCONNECT,
              NLM_F_REQUEST | NLM_F_ACK, seq);
    put_u32(m, NL80211_ATTR_IFINDEX, nl->ifindex);
    /* u16, not u32. The policy says NLA_U16 and a four-byte reason code
     * is rejected with a bare EINVAL. */
    put_u16(m, NL80211_ATTR_REASON_CODE, reason);
    return msg_finish(m);
}

bool nl_disconnect(nl_t *nl, u16 reason)
{
    u8 buf[64];
    nlbuild_t m;
    u32 seq = ++nl->seq;

    if (!build_disconnect(nl, &m, buf, sizeof buf, seq, reason)) {
        nl_fail(nl, 22, "DISCONNECT: request too large");
        return false;
    }
    return nl_transact(nl, buf, m.len, seq, "NL80211_CMD_DISCONNECT",
                       NULL, NULL, NL_DEFAULT_TIMEOUT_MS) == 0;
}

/* ══ Keys ════════════════════════════════════════════════════════════
 *
 * The flat form, not the nested NL80211_ATTR_KEY one. Both work -
 * nl80211_parse_key falls back to nl80211_parse_key_old when there is
 * no nest - and the flat form is what wpa_supplicant has always sent,
 * so it is the path every driver has actually been tested on. On a
 * chip whose firmware is the only implementation we have, "what
 * everybody else sends" is a real argument.
 *
 * Note what is absent: NL80211_ATTR_KEY_DEFAULT. Under WPA a station
 * transmits with its pairwise key, and marking the group key as the
 * default transmit key makes it send broadcast-keyed unicast frames
 * that the AP drops. wpa_supplicant skips it for exactly this reason. */
static bool build_new_key(nl_t *nl, nlbuild_t *m, u8 *buf, size_t cap,
                          u32 seq, const u8 *addr_or_null, int keyidx,
                          u32 cipher, const u8 *key, size_t keylen,
                          const u8 *rsc, size_t rsclen)
{
    msg_begin(m, buf, cap, nl->family, NL80211_CMD_NEW_KEY,
              NLM_F_REQUEST | NLM_F_ACK, seq);
    put_u32(m, NL80211_ATTR_IFINDEX, nl->ifindex);
    put_bytes(m, NL80211_ATTR_KEY_DATA, key, keylen);
    /* One byte. The policy is NLA_POLICY_MAX(NLA_U8, 7); a u32 index is
     * rejected on length before anybody looks at the value. */
    put_u8(m, NL80211_ATTR_KEY_IDX, (u8)keyidx);
    put_u32(m, NL80211_ATTR_KEY_CIPHER, cipher);

    if (rsc && rsclen)
        put_bytes(m, NL80211_ATTR_KEY_SEQ, rsc, rsclen);

    if (addr_or_null) {
        put_bytes(m, NL80211_ATTR_MAC, addr_or_null, 6);
        put_u32(m, NL80211_ATTR_KEY_TYPE, NL80211_KEYTYPE_PAIRWISE);
    }
    return msg_finish(m);
}

bool nl_set_key(nl_t *nl, const u8 *addr_or_null, int keyidx, u32 cipher,
                const u8 *key, size_t keylen, const u8 *rsc, size_t rsclen)
{
    u8 buf[256];
    nlbuild_t m;
    u32 seq = ++nl->seq;

    if (!key || keylen == 0 || keylen > 64) {
        nl_fail(nl, 22, "NEW_KEY: implausible key length");
        return false;
    }
    if (keyidx < 0 || keyidx > 7) {
        nl_fail(nl, 22, "NEW_KEY: key index out of range");
        return false;
    }
    if (rsclen > 16) {
        nl_fail(nl, 22, "NEW_KEY: sequence counter too long");
        return false;
    }

    if (!build_new_key(nl, &m, buf, sizeof buf, seq, addr_or_null, keyidx,
                       cipher, key, keylen, rsc, rsclen)) {
        nl_fail(nl, 22, "NEW_KEY: request too large");
        return false;
    }
    return nl_transact(nl, buf, m.len, seq, "NL80211_CMD_NEW_KEY",
                       NULL, NULL, NL_DEFAULT_TIMEOUT_MS) == 0;
}

/* The cipher a key of this length must be. 16 bytes is CCMP; 32 is
 * TKIP, which is a 16-byte key plus two 8-byte Michael MIC keys. A
 * caller that means GCMP-256 - also 32 - has to say so through
 * nl_set_key, and this is why. */
static bool cipher_for_len(nl_t *nl, size_t len, const char *what, u32 *out)
{
    switch (len) {
    case 16: *out = WLAN_CIPHER_SUITE_CCMP; return true;
    case 32: *out = WLAN_CIPHER_SUITE_TKIP; return true;
    default: {
        char msg[80];
        snprintf(msg, sizeof msg,
                 "%s: %d-byte key names no cipher - use nl_set_key",
                 what, (int)len);
        nl_fail(nl, 22, msg);
        return false;
    }
    }
}

bool nl_set_ptk(nl_t *nl, const u8 bssid[6], const u8 *tk, size_t tklen)
{
    u32 cipher;
    if (!cipher_for_len(nl, tklen, "set pairwise key", &cipher))
        return false;
    return nl_set_key(nl, bssid, 0, cipher, tk, tklen, NULL, 0);
}

bool nl_set_gtk(nl_t *nl, int keyidx, const u8 *gtk, size_t len,
                const u8 rsc[6])
{
    u32 cipher;
    if (!cipher_for_len(nl, len, "set group key", &cipher))
        return false;
    return nl_set_key(nl, NULL, keyidx, cipher, gtk, len, rsc, rsc ? 6 : 0);
}

/* ══ Opening the port ════════════════════════════════════════════════
 *
 * NL80211_CMD_SET_STATION on our own interface, naming the access point,
 * with NL80211_ATTR_STA_FLAGS2 = { mask AUTHORIZED, set AUTHORIZED }.
 * cfg80211 allows exactly this one flag on a managed interface's AP
 * entry (cfg80211_check_station_change, CFG80211_STA_AP_STA); mac80211
 * then moves the station to AUTHORIZED and data starts to flow. It is
 * what wpa_supplicant's sta_set_flags does after the four-way handshake.
 * The struct is two host-order u32s, nl80211_sta_flag_update. */
bool nl_authorize(nl_t *nl, const u8 peer[6])
{
    u8 buf[96];
    nlbuild_t m;
    u32 seq = ++nl->seq;
    u32 upd[2] = { 1u << NL80211_STA_FLAG_AUTHORIZED,
                   1u << NL80211_STA_FLAG_AUTHORIZED };

    msg_begin(&m, buf, sizeof buf, nl->family, NL80211_CMD_SET_STATION,
              NLM_F_REQUEST | NLM_F_ACK, seq);
    put_u32(&m, NL80211_ATTR_IFINDEX, nl->ifindex);
    put_bytes(&m, NL80211_ATTR_MAC, peer, 6);
    put_bytes(&m, NL80211_ATTR_STA_FLAGS2, upd, sizeof upd);
    if (!msg_finish(&m)) {
        nl_fail(nl, 22, "SET_STATION: request too large");
        return false;
    }
    return nl_transact(nl, buf, m.len, seq, "NL80211_CMD_SET_STATION",
                       NULL, NULL, NL_DEFAULT_TIMEOUT_MS) == 0;
}

/* ══ Regulatory domain ═══════════════════════════════════════════════ */

static bool reg_cb(nl_t *nl, const nlmsg_t *m, void *arg)
{
    char *country = arg;
    const u8 *p; u16 n;

    (void)nl;
    if (m->attrs &&
        nla_find(m->attrs, m->attrlen, NL80211_ATTR_REG_ALPHA2, &p, &n) &&
        n >= 2) {
        country[0] = (char)p[0];
        country[1] = (char)p[1];
        country[2] = '\0';
    }
    return true;
}

static bool build_get_reg(nl_t *nl, nlbuild_t *m, u8 *buf, size_t cap,
                          u32 seq)
{
    msg_begin(m, buf, cap, nl->family, NL80211_CMD_GET_REG,
              NLM_F_REQUEST | NLM_F_ACK, seq);
    /* Naming the wiphy matters on a device with its own regulatory
     * domain - a self-managed one has no entry in the global table and
     * the answer without this would describe some other radio. */
    if (nl->wiphy)
        put_u32(m, NL80211_ATTR_WIPHY, nl->wiphy);
    return msg_finish(m);
}

bool nl_get_reg(nl_t *nl, char country[4])
{
    u8 buf[64];
    nlbuild_t m;
    u32 seq = ++nl->seq;

    country[0] = '\0';

    if (!build_get_reg(nl, &m, buf, sizeof buf, seq)) {
        nl_fail(nl, 22, "GET_REG: request too large");
        return false;
    }
    if (nl_transact(nl, buf, m.len, seq, "NL80211_CMD_GET_REG",
                    reg_cb, country, NL_DEFAULT_TIMEOUT_MS) < 0)
        return false;

    if (!country[0]) {
        nl_fail(nl, 61 /* ENODATA */, "GET_REG: no country in the reply");
        return false;
    }
    return true;
}

/* ══ Names ═══════════════════════════════════════════════════════════
 *
 * Only the ones a message about this system would ever print. An
 * unknown value comes back as a string saying so rather than as NULL,
 * because a diagnostic that crashes is worse than one that is vague. */

const char *nl_cmd_name(u8 cmd)
{
    switch (cmd) {
    case NL80211_CMD_GET_INTERFACE:     return "GET_INTERFACE";
    case NL80211_CMD_NEW_INTERFACE:     return "NEW_INTERFACE";
    case NL80211_CMD_NEW_KEY:           return "NEW_KEY";
    case NL80211_CMD_GET_REG:           return "GET_REG";
    case NL80211_CMD_GET_SCAN:          return "GET_SCAN";
    case NL80211_CMD_TRIGGER_SCAN:      return "TRIGGER_SCAN";
    case NL80211_CMD_NEW_SCAN_RESULTS:  return "NEW_SCAN_RESULTS";
    case NL80211_CMD_SCAN_ABORTED:      return "SCAN_ABORTED";
    case NL80211_CMD_AUTHENTICATE:      return "AUTHENTICATE";
    case NL80211_CMD_ASSOCIATE:         return "ASSOCIATE";
    case NL80211_CMD_DEAUTHENTICATE:    return "DEAUTHENTICATE";
    case NL80211_CMD_DISASSOCIATE:      return "DISASSOCIATE";
    case NL80211_CMD_CONNECT:           return "CONNECT";
    case NL80211_CMD_ROAM:              return "ROAM";
    case NL80211_CMD_DISCONNECT:        return "DISCONNECT";
    case NL80211_CMD_PORT_AUTHORIZED:   return "PORT_AUTHORIZED";
    default:                            return "an nl80211 command";
    }
}

const char *nl_iftype_name(u32 iftype)
{
    switch (iftype) {
    case NL80211_IFTYPE_STATION: return "station";
    case NL80211_IFTYPE_AP:      return "access point";
    case NL80211_IFTYPE_MONITOR: return "monitor";
    default:                     return "some other interface type";
    }
}

/* IEEE 802.11-2020 table 9-50. Only the ones a station joining a WPA2
 * network can actually be told. */
const char *nl_status_name(u16 status)
{
    switch (status) {
    case 0:  return "success";
    case 1:  return "unspecified failure";
    case 10: return "the capabilities in the request are not supported";
    case 11: return "reassociation denied: no prior association";
    case 12: return "association denied for a reason outside this standard";
    case 13: return "the authentication algorithm is not supported";
    case 15: return "authentication rejected: challenge failure";
    case 16: return "authentication rejected: timed out waiting";
    case 17: return "the access point is full";
    case 18: return "the basic rates are not all supported";
    case 40: return "an information element was invalid";
    case 43: return "the pairwise cipher is not valid";
    case 44: return "the AKM is not valid";
    case 45: return "the RSN capabilities are not supported";
    case 53: return "invalid PMKID";
    default: return "an 802.11 status code";
    }
}

/* IEEE 802.11-2020 table 9-49. Reason 15 is the one this whole exercise
 * is about: the four-way handshake timed out. */
const char *nl_reason_name(u16 reason)
{
    switch (reason) {
    case 1:  return "unspecified";
    case 2:  return "the previous authentication is no longer valid";
    case 3:  return "we are leaving";
    case 4:  return "inactivity";
    case 6:  return "a frame arrived from a station we have not authenticated";
    case 7:  return "a frame arrived from a station we are not associated with";
    case 8:  return "the station is leaving";
    case 14: return "the Michael MIC failed";
    case 15: return "the four-way handshake timed out";
    case 16: return "the group key handshake timed out";
    case 17: return "message 3 did not match what we negotiated";
    case 18: return "the group cipher is not valid";
    case 19: return "the pairwise cipher is not valid";
    case 20: return "the AKM is not valid";
    case 23: return "802.1X authentication failed";
    case 24: return "the cipher suite was rejected by policy";
    default: return "an 802.11 reason code";
    }
}

const char *nl_cipher_name(u32 suite)
{
    switch (suite) {
    case WLAN_CIPHER_SUITE_WEP40:    return "WEP-40";
    case WLAN_CIPHER_SUITE_TKIP:     return "TKIP";
    case WLAN_CIPHER_SUITE_CCMP:     return "CCMP";
    case WLAN_CIPHER_SUITE_WEP104:   return "WEP-104";
    case WLAN_CIPHER_SUITE_AES_CMAC: return "BIP-CMAC-128";
    case WLAN_CIPHER_SUITE_GCMP:     return "GCMP";
    case WLAN_CIPHER_SUITE_GCMP_256: return "GCMP-256";
    case WLAN_CIPHER_SUITE_CCMP_256: return "CCMP-256";
    case 0:                          return "none";
    default:                         return "an unknown cipher";
    }
}

const char *nl_akm_name(u32 suite)
{
    switch (suite) {
    case WLAN_AKM_SUITE_8021X:        return "802.1X";
    case WLAN_AKM_SUITE_PSK:          return "PSK";
    case WLAN_AKM_SUITE_FT_8021X:     return "FT/802.1X";
    case WLAN_AKM_SUITE_FT_PSK:       return "FT/PSK";
    case WLAN_AKM_SUITE_8021X_SHA256: return "802.1X-SHA256";
    case WLAN_AKM_SUITE_PSK_SHA256:   return "PSK-SHA256";
    case WLAN_AKM_SUITE_SAE:          return "SAE";
    default:                          return "an unknown AKM";
    }
}
