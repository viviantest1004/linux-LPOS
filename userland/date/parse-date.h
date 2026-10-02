/* parse-date.h - the GNU date string parser, shared by date -d and
 * touch -d. It was date.c's own; touch needs the same reading of
 * "2026-09-28 14:00", "@1790000000", "yesterday", "3 days ago" and
 * the rest, so that `touch -d X f` means what `date -d X` shows.
 * Static functions only, included by one .c file each. Needs types.h,
 * string.h, stdlib.h and unistd.h (for tz.h) before it. */
#ifndef LP_PARSE_DATE_H
#define LP_PARSE_DATE_H

/* ══ The -d parser ═══════════════════════════════════════════════════ */

enum {
    T_END = 256, T_UNUM, T_SNUM, T_UDEC, T_SDEC, T_MONTH, T_WDAY, T_MERID,
    T_DST, T_ZONE, T_DAYZONE, T_LOCALZONE, T_YEAR_U, T_MONTH_U, T_DAY_U,
    T_HOUR_U, T_MIN_U, T_SEC_U, T_DAYSHIFT, T_ORDINAL, T_AGO, T_BAD
};

typedef struct {
    int  type;
    s64  val;          /* numbers; for words, the table value */
    int  digits;       /* numbers: how many digits were written */
    long ns;           /* decimals: the fraction, in nanoseconds */
} tok_t;

#define MAXTOK 64

typedef struct { s64 year, month, day, hour, min, sec, ns; } rel_t;

typedef struct {
    tok_t  t[MAXTOK];
    int    n, i;
    /* what was said */
    s64    year; int year_digits;
    s64    month, day;
    s64    hour, min, sec; long ns;
    int    merid;                    /* 0 24h, 1 am, 2 pm */
    int    day_ordinal, day_number;
    s64    time_zone;                /* minutes east */
    int    local_isdst;
    rel_t  rel;
    int    times, dates, days, zones, local_zones, dsts;
    bool   rels, timespec;
    s64    epoch; long epoch_ns;
    /* the local zone's own abbreviations, which mean "this zone, summer
     * or winter" rather than a fixed offset */
    char   lz_name[2][16];
    int    lz_isdst[2];
    int    nlz;
} pc_t;

#define HOUR(x) ((x) * 60)

typedef struct { const char *name; int type; int val; } word_t;

static const word_t MONTH_DAY[] = {
    { "JANUARY", T_MONTH, 1 }, { "FEBRUARY", T_MONTH, 2 }, { "MARCH", T_MONTH, 3 },
    { "APRIL", T_MONTH, 4 }, { "MAY", T_MONTH, 5 }, { "JUNE", T_MONTH, 6 },
    { "JULY", T_MONTH, 7 }, { "AUGUST", T_MONTH, 8 }, { "SEPTEMBER", T_MONTH, 9 },
    { "SEPT", T_MONTH, 9 }, { "OCTOBER", T_MONTH, 10 }, { "NOVEMBER", T_MONTH, 11 },
    { "DECEMBER", T_MONTH, 12 },
    { "SUNDAY", T_WDAY, 0 }, { "MONDAY", T_WDAY, 1 }, { "TUESDAY", T_WDAY, 2 },
    { "TUES", T_WDAY, 2 }, { "WEDNESDAY", T_WDAY, 3 }, { "WEDNES", T_WDAY, 3 },
    { "THURSDAY", T_WDAY, 4 }, { "THUR", T_WDAY, 4 }, { "THURS", T_WDAY, 4 },
    { "FRIDAY", T_WDAY, 5 }, { "SATURDAY", T_WDAY, 6 }, { NULL, 0, 0 }
};

static const word_t UNITS[] = {
    { "YEAR", T_YEAR_U, 1 }, { "MONTH", T_MONTH_U, 1 }, { "FORTNIGHT", T_DAY_U, 14 },
    { "WEEK", T_DAY_U, 7 }, { "DAY", T_DAY_U, 1 }, { "HOUR", T_HOUR_U, 1 },
    { "MINUTE", T_MIN_U, 1 }, { "MIN", T_MIN_U, 1 }, { "SECOND", T_SEC_U, 1 },
    { "SEC", T_SEC_U, 1 }, { NULL, 0, 0 }
};

