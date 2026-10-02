/* firewall.c - a packet filter, built out of nftables netlink messages.
 *
 * The kernel has nftables built in. What it does not have is nft, which
 * is a parser, a bytecode compiler and libmnl and libnftnl on top of it -
 * a couple of megabytes to express a policy that never changes. So this
 * skips the language entirely and writes the netlink messages by hand.
 *
 * The whole ruleset goes to the kernel as ONE transaction:
 *
 *     NFNL_MSG_BATCH_BEGIN
 *       delete the old table   (only when there is one)
 *       create the table
 *       create input / forward / output
 *       append every rule
 *     NFNL_MSG_BATCH_END
 *
 * nftables applies a batch all at once or not at all. That matters more
 * here than anywhere else: the machine is administered over SSH, so a
 * moment between "input now drops everything" and "except port 22" is a
 * moment in which the only way back in is a power cycle. There is no
 * such moment. Either the rule that keeps SSH alive is in place before
 * the drop policy is, or neither of them is.
 *
 * Two more things stand between a mistake here and a locked door:
 *
 *   - the first input rule accepts anything conntrack already knows, so
 *     the session you are typing this in survives;
 *   - the SSH rule does not depend on conntrack at all, so it survives
 *     even if conntrack has never seen your connection.
 *
 * Extra ports come from /boot/firewall.conf, which is on the FAT boot
 * partition - editable from any computer with an SD card reader, which
 * is the one repair path that works when the network is the problem.
 */
#include "types.h"
#include "syscall.h"
#include "unistd.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "net.h"

/* ── netlink ─────────────────────────────────────────────────────── */

#define AF_NETLINK          16
#define NETLINK_NETFILTER   12

#define NLM_F_REQUEST   0x001
#define NLM_F_MULTI     0x002
#define NLM_F_ACK       0x004
#define NLM_F_DUMP      0x300
#define NLM_F_EXCL      0x200
#define NLM_F_CREATE    0x400
#define NLM_F_APPEND    0x800

#define NLMSG_ERROR     2
#define NLMSG_DONE      3

#define NLA_F_NESTED    0x8000

typedef struct {
    u32 nlmsg_len;
    u16 nlmsg_type;
    u16 nlmsg_flags;
    u32 nlmsg_seq;
    u32 nlmsg_pid;
} nlmsghdr_t;

typedef struct {
    u16 nla_len;    /* header included */
    u16 nla_type;
} nlattr_t;

typedef struct {
    u16 nl_family;
    u16 nl_pad;
    u32 nl_pid;
    u32 nl_groups;
} sockaddr_nl_t;

/* nfnetlink puts this after every nlmsghdr */
typedef struct {
    u8  nfgen_family;
    u8  version;
    u16 res_id;     /* network byte order */
} nfgenmsg_t;

#define NFNL_SUBSYS_NFTABLES  10
#define NFNL_MSG_BATCH_BEGIN  16
#define NFNL_MSG_BATCH_END    17

#define NFT_MSG_NEWTABLE   0
#define NFT_MSG_GETTABLE   1
#define NFT_MSG_DELTABLE   2
#define NFT_MSG_NEWCHAIN   3
#define NFT_MSG_NEWRULE    6
#define NFT_MSG_GETRULE    7

#define NFT(m)  ((u16)((NFNL_SUBSYS_NFTABLES << 8) | (m)))

/* Address families as netfilter numbers them */
#define NFPROTO_INET   1

/* ── nftables attributes ─────────────────────────────────────────── */

#define NFTA_LIST_ELEM        1

#define NFTA_TABLE_NAME       1

#define NFTA_CHAIN_TABLE      1
#define NFTA_CHAIN_NAME       3
#define NFTA_CHAIN_HOOK       4
#define NFTA_CHAIN_POLICY     5
#define NFTA_CHAIN_TYPE       7

#define NFTA_HOOK_HOOKNUM     1
#define NFTA_HOOK_PRIORITY    2

#define NFTA_RULE_TABLE       1
#define NFTA_RULE_CHAIN       2
#define NFTA_RULE_HANDLE      3
#define NFTA_RULE_EXPRESSIONS 4
#define NFTA_RULE_USERDATA    7

#define NFTA_EXPR_NAME        1
#define NFTA_EXPR_DATA        2

#define NFTA_DATA_VALUE       1
#define NFTA_DATA_VERDICT     2
#define NFTA_VERDICT_CODE     1

#define NFTA_META_DREG        1
#define NFTA_META_KEY         2

#define NFTA_PAYLOAD_DREG     1
#define NFTA_PAYLOAD_BASE     2
#define NFTA_PAYLOAD_OFFSET   3
#define NFTA_PAYLOAD_LEN      4

#define NFTA_CMP_SREG         1
#define NFTA_CMP_OP           2
#define NFTA_CMP_DATA         3

#define NFTA_BITWISE_SREG     1
#define NFTA_BITWISE_DREG     2
#define NFTA_BITWISE_LEN      3
#define NFTA_BITWISE_MASK     4
#define NFTA_BITWISE_XOR      5

#define NFTA_CT_DREG          1
#define NFTA_CT_KEY           2

#define NFTA_LIMIT_RATE       1
#define NFTA_LIMIT_UNIT       2
#define NFTA_LIMIT_BURST      3
#define NFTA_LIMIT_TYPE       4

#define NFTA_COUNTER_BYTES    1
#define NFTA_COUNTER_PACKETS  2


#define NFTA_IMMEDIATE_DREG   1
#define NFTA_IMMEDIATE_DATA   2

/* Registers. Register 0 holds the verdict; 1..4 hold data. */
#define NFT_REG_VERDICT   0
#define NFT_REG_1         1

/* Verdicts. The positive ones are netfilter's, the negative ones are
 * nftables' own ("keep going in this chain", "this rule did not match"). */
#define NF_DROP        0
#define NF_ACCEPT      1
#define NFT_CONTINUE  (-1)

/* meta keys. These are positions in an enum, so getting one wrong does
 * not fail loudly - it asks for a different thing of a different size.
 * IIFNAME was 11 here once, which is SKGID: four bytes, only legal on
 * the way out, and the kernel refused the rule with an error that said
 * nothing about interface names. */
#define NFT_META_IIFNAME   6
#define NFT_META_OIFNAME   7
#define NFT_META_NFPROTO  15
#define NFT_META_L4PROTO  16

