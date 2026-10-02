/* tz.c - the calendar, time zones, local time and strftime.
 *
 * See tz.h for where the zone comes from and why it is tzdata now. What
 * is here, in the order it appears:
 *
 *   the calendar     days <-> dates, in constant time. The loops this
 *                    replaced walked a year at a time, which is fine for
 *                    2026 and a hang for `date -d @-67768040609740800`.
 *   zone files       TZif versions 1 to 4 (RFC 8536): the 64-bit data
 *                    when there is any, and the POSIX rule in the footer
 *                    for every instant after the last transition.
 *   POSIX rules      "EST5EDT,M3.2.0,M11.1.0" and the <+0530> quoting,
 *                    with version 3's hours past 24 and negative times.
 *   the older line   "<minutes> <label> [EU|US|AU|NZ] [summer label]",
 *                    what `date -z` wrote before there was tzdata. It is
 *                    turned into a POSIX rule and goes the same way.
 *   lookups          localtime, mktime and the offset/label pair the
 *                    older callers use.
 *   strftime         the GNU set; see the note by it.
 *
 * Nothing here trusts a zone file's counts: every length is checked
 * against the bytes actually read before anything is indexed by it.
 * sudo formats times too, and a corrupt /etc/localtime must be a wrong
 * clock, not a read past the end of a buffer in a setuid program.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"

/* ── The calendar ─────────────────────────────────────────────────────
 *
 * Howard Hinnant's days_from_civil and civil_from_days: proleptic
 * Gregorian, any year, no loops, and exact for negative days. The only
 * subtlety is division: C truncates toward zero, and the era arithmetic
 * wants floor, hence the (y >= 0 ? y : y - 399) spellings. */