static const word_t RELATIVE[] = {
    { "TOMORROW", T_DAYSHIFT, 1 }, { "YESTERDAY", T_DAYSHIFT, -1 },
    { "TODAY", T_DAYSHIFT, 0 }, { "NOW", T_DAYSHIFT, 0 },
    { "LAST", T_ORDINAL, -1 }, { "THIS", T_ORDINAL, 0 }, { "NEXT", T_ORDINAL, 1 },
    { "FIRST", T_ORDINAL, 1 }, { "THIRD", T_ORDINAL, 3 }, { "FOURTH", T_ORDINAL, 4 },
    { "FIFTH", T_ORDINAL, 5 }, { "SIXTH", T_ORDINAL, 6 }, { "SEVENTH", T_ORDINAL, 7 },
    { "EIGHTH", T_ORDINAL, 8 }, { "NINTH", T_ORDINAL, 9 }, { "TENTH", T_ORDINAL, 10 },
    { "ELEVENTH", T_ORDINAL, 11 }, { "TWELFTH", T_ORDINAL, 12 },
    { "AGO", T_AGO, -1 }, { "HENCE", T_AGO, 1 }, { NULL, 0, 0 }
};

static const word_t UNIVERSAL[] = {
    { "GMT", T_ZONE, 0 }, { "UT", T_ZONE, 0 }, { "UTC", T_ZONE, 0 }, { NULL, 0, 0 }
};

/* gnulib's list, which is deliberately short: an abbreviation that
 * means two places (IST is India, Ireland and Israel) is read as the one
 * GNU reads it as, and one it does not list is refused. */
static const word_t ZONES[] = {
    { "WET", T_ZONE, HOUR(0) }, { "WEST", T_DAYZONE, HOUR(0) },
    { "BST", T_DAYZONE, HOUR(0) }, { "ART", T_ZONE, -HOUR(3) },
    { "BRT", T_ZONE, -HOUR(3) }, { "BRST", T_DAYZONE, -HOUR(3) },
    { "NST", T_ZONE, -(HOUR(3) + 30) }, { "NDT", T_DAYZONE, -(HOUR(3) + 30) },
    { "AST", T_ZONE, -HOUR(4) }, { "ADT", T_DAYZONE, -HOUR(4) },
    { "CLT", T_ZONE, -HOUR(4) }, { "CLST", T_DAYZONE, -HOUR(4) },
    { "EST", T_ZONE, -HOUR(5) }, { "EDT", T_DAYZONE, -HOUR(5) },
    { "CST", T_ZONE, -HOUR(6) }, { "CDT", T_DAYZONE, -HOUR(6) },
    { "MST", T_ZONE, -HOUR(7) }, { "MDT", T_DAYZONE, -HOUR(7) },
    { "PST", T_ZONE, -HOUR(8) }, { "PDT", T_DAYZONE, -HOUR(8) },
    { "AKST", T_ZONE, -HOUR(9) }, { "AKDT", T_DAYZONE, -HOUR(9) },
    { "HST", T_ZONE, -HOUR(10) }, { "HAST", T_ZONE, -HOUR(10) },
    { "HADT", T_DAYZONE, -HOUR(10) }, { "SST", T_ZONE, -HOUR(12) },
    { "WAT", T_ZONE, HOUR(1) }, { "CET", T_ZONE, HOUR(1) },
    { "CEST", T_DAYZONE, HOUR(1) }, { "MET", T_ZONE, HOUR(1) },
    { "MEZ", T_ZONE, HOUR(1) }, { "MEST", T_DAYZONE, HOUR(1) },
    { "MESZ", T_DAYZONE, HOUR(1) }, { "EET", T_ZONE, HOUR(2) },
    { "EEST", T_DAYZONE, HOUR(2) }, { "CAT", T_ZONE, HOUR(2) },
    { "SAST", T_ZONE, HOUR(2) }, { "EAT", T_ZONE, HOUR(3) },
    { "MSK", T_ZONE, HOUR(3) }, { "MSD", T_DAYZONE, HOUR(3) },
    { "IST", T_ZONE, HOUR(5) + 30 }, { "SGT", T_ZONE, HOUR(8) },
    { "KST", T_ZONE, HOUR(9) }, { "JST", T_ZONE, HOUR(9) },
    { "GST", T_ZONE, HOUR(10) }, { "NZST", T_ZONE, HOUR(12) },
    { "NZDT", T_DAYZONE, HOUR(12) }, { NULL, 0, 0 }
};

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                                      c == '\f' || c == '\v'; }