/* payload bases */
#define NFT_PAYLOAD_NETWORK_HEADER    1
#define NFT_PAYLOAD_TRANSPORT_HEADER  2

/* cmp operators */
#define NFT_CMP_EQ    0
#define NFT_CMP_NEQ   1

/* ct keys, and the bits ct state is made of */
#define NFT_CT_STATE  0
#define CT_ESTABLISHED  (1u << 1)
#define CT_RELATED      (1u << 2)
#define CT_NEW          (1u << 3)

/* netfilter hooks */
#define NF_INET_LOCAL_IN   1
#define NF_INET_FORWARD    2
#define NF_INET_LOCAL_OUT  3

/* IP protocol numbers */
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP    17
#define IPPROTO_ICMP6  58

#define TABLE_NAME  "lpzero"

/* ── building a batch ────────────────────────────────────────────── */

#define BATCH_MAX  16384

typedef struct {
    u8   buf[BATCH_MAX];
    u32  len;
    u32  msg_start;
    u32  nest[8];
    int  depth;
    u32  seq;
    bool overflow;
} batch_t;

static u32 align4(u32 n) { return (n + 3u) & ~3u; }

static void *reserve(batch_t *b, u32 n)
{
    static u8 sink[256];
    if (b->len + n > BATCH_MAX) {
        b->overflow = true;
        return sink;               /* keep writing somewhere harmless */
    }
    u8 *p = b->buf + b->len;
    memset(p, 0, n);
    b->len += n;
    return p;
}

static void pad4(batch_t *b)
{
    while (b->len & 3u) {
        if (b->len >= BATCH_MAX) { b->overflow = true; return; }
        b->buf[b->len++] = 0;
    }
}

static void msg_begin(batch_t *b, u16 type, u16 flags, u8 family, u16 res_id)
{
    pad4(b);
    b->msg_start = b->len;
    nlmsghdr_t *h = reserve(b, sizeof *h);
    h->nlmsg_type  = type;
    h->nlmsg_flags = flags;
    h->nlmsg_seq   = ++b->seq;
    h->nlmsg_pid   = 0;
    nfgenmsg_t *g = reserve(b, sizeof *g);
    g->nfgen_family = family;
    g->version      = 0;
    g->res_id       = htons(res_id);
}

static void msg_end(batch_t *b)
{
    if (b->overflow) return;
    nlmsghdr_t *h = (nlmsghdr_t *)(b->buf + b->msg_start);
    h->nlmsg_len = b->len - b->msg_start;
    pad4(b);
}

static void attr_put(batch_t *b, u16 type, const void *data, u16 len)
{
    nlattr_t *a = reserve(b, sizeof *a);
    a->nla_type = type;
    a->nla_len  = (u16)(sizeof *a + len);
    if (len) memcpy(reserve(b, len), data, len);
    pad4(b);
}

static void attr_u32(batch_t *b, u16 type, u32 v)
{
    u32 be = htonl(v);
    attr_put(b, type, &be, 4);
}

static void attr_u64(batch_t *b, u16 type, u64 v)
{
    u8 be[8];
    for (int i = 0; i < 8; i++) be[i] = (u8)(v >> (56 - 8 * i));
    attr_put(b, type, be, 8);
}

static void attr_str(batch_t *b, u16 type, const char *s)
{
    attr_put(b, type, s, (u16)(strlen(s) + 1));
}

static void nest_begin(batch_t *b, u16 type)
{
    if (b->depth >= 8) { b->overflow = true; return; }
    b->nest[b->depth++] = b->len;
    nlattr_t *a = reserve(b, sizeof *a);
    a->nla_type = (u16)(type | NLA_F_NESTED);
    a->nla_len  = 0;                     /* nest_end fills this in */
}

static void nest_end(batch_t *b)
{
    if (b->depth <= 0 || b->overflow) return;
    u32 off = b->nest[--b->depth];
    nlattr_t *a = (nlattr_t *)(b->buf + off);
    a->nla_len = (u16)(b->len - off);
}

/* ── expressions ─────────────────────────────────────────────────── */

static void expr_begin(batch_t *b, const char *name)
{
    nest_begin(b, NFTA_LIST_ELEM);
    attr_str(b, NFTA_EXPR_NAME, name);
    nest_begin(b, NFTA_EXPR_DATA);
}

static void expr_end(batch_t *b)
{
    nest_end(b);        /* NFTA_EXPR_DATA */
    nest_end(b);        /* NFTA_LIST_ELEM */
}

/* meta: load something about the packet that is not in the packet -
 * which interface it came in on, which protocol the kernel decided it is. */
static void e_meta(batch_t *b, u32 key)
{
    expr_begin(b, "meta");
    attr_u32(b, NFTA_META_KEY,  key);
    attr_u32(b, NFTA_META_DREG, NFT_REG_1);
    expr_end(b);
}

/* payload: copy bytes straight out of the packet. */
static void e_payload(batch_t *b, u32 base, u32 off, u32 len)
{
    expr_begin(b, "payload");
    attr_u32(b, NFTA_PAYLOAD_DREG,   NFT_REG_1);
    attr_u32(b, NFTA_PAYLOAD_BASE,   base);
    attr_u32(b, NFTA_PAYLOAD_OFFSET, off);
    attr_u32(b, NFTA_PAYLOAD_LEN,    len);
    expr_end(b);
}

/* cmp: if register 1 does not compare this way, the rule stops here. */
static void e_cmp(batch_t *b, u32 op, const void *data, u16 len)
{
    expr_begin(b, "cmp");
    attr_u32(b, NFTA_CMP_SREG, NFT_REG_1);
    attr_u32(b, NFTA_CMP_OP,   op);
    nest_begin(b, NFTA_CMP_DATA);
    attr_put(b, NFTA_DATA_VALUE, data, len);
    nest_end(b);
    expr_end(b);
}

static void e_cmp_u8(batch_t *b, u32 op, u8 v)   { e_cmp(b, op, &v, 1); }

static void e_cmp_port(batch_t *b, u16 port)
{
    u16 be = htons(port);
    e_cmp(b, NFT_CMP_EQ, &be, 2);
}

/* An interface name sits in the register as 16 bytes, NUL padded. */
static void e_cmp_ifname(batch_t *b, const char *name)
{
    char buf[IFNAMSIZ];
    memset(buf, 0, sizeof buf);
    strlcpy(buf, name, sizeof buf);
    e_cmp(b, NFT_CMP_EQ, buf, (u16)sizeof buf);
}