bool lp_is_leap(long y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

int lp_days_in_month(long y, int m)
{
    static const u8 LEN[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m < 1 || m > 12)
        return 30;
    return (m == 2 && lp_is_leap(y)) ? 29 : LEN[m - 1];
}

s64 lp_days_from_civil(long year, int mon, int day)
{
    /* Carry an out-of-range month into the year first. */
    s64 y = year;
    s64 m0 = (s64)mon - 1;
    y += m0 >= 0 ? m0 / 12 : -((11 - m0) / 12);
    int m = (int)(m0 - (m0 >= 0 ? m0 / 12 : -((11 - m0) / 12)) * 12) + 1;

    y -= m <= 2;
    s64 era = (y >= 0 ? y : y - 399) / 400;
    s64 yoe = y - era * 400;                               /* 0..399 */
    s64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    s64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;       /* 0..146096 */
    return era * 146097 + doe - 719468;
}

void lp_civil_from_days(s64 z, long *year, int *mon, int *day)
{
    z += 719468;
    s64 era = (z >= 0 ? z : z - 146096) / 146097;
    s64 doe = z - era * 146097;
    s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    s64 y   = yoe + era * 400;
    s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    s64 mp  = (5 * doy + 2) / 153;
    int d   = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m   = (int)(mp < 10 ? mp + 3 : mp - 9);
    *year = (long)(y + (m <= 2));
    *mon  = m;
    *day  = d;
}

/* Floor division and its remainder, for seconds before 1970. */
static s64 fdiv(s64 a, s64 b) { s64 q = a / b; return (a % b < 0) ? q - 1 : q; }
static s64 fmod64(s64 a, s64 b) { s64 r = a % b; return r < 0 ? r + b : r; }

/* Fill the seven calendar fields and yday from a count of seconds that
 * is already in the zone wanted. */
static void split_secs(s64 t, lp_tm_t *out)
{
    s64 days = fdiv(t, 86400);
    s64 rem  = fmod64(t, 86400);
    long y; int m, d;
    lp_civil_from_days(days, &y, &m, &d);
    out->year = (int)y;
    out->mon  = m;
    out->day  = d;
    out->hour = (int)(rem / 3600);
    out->min  = (int)(rem % 3600 / 60);
    out->sec  = (int)(rem % 60);
    out->wday = (int)fmod64(days + 4, 7);           /* 1970-01-01: Thursday */
    out->yday = (int)(days - lp_days_from_civil(y, 1, 1));
}

void lp_gmtime(s64 t, lp_tm_t *out)
{
    split_secs(t, out);
    out->isdst  = 0;
    out->gmtoff = 0;
    strlcpy(out->zone, "UTC", sizeof out->zone);
}

s64 lp_timegm(const lp_tm_t *tm)
{
    return lp_days_from_civil(tm->year, tm->mon, 1) * 86400
         + ((s64)tm->day - 1) * 86400
         + (s64)tm->hour * 3600 + (s64)tm->min * 60 + tm->sec;
}

s64 lp_time_ns(long *ns)
{
    s64 ts[2] = { 0, 0 };
    if (sys_call2(SYS_clock_gettime, 0 /* CLOCK_REALTIME */, (long)ts) < 0) {
        if (ns) *ns = 0;
        return 0;
    }
    if (ns) *ns = (long)ts[1];
    return ts[0];
}

/* ── A zone, however it was described ──────────────────────────────── */

/* One date in a POSIX rule: Jn (1-365, never Feb 29), n (0-365), or
 * Mm.w.d (weekday d of week w of month m, w = 5 meaning the last). */
typedef struct {
    char kind;          /* 'J', 'N' or 'M' */
    int  n, m, w, d;
    s32  time;          /* seconds after local midnight; may be negative */
} rdate_t;

typedef struct {
    char    std[16], dst[16];
    s32     std_off, dst_off;   /* seconds east of UTC */
    bool    has_dst;
    rdate_t start, end;
} prule_t;

typedef struct {
    s32 off;
    u8  isdst;
    u8  abbr;           /* index into chars */
} ttype_t;

typedef struct {
    int      nt;        /* transitions */
    s64     *at;
    u8      *ti;
    int      ntypes;
    ttype_t *types;
    char    *chars;
    int      nchars;
    bool     has_rule;
    prule_t  rule;
    void    *blob;      /* the one allocation the arrays live in */
} zone_t;

typedef struct { s32 off; bool isdst; const char *abbr; } zinfo_t;

static void zone_free(zone_t *z)
{
    free(z->blob);
    memset(z, 0, sizeof *z);
}

/* ── POSIX TZ rules ────────────────────────────────────────────────── */

static bool isdig(char c)   { return c >= '0' && c <= '9'; }
static bool isalpha_(char c){ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/* A zone abbreviation: three or more letters, or anything within <>. */
static const char *p_name(const char *s, char *out, size_t cap)
{
    size_t k = 0;
    bool quoted = *s == '<';
    if (quoted) {
        s++;
        while (*s && *s != '>') {
            if (k < cap - 1) out[k++] = *s;
            s++;
        }
        if (*s != '>') return NULL;
        s++;
    } else {
        while (isalpha_(*s)) {
            if (k < cap - 1) out[k++] = *s;
            s++;
        }
    }
    out[k] = '\0';
    /* POSIX: three letters at least, unless quoted. */
    return (quoted ? k >= 1 : k >= 3) ? s : NULL;
}

/* [+-]hh[:mm[:ss]]. Hours up to 167, which version 3 allows for the
 * transition times; offsets themselves stay under 25. */
static const char *p_hms(const char *s, s32 *out)
{
    int sign = 1;
    if (*s == '+') s++;
    else if (*s == '-') { sign = -1; s++; }
    if (!isdig(*s)) return NULL;
    s32 h = 0;
    while (isdig(*s)) { h = h * 10 + (*s++ - '0'); if (h > 167) return NULL; }
    s32 m = 0, sec = 0;
    if (*s == ':') {
        s++;
        if (!isdig(*s)) return NULL;
        while (isdig(*s)) { m = m * 10 + (*s++ - '0'); if (m > 59) return NULL; }
        if (*s == ':') {
            s++;
            if (!isdig(*s)) return NULL;
            while (isdig(*s)) { sec = sec * 10 + (*s++ - '0'); if (sec > 59) return NULL; }
        }
    }
    *out = sign * (h * 3600 + m * 60 + sec);
    return s;
}

static const char *p_num(const char *s, int *out, int lo, int hi)
{
    if (!isdig(*s)) return NULL;
    int v = 0;
    while (isdig(*s)) { v = v * 10 + (*s++ - '0'); if (v > hi) return NULL; }
    if (v < lo) return NULL;
    *out = v;
    return s;
}

static const char *p_rdate(const char *s, rdate_t *r)
{
    memset(r, 0, sizeof *r);
    if (*s == 'J') {
        r->kind = 'J';
        s = p_num(s + 1, &r->n, 1, 365);
    } else if (*s == 'M') {
        r->kind = 'M';
        s = p_num(s + 1, &r->m, 1, 12);
        if (!s || *s != '.') return NULL;
        s = p_num(s + 1, &r->w, 1, 5);
        if (!s || *s != '.') return NULL;
        s = p_num(s + 1, &r->d, 0, 6);
    } else {
        r->kind = 'N';
        s = p_num(s, &r->n, 0, 365);
    }
    if (!s) return NULL;
    r->time = 2 * 3600;
    if (*s == '/') {
        s32 t;
        s = p_hms(s + 1, &t);
        if (!s) return NULL;
        r->time = t;
    }
    return s;
}

/* Parse a whole POSIX TZ value. Returns false when even the first name
 * cannot be read. A name with no offset is UTC under that name, which is
 * what glibc does with TZ=Foo, and a malformed tail leaves what was
 * already understood in place for the same reason. */
static bool parse_posix(const char *s, prule_t *r)
{
    memset(r, 0, sizeof *r);
    s = p_name(s, r->std, sizeof r->std);
    if (!s) return false;
    s32 off;
    const char *t = p_hms(s, &off);
    if (!t) return true;                 /* "Foo": UTC called Foo */
    r->std_off = -off;                   /* POSIX counts west as positive */
    s = t;
    if (!*s) return true;

    const char *after = p_name(s, r->dst, sizeof r->dst);
    if (!after) return true;
    s = after;
    r->has_dst = true;
    r->dst_off = r->std_off + 3600;
    if (*s && *s != ',') {
        t = p_hms(s, &off);
        if (!t) return true;
        r->dst_off = -off;
        s = t;
    }
    if (*s != ',') {
        /* No dates: the US rules, which is what glibc falls back to
         * when there is no posixrules file to ask. */
        r->start = (rdate_t){ 'M', 0, 3, 2, 0, 2 * 3600 };
        r->end   = (rdate_t){ 'M', 0, 11, 1, 0, 2 * 3600 };
        return true;
    }
    rdate_t a, b;
    s = p_rdate(s + 1, &a);
    if (!s || *s != ',') { r->has_dst = false; return true; }
    s = p_rdate(s + 1, &b);
    if (!s) { r->has_dst = false; return true; }
    r->start = a;
    r->end   = b;
    return true;
}

/* The day (days since 1970) a rule date falls on in year y. */
static s64 rdate_day(const rdate_t *r, long y)
{
    s64 jan1 = lp_days_from_civil(y, 1, 1);
    switch (r->kind) {
    case 'J': {
        int n = r->n - 1;                      /* Feb 29 is never counted */
        if (lp_is_leap(y) && r->n >= 60) n++;
        return jan1 + n;
    }
    case 'N':
        return jan1 + r->n;
    default: {
        s64 first = lp_days_from_civil(y, r->m, 1);
        int wd1 = (int)fmod64(first + 4, 7);
        int d = 1 + (r->d - wd1 + 7) % 7 + 7 * (r->w - 1);
        int len = lp_days_in_month(y, r->m);
        while (d > len) d -= 7;
        return first + d - 1;
    }
    }
}

static zinfo_t rule_info(const prule_t *r, s64 t)
{
    zinfo_t std = { r->std_off, false, r->std };
    if (!r->has_dst)
        return std;
    zinfo_t dst = { r->dst_off, true, r->dst };

    /* The year as the local standard clock reads it. Transitions never
     * sit on New Year's Eve, so the year either side of midnight UTC
     * gives the same answer. */
    s64 local = t + r->std_off;
    long y; int m, d;
    lp_civil_from_days(fdiv(local, 86400), &y, &m, &d);

    s64 start = rdate_day(&r->start, y) * 86400 + r->start.time - r->std_off;
    s64 end   = rdate_day(&r->end,   y) * 86400 + r->end.time   - r->dst_off;

    bool in;
    if (start < end) in = t >= start && t < end;
    else             in = !(t >= end && t < start);   /* southern summer */
    return in ? dst : std;
}

/* ── Zone files ────────────────────────────────────────────────────── */

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static s64 be64(const u8 *p)
{
    return (s64)(((u64)be32(p) << 32) | be32(p + 4));
}

#define TZ_FILE_MAX 65536

/* Read a whole file into a fresh buffer. */
static u8 *slurp(const char *path, long *len)
{
    long fd = lp_open(path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return NULL;
    u8 *buf = malloc(TZ_FILE_MAX);
    if (!buf) { lp_close((int)fd); return NULL; }
    long n = 0;
    for (;;) {
        long r = lp_read((int)fd, buf + n, (size_t)(TZ_FILE_MAX - n));
        if (r <= 0) break;
        n += r;
        if (n >= TZ_FILE_MAX) break;
    }
    lp_close((int)fd);
    *len = n;
    return buf;
}

/* One header's six counts, checked against what is left of the file. */
typedef struct { u32 isut, isstd, leap, time, type, chars; } tzhead_t;

static bool read_head(const u8 *p, long left, tzhead_t *h)
{
    if (left < 44 || memcmp(p, "TZif", 4) != 0)
        return false;
    h->isut  = be32(p + 20);
    h->isstd = be32(p + 24);
    h->leap  = be32(p + 28);
    h->time  = be32(p + 32);
    h->type  = be32(p + 36);
    h->chars = be32(p + 40);
    /* The limits tzcode itself enforces; anything bigger is not a zone
     * file, whatever its first four bytes say. */
    return h->type >= 1 && h->type <= 256 && h->time <= 2000 * 2 &&
           h->chars <= 256 * 8 && h->leap <= 600 &&
           (h->isut == 0 || h->isut == h->type) &&
           (h->isstd == 0 || h->isstd == h->type);
}

static long block_len(const tzhead_t *h, int tsize)
{
    return (long)h->time * tsize + h->time + h->type * 6 + h->chars +
           (long)h->leap * (tsize + 4) + h->isstd + h->isut;
}

/* Parse a TZif image into z. The 64-bit block of version 2 and later is
 * preferred: the 32-bit one stops in 2038 and starts in 1901, and a zone
 * read from it forgets everything outside that. */
static bool parse_tzif(const u8 *p, long len, zone_t *z)
{
    memset(z, 0, sizeof *z);
    tzhead_t h;
    if (!read_head(p, len, &h)) return false;
    int tsize = 4;
    const u8 *d = p + 44;
    long left = len - 44;

    if (p[4] >= '2') {
        long skip = block_len(&h, 4);
        if (skip > left) return false;
        d += skip;
        left -= skip;
        if (!read_head(d, left, &h)) return false;
        d += 44;
        left -= 44;
        tsize = 8;
    }
    long need = block_len(&h, tsize);
    if (need > left) return false;

    size_t bytes = h.time * sizeof(s64) + h.time + h.type * sizeof(ttype_t) +
                   h.chars + 1 + 16;
    u8 *blob = malloc(bytes);
    if (!blob) return false;
    memset(blob, 0, bytes);
    z->blob   = blob;
    z->at     = (s64 *)blob;
    z->types  = (ttype_t *)(blob + h.time * sizeof(s64));
    z->ti     = (u8 *)z->types + h.type * sizeof(ttype_t);
    z->chars  = (char *)z->ti + h.time;
    z->nt     = (int)h.time;
    z->ntypes = (int)h.type;
    z->nchars = (int)h.chars;

    const u8 *q = d;
    for (u32 i = 0; i < h.time; i++, q += tsize)
        z->at[i] = tsize == 8 ? be64(q) : (s64)(s32)be32(q);
    for (u32 i = 0; i < h.time; i++, q++) {
        if (*q >= h.type) { zone_free(z); return false; }
        z->ti[i] = *q;
        if (i && z->at[i] <= z->at[i - 1]) { zone_free(z); return false; }
    }
    for (u32 i = 0; i < h.type; i++, q += 6) {
        z->types[i].off   = (s32)be32(q);
        z->types[i].isdst = q[4] ? 1 : 0;
        z->types[i].abbr  = q[5];
        if (q[5] >= h.chars) { zone_free(z); return false; }
    }
    memcpy(z->chars, q, h.chars);
    z->chars[h.chars] = '\0';
    q += h.chars;
    /* Leap seconds (the right/ zones) are skipped: this clock is POSIX
     * time, which does not count them, and so is every clock that sets
     * it. A right/ zone therefore reads the same as its plain twin. */
    q += (long)h.leap * (tsize + 4) + h.isstd + h.isut;

    /* The footer: "\n<rule>\n", from version 2 on. */
    if (tsize == 8) {
        long rest = len - (q - p);
        if (rest >= 2 && q[0] == '\n') {
            char rule[128];
            long k = 0;
            const u8 *r = q + 1;
            while (k < rest - 1 && r[k] != '\n' && k < (long)sizeof rule - 1) {
                rule[k] = (char)r[k];
                k++;
            }
            rule[k] = '\0';
            if (k > 0 && parse_posix(rule, &z->rule))
                z->has_rule = true;
        }
    }
    return true;
}

static zinfo_t zone_info(const zone_t *z, s64 t)
{
    static const zinfo_t UTC = { 0, false, "UTC" };
    if (z->nt == 0 || t >= z->at[z->nt - 1]) {
        if (z->has_rule)
            return rule_info(&z->rule, t);
        if (z->nt == 0) {
            if (z->ntypes == 0) return UTC;
            const ttype_t *tt = &z->types[0];
            return (zinfo_t){ tt->off, tt->isdst, z->chars + tt->abbr };
        }
    }
    const ttype_t *tt;
    if (z->nt == 0 || t < z->at[0]) {
        /* Before the first transition: time type 0, as RFC 8536 says. */
        tt = &z->types[0];
    } else {
        int lo = 0, hi = z->nt - 1;          /* largest i with at[i] <= t */
        while (lo < hi) {
            int mid = lo + (hi - lo + 1) / 2;
            if (z->at[mid] <= t) lo = mid;
            else hi = mid - 1;
        }
        tt = &z->types[z->ti[lo]];
    }
    return (zinfo_t){ tt->off, tt->isdst, z->chars + tt->abbr };
}

/* ── The older one-line format ──────────────────────────────────────
 *
 * "<minutes> <label> [rule] [summer label]". The four rules are written
 * as the POSIX rules they always were, so they go through the same code
 * as a zone file's footer rather than a second implementation of summer
 * time. EU changes at 01:00 UTC everywhere, which in local standard time
 * is 01:00 plus the zone's offset. */
static bool parse_legacy(const char *s, prule_t *r)
{
    memset(r, 0, sizeof *r);
    while (*s == ' ' || *s == '\t') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    if (!isdig(*s)) return false;
    int minutes = 0;
    while (isdig(*s)) { minutes = minutes * 10 + (*s++ - '0'); if (minutes > 1560) return false; }
    minutes *= sign;
    r->std_off = minutes * 60;

    char words[3][16] = { "", "", "" };
    for (int w = 0; w < 3; w++) {
        while (*s == ' ' || *s == '\t') s++;
        size_t k = 0;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') {
            if (k < sizeof words[w] - 1) words[w][k++] = *s;
            s++;
        }
        words[w][k] = '\0';
    }
    strlcpy(r->std, words[0][0] ? words[0] : "UTC", sizeof r->std);
    strlcpy(r->dst, words[2][0] ? words[2] : r->std, sizeof r->dst);

    const char *rule = words[1];
    s32 std = r->std_off;
    if (strcmp(rule, "EU") == 0) {
        r->start = (rdate_t){ 'M', 0, 3, 5, 0, 3600 + std };
        r->end   = (rdate_t){ 'M', 0, 10, 5, 0, 3600 + std + 3600 };
    } else if (strcmp(rule, "US") == 0) {
        r->start = (rdate_t){ 'M', 0, 3, 2, 0, 2 * 3600 };
        r->end   = (rdate_t){ 'M', 0, 11, 1, 0, 2 * 3600 };
    } else if (strcmp(rule, "AU") == 0) {
        r->start = (rdate_t){ 'M', 0, 10, 1, 0, 2 * 3600 };
        r->end   = (rdate_t){ 'M', 0, 4, 1, 0, 3 * 3600 };
    } else if (strcmp(rule, "NZ") == 0) {
        r->start = (rdate_t){ 'M', 0, 9, 5, 0, 2 * 3600 };
        r->end   = (rdate_t){ 'M', 0, 4, 1, 0, 3 * 3600 };
    } else {
        return true;
    }
    r->has_dst = true;
    r->dst_off = std + 3600;
    return true;
}

/* ── Which zone, and keeping it fresh ─────────────────────────────── */

static zone_t cur;                 /* the zone in force */
static bool   cur_ok;
static char   cur_name[128];       /* "Asia/Seoul", or "" */
static char   cur_source[40];
static char   cur_key[512];        /* what was read, to notice a change */
static s64    cur_checked = -1;

void lp_tz_forget(void)
{
    cur_checked = -1;
    cur_key[0] = '\0';
}

const char *lp_tz_dir(void)
{
    const char *d = getenv("TZDIR");
    return (d && d[0] == '/') ? d : "/usr/share/zoneinfo";
}

/* A zone name somebody typed may name only a file inside the zone
 * directory: no absolute path, no "..", nothing hidden. */
static bool safe_name(const char *n)
{
    if (!n[0] || n[0] == '/' || strlen(n) > 100) return false;
    for (const char *p = n; *p; p++) {
        if (*p == '.' && (p == n || p[-1] == '/')) return false;
        if (!(isalpha_(*p) || isdig(*p) || *p == '/' || *p == '_' ||
              *p == '-' || *p == '+' || *p == '.'))
            return false;
    }
    return true;
}

static bool load_file(const char *path, zone_t *z)
{
    long len = 0;
    u8 *buf = slurp(path, &len);
    if (!buf) return false;
    bool ok = parse_tzif(buf, len, z);
    free(buf);
    return ok;
}

static bool load_named(const char *name, zone_t *z)
{
    if (!safe_name(name)) return false;
    char path[256];
    if (snprintf(path, sizeof path, "%s/%s", lp_tz_dir(), name) >= (int)sizeof path)
        return false;
    return load_file(path, z);
}

bool lp_tz_valid(const char *name)
{
    zone_t z;
    if (!load_named(name, &z)) return false;
    zone_free(&z);
    return true;
}

/* A zone made from a rule alone, so every lookup has one shape. */
static void zone_from_rule(zone_t *z, const prule_t *r)
{
    memset(z, 0, sizeof *z);
    z->has_rule = true;
    z->rule = *r;
}

/* The name inside the zone directory that a path names, or "". */
static void name_from_path(const char *path, char *out, size_t cap)
{
    const char *p = strstr(path, "zoneinfo/");
    out[0] = '\0';
    if (p) {
        p += 9;
        /* posix/ and right/ are the same zones under other rules. */
        if (strncmp(p, "posix/", 6) == 0) p += 6;
        strlcpy(out, p, cap);
    }
}

/* Read a small text file's first line. */
static bool read_line_file(const char *path, char *out, size_t cap)
{
    long fd = lp_open(path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return false;
    long n = lp_read((int)fd, out, cap - 1);
    lp_close((int)fd);
    if (n <= 0) return false;
    out[n] = '\0';
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    size_t l = strlen(out);
    while (l && (out[l - 1] == ' ' || out[l - 1] == '\t' || out[l - 1] == '\r'))
        out[--l] = '\0';
    return out[0] != '\0';
}

/* A timezone file's contents: a zone name, or the older numeric line. */
static bool load_setting(const char *text, zone_t *z, char *name, size_t cap)
{
    const char *s = text;
    while (*s == ' ' || *s == '\t') s++;
    if (isdig(*s) || ((*s == '-' || *s == '+') && isdig(s[1]))) {
        prule_t r;
        if (!parse_legacy(s, &r)) return false;
        zone_from_rule(z, &r);
        name[0] = '\0';
        return true;
    }
    if (!load_named(s, z)) return false;
    strlcpy(name, s, cap);
    return true;
}

/* Identity of a file for change detection: mtime, size, inode. */
static void file_key(const char *path, bool follow, char *out, size_t cap)
{
    lp_stat_t st;
    if (lp_stat(path, &st, follow) < 0) {
        snprintf(out, cap, "-");
        return;
    }
    snprintf(out, cap, "%lld.%u/%llu/%llu", (long long)st.mtime, st.mtime_ns,
             (unsigned long long)st.size, (unsigned long long)st.ino);
}

static s64 file_mtime(const char *path, bool follow)
{
    lp_stat_t st;
    if (lp_stat(path, &st, follow) < 0) return -1;
    return st.mtime * 1000000000LL + st.mtime_ns;
}

static void set_utc(const char *label)
{
    prule_t r;
    memset(&r, 0, sizeof r);
    strlcpy(r.std, label, sizeof r.std);
    zone_from_rule(&cur, &r);
}

/* TZ, as glibc reads it. */
static void load_from_env(const char *tz)
{
    strlcpy(cur_source, "TZ", sizeof cur_source);
    if (*tz == ':') tz++;
    /* glibc turns an empty TZ into "Universal" and reads that zone. */
    if (!*tz) tz = "Universal";

    if (tz[0] == '/') {
        if (load_file(tz, &cur)) {
            name_from_path(tz, cur_name, sizeof cur_name);
            return;
        }
    } else if (load_named(tz, &cur)) {
        strlcpy(cur_name, tz, sizeof cur_name);
        return;
    }
    prule_t r;
    if (parse_posix(tz, &r)) {
        zone_from_rule(&cur, &r);
        return;
    }
    set_utc("UTC");
}

/* The TZ the cache was built under. Compared on every call, not every
 * ten seconds: a program that does setenv("TZ", ...) - `date -u` does,
 * and `date -d 'TZ="Asia/Seoul" ...'` - means the very next call. */
static char   cur_env[128];
static bool   cur_env_set;

static void tz_load(void)
{
    s64 now = lp_monotonic_ms();
    const char *tz = getenv("TZ");
    bool same_env = tz ? (cur_env_set && strcmp(tz, cur_env) == 0) : !cur_env_set;
    if (same_env && cur_checked >= 0 && now - cur_checked < 10000)
        return;
    cur_checked = now;
    cur_env_set = tz != NULL;
    strlcpy(cur_env, tz ? tz : "", sizeof cur_env);

    /* What decides the zone, gathered cheaply, so an unchanged setting
     * costs three stats and no parsing. */
    char key[512], a[96], b[96], c[96], d[96];
    if (tz) {
        snprintf(key, sizeof key, "TZ=%s|%s", tz, lp_tz_dir());
    } else {
        file_key("/data/timezone", true, a, sizeof a);
        file_key("/etc/localtime", false, b, sizeof b);
        file_key("/etc/localtime", true, c, sizeof c);
        file_key("/etc/timezone", true, d, sizeof d);
        snprintf(key, sizeof key, "%s|%s|%s|%s|%s", a, b, c, d, lp_tz_dir());
    }
    if (cur_ok && strcmp(key, cur_key) == 0)
        return;
    strlcpy(cur_key, key, sizeof cur_key);

    zone_free(&cur);
    cur_ok = true;
    cur_name[0] = '\0';

    if (tz) {
        load_from_env(tz);
        return;
    }

    char text[128];
    bool have_data = read_line_file("/data/timezone", text, sizeof text);
    s64  data_mt   = have_data ? file_mtime("/data/timezone", true) : -1;
    s64  lt_mt     = file_mtime("/etc/localtime", false);

    /* /data/timezone when it is newer than /etc/localtime, or when there
     * is no /etc/localtime at all (a Pi). */
    if (have_data && (lt_mt < 0 || data_mt >= lt_mt) &&
        load_setting(text, &cur, cur_name, sizeof cur_name)) {
        strlcpy(cur_source, "/data/timezone", sizeof cur_source);
        return;
    }
    if (load_file("/etc/localtime", &cur)) {
        strlcpy(cur_source, "/etc/localtime", sizeof cur_source);
        char link[256];
        long n = lp_readlink("/etc/localtime", link, sizeof link - 1);
        if (n > 0) {
            link[n] = '\0';
            name_from_path(link, cur_name, sizeof cur_name);
        }
        /* A copied file rather than a link: /etc/timezone may say. */
        if (!cur_name[0] && read_line_file("/etc/timezone", text, sizeof text) &&
            safe_name(text))
            strlcpy(cur_name, text, sizeof cur_name);
        return;
    }
    if (have_data && load_setting(text, &cur, cur_name, sizeof cur_name)) {
        strlcpy(cur_source, "/data/timezone", sizeof cur_source);
        return;
    }
    if (read_line_file("/etc/timezone", text, sizeof text) &&
        load_setting(text, &cur, cur_name, sizeof cur_name)) {
        strlcpy(cur_source, "/etc/timezone", sizeof cur_source);
        return;
    }
    strlcpy(cur_source, "default", sizeof cur_source);
    strlcpy(cur_name, "UTC", sizeof cur_name);
    set_utc("UTC");
}

const char *lp_tz_name(void)   { tz_load(); return cur_name; }
const char *lp_tz_source(void) { tz_load(); return cur_source; }

static zinfo_t info_at(s64 t)
{
    tz_load();
    return zone_info(&cur, t);
}

int lp_tz_offset_sec(s64 utc)
{
    return (int)info_at(utc).off;
}

int lp_tz_offset(s64 utc)
{
    /* Toward zero, as %z prints it: local mean time in Caracas was
     * -4:27:44, which is -0427, not -0428. */
    return lp_tz_offset_sec(utc) / 60;
}

const char *lp_tz_label(s64 utc)
{
    /* A copy, so a reload ten seconds later cannot pull the text out
     * from under a caller still holding the pointer. */
    static char label[16];
    strlcpy(label, info_at(utc).abbr, sizeof label);
    return label;
}

static void fill_local(s64 t, zinfo_t zi, lp_tm_t *out)
{
    split_secs(t + zi.off, out);
    out->isdst  = zi.isdst ? 1 : 0;
    out->gmtoff = zi.off;
    strlcpy(out->zone, zi.abbr, sizeof out->zone);
}

void lp_localtime(s64 t, lp_tm_t *out)
{
    fill_local(t, info_at(t), out);
}

/* Local wall-clock seconds -> the instant. Every offset in force within
 * a day either side is a candidate; a candidate is right when the offset
 * at the instant it gives is itself. None right means the hour summer
 * time skipped; two means the hour that happened twice. */
static s64 from_local(s64 L, int isdst, zinfo_t *zi_out)
{
    tz_load();
    s32 cand[5];
    int nc = 0;
    for (int k = -2; k <= 2; k++) {
        s32 o = zone_info(&cur, L + (s64)k * 43200).off;
        bool seen = false;
        for (int i = 0; i < nc; i++) if (cand[i] == o) seen = true;
        if (!seen) cand[nc++] = o;
    }
    s64 best = 0;
    zinfo_t bi = { 0, false, "UTC" };
    int found = 0;
    for (int i = 0; i < nc; i++) {
        s64 t = L - cand[i];
        zinfo_t zi = zone_info(&cur, t);
        if (zi.off != cand[i]) continue;
        bool better;
        if (!found)                         better = true;
        else if (isdst >= 0 && (int)zi.isdst == isdst && (int)bi.isdst != isdst)
                                            better = true;
        else if (isdst >= 0 && (int)bi.isdst == isdst && (int)zi.isdst != isdst)
                                            better = false;
        else                                better = t < best;    /* the first */
        if (better) { best = t; bi = zi; }
        found++;
    }
    if (!found) {
        /* Skipped: read it with the offset from before the jump, which
         * lands the same distance past the jump - 02:30 on the morning
         * the clocks go forward is 03:30. */
        zinfo_t before = zone_info(&cur, L - 86400);
        best = L - before.off;
        bi = zone_info(&cur, best);
    }
    if (isdst >= 0 && (int)bi.isdst != isdst) {
        /* Summer time asked for in winter, or the reverse. glibc reads
         * the clock with the offset of the nearest time that does have
         * the flag asked for - so 11:28 "summer" in January is 10:28
         * winter - and `date -d '8 months ago'` depends on exactly that,
         * so it is copied: probes a week apart, nearest first, out to
         * seventeen years, as glibc's mktime does. */
        for (s64 k = 1; k <= 17 * 53; k++) {
            for (int dir = -1; dir <= 1; dir += 2) {
                zinfo_t p = zone_info(&cur, L - cand[0] + dir * k * 601200);
                if ((int)p.isdst == isdst) {
                    best = L - p.off;
                    bi = zone_info(&cur, best);
                    goto done;
                }
            }
        }
    }
done:
    if (zi_out) *zi_out = bi;
    return best;
}

s64 lp_timelocal(const lp_tm_t *tm)
{
    return from_local(lp_timegm(tm), -1, NULL);
}

s64 lp_mktime(lp_tm_t *tm)
{
    zinfo_t zi;
    s64 t = from_local(lp_timegm(tm), tm->isdst, &zi);
    fill_local(t, zi, tm);
    return t;
}

bool lp_tz_next_change(s64 t, s64 *when)
{
    tz_load();
    zinfo_t a = zone_info(&cur, t);
    /* Transitions in the table first; a binary search would do, but the
     * tables are a few hundred entries and this is asked once. */
    for (int i = 0; i < cur.nt; i++) {
        if (cur.at[i] > t) {
            zinfo_t b = zone_info(&cur, cur.at[i]);
            if (b.off != a.off || b.isdst != a.isdst) { *when = cur.at[i]; return true; }
        }
    }
    if (!cur.has_rule || !cur.rule.has_dst) return false;
    /* Past the table: walk the rule a day at a time, then narrow it to
     * the second. Two years is always enough to meet a change. */
    s64 lo = t, hi = t;
    for (int day = 1; day <= 800; day++) {
        hi = t + (s64)day * 86400;
        zinfo_t b = zone_info(&cur, hi);
        if (b.off != a.off || b.isdst != a.isdst) break;
        lo = hi;
        if (day == 800) return false;
    }
    while (hi - lo > 1) {
        s64 mid = lo + (hi - lo) / 2;
        zinfo_t b = zone_info(&cur, mid);
        if (b.off != a.off || b.isdst != a.isdst) hi = mid;
        else lo = mid;
    }
    *when = hi;
    return true;
}

/* ── Names ──────────────────────────────────────────────────────────── */

static const char *const DAY_AB[7]  = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
static const char *const DAY_FULL[7] = { "Sunday","Monday","Tuesday","Wednesday",
                                        "Thursday","Friday","Saturday" };
static const char *const MON_AB[12] = { "Jan","Feb","Mar","Apr","May","Jun",
                                        "Jul","Aug","Sep","Oct","Nov","Dec" };
static const char *const MON_FULL[12] = { "January","February","March","April",
                                          "May","June","July","August",
                                          "September","October","November",
                                          "December" };
/* glibc's ko_KR: one-syllable weekdays, and abbreviated months padded
 * to two columns so a column of them lines up. */
static const char *const KO_DAY_AB[7]  = { "일","월","화","수","목","금","토" };
static const char *const KO_DAY_FULL[7] = { "일요일","월요일","화요일","수요일",
                                           "목요일","금요일","토요일" };
static const char *const KO_MON_AB[12] = { " 1월"," 2월"," 3월"," 4월"," 5월",
                                           " 6월"," 7월"," 8월"," 9월","10월",
                                           "11월","12월" };
static const char *const KO_MON_FULL[12] = { "1월","2월","3월","4월","5월","6월",
                                             "7월","8월","9월","10월","11월",
                                             "12월" };

lp_lang_t lp_time_lang(void)
{
    const char *vars[3] = { "LC_ALL", "LC_TIME", "LANG" };
    for (int i = 0; i < 3; i++) {
        const char *v = getenv(vars[i]);
        if (v && *v)
            return (v[0] == 'k' && v[1] == 'o') ? LP_LANG_KO : LP_LANG_C;
    }
    return LP_LANG_C;
}

const char *lp_day_name(int wday, bool full, lp_lang_t lang)
{
    wday = ((wday % 7) + 7) % 7;
    if (lang == LP_LANG_KO) return full ? KO_DAY_FULL[wday] : KO_DAY_AB[wday];
    return full ? DAY_FULL[wday] : DAY_AB[wday];
}

const char *lp_month_name(int mon, bool full, lp_lang_t lang)
{
    if (mon < 1 || mon > 12) return "?";
    if (lang == LP_LANG_KO) return full ? KO_MON_FULL[mon - 1] : KO_MON_AB[mon - 1];
    return full ? MON_FULL[mon - 1] : MON_AB[mon - 1];
}

/* ── strftime ─────────────────────────────────────────────────────────
 *
 * Written to match gnulib's nstrftime, which is what `date` on Ubuntu
 * runs, and laid out the way it is - one pass over the format, numbers
 * through a single sign-and-padding step, subformats (%D %F %R %T) run
 * recursively with the flag handed down to the years only - because the
 * first version was a tidy switch that treated each conversion on its
 * own, and a differential test against GNU date found 2,900 cases where
 * the tidy version and GNU disagreed: %1z is "+900", %-:z is "+9:00",
 * %3% is " %3%", %-D is "02/14/9", %+3y is "+70". None of that is
 * sensible and all of it is what a script tested on Ubuntu relies on.
 *
 * Some conversions GNU does not do itself: coreutils hands %a %A %b %B
 * %c %p %r %x %X, and anything with an E or O modifier, to glibc's own
 * strftime and pads what comes back as text. glibc differs in small
 * ways - its %Y is not padded to four digits, so %c of year 1 ends in
 * "1" while %Y is "0001" - and glibc_conv below is that half.
 *
 * Widths count bytes, as GNU's do: %10A of 월요일 (nine bytes) is one
 * space and the word. yday and wday are recomputed from the date rather
 * than trusted, so a time built by hand with only year, month and day
 * still prints the right %j and %A.
 */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;         /* the length it would be, even past cap */
} out_t;

typedef struct {
    const lp_tm_t *tm;
    long      ns;
    lp_lang_t lang;
    int       wday, yday;
    const char *zone;
} tctx_t;

#define WIDTH_MAX 65535

static void put_c(out_t *o, char c)
{
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void put_n(out_t *o, char c, long n)
{
    while (n-- > 0) put_c(o, c);
}

static void put_s(out_t *o, const char *s)
{
    while (*s) put_c(o, *s++);
}

/* gnulib's width_add: the padding in front of n bytes. A '-' pad or no
 * width means none; '0' and '+' pad with zeros, the rest with spaces. */
static void pad_to(out_t *o, int width, int pad, size_t n)
{
    if (pad == '-' || width < 0 || n >= (size_t)width) return;
    put_n(o, (pad == '0' || pad == '+') ? '0' : ' ', (long)((size_t)width - n));
}

/* gnulib's cpy: padded bytes, lowercasing winning over uppercasing. */
static void cpy(out_t *o, int width, int pad, const char *s, size_t n,
                bool up, bool low)
{
    pad_to(o, width, pad, n);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (low && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        else if (!low && up && c >= 'a' && c <= 'z') c = (char)(c - 32);
        put_c(o, c);
    }
}

/* ISO 8601 week-based year and week number. */
static void iso_week(long year, int yday, int wday, long *wyear, int *week)
{
    int wd = (wday + 6) % 7;                       /* Monday = 0 */
    int w = (yday - wd + 10) / 7;
    long y = year;
    if (w < 1) {
        y--;
        int plen = lp_is_leap(y) ? 366 : 365;
        w = (yday + plen - wd + 10) / 7;
    } else {
        int len = lp_is_leap(y) ? 366 : 365;
        /* Dec 29-31 on a Monday-Wednesday belong to week 1 next year. */
        if (yday - wd + 3 >= len) { w = 1; y++; }
    }
    *wyear = y;
    *week  = w;
}

/* ── glibc's half ── */

static void glibc_fmt(out_t *o, const tctx_t *c, const char *f);

static void gnum(out_t *o, s64 v, int digits, char fill)
{
    char b[24];
    int n = 0;
    bool neg = v < 0;
    u64 a = neg ? (u64)0 - (u64)v : (u64)v;
    do { b[n++] = (char)('0' + a % 10); a /= 10; } while (a);
    if (neg) put_c(o, '-');
    put_n(o, fill, digits - n - (neg ? 1 : 0));
    while (n) put_c(o, b[--n]);
}

/* One conversion as glibc's strftime does it with no flags, in the C
 * locale or ko_KR. What glibc does not know it copies as written. */
static void glibc_conv(out_t *o, const tctx_t *c, int mod, int fc)
{
    const lp_tm_t *tm = c->tm;
    bool ko = c->lang == LP_LANG_KO;
    int hour12 = tm->hour % 12 ? tm->hour % 12 : 12;
    long wy; int wk;

    switch (fc) {
    case 'a': case 'A':
        if (mod) break;
        put_s(o, lp_day_name(c->wday, fc == 'A', c->lang));
        return;
    case 'b': case 'h': case 'B':
        if (mod == 'E') break;
        put_s(o, lp_month_name(tm->mon, fc == 'B', c->lang));
        return;
    case 'p':
        put_s(o, ko ? (tm->hour < 12 ? "오전" : "오후") : (tm->hour < 12 ? "AM" : "PM"));
        return;
    case 'c':
        if (mod == 'O') break;
        glibc_fmt(o, c, ko ? "%x (%a) %r" : "%a %b %e %H:%M:%S %Y");
        return;
    case 'x':
        if (mod == 'O') break;
        glibc_fmt(o, c, ko ? "%Y년 %m월 %d일" : "%m/%d/%y");
        return;
    case 'X':
        if (mod == 'O') break;
        glibc_fmt(o, c, ko ? "%H시 %M분 %S초" : "%H:%M:%S");
        return;
    case 'r':
        glibc_fmt(o, c, ko ? "%p %I시 %M분 %S초" : "%I:%M:%S %p");
        return;
    case 'C': gnum(o, fdiv(tm->year, 100), 1, '0'); return;
    case 'Y': if (mod == 'O') break; gnum(o, tm->year, 1, '0'); return;
    case 'y': gnum(o, fmod64(tm->year, 100), 2, '0'); return;
    case 'G': case 'g': case 'V':
        iso_week(tm->year, c->yday, c->wday, &wy, &wk);
        if (fc == 'G') gnum(o, wy, 1, '0');
        else if (fc == 'g') gnum(o, fmod64(wy, 100), 2, '0');
        else gnum(o, wk, 2, '0');
        return;
    case 'd': gnum(o, tm->day, 2, '0'); return;
    case 'e': gnum(o, tm->day, 2, ' '); return;
    case 'H': gnum(o, tm->hour, 2, '0'); return;
    case 'k': gnum(o, tm->hour, 2, ' '); return;
    case 'I': gnum(o, hour12, 2, '0'); return;
    case 'l': gnum(o, hour12, 2, ' '); return;
    case 'j': gnum(o, c->yday + 1, 3, '0'); return;
    case 'm': gnum(o, tm->mon, 2, '0'); return;
    case 'M': gnum(o, tm->min, 2, '0'); return;
    case 'S': gnum(o, tm->sec, 2, '0'); return;
    case 'u': gnum(o, c->wday ? c->wday : 7, 1, '0'); return;
    case 'w': gnum(o, c->wday, 1, '0'); return;
    case 'U': gnum(o, (c->yday - c->wday + 7) / 7, 2, '0'); return;
    case 'W': gnum(o, (c->yday - (c->wday + 6) % 7 + 7) / 7, 2, '0'); return;
    case 's': gnum(o, lp_timegm(tm) - tm->gmtoff, 1, '0'); return;
    case 'Z': put_s(o, c->zone); return;
    case 'z': {
        s32 off = tm->gmtoff;
        put_c(o, off < 0 ? '-' : '+');
        s32 a = off < 0 ? -off : off;
        gnum(o, (a / 3600) * 100 + a / 60 % 60, 4, '0');
        return;
    }
    case 'n': put_c(o, '\n'); return;
    case 't': put_c(o, '\t'); return;
    default: break;
    }
    put_c(o, '%');
    if (mod) put_c(o, (char)mod);
    put_c(o, (char)fc);
}

static void glibc_fmt(out_t *o, const tctx_t *c, const char *f)
{
    for (; *f; f++) {
        if (*f == '%' && f[1]) glibc_conv(o, c, 0, *++f);
        else put_c(o, *f);
    }
}

/* ── GNU's half ── */

typedef struct {
    int  pad, width, modifier, fc;
    bool up, low;
} spec_t;

/* Hand one conversion to "the underlying strftime" and pad the answer
 * as text, as gnulib does for everything locale-dependent. */
static void underlying(out_t *o, const tctx_t *c, const spec_t *s)
{
    char tmp[256];
    out_t t = { tmp, sizeof tmp, 0 };
    glibc_conv(&t, c, s->modifier, s->fc);
    size_t n = t.len < sizeof tmp ? t.len : sizeof tmp - 1;
    cpy(o, s->width, s->pad, tmp, n, s->up, s->low);
}

/* gnulib's do_number_sign_and_padding, on digits already written. */
static void sign_and_pad(out_t *o, const char *digs, int numlen, bool neg,
                         bool always_sign, int pad, int width, int digits)
{
    if (pad == 0) pad = '0';
    if (width < 0) width = digits;
    char sign = neg ? '-' : always_sign ? '+' : 0;
    int shortage = width - (sign ? 1 : 0) - numlen;
    int padding = (pad == '-' || shortage <= 0) ? 0 : shortage;
    if (sign) {
        if (pad == '_') {
            put_n(o, ' ', padding);
            width -= padding;
        }
        put_c(o, sign);
        width--;
    }
    cpy(o, width, pad, digs, (size_t)numlen, false, false);
}

/* gnulib's do_number_body. u is the value as an unsigned number and neg
 * says it was negative, exactly as gnulib carries it; colon_mask puts a
 * colon before the i'th digit from the right for each bit i. */
static void number(out_t *o, const tctx_t *c, const spec_t *s, u64 u,
                   bool neg, bool always_sign, int colon_mask, int digits)
{
    if (s->modifier == 'O' && !neg) { underlying(o, c, s); return; }
    char buf[48];
    char *b = buf + sizeof buf;
    if (neg) u = (u64)0 - u;
    do {
        if (colon_mask & 1) *--b = ':';
        colon_mask >>= 1;
        *--b = (char)('0' + u % 10);
        u /= 10;
    } while (u || colon_mask);
    sign_and_pad(o, b, (int)(buf + sizeof buf - b), neg, always_sign,
                 s->pad, s->width, digits);
}

static void do_num(out_t *o, const tctx_t *c, const spec_t *s, int digits, s64 v)
{
    number(o, c, s, (u64)v, v < 0, false, 0, digits);
}

static void do_spacepad(out_t *o, const tctx_t *c, spec_t *s, int digits, s64 v)
{
    if (s->pad == 0) s->pad = '_';
    do_num(o, c, s, digits, v);
}

static void do_yearish(out_t *o, const tctx_t *c, spec_t *s, int yr_spec,
                       int digits, bool neg, s64 v)
{
    if (s->pad == 0) s->pad = yr_spec;
    u64 u = (u64)v;
    bool always = s->pad == '+' &&
                  ((digits == 2 ? 99u : 9999u) < u || digits < s->width);
    number(o, c, s, u, neg, always, 0, digits);
}

static void gnu_fmt(out_t *o, const char *format, const tctx_t *c,
                    bool upcase, int yr_spec, int width);

/* %D %F %R %T: the subformat runs with the pad as its year flag and the
 * given width for its first conversion, and the whole of it is padded to
 * this conversion's width. */
static void subformat(out_t *o, const tctx_t *c, const spec_t *s,
                      const char *subfmt, int subwidth)
{
    out_t count = { NULL, 0, 0 };
    gnu_fmt(&count, subfmt, c, s->up, s->pad, subwidth);
    pad_to(o, s->width, s->pad, count.len);
    size_t start = o->len;
    gnu_fmt(o, subfmt, c, s->up, s->pad, subwidth);
    if (s->up)
        for (size_t i = start; i < o->len && i + 1 < o->cap; i++)
            if (o->buf[i] >= 'a' && o->buf[i] <= 'z') o->buf[i] = (char)(o->buf[i] - 32);
}

static void gnu_fmt(out_t *o, const char *format, const tctx_t *c,
                    bool upcase, int yr_spec, int width)
{
    const lp_tm_t *tm = c->tm;
    long year = tm->year;
    long tm_year = year - 1900;             /* gnulib works in these */
    int hour12 = tm->hour % 12 ? tm->hour % 12 : 12;

    for (const char *f = format; *f; width = -1, f++) {
        if (*f != '%') {
            pad_to(o, width, 0, 1);
            put_c(o, *f);
            continue;
        }
        const char *percent = f;
        spec_t s = { 0, width, 0, 0, upcase, false };
        bool change_case = false;

        for (;;) {
            char ch = *++f;
            if (ch == '_' || ch == '-' || ch == '+' || ch == '0') s.pad = ch;
            else if (ch == '^') s.up = true;
            else if (ch == '#') change_case = true;
            else break;
        }
        if (isdig(*f)) {
            s.width = 0;
            do {
                s.width = s.width * 10 + (*f - '0');
                if (s.width > WIDTH_MAX) s.width = WIDTH_MAX;
                f++;
            } while (isdig(*f));
        }
        if (*f == 'E' || *f == 'O') s.modifier = *f++;
        s.fc = *f;

        switch (s.fc) {
        case '%':
            if (f - 1 != percent) goto bad_percent;
            pad_to(o, s.width, s.pad, 1);
            put_c(o, '%');
            break;

        case 'a': case 'A':
            if (s.modifier) goto bad_format;
            if (change_case) { s.up = true; s.low = false; }
            underlying(o, c, &s);
            break;
        case 'b': case 'h':
            if (change_case) { s.up = true; s.low = false; }
            if (s.modifier == 'E') goto bad_format;
            underlying(o, c, &s);
            break;
        case 'B':
            if (s.modifier == 'E') goto bad_format;
            if (change_case) { s.up = true; s.low = false; }
            underlying(o, c, &s);
            break;
        case 'c': case 'x': case 'X':
            if (s.modifier == 'O') goto bad_format;
            underlying(o, c, &s);
            break;
        case 'r':
            underlying(o, c, &s);
            break;
        case 'P':
            s.low = true;
            s.fc = 'p';
            /* fall through */
        case 'p':
            if (change_case) { s.up = false; s.low = true; }
            underlying(o, c, &s);
            break;

        case 'C': {
            if (s.modifier == 'E') { underlying(o, c, &s); break; }
            bool negative_year = tm_year < -1900;
            bool zero_thru_1899 = !negative_year && tm_year < 0;
            s64 century = (tm_year - 99 * zero_thru_1899) / 100 + 19;
            do_yearish(o, c, &s, yr_spec, 2, negative_year, century);
            break;
        }
        case 'y': {
            if (s.modifier == 'E') { underlying(o, c, &s); break; }
            long yy = tm_year % 100;
            if (yy < 0) yy = tm_year < -1900 ? -yy : yy + 100;
            do_yearish(o, c, &s, yr_spec, 2, false, yy);
            break;
        }
        case 'Y':
            if (s.modifier == 'E') { underlying(o, c, &s); break; }
            if (s.modifier == 'O') goto bad_format;
            do_yearish(o, c, &s, yr_spec, 4, tm_year < -1900, year);
            break;

        case 'D':
            if (s.modifier) goto bad_format;
            subformat(o, c, &s, "%m/%d/%y", -1);
            break;
        case 'F': {
            if (s.modifier) goto bad_format;
            int subwidth;
            if (s.pad == 0 && s.width < 0) { s.pad = '+'; subwidth = 4; }
            else { subwidth = s.width - 6; if (subwidth < 0) subwidth = 0; }
            subformat(o, c, &s, "%Y-%m-%d", subwidth);
            break;
        }
        case 'R': subformat(o, c, &s, "%H:%M", -1); break;
        case 'T': subformat(o, c, &s, "%H:%M:%S", -1); break;

        case 'd': if (s.modifier == 'E') goto bad_format; do_num(o, c, &s, 2, tm->day); break;
        case 'e': if (s.modifier == 'E') goto bad_format; do_spacepad(o, c, &s, 2, tm->day); break;
        case 'H': if (s.modifier == 'E') goto bad_format; do_num(o, c, &s, 2, tm->hour); break;
        case 'I': if (s.modifier == 'E') goto bad_format; do_num(o, c, &s, 2, hour12); break;
        case 'k': if (s.modifier == 'E') goto bad_format; do_spacepad(o, c, &s, 2, tm->hour); break;
        case 'l': if (s.modifier == 'E') goto bad_format; do_spacepad(o, c, &s, 2, hour12); break;
        case 'j': if (s.modifier == 'E') goto bad_format; do_num(o, c, &s, 3, c->yday + 1); break;
        case 'M': if (s.modifier == 'E') goto bad_format; do_num(o, c, &s, 2, tm->min); break;
        case 'm': if (s.modifier == 'E') goto bad_format; do_num(o, c, &s, 2, tm->mon); break;
        case 'S': if (s.modifier == 'E') goto bad_format; do_num(o, c, &s, 2, tm->sec); break;
        case 'q': do_num(o, c, &s, 1, ((tm->mon - 1) * 11 >> 5) + 1); break;
        case 'u': do_num(o, c, &s, 1, c->wday ? c->wday : 7); break;
        case 'U':
            if (s.modifier == 'E') goto bad_format;
            do_num(o, c, &s, 2, (c->yday - c->wday + 7) / 7);
            break;
        case 'W':
            if (s.modifier == 'E') goto bad_format;
            do_num(o, c, &s, 2, (c->yday - (c->wday + 6) % 7 + 7) / 7);
            break;
        case 'w': if (s.modifier == 'E') goto bad_format; do_num(o, c, &s, 1, c->wday); break;
        case 'V': case 'g': case 'G': {
            if (s.modifier == 'E') goto bad_format;
            long wy; int wk;
            iso_week(year, c->yday, c->wday, &wy, &wk);
            int adj = (int)(wy - year);
            if (s.fc == 'V') { do_num(o, c, &s, 2, wk); break; }
            if (s.fc == 'G') {
                do_yearish(o, c, &s, yr_spec, 4, tm_year < -1900 - adj, wy);
                break;
            }
            long yy = (tm_year % 100 + adj) % 100;
            do_yearish(o, c, &s, yr_spec, 2, false,
                       yy >= 0 ? yy : tm_year < -1900 - adj ? -yy : yy + 100);
            break;
        }

        case 's': {
            s64 t = lp_timegm(tm) - tm->gmtoff;
            char buf[24];
            char *b = buf + sizeof buf;
            bool neg = t < 0;
            do {
                int d = (int)(t % 10);
                t /= 10;
                *--b = (char)((neg ? -d : d) + '0');
            } while (t != 0);
            sign_and_pad(o, b, (int)(buf + sizeof buf - b), neg, false,
                         s.pad, s.width, 1);
            break;
        }

        case 'N': {
            /* The width is how many digits, cut rather than rounded; the
             * digits' trailing zeros become padding, which '-' drops -
             * but only under an explicit width: a bare %-N is all nine,
             * as GNU prints it. */
            if (s.modifier == 'E') goto bad_format;
            long n = c->ns < 0 ? 0 : c->ns % 1000000000L;
            bool keep = s.pad == '-' && s.width <= 0;
            int w = s.width <= 0 ? 9 : s.width;
            int ndigs = 9;
            char buf[9];
            while (w < ndigs || (!keep && 1 < ndigs && n % 10 == 0)) { ndigs--; n /= 10; }
            for (int j = ndigs; j > 0; j--) { buf[j - 1] = (char)('0' + n % 10); n /= 10; }
            int pad = s.pad ? s.pad : '0';
            cpy(o, 0, pad, buf, (size_t)ndigs, false, false);
            pad_to(o, w - ndigs, pad, 0);
            break;
        }

        case 'n':
            pad_to(o, s.width, s.pad, 1);
            put_c(o, '\n');
            break;
        case 't':
            pad_to(o, s.width, s.pad, 1);
            put_c(o, '\t');
            break;

        case 'Z':
            if (change_case) { s.up = false; s.low = true; }
            cpy(o, s.width, s.pad, c->zone, strlen(c->zone), s.up, s.low);
            break;

        case ':':
        case 'z': {
            int colons = 0;
            if (s.fc == ':') {
                for (colons = 1; f[colons] == ':'; colons++) {}
                if (f[colons] != 'z') goto bad_format;
                f += colons;
            }
            if (tm->isdst < 0) break;
            s32 diff = tm->gmtoff;
            /* "-00" is tzdata's "offset unknown" (RFC 3339's -00:00),
             * and GNU prints its zero with a minus to keep that. */
            bool neg = diff < 0 || (diff == 0 && c->zone[0] == '-');
            s32 hd = diff / 3600, md = diff / 60 % 60, sd = diff % 60;
            if (colons == 3) colons = sd ? 2 : md ? 1 : 3;
            if (colons == 0)
                number(o, c, &s, (u64)(s64)(hd * 100 + md), neg, true, 0, 5);
            else if (colons == 1)
                number(o, c, &s, (u64)(s64)(hd * 100 + md), neg, true, 04, 6);
            else if (colons == 2)
                number(o, c, &s, (u64)(s64)(hd * 10000 + md * 100 + sd), neg, true, 024, 9);
            else if (colons == 3)
                number(o, c, &s, (u64)(s64)hd, neg, true, 0, 3);
            else
                goto bad_format;
            break;
        }

        case '\0':
        bad_percent:
            f--;
            /* fall through */
        default:
        bad_format:
            /* Not a conversion: copied as written, padded to its width. */
            cpy(o, s.width, s.pad, percent, (size_t)(f - percent + 1), false, false);
            break;
        }
    }
}

size_t lp_strftime_lang(char *buf, size_t n, const char *f,
                        const lp_tm_t *tm, long ns, lp_lang_t lang)
{
    s64 days = lp_days_from_civil(tm->year, tm->mon, tm->day);
    tctx_t c = {
        .tm = tm, .ns = ns, .lang = lang,
        .wday = (int)fmod64(days + 4, 7),
        .yday = (int)(days - lp_days_from_civil(tm->year, 1, 1)),
        .zone = tm->zone[0] ? tm->zone : "UTC",
    };
    out_t o = { buf, n, 0 };
    gnu_fmt(&o, f, &c, false, 0, -1);
    if (n) buf[o.len < n ? o.len : n - 1] = '\0';
    return o.len;
}

size_t lp_strftime(char *buf, size_t n, const char *f, const lp_tm_t *tm)
{
    return lp_strftime_lang(buf, n, f, tm, 0, LP_LANG_C);
}