static const word_t *find_zone(const pc_t *pc, const char *w, word_t *tmp)
{
    for (const word_t *z = UNIVERSAL; z->name; z++)
        if (strcmp(w, z->name) == 0) return z;
    /* The local zone's own names come before the fixed table: in Seoul,
     * KST is "local time", not "+09:00 regardless". */
    for (int i = 0; i < pc->nlz; i++)
        if (strcmp(w, pc->lz_name[i]) == 0) {
            tmp->name = pc->lz_name[i];
            tmp->type = T_LOCALZONE;
            tmp->val  = pc->lz_isdst[i];
            return tmp;
        }
    for (const word_t *z = ZONES; z->name; z++)
        if (strcmp(w, z->name) == 0) return z;
    return NULL;
}

/* gnulib's lookup_word, in its order. */
static bool lookup_word(const pc_t *pc, char *w, tok_t *t)
{
    word_t tmp;
    size_t len = strlen(w);
    for (char *p = w; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);

    if (strcmp(w, "AM") == 0 || strcmp(w, "A.M.") == 0) { t->type = T_MERID; t->val = 1; return true; }
    if (strcmp(w, "PM") == 0 || strcmp(w, "P.M.") == 0) { t->type = T_MERID; t->val = 2; return true; }

    bool abbrev = len == 3 || (len == 4 && w[3] == '.');
    for (const word_t *e = MONTH_DAY; e->name; e++)
        if (abbrev ? strncmp(w, e->name, 3) == 0 : strcmp(w, e->name) == 0) {
            t->type = e->type; t->val = e->val; return true;
        }
    const word_t *z = find_zone(pc, w, &tmp);
    if (z) { t->type = z->type; t->val = z->val; return true; }
    if (strcmp(w, "DST") == 0) { t->type = T_DST; t->val = 0; return true; }
    for (const word_t *e = UNITS; e->name; e++)
        if (strcmp(w, e->name) == 0) { t->type = e->type; t->val = e->val; return true; }
    if (len > 1 && w[len - 1] == 'S') {
        w[len - 1] = '\0';
        for (const word_t *e = UNITS; e->name; e++)
            if (strcmp(w, e->name) == 0) {
                t->type = e->type; t->val = e->val; w[len - 1] = 'S'; return true;
            }
        w[len - 1] = 'S';
    }
    for (const word_t *e = RELATIVE; e->name; e++)
        if (strcmp(w, e->name) == 0) { t->type = e->type; t->val = e->val; return true; }

    /* Military zones: A-I +1..+9, K-M +10..+12, N-Y -1..-12, Z UTC. T is
     * both a zone and the separator in 2026-09-26T12:00, and the parser
     * tells which; J is "local time" and changes nothing. */
    if (len == 1) {
        char c = w[0];
        if (c == 'T') { t->type = 'T'; t->val = 0; return true; }
        if (c == 'J') { t->type = T_LOCALZONE; t->val = -1; return true; }
        if (c == 'Z') { t->type = T_ZONE; t->val = 0; return true; }
        if (c >= 'A' && c <= 'I') { t->type = T_ZONE; t->val = HOUR(c - 'A' + 1); return true; }
        if (c >= 'K' && c <= 'M') { t->type = T_ZONE; t->val = HOUR(c - 'K' + 10); return true; }
        if (c >= 'N' && c <= 'Y') { t->type = T_ZONE; t->val = -HOUR(c - 'N' + 1); return true; }
    }
    /* "E.S.T." */
    char q[32];
    size_t k = 0;
    bool period = false;
    for (const char *p = w; *p && k < sizeof q - 1; p++) {
        if (*p == '.') period = true;
        else q[k++] = *p;
    }
    q[k] = '\0';
    if (period && (z = find_zone(pc, q, &tmp))) { t->type = z->type; t->val = z->val; return true; }
    return false;
}

/* gnulib's yylex. A sign followed by anything but a digit is dropped,
 * which is why "tomorrow + 2 hours" works; parentheses are comments. */