/* bitwise: register 1 &= mask. Used to keep only the conntrack state
 * bits we care about before comparing. The mask is a plain host-order
 * word - the kernel ands it word by word, it never byte swaps. */
static void e_mask(batch_t *b, u32 mask)
{
    u32 zero = 0;
    expr_begin(b, "bitwise");
    attr_u32(b, NFTA_BITWISE_SREG, NFT_REG_1);
    attr_u32(b, NFTA_BITWISE_DREG, NFT_REG_1);
    attr_u32(b, NFTA_BITWISE_LEN,  4);
    nest_begin(b, NFTA_BITWISE_MASK);
    attr_put(b, NFTA_DATA_VALUE, &mask, 4);
    nest_end(b);
    nest_begin(b, NFTA_BITWISE_XOR);
    attr_put(b, NFTA_DATA_VALUE, &zero, 4);
    nest_end(b);
    expr_end(b);
}

/* ct: ask conntrack what it knows about the connection this packet
 * belongs to. This is what makes a reply to a request we made get in
 * without a rule that lets the whole internet in behind it. */
static void e_ct_state(batch_t *b)
{
    expr_begin(b, "ct");
    attr_u32(b, NFTA_CT_KEY,  NFT_CT_STATE);
    attr_u32(b, NFTA_CT_DREG, NFT_REG_1);
    expr_end(b);
}

/* "any of these bits set" - mask, then compare against zero the other way. */
static void e_ct_state_any(batch_t *b, u32 bits)
{
    u32 zero = 0;
    e_ct_state(b);
    e_mask(b, bits);
    e_cmp(b, NFT_CMP_NEQ, &zero, 4);
}

/* limit: let `rate` packets through per `secs`, allowing a burst.
 * Over the rate the rule stops matching, so whatever comes after it in
 * the chain - the drop policy - deals with the excess. */
static void e_limit(batch_t *b, u64 rate, u64 secs, u32 burst)
{
    expr_begin(b, "limit");
    attr_u64(b, NFTA_LIMIT_RATE,  rate);
    attr_u64(b, NFTA_LIMIT_UNIT,  secs);
    attr_u32(b, NFTA_LIMIT_BURST, burst);
    attr_u32(b, NFTA_LIMIT_TYPE,  0);        /* count packets, not bytes */
    expr_end(b);
}

static void e_counter(batch_t *b)
{
    expr_begin(b, "counter");
    attr_u64(b, NFTA_COUNTER_BYTES,   0);
    attr_u64(b, NFTA_COUNTER_PACKETS, 0);
    expr_end(b);
}

/* immediate: write a verdict into register 0, which ends the rule. */
static void e_verdict(batch_t *b, int verdict)
{
    expr_begin(b, "immediate");
    attr_u32(b, NFTA_IMMEDIATE_DREG, NFT_REG_VERDICT);
    nest_begin(b, NFTA_IMMEDIATE_DATA);
    nest_begin(b, NFTA_DATA_VERDICT);
    attr_u32(b, NFTA_VERDICT_CODE, (u32)verdict);
    nest_end(b);
    nest_end(b);
    expr_end(b);
}

/* ── tables, chains, rules ───────────────────────────────────────── */

static void m_table(batch_t *b, u16 msg, u16 extra_flags)
{
    msg_begin(b, NFT(msg), (u16)(NLM_F_REQUEST | NLM_F_ACK | extra_flags),
              NFPROTO_INET, 0);
    attr_str(b, NFTA_TABLE_NAME, TABLE_NAME);
    msg_end(b);
}

static void m_chain(batch_t *b, const char *name, u32 hook, int policy)
{
    msg_begin(b, NFT(NFT_MSG_NEWCHAIN),
              NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE, NFPROTO_INET, 0);
    attr_str(b, NFTA_CHAIN_TABLE, TABLE_NAME);
    attr_str(b, NFTA_CHAIN_NAME,  name);
    nest_begin(b, NFTA_CHAIN_HOOK);
    attr_u32(b, NFTA_HOOK_HOOKNUM,  hook);
    attr_u32(b, NFTA_HOOK_PRIORITY, 0);      /* where iptables' filter sits */
    nest_end(b);
    attr_u32(b, NFTA_CHAIN_POLICY, (u32)policy);
    attr_str(b, NFTA_CHAIN_TYPE,   "filter");
    msg_end(b);
}

/* Every rule carries a name, so `firewall` can print counters people can
 * read instead of a list of handles. nft stores comments the same way,
 * so a rule of ours shown by a real nft still reads properly. */
static void rule_begin(batch_t *b, const char *chain, const char *label)
{
    u8 udata[64];
    size_t n = strlen(label) + 1;
    if (n > sizeof udata - 2) n = sizeof udata - 2;
    udata[0] = 0;                    /* NFTNL_UDATA_RULE_COMMENT */
    udata[1] = (u8)n;
    memcpy(udata + 2, label, n - 1);
    udata[2 + n - 1] = 0;

    msg_begin(b, NFT(NFT_MSG_NEWRULE),
              NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_APPEND,
              NFPROTO_INET, 0);
    attr_str(b, NFTA_RULE_TABLE, TABLE_NAME);
    attr_str(b, NFTA_RULE_CHAIN, chain);
    attr_put(b, NFTA_RULE_USERDATA, udata, (u16)(n + 2));
    nest_begin(b, NFTA_RULE_EXPRESSIONS);
}

static void rule_end(batch_t *b)
{
    nest_end(b);
    msg_end(b);
}

/* ── talking to the kernel ───────────────────────────────────────── */

static int nl_open(void)
{
    long fd = lp_socket(AF_NETLINK, SOCK_RAW, NETLINK_NETFILTER);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "firewall: the kernel has no netfilter netlink socket\n");
        return -1;
    }
    sockaddr_nl_t me = { .nl_family = AF_NETLINK, .nl_pad = 0,
                         .nl_pid = 0, .nl_groups = 0 };
    if (lp_bind((int)fd, &me, sizeof me) < 0) {
        lp_close((int)fd);
        return -1;
    }
    s64 tv[2] = { 3, 0 };
    lp_setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO_NEW, tv, sizeof tv);
    return (int)fd;
}