static bool lex(pc_t *pc, const char *s)
{
    pc->n = 0;
    for (;;) {
        while (is_space(*s)) s++;
        if (pc->n >= MAXTOK - 1) return false;
        tok_t *t = &pc->t[pc->n];
        memset(t, 0, sizeof *t);
        char c = *s;
        if (!c) { t->type = T_END; pc->n++; return true; }

        if (is_digit(c) || c == '-' || c == '+') {
            int sign = 0;
            if (c == '-' || c == '+') {
                sign = c == '-' ? -1 : 1;
                s++;
                while (is_space(*s)) s++;
                if (!is_digit(*s)) continue;
            }
            u64 v = 0;
            int digits = 0;
            while (is_digit(*s)) {
                if (v > 922337203685477580ULL) return false;
                v = v * 10 + (u64)(*s++ - '0');
                digits++;
            }
            if ((*s == '.' || *s == ',') && is_digit(s[1])) {
                s++;
                long ns = 0;
                int nd = 0;
                bool rest = false;
                while (is_digit(*s)) {
                    if (nd < 9) { ns = ns * 10 + (*s - '0'); nd++; }
                    else if (*s != '0') rest = true;
                    s++;
                }
                while (nd < 9) { ns *= 10; nd++; }
                s64 sec = (s64)v;
                /* A negative fraction rounds toward minus infinity, so
                 * -1.5 is -2 seconds plus half a second. */
                if (sign < 0) {
                    sec = -sec;
                    if (ns || rest) { sec--; ns = 1000000000L - ns - (rest ? 1 : 0); }
                }
                t->type = sign ? T_SDEC : T_UDEC;
                t->val = sec;
                t->ns = ns;
            } else {
                t->type = sign ? T_SNUM : T_UNUM;
                t->val = sign < 0 ? -(s64)v : (s64)v;
                t->digits = digits;
            }
            pc->n++;
            continue;
        }
        if (is_alpha(c)) {
            char w[24];
            size_t k = 0;
            while (is_alpha(*s) || *s == '.') {
                if (k < sizeof w - 1) w[k++] = *s;
                s++;
            }
            w[k] = '\0';
            if (!lookup_word(pc, w, t)) return false;
            pc->n++;
            continue;
        }
        if (c == '(') {
            int depth = 0;
            do {
                c = *s++;
                if (!c) { t->type = T_END; pc->n++; return true; }
                if (c == '(') depth++;
                else if (c == ')') depth--;
            } while (depth > 0);
            continue;
        }
        t->type = (unsigned char)c;
        s++;
        pc->n++;
    }
}

static tok_t *cur(pc_t *pc)           { return &pc->t[pc->i]; }
static tok_t *peek(pc_t *pc, int k)
{
    int j = pc->i + k;
    return &pc->t[j < pc->n ? j : pc->n - 1];
}
static bool is_unit(int ty)
{
    return ty == T_YEAR_U || ty == T_MONTH_U || ty == T_DAY_U ||
           ty == T_HOUR_U || ty == T_MIN_U || ty == T_SEC_U;
}

static void add_rel(pc_t *pc, int unit, s64 n, s64 unitval)
{
    s64 v = n * unitval;
    switch (unit) {
    case T_YEAR_U:  pc->rel.year  += v; break;
    case T_MONTH_U: pc->rel.month += v; break;
    case T_DAY_U:   pc->rel.day   += v; break;
    case T_HOUR_U:  pc->rel.hour  += v; break;
    case T_MIN_U:   pc->rel.min   += v; break;
    case T_SEC_U:   pc->rel.sec   += v; break;
    }
    pc->rels = true;
}

/* A relative item ends here: "ago" and "hence" scale everything it
 * added. */
static void maybe_ago(pc_t *pc, rel_t before)
{
    if (cur(pc)->type != T_AGO) return;
    s64 f = cur(pc)->val;
    pc->i++;
    rel_t *r = &pc->rel;
    r->year  = before.year  + (r->year  - before.year)  * f;
    r->month = before.month + (r->month - before.month) * f;
    r->day   = before.day   + (r->day   - before.day)   * f;
    r->hour  = before.hour  + (r->hour  - before.hour)  * f;
    r->min   = before.min   + (r->min   - before.min)   * f;
    r->sec   = before.sec   + (r->sec   - before.sec)   * f;
    r->ns    = before.ns    + (r->ns    - before.ns)    * f;
}

/* "+0900", "-5", "+05:30": an offset in minutes, or false. */
static bool zone_hhmm(pc_t *pc, tok_t *s, s64 *out)
{
    s64 m = -1;
    if (cur(pc)->type == ':' && peek(pc, 1)->type == T_UNUM) {
        m = peek(pc, 1)->val;
        pc->i += 2;
    }
    s64 v = s->val < 0 ? -s->val : s->val;
    s64 n;
    if (m < 0) n = s->digits <= 2 ? v * 60 : (v / 100) * 60 + v % 100;
    else       n = v * 60 + m;
    if (s->val < 0) n = -n;
    if (n < -24 * 60 || n > 24 * 60) return false;
    *out = n;
    return true;
}

static void set_time(pc_t *pc, s64 h, s64 m, s64 sec, long ns, int merid)
{
    pc->hour = h; pc->min = m; pc->sec = sec; pc->ns = ns;
    pc->merid = merid;
    pc->times++;
}