static long nl_send(int fd, const void *buf, u32 len)
{
    sockaddr_nl_t kernel = { .nl_family = AF_NETLINK, .nl_pad = 0,
                             .nl_pid = 0, .nl_groups = 0 };
    return lp_sendto(fd, buf, len, 0, &kernel, sizeof kernel);
}

/* Say what the kernel's -errno means in the context of a firewall. */
static const char *why(int err)
{
    switch (err) {
    case 1:   return "not permitted - this has to run as root";
    case 2:   return "no such table or chain";
    case 17:  return "it is already there";
    case 22:  return "the kernel rejected the rule as malformed";
    case 93:  return "this kernel was built without nftables";
    case 95:  return "the kernel has no such expression, or it is not "
                     "allowed in this chain";
    default:  return "rejected";
    }
}

/* Read replies until the acknowledgement for `last_seq` arrives.
 * Returns 0 if every message in the batch was accepted. */
static int nl_wait(int fd, u32 last_seq, bool quiet)
{
    u8 buf[8192];
    int failed = 0;

    for (;;) {
        long n = lp_recvfrom(fd, buf, sizeof buf, 0, 0, 0);
        if (n <= 0) {
            /* A batch that failed is abandoned: the messages after the
             * one that broke are never acknowledged, so running out of
             * replies is the normal end of a failure, not a second one
             * worth reporting. */
            if (!quiet && !failed)
                dprintf(STDERR_FILENO,
                        "firewall: the kernel did not answer\n");
            return -1;
        }
        u32 off = 0;
        while (off + sizeof(nlmsghdr_t) <= (u32)n) {
            nlmsghdr_t *h = (nlmsghdr_t *)(buf + off);
            if (h->nlmsg_len < sizeof(nlmsghdr_t) ||
                off + h->nlmsg_len > (u32)n) return failed ? -1 : 0;

            if (h->nlmsg_type == NLMSG_ERROR) {
                int err = *(int *)(buf + off + sizeof(nlmsghdr_t));
                if (err != 0) {
                    failed = 1;
                    if (!quiet)
                        dprintf(STDERR_FILENO,
                                "firewall: message %u refused: %s (%d)\n",
                                h->nlmsg_seq, why(-err), -err);
                }
            }
            if (h->nlmsg_seq >= last_seq &&
                (h->nlmsg_type == NLMSG_ERROR || h->nlmsg_type == NLMSG_DONE))
                return failed ? -1 : 0;
            off += align4(h->nlmsg_len);
        }
    }
}

/* Is our table there? Asked outside a batch, so a missing table is an
 * ordinary -ENOENT rather than something that aborts a transaction. */
static bool table_exists(int fd)
{
    batch_t b;
    memset(&b, 0, sizeof b);
    msg_begin(&b, NFT(NFT_MSG_GETTABLE), NLM_F_REQUEST | NLM_F_ACK,
              NFPROTO_INET, 0);
    attr_str(&b, NFTA_TABLE_NAME, TABLE_NAME);
    msg_end(&b);
    if (nl_send(fd, b.buf, b.len) < 0) return false;
    return nl_wait(fd, b.seq, true) == 0;
}

/* ── the policy ──────────────────────────────────────────────────── */

/* Ports the boot partition asked us to open, on top of the standard set. */
#define MAX_EXTRA 24
typedef struct { u8 proto; u16 port; } extra_t;

static extra_t extra[MAX_EXTRA];
static int     extra_n;
static char    conf_mode[16];

/* /boot/firewall.conf:
 *
 *     # anything after a # is a comment
 *     mode strict
 *     tcp 8080
 *     udp 5353
 */
static void read_conf_into(const char *path, extra_t *list, int *listn,
                           char *mode, unsigned modesz)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0) return;
    char line[128];
    /* >= 0, not > 0. readline returns 0 for an empty line and -1 only
     * at the end, so "> 0" stopped at the first blank line - and a
     * blank line between sections is the obvious way to write firewall.conf.
     * Everything after it was dropped in silence, with the rules that
     * did apply reported as a success. */
    while (readline((int)fd, line, sizeof line) >= 0) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;
        char *sp = p;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        if (!*sp) continue;
        *sp++ = 0;
        while (*sp == ' ' || *sp == '\t') sp++;
        char *end = sp + strlen(sp);
        while (end > sp && (end[-1] == ' ' || end[-1] == '\t' ||
                            end[-1] == '\r')) *--end = 0;
        if (!*sp) continue;

        if (strcmp(p, "mode") == 0) {
            strlcpy(mode, sp, modesz);
        } else if (strcmp(p, "tcp") == 0 || strcmp(p, "udp") == 0) {
            int port = atoi(sp);
            if (port <= 0 || port > 65535) {
                dprintf(STDERR_FILENO,
                        "firewall: %s: \"%s\" is not a port number\n",
                        path, sp);
                continue;
            }
            if (*listn >= MAX_EXTRA) {
                dprintf(STDERR_FILENO,
                        "firewall: %s: no room for more than %d extra ports\n",
                        path, MAX_EXTRA);
                continue;
            }
            list[*listn].proto = (p[0] == 't') ? IPPROTO_TCP : IPPROTO_UDP;
            list[*listn].port  = (u16)port;
            (*listn)++;
        } else {
            dprintf(STDERR_FILENO, "firewall: %s: ignoring \"%s\"\n", path, p);
        }
    }
    lp_close((int)fd);
}

static void read_conf(const char *path)
{
    read_conf_into(path, extra, &extra_n, conf_mode, sizeof conf_mode);
}

/* Declared here because the commands below need them and they are
 * defined further down, next to the rest of the netlink work. */
static int  apply(bool strict);
static int  nl_open(void);
static bool table_exists(int fd);

/* ── opening a port without a card reader ────────────────────────── */

/* Until now the only way to open a port was to edit
 * /boot/firewall.conf, which means shutting the board down, taking the
 * card out, finding a computer with a slot, and putting it all back. For
 * a board three metres up a wall that is the difference between a
 * machine you can run a web server on and one you cannot - and "the
 * firewall is too tight to use" is how a firewall ends up turned off
 * altogether.
 *
 * So `firewall allow 8080` writes the port into /data/firewall.conf and
 * puts the new ruleset in place immediately.
 *
 * /data rather than /boot, because /boot is mounted read-only and stays
 * that way: it is the way back in when /data is gone, and a partition
 * nothing writes to is one a power cut cannot corrupt. /boot still gets
 * the last word - a port listed there cannot be closed from in here -
 * which is what you want when you are holding the card and the machine
 * is not behaving. */