/* hh:mm[:ss[.frac]] after the first number, then a meridian or a zone
 * offset. */
static bool p_clock(pc_t *pc, s64 h)
{
    s64 m = 0, sec = 0;
    long ns = 0;
    if (cur(pc)->type == ':' ) {
        if (peek(pc, 1)->type != T_UNUM) return false;
        m = peek(pc, 1)->val;
        pc->i += 2;
        if (cur(pc)->type == ':') {
            tok_t *s = peek(pc, 1);
            if (s->type == T_UNUM)      { sec = s->val; }
            else if (s->type == T_UDEC) { sec = s->val; ns = s->ns; }
            else return false;
            pc->i += 2;
        }
    }
    if (cur(pc)->type == T_MERID) {
        set_time(pc, h, m, sec, ns, (int)cur(pc)->val);
        pc->i++;
        return true;
    }
    set_time(pc, h, m, sec, ns, 0);
    if (cur(pc)->type == T_SNUM) {
        tok_t *s = cur(pc);
        pc->i++;
        s64 z;
        if (!zone_hhmm(pc, s, &z)) return false;
        pc->time_zone = z;
        pc->zones++;
    }
    return true;
}

/* The bare number: a year, a yyyymmdd date, or an hhmm time, depending
 * on what came before it and how many digits it has. */
static void p_number(pc_t *pc, tok_t *t)
{
    if (pc->dates && !pc->year_digits && !pc->rels && (pc->times || t->digits > 2)) {
        pc->year = t->val;
        pc->year_digits = t->digits;
    } else if (t->digits > 4) {
        pc->dates++;
        pc->day = t->val % 100;
        pc->month = (t->val / 100) % 100;
        pc->year = t->val / 10000;
        pc->year_digits = t->digits - 4;
    } else {
        pc->times++;
        if (t->digits <= 2) { pc->hour = t->val; pc->min = 0; }
        else { pc->hour = t->val / 100; pc->min = t->val % 100; }
        pc->sec = 0; pc->ns = 0;
        pc->merid = 0;
    }
}

static bool p_item(pc_t *pc)
{
    tok_t *t = cur(pc);
    tok_t *n1 = peek(pc, 1);
    rel_t before = pc->rel;

    switch (t->type) {
    case T_UNUM:
        /* iso date: 2026 -09 -26, perhaps followed by T and a time */
        if (n1->type == T_SNUM && peek(pc, 2)->type == T_SNUM) {
            pc->year = t->val; pc->year_digits = t->digits;
            pc->month = -n1->val; pc->day = -peek(pc, 2)->val;
            pc->dates++;
            pc->i += 3;
            /* A T straight after an ISO date commits to a date-time, as
             * in GNU's grammar: "2026-09-26T12" is refused rather than
             * read as noon in military zone T. */
            if (cur(pc)->type == 'T') {
                if (peek(pc, 1)->type != T_UNUM ||
                    (peek(pc, 2)->type != ':' && peek(pc, 2)->type != T_SNUM))
                    return false;
                s64 h = peek(pc, 1)->val;
                pc->i += 2;
                if (cur(pc)->type == T_SNUM) {           /* 12+09 */
                    tok_t *s = cur(pc);
                    pc->i++;
                    set_time(pc, h, 0, 0, 0, 0);
                    s64 z;
                    if (!zone_hhmm(pc, s, &z)) return false;
                    pc->time_zone = z;
                    pc->zones++;
                    return true;
                }
                return p_clock(pc, h);
            }
            return true;
        }
        if (n1->type == ':') { pc->i++; return p_clock(pc, t->val); }
        if (n1->type == T_MERID) {
            set_time(pc, t->val, 0, 0, 0, (int)n1->val);
            pc->i += 2;
            return true;
        }
        if (n1->type == T_SNUM) {
            if (is_unit(peek(pc, 2)->type)) {
                /* hybrid: "10 -2 hours" is 10:00, then two hours back */
                pc->i++;
                p_number(pc, t);
                return true;
            }
            /* iso time with an offset: "12 +0900" */
            pc->i += 2;
            set_time(pc, t->val, 0, 0, 0, 0);
            s64 z;
            if (!zone_hhmm(pc, n1, &z)) return false;
            pc->time_zone = z;
            pc->zones++;
            return true;
        }
        if (n1->type == T_MONTH) {                    /* 22 Nov [2025] */
            pc->day = t->val; pc->month = n1->val;
            pc->dates++;
            pc->i += 2;
            tok_t *y = cur(pc);
            if (y->type == T_SNUM) { pc->year = -y->val; pc->year_digits = y->digits; pc->i++; }
            else if (y->type == T_UNUM) { pc->year = y->val; pc->year_digits = y->digits; pc->i++; }
            return true;
        }
        if (n1->type == '/' && peek(pc, 2)->type == T_UNUM) {
            tok_t *b = peek(pc, 2);
            pc->i += 3;
            pc->dates++;
            if (cur(pc)->type == '/' && peek(pc, 1)->type == T_UNUM) {
                tok_t *c = peek(pc, 1);
                pc->i += 2;
                if (t->digits >= 4) {
                    pc->year = t->val; pc->year_digits = t->digits;
                    pc->month = b->val; pc->day = c->val;
                } else {
                    pc->month = t->val; pc->day = b->val;
                    pc->year = c->val; pc->year_digits = c->digits;
                }
            } else {
                pc->month = t->val; pc->day = b->val;
            }
            return true;
        }
        if (n1->type == T_WDAY) {                     /* "3 friday" */
            pc->day_ordinal = (int)t->val; pc->day_number = (int)n1->val;
            pc->days++;
            pc->i += 2;
            return true;
        }
        if (is_unit(n1->type)) {
            add_rel(pc, n1->type, t->val, n1->val);
            pc->i += 2;
            maybe_ago(pc, before);
            return true;
        }
        pc->i++;
        p_number(pc, t);
        return true;

    case T_SNUM:
        if (!is_unit(n1->type)) return false;
        add_rel(pc, n1->type, t->val, n1->val);
        pc->i += 2;
        maybe_ago(pc, before);
        return true;

    case T_UDEC: case T_SDEC:
        if (n1->type != T_SEC_U) return false;
        pc->rel.sec += t->val;
        pc->rel.ns  += t->ns;
        pc->rels = true;
        pc->i += 2;
        maybe_ago(pc, before);
        return true;

    case T_MONTH: {
        pc->month = t->val;
        pc->dates++;
        pc->i++;
        tok_t *a = cur(pc), *b = peek(pc, 1);
        if (a->type == T_SNUM && b->type == T_SNUM) {  /* Nov-22-2025 */
            pc->day = -a->val;
            pc->year = -b->val; pc->year_digits = b->digits;
            pc->i += 2;
        } else if (a->type == T_UNUM) {
            pc->day = a->val;
            pc->i++;
            if (cur(pc)->type == ',' && peek(pc, 1)->type == T_UNUM) {
                pc->year = peek(pc, 1)->val; pc->year_digits = peek(pc, 1)->digits;
                pc->i += 2;
            }
        } else {
            return false;
        }
        return true;
    }

    case T_WDAY:
        pc->day_ordinal = 0; pc->day_number = (int)t->val;
        pc->days++;
        pc->i++;
        if (cur(pc)->type == ',') pc->i++;
        return true;

    case T_ORDINAL:
        if (n1->type == T_WDAY) {
            pc->day_ordinal = (int)t->val; pc->day_number = (int)n1->val;
            pc->days++;
            pc->i += 2;
            return true;
        }
        if (is_unit(n1->type)) {
            add_rel(pc, n1->type, t->val, n1->val);
            pc->i += 2;
            maybe_ago(pc, before);
            return true;
        }
        return false;

    case T_YEAR_U: case T_MONTH_U: case T_DAY_U:
    case T_HOUR_U: case T_MIN_U: case T_SEC_U:
        add_rel(pc, t->type, 1, t->val);
        pc->i++;
        maybe_ago(pc, before);
        return true;

    case T_DAYSHIFT:
        pc->rel.day += t->val;
        pc->rels = true;
        pc->i++;
        return true;

    case T_ZONE:
        pc->i++;
        pc->zones++;
        pc->time_zone = t->val;
        if (cur(pc)->type == T_DST) { pc->time_zone += 60; pc->i++; return true; }
        if (cur(pc)->type == T_SNUM) {
            tok_t *s = cur(pc);
            if (is_unit(peek(pc, 1)->type)) {          /* "UTC -5 hours" */
                add_rel(pc, peek(pc, 1)->type, s->val, peek(pc, 1)->val);
                pc->i += 2;
                maybe_ago(pc, before);
                return true;
            }
            pc->i++;                                   /* "UTC+9", "GMT-05:00" */
            s64 z;
            if (!zone_hhmm(pc, s, &z)) return false;
            pc->time_zone += z;
            if (pc->time_zone < -24 * 60 || pc->time_zone > 24 * 60) return false;
        }
        return true;

    case 'T':
        pc->i++;
        pc->zones++;
        pc->time_zone = -HOUR(7);
        return true;

    case T_DAYZONE:
        pc->i++;
        pc->zones++;
        pc->time_zone = t->val + 60;
        return true;

    case T_LOCALZONE:
        pc->i++;
        pc->local_zones++;
        pc->local_isdst = (int)t->val;
        if (cur(pc)->type == T_DST) { pc->local_isdst = 1; pc->dsts++; pc->i++; }
        return true;

    default:
        return false;
    }
}