#define DATA_CONF "/data/firewall.conf"

static bool write_data_conf(const extra_t *list, int n, const char *mode)
{
    char out[2048];
    int  len = 0;
    len += snprintf(out + len, sizeof out - len,
                    "# Written by `firewall allow` / `firewall deny`.\n"
                    "# Ports listed here are opened on top of ssh, ping,\n"
                    "# DHCP replies and loopback. /boot/firewall.conf, if\n"
                    "# it exists, is read as well and cannot be edited\n"
                    "# from here.\n");
    if (mode && mode[0])
        len += snprintf(out + len, sizeof out - len, "mode %s\n", mode);
    for (int i = 0; i < n && len < (int)sizeof out - 32; i++)
        len += snprintf(out + len, sizeof out - len, "%s %u\n",
                        list[i].proto == IPPROTO_TCP ? "tcp" : "udp",
                        (unsigned)list[i].port);

    /* Written to a temporary name and renamed over the top: a power cut
     * halfway through a rewrite would otherwise leave a truncated file,
     * and a truncated firewall.conf is a board with ports the person
     * thinks are open and are not. */
    long fd = lp_open(DATA_CONF ".new", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = lp_write((int)fd, out, (unsigned)len) == len;
    lp_close((int)fd);
    if (!ok) { lp_unlink(DATA_CONF ".new"); return false; }
    return lp_rename(DATA_CONF ".new", DATA_CONF) == 0;
}

/* "8080" or "tcp 8080" or "udp 5353". Returns false and says why if it
 * is neither. */
static bool parse_port(int argc, char **argv, int i, u8 *proto, u16 *port)
{
    const char *pstr = NULL;
    *proto = IPPROTO_TCP;

    if (i < argc && (strcmp(argv[i], "tcp") == 0 || strcmp(argv[i], "udp") == 0)) {
        *proto = (argv[i][0] == 't') ? IPPROTO_TCP : IPPROTO_UDP;
        i++;
    }
    if (i < argc) pstr = argv[i];

    if (!pstr) {
        dprintf(STDERR_FILENO,
                "firewall: which port? e.g. `firewall allow 8080`"
                " or `firewall allow udp 5353`\n");
        return false;
    }
    int n = atoi(pstr);
    if (n <= 0 || n > 65535) {
        dprintf(STDERR_FILENO, "firewall: \"%s\" is not a port number\n", pstr);
        return false;
    }
    *port = (u16)n;
    return true;
}

static int cmd_allow(bool add, int argc, char **argv)
{
    u8  proto; u16 port;
    if (!parse_port(argc, argv, 2, &proto, &port)) return 2;

    extra_t list[MAX_EXTRA];
    int     n = 0;
    char    mode[16] = "";
    read_conf_into(DATA_CONF, list, &n, mode, sizeof mode);

    /* Drop any existing entry for this port first, so `allow` twice is
     * one rule and `deny` after `allow` really removes it. */
    int keep = 0;
    bool had = false;
    for (int i = 0; i < n; i++) {
        if (list[i].proto == proto && list[i].port == port) { had = true; continue; }
        list[keep++] = list[i];
    }
    n = keep;

    if (add) {
        if (n >= MAX_EXTRA) {
            dprintf(STDERR_FILENO,
                    "firewall: no room for more than %d extra ports\n", MAX_EXTRA);
            return 1;
        }
        list[n].proto = proto;
        list[n].port  = port;
        n++;
    } else if (!had) {
        printf("firewall: %s %u was not open from here\n",
               proto == IPPROTO_TCP ? "tcp" : "udp", (unsigned)port);
        /* It may still be open because /boot says so. Say that rather
         * than let someone believe a port is closed when it is not. */
        extra_t b[MAX_EXTRA]; int bn = 0; char bm[16] = "";
        read_conf_into("/boot/firewall.conf", b, &bn, bm, sizeof bm);
        for (int i = 0; i < bn; i++)
            if (b[i].proto == proto && b[i].port == port)
                printf("firewall:   it is open because /boot/firewall.conf"
                       " lists it, and that file is read-only here\n");
        return 0;
    }

    if (!write_data_conf(list, n, mode)) {
        dprintf(STDERR_FILENO,
                "firewall: cannot write %s - is /data mounted?\n", DATA_CONF);
        return 1;
    }

    printf("firewall: %s %u %s\n",
           proto == IPPROTO_TCP ? "tcp" : "udp", (unsigned)port,
           add ? "is now open" : "is no longer open");

    /* Put it into effect now. Writing the file and waiting for a reboot
     * is the behaviour that makes people stop trusting the command. */
    int fd = nl_open();
    if (fd < 0) return 1;
    bool live = table_exists(fd);
    lp_close(fd);
    if (!live) {
        printf("firewall:   the firewall is off, so nothing is blocked"
               " anyway - `firewall on` applies this\n");
        return 0;
    }

    /* Re-read both files so the new ruleset is the whole policy again. */
    extra_n = 0; conf_mode[0] = 0;
    read_conf("/boot/firewall.conf");
    if (!conf_mode[0]) read_conf(DATA_CONF);
    return apply(strcmp(conf_mode, "strict") == 0);
}

static int cmd_ports(void)
{
    extra_t b[MAX_EXTRA]; int bn = 0; char bm[16] = "";
    extra_t d[MAX_EXTRA]; int dn = 0; char dm[16] = "";
    read_conf_into("/boot/firewall.conf", b, &bn, bm, sizeof bm);
    read_conf_into(DATA_CONF, d, &dn, dm, sizeof dm);

    printf("open on top of ssh, ping, DHCP replies and loopback:\n");
    if (!bn && !dn) {
        printf("  nothing yet - `firewall allow 8080` opens one\n");
        return 0;
    }
    for (int i = 0; i < dn; i++)
        printf("  %-3s %-5u  from here (%s)\n",
               d[i].proto == IPPROTO_TCP ? "tcp" : "udp",
               (unsigned)d[i].port, DATA_CONF);
    for (int i = 0; i < bn; i++)
        printf("  %-3s %-5u  from the boot partition - cannot be closed"
               " from here\n",
               b[i].proto == IPPROTO_TCP ? "tcp" : "udp", (unsigned)b[i].port);
    return 0;
}

/* A rule shaped "this protocol, this destination port -> accept". */
static void rule_port(batch_t *b, const char *chain, const char *label,
                      u8 proto, u16 port)
{
    rule_begin(b, chain, label);
    e_meta(b, NFT_META_L4PROTO);
    e_cmp_u8(b, NFT_CMP_EQ, proto);
    e_payload(b, NFT_PAYLOAD_TRANSPORT_HEADER, 2, 2);
    e_cmp_port(b, port);
    e_counter(b);
    e_verdict(b, NF_ACCEPT);
    rule_end(b);
}

static void build_input(batch_t *b)
{
    /* 1. Anything conntrack already knows. This is what keeps the SSH
     *    session you are typing in alive across `firewall on`. */
    rule_begin(b, "input", "established");
    e_ct_state_any(b, CT_ESTABLISHED | CT_RELATED);
    e_counter(b);
    e_verdict(b, NF_ACCEPT);
    rule_end(b);

    /* 2. Loopback. Half the things a machine does talk to themselves. */
    rule_begin(b, "input", "loopback");
    e_meta(b, NFT_META_IIFNAME);
    e_cmp_ifname(b, "lo");
    e_counter(b);
    e_verdict(b, NF_ACCEPT);
    rule_end(b);

    /* 3. SSH. Deliberately not conditional on conntrack: if conntrack
     *    has never seen your connection - it was made before the
     *    firewall existed - rule 1 does not cover it and this does.
     *
     *    New connections are limited to 30 a minute with a burst of 30.
     *    A person reconnecting will never notice; something working
     *    through a password list will not get through the door. Nothing
     *    here can shut an existing session: an established connection
     *    left rule 1 long before it reached this one. */
    rule_begin(b, "input", "ssh (new, rate limited)");
    e_meta(b, NFT_META_L4PROTO);
    e_cmp_u8(b, NFT_CMP_EQ, IPPROTO_TCP);
    e_payload(b, NFT_PAYLOAD_TRANSPORT_HEADER, 2, 2);
    e_cmp_port(b, 22);
    e_ct_state_any(b, CT_NEW);
    e_limit(b, 30, 60, 30);
    e_counter(b);
    e_verdict(b, NF_ACCEPT);
    rule_end(b);

    /* And SSH that conntrack cannot place at all, so that a session
     * begun before the firewall did keeps working. */
    rule_port(b, "input", "ssh (already open)", IPPROTO_TCP, 22);

    /* 4. ICMPv6. Not optional: neighbour discovery is how IPv6 finds
     *    anything at all, and blocking it does not harden the machine,
     *    it unplugs it. */
    rule_begin(b, "input", "icmpv6");
    e_meta(b, NFT_META_L4PROTO);
    e_cmp_u8(b, NFT_CMP_EQ, IPPROTO_ICMP6);
    e_counter(b);
    e_verdict(b, NF_ACCEPT);
    rule_end(b);

    /* 5. ping, capped so that being pinged cannot become the only thing
     *    a 512MB single-board computer does. */
    rule_begin(b, "input", "ping");
    e_meta(b, NFT_META_L4PROTO);
    e_cmp_u8(b, NFT_CMP_EQ, IPPROTO_ICMP);
    e_limit(b, 20, 1, 50);
    e_counter(b);
    e_verdict(b, NF_ACCEPT);
    rule_end(b);

    /* 6. DHCP replies.
     *
     *    This one is not optional and is easy to miss. A lease is a
     *    loan, and renewing it starts with a broadcast from 0.0.0.0 -
     *    which conntrack cannot tie to anything, so rule 1 does not
     *    cover the answer. Without this the machine keeps the address
     *    it has until the lease runs out and then has none, which on a
     *    box administered over SSH is the same as gone. */
    rule_port(b, "input", "dhcp reply", IPPROTO_UDP, 68);

    /* 7. Whatever /boot/firewall.conf asked for. */
    for (int i = 0; i < extra_n; i++) {
        char label[48];
        snprintf(label, sizeof label, "%s port %d",
                 extra[i].proto == IPPROTO_TCP ? "tcp" : "udp",
                 (int)extra[i].port);
        rule_port(b, "input", label, extra[i].proto, extra[i].port);
    }

    /* 8. Count what the policy is about to drop, so `firewall` can say
     *    how much is being turned away rather than just that it is on. */
    rule_begin(b, "input", "denied");
    e_counter(b);
    rule_end(b);
}

static void build_output(batch_t *b, bool strict)
{
    if (!strict) {
        rule_begin(b, "output", "sent");
        e_counter(b);
        rule_end(b);
        return;
    }

    /* Strict mode exists for one reason: if something does get onto the
     * machine, this is what stops it phoning home on a port of its
     * choosing. Everything the system itself needs is listed; anything
     * else has to be added to /boot/firewall.conf on purpose. */
    rule_begin(b, "output", "established");
    e_ct_state_any(b, CT_ESTABLISHED | CT_RELATED);
    e_counter(b);
    e_verdict(b, NF_ACCEPT);
    rule_end(b);

    rule_begin(b, "output", "loopback");
    e_meta(b, NFT_META_OIFNAME);
    e_cmp_ifname(b, "lo");
    e_counter(b);
    e_verdict(b, NF_ACCEPT);
    rule_end(b);

    rule_port(b, "output", "dns",       IPPROTO_UDP,  53);
    rule_port(b, "output", "dns (tcp)", IPPROTO_TCP,  53);
    rule_port(b, "output", "dhcp",      IPPROTO_UDP,  67);
    rule_port(b, "output", "ntp",       IPPROTO_UDP, 123);
    rule_port(b, "output", "http",      IPPROTO_TCP,  80);
    rule_port(b, "output", "https",     IPPROTO_TCP, 443);
    rule_port(b, "output", "ssh out",   IPPROTO_TCP,  22);

    rule_begin(b, "output", "icmpv6");
    e_meta(b, NFT_META_L4PROTO);
    e_cmp_u8(b, NFT_CMP_EQ, IPPROTO_ICMP6);
    e_counter(b);
    e_verdict(b, NF_ACCEPT);
    rule_end(b);

    rule_begin(b, "output", "ping out");
    e_meta(b, NFT_META_L4PROTO);
    e_cmp_u8(b, NFT_CMP_EQ, IPPROTO_ICMP);
    e_counter(b);
    e_verdict(b, NF_ACCEPT);
    rule_end(b);

    /* Dropped rather than refused. An ICMP "administratively
     * prohibited" generated here would be the friendlier answer, but on
     * this kernel it never reaches the local socket that is waiting for
     * it, so it buys nothing. What actually keeps a blocked connection
     * from looking like a hang is on the other side: our own TCP sockets
     * stop retransmitting the SYN after about seven seconds. */
    rule_begin(b, "output", "blocked");
    e_counter(b);
    rule_end(b);
}

static int apply(bool strict)
{
    int fd = nl_open();
    if (fd < 0) return 1;

    bool replacing = table_exists(fd);

    batch_t b;
    memset(&b, 0, sizeof b);

    msg_begin(&b, NFNL_MSG_BATCH_BEGIN, NLM_F_REQUEST, 0,
              NFNL_SUBSYS_NFTABLES);
    msg_end(&b);

    /* Replacing the table wholesale is what makes this idempotent: run
     * `firewall on` twice and you get one ruleset, not two. */
    if (replacing) m_table(&b, NFT_MSG_DELTABLE, 0);
    m_table(&b, NFT_MSG_NEWTABLE, NLM_F_CREATE);

    m_chain(&b, "input",   NF_INET_LOCAL_IN,  NF_DROP);
    m_chain(&b, "forward", NF_INET_FORWARD,   NF_DROP);
    m_chain(&b, "output",  NF_INET_LOCAL_OUT, strict ? NF_DROP : NF_ACCEPT);

    build_input(&b);

    /* This machine is not a router. Anything asking it to forward a
     * packet is either lost or probing. */
    rule_begin(&b, "forward", "forwarded");
    e_counter(&b);
    rule_end(&b);

    build_output(&b, strict);

    /* The last message that asked to be acknowledged. BATCH_END does
     * not ask, and gets no reply - so waiting for its sequence number
     * is waiting for something that never comes, and a ruleset that
     * applied perfectly well is reported as a failure. */
    u32 ack_seq = b.seq;

    msg_begin(&b, NFNL_MSG_BATCH_END, NLM_F_REQUEST, 0, NFNL_SUBSYS_NFTABLES);
    msg_end(&b);

    if (b.overflow) {
        dprintf(STDERR_FILENO,
                "firewall: too many rules to send in one transaction - "
                "trim /boot/firewall.conf\n");
        lp_close(fd);
        return 1;
    }

    if (nl_send(fd, b.buf, b.len) < 0) {
        dprintf(STDERR_FILENO, "firewall: could not reach the kernel\n");
        lp_close(fd);
        return 1;
    }
    int rc = nl_wait(fd, ack_seq, false);
    lp_close(fd);

    if (rc != 0) {
        dprintf(STDERR_FILENO,
                "firewall: nothing was changed - the kernel takes the whole "
                "ruleset or none of it\n");
        return 1;
    }

    printf("firewall: %s\n", strict ? "on (strict)" : "on");
    printf("  in    established, loopback, ssh, icmp, dhcp");
    for (int i = 0; i < extra_n; i++)
        printf(", %s/%d", extra[i].proto == IPPROTO_TCP ? "tcp" : "udp",
               (int)extra[i].port);
    printf(" - everything else dropped\n");
    printf("  out   %s\n", strict ? "dns, ntp, http, https, ssh, icmp - "
                                    "everything else dropped"
                                  : "unrestricted");
    printf("  fwd   dropped (this machine does not route)\n");
    return 0;
}

static int turn_off(void)
{
    int fd = nl_open();
    if (fd < 0) return 1;

    if (!table_exists(fd)) {
        printf("firewall: already off\n");
        lp_close(fd);
        return 0;
    }

    batch_t b;
    memset(&b, 0, sizeof b);
    msg_begin(&b, NFNL_MSG_BATCH_BEGIN, NLM_F_REQUEST, 0,
              NFNL_SUBSYS_NFTABLES);
    msg_end(&b);
    m_table(&b, NFT_MSG_DELTABLE, 0);
    u32 ack_seq = b.seq;
    msg_begin(&b, NFNL_MSG_BATCH_END, NLM_F_REQUEST, 0, NFNL_SUBSYS_NFTABLES);
    msg_end(&b);

    nl_send(fd, b.buf, b.len);
    int rc = nl_wait(fd, ack_seq, false);
    lp_close(fd);
    if (rc != 0) return 1;
    printf("firewall: off - every port is open\n");
    return 0;
}

/* ── reading the ruleset back ────────────────────────────────────── */

/* Walk a nested attribute run, returning the payload of the first
 * attribute of `want`. Length comes back in *len. */
static const u8 *find_attr(const u8 *p, u32 size, u16 want, u32 *len)
{
    u32 off = 0;
    while (off + sizeof(nlattr_t) <= size) {
        const nlattr_t *a = (const nlattr_t *)(p + off);
        if (a->nla_len < sizeof(nlattr_t) || off + a->nla_len > size) break;
        if ((a->nla_type & 0x3FFF) == want) {
            *len = a->nla_len - (u32)sizeof(nlattr_t);
            return p + off + sizeof(nlattr_t);
        }
        off += align4(a->nla_len);
    }
    *len = 0;
    return 0;
}

static u64 be64_at(const u8 *p)
{
    u64 v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

/* Pull packets and bytes out of the counter expression, if the rule has
 * one. Every rule we write does. */
static bool rule_counter(const u8 *exprs, u32 size, u64 *pkts, u64 *bytes)
{
    u32 off = 0;
    while (off + sizeof(nlattr_t) <= size) {
        const nlattr_t *a = (const nlattr_t *)(exprs + off);
        if (a->nla_len < sizeof(nlattr_t) || off + a->nla_len > size) break;
        const u8 *elem = exprs + off + sizeof(nlattr_t);
        u32 elen = a->nla_len - (u32)sizeof(nlattr_t);
        u32 nlen = 0;
        const u8 *name = find_attr(elem, elen, NFTA_EXPR_NAME, &nlen);
        if (name && nlen >= 8 && memcmp(name, "counter", 8) == 0) {
            u32 dlen = 0;
            const u8 *data = find_attr(elem, elen, NFTA_EXPR_DATA, &dlen);
            if (data) {
                u32 l = 0;
                const u8 *pb = find_attr(data, dlen, NFTA_COUNTER_PACKETS, &l);
                const u8 *bb = find_attr(data, dlen, NFTA_COUNTER_BYTES, &l);
                if (pb && bb) {
                    *pkts  = be64_at(pb);
                    *bytes = be64_at(bb);
                    return true;
                }
            }
        }
        off += align4(a->nla_len);
    }
    return false;
}

static void human(u64 bytes, char *out, size_t n)
{
    static const char *unit[] = { "B", "K", "M", "G" };
    int u = 0;
    u64 v = bytes;
    while (v >= 10240 && u < 3) { v /= 1024; u++; }
    snprintf(out, n, "%lu%s", (unsigned long)v, unit[u]);
}

static int status(void)
{
    int fd = nl_open();
    if (fd < 0) return 1;

    if (!table_exists(fd)) {
        printf("firewall: off - every port is open\n");
        printf("  turn it on with `firewall on`\n");
        lp_close(fd);
        return 0;
    }

    batch_t b;
    memset(&b, 0, sizeof b);
    msg_begin(&b, NFT(NFT_MSG_GETRULE), NLM_F_REQUEST | NLM_F_DUMP,
              NFPROTO_INET, 0);
    attr_str(&b, NFTA_RULE_TABLE, TABLE_NAME);
    msg_end(&b);
    if (nl_send(fd, b.buf, b.len) < 0) { lp_close(fd); return 1; }

    printf("firewall: on\n");
    printf("  %-9s %-24s %10s %9s\n", "chain", "rule", "packets", "bytes");

    u8 buf[16384];
    bool done = false;
    while (!done) {
        long n = lp_recvfrom(fd, buf, sizeof buf, 0, 0, 0);
        if (n <= 0) break;
        u32 off = 0;
        while (off + sizeof(nlmsghdr_t) <= (u32)n) {
            nlmsghdr_t *h = (nlmsghdr_t *)(buf + off);
            if (h->nlmsg_len < sizeof(nlmsghdr_t) ||
                off + h->nlmsg_len > (u32)n) { done = true; break; }
            if (h->nlmsg_type == NLMSG_DONE ||
                h->nlmsg_type == NLMSG_ERROR) { done = true; break; }

            u32 hdr = (u32)sizeof(nlmsghdr_t) + (u32)sizeof(nfgenmsg_t);
            const u8 *attrs = buf + off + hdr;
            u32 alen = h->nlmsg_len - hdr;

            u32 l = 0;
            const u8 *chain = find_attr(attrs, alen, NFTA_RULE_CHAIN, &l);
            const u8 *ud    = find_attr(attrs, alen, NFTA_RULE_USERDATA, &l);
            char label[48];
            if (ud && l >= 3 && ud[0] == 0) {
                u32 n2 = ud[1];
                if (n2 > sizeof label) n2 = sizeof label;
                strlcpy(label, (const char *)ud + 2, n2);
            } else {
                strlcpy(label, "(unnamed)", sizeof label);
            }

            u32 elen = 0;
            const u8 *exprs =
                find_attr(attrs, alen, NFTA_RULE_EXPRESSIONS, &elen);
            u64 pkts = 0, bytes = 0;
            char hb[16];
            if (exprs && rule_counter(exprs, elen, &pkts, &bytes)) {
                human(bytes, hb, sizeof hb);
                printf("  %-9s %-24s %10lu %9s\n",
                       chain ? (const char *)chain : "?", label,
                       (unsigned long)pkts, hb);
            } else {
                printf("  %-9s %-24s %10s %9s\n",
                       chain ? (const char *)chain : "?", label, "-", "-");
            }
            off += align4(h->nlmsg_len);
        }
    }
    lp_close(fd);
    printf("\n  \"denied\" and \"blocked\" count what was turned away.\n");
    return 0;
}

static void usage(void)
{
    printf("firewall - what is allowed to reach this machine\n\n");
    printf("  firewall              show the rules and how much each has seen\n");
    printf("  firewall on           accept ssh, ping and replies; drop the rest\n");
    printf("  firewall strict       the same, and only let the system itself out\n");
    printf("  firewall off          remove every rule\n\n");
    printf("  firewall allow 8080   open a port, right now\n");
    printf("  firewall allow udp 5353\n");
    printf("  firewall deny 8080    close it again\n");
    printf("  firewall ports        which extra ports are open, and why\n\n");
    printf("`allow` and `deny` write /data/firewall.conf and take effect\n");
    printf("at once - no reboot, no card reader.\n\n");
    printf("/boot/firewall.conf is read as well, and wins: it is on the\n");
    printf("FAT boot partition, so any computer with a card reader can\n");
    printf("set it, and nothing on the machine can undo it.\n\n");
    printf("  mode strict\n");
    printf("  tcp 8080\n");
    printf("  udp 5353\n\n");
    printf("Turning it on never closes the connection you are typing in:\n");
    printf("the whole ruleset is handed to the kernel as one transaction,\n");
    printf("and the rule that keeps ssh open is part of it.\n");
}

int main(int argc, char **argv)
{
    read_conf("/boot/firewall.conf");
    if (!conf_mode[0]) read_conf("/data/firewall.conf");

    const char *cmd = (argc > 1) ? argv[1] : "status";

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "help") == 0) { usage(); return 0; }

    /* `firewall boot` is what /etc/rc runs: it does whatever
     * /boot/firewall.conf says, and nothing if it says nothing. */
    if (strcmp(cmd, "boot") == 0) {
        if (strcmp(conf_mode, "off") == 0) {
            printf("firewall: off, because /boot/firewall.conf says so\n");
            return 0;
        }
        return apply(strcmp(conf_mode, "strict") == 0);
    }

    if (strcmp(cmd, "on") == 0)     return apply(false);
    if (strcmp(cmd, "strict") == 0) return apply(true);
    if (strcmp(cmd, "off") == 0)    return turn_off();
    if (strcmp(cmd, "status") == 0) return status();
    if (strcmp(cmd, "allow") == 0 || strcmp(cmd, "open") == 0)
        return cmd_allow(true, argc, argv);
    if (strcmp(cmd, "deny") == 0 || strcmp(cmd, "close") == 0)
        return cmd_allow(false, argc, argv);
    if (strcmp(cmd, "ports") == 0)  return cmd_ports();

    dprintf(STDERR_FILENO, "firewall: no idea what \"%s\" means\n", cmd);
    dprintf(STDERR_FILENO,
            "try: firewall on | strict | off | status | ports"
            " | allow <port> | deny <port>\n");
    return 1;
}