static int to_hour(s64 h, int merid)
{
    if (merid == 0) return (h >= 0 && h <= 23) ? (int)h : -1;
    if (h < 1 || h > 12) return -1;
    if (merid == 1) return h == 12 ? 0 : (int)h;
    return h == 12 ? 12 : (int)h + 12;
}

/* The fields must survive normalisation unchanged, or the date did not
 * exist: February 30, 12:00:60, and 02:30 on the morning summer time
 * starts are all refused this way, as GNU refuses them. */
static bool same_fields(const lp_tm_t *a, const lp_tm_t *b)
{
    return a->year == b->year && a->mon == b->mon && a->day == b->day &&
           a->hour == b->hour && a->min == b->min && a->sec == b->sec;
}

static void norm_ns(s64 *sec, long *ns)
{
    while (*ns < 0) { *ns += 1000000000L; (*sec)--; }
    while (*ns >= 1000000000L) { *ns -= 1000000000L; (*sec)++; }
}

/* Read the TZ="..." prefix GNU accepts; returns the rest of the string,
 * or NULL when the quoting is broken. */
static const char *tz_prefix(const char *s, char *zone, size_t cap)
{
    zone[0] = '\0';
    while (is_space(*s)) s++;
    if (strncmp(s, "TZ=\"", 4) != 0) return s;
    s += 4;
    size_t k = 0;
    for (; *s && *s != '"'; s++) {
        if (*s == '\\') {
            s++;
            if (*s != '\\' && *s != '"') return NULL;
        }
        if (k < cap - 1) zone[k++] = *s;
    }
    if (*s != '"') return NULL;
    zone[k] = '\0';
    return s + 1;
}

/* The local zone's two abbreviations: the one in force now, and the
 * first one within the next nine months that has the other summer flag. */
static void local_names(pc_t *pc, s64 now)
{
    lp_tm_t tm;
    lp_localtime(now, &tm);
    pc->nlz = 0;
    strlcpy(pc->lz_name[0], tm.zone, sizeof pc->lz_name[0]);
    pc->lz_isdst[0] = tm.isdst;
    pc->nlz = 1;
    for (int q = 1; q <= 3; q++) {
        lp_tm_t p;
        lp_localtime(now + (s64)q * 90 * 86400, &p);
        if (p.isdst != tm.isdst && strcmp(p.zone, tm.zone) != 0) {
            strlcpy(pc->lz_name[1], p.zone, sizeof pc->lz_name[1]);
            pc->lz_isdst[1] = p.isdst;
            pc->nlz = 2;
            break;
        }
    }
    for (int i = 0; i < pc->nlz; i++)
        for (char *c = pc->lz_name[i]; *c; c++)
            if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
}

static bool parse_body(const char *s, s64 now, long now_ns, s64 *out, long *out_ns)
{
    static pc_t pc;
    memset(&pc, 0, sizeof pc);
    local_names(&pc, now);

    while (is_space(*s)) s++;
    if (*s == '@') {
        if (!lex(&pc, s + 1)) return false;
        tok_t *t = &pc.t[0];
        if (pc.n != 2 || pc.t[1].type != T_END) return false;
        if (t->type == T_UNUM || t->type == T_SNUM) { *out = t->val; *out_ns = 0; return true; }
        if (t->type == T_UDEC || t->type == T_SDEC) { *out = t->val; *out_ns = t->ns; return true; }
        return false;
    }
    if (!lex(&pc, s)) return false;

    lp_tm_t now_tm;
    lp_localtime(now, &now_tm);
    pc.year = now_tm.year; pc.month = now_tm.mon; pc.day = now_tm.day;
    pc.hour = now_tm.hour; pc.min = now_tm.min; pc.sec = now_tm.sec;
    pc.ns = now_ns;

    pc.i = 0;
    while (cur(&pc)->type != T_END)
        if (!p_item(&pc)) return false;

    if (pc.times > 1 || pc.dates > 1 || pc.days > 1 || pc.dsts > 1 ||
        pc.local_zones + pc.zones > 1)
        return false;

    lp_tm_t tm;
    memset(&tm, 0, sizeof tm);
    s64 year = pc.year;
    if (pc.year_digits == 2) year += year < 69 ? 2000 : 1900;
    if (year < -100000000 || year > 100000000 || pc.month < -1000 || pc.month > 1000 ||
        pc.day < -100000 || pc.day > 100000)
        return false;
    tm.year = (int)year; tm.mon = (int)pc.month; tm.day = (int)pc.day;

    long ns;
    if (pc.times || (pc.rels && !pc.dates && !pc.days)) {
        int h = to_hour(pc.hour, pc.merid);
        if (h < 0 || pc.min < 0 || pc.min > 59 || pc.sec < 0 || pc.sec > 60) return false;
        tm.hour = h; tm.min = (int)pc.min; tm.sec = (int)pc.sec;
        ns = pc.ns;
    } else {
        tm.hour = tm.min = tm.sec = 0;
        ns = 0;
    }
    tm.isdst = (pc.dates || pc.days || pc.times) ? -1 : now_tm.isdst;
    if (pc.local_zones && pc.local_isdst >= 0) tm.isdst = pc.local_isdst;
    lp_tm_t tm0 = tm;
    s64 start;

    if (pc.zones) {
        /* A zone was named: the fields are that zone's wall clock, so the
         * arithmetic is done there and the offset taken off at the end.
         * This is what gnulib's mktime-then-correct amounts to, without
         * refusing a time that happens not to exist in the local zone. */
        lp_tm_t chk;
        s64 w = lp_timegm(&tm);
        lp_gmtime(w, &chk);
        if (!same_fields(&tm0, &chk)) return false;
        if (pc.days && !pc.dates) {
            int wd = chk.wday;
            w += 86400 * (s64)((pc.day_number - wd + 7) % 7 +
                 7 * (pc.day_ordinal - (0 < pc.day_ordinal && wd != pc.day_number)));
        }
        if (pc.rel.year || pc.rel.month || pc.rel.day) {
            lp_gmtime(w, &chk);
            chk.year += (int)pc.rel.year; chk.mon += (int)pc.rel.month;
            chk.day += (int)pc.rel.day;
            chk.hour = tm0.hour; chk.min = tm0.min; chk.sec = tm0.sec;
            w = lp_timegm(&chk);
        }
        start = w - pc.time_zone * 60;
    } else {
        start = lp_mktime(&tm);
        if (!same_fields(&tm0, &tm)) return false;
        if (pc.days && !pc.dates) {
            tm.day += (pc.day_number - tm.wday + 7) % 7 +
                      7 * (pc.day_ordinal - (0 < pc.day_ordinal && tm.wday != pc.day_number));
            tm.isdst = -1;
            start = lp_mktime(&tm);
        }
        if (pc.rel.year || pc.rel.month || pc.rel.day) {
            tm.year += (int)pc.rel.year; tm.mon += (int)pc.rel.month;
            tm.day += (int)pc.rel.day;
            tm.hour = tm0.hour; tm.min = tm0.min; tm.sec = tm0.sec;
            tm.isdst = tm0.isdst;
            start = lp_mktime(&tm);
        }
    }
    start += pc.rel.hour * 3600 + pc.rel.min * 60 + pc.rel.sec;
    ns += (long)pc.rel.ns;
    norm_ns(&start, &ns);
    *out = start;
    *out_ns = ns;
    return true;
}

/* The whole of -d: an optional TZ="..." that the rest is read in, then
 * the body. The zone is put back afterwards, because the result is
 * printed in the zone the command was run in, as GNU does. */
static bool parse_date(const char *s, s64 now, long now_ns, s64 *out, long *out_ns)
{
    char zone[128];
    const char *rest = tz_prefix(s, zone, sizeof zone);
    if (!rest) return false;
    if (!zone[0]) return parse_body(rest, now, now_ns, out, out_ns);

    const char *old = getenv("TZ");
    char saved[256];
    bool had = old != NULL;
    if (had) strlcpy(saved, old, sizeof saved);
    setenv("TZ", zone, 1);
    bool ok = parse_body(rest, now, now_ns, out, out_ns);
    if (had) setenv("TZ", saved, 1);
    else unsetenv("TZ");
    return ok;
}

#endif
