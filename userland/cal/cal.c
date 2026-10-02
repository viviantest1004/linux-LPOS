/* cal - a calendar, the way util-linux's cal prints it.
 *
 *   cal                  this month, today highlighted on a terminal
 *   cal 2026             the whole year
 *   cal 9 2026           one month (a name works too: cal sep 2026)
 *   cal 26 9 2026        that month with that day highlighted
 *   cal -3               last month, this month, next month
 *   cal -y / -Y          this year / the next twelve months
 *   cal -m / -s          weeks start on Monday / Sunday
 *   cal -j -w -v -n N -S  day of year, week numbers, vertical, N months, span
 *
 * Why util-linux's layout and not BSD ncal's: it is the cal that most
 * distributions ship and most answers online show, and its output is a
 * fixed grid (20 columns a month, 2 between months, 3 in the year view)
 * that scripts cut columns out of. The differential test in the CLI
 * track's scratch directory compares this program with util-linux 2.39
 * cal byte for byte, 1752 included.
 *
 * The calendar is proleptic Julian up to 2 September 1752 and Gregorian
 * from 14 September 1752 (Great Britain's reform, which is what cal has
 * always assumed); --reform and --iso choose otherwise.
 *
 * Month and day names follow the language: English by default, Korean
 * ("9월 2026", "일 월 화 수 목 금 토") when LC_ALL/LC_TIME/LANG says ko.
 * Korean day names are two columns wide, so the grid does not move.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define SUNDAY 0
#define MONDAY 1
#define MAXDAYS 42
#define SPACE (-1)
#define NONEDAY (-1)
#define MISSING 11
#define YDAY_AFTER_MISSING 258

#define WEEK_NUM_MASK 0xff
#define WEEK_NUM_ISO  0x100
#define WEEK_NUM_US   0x200

#define REFORM_1752   1752
#define REFORM_GREG   (-2147483647)      /* Gregorian all the way back */
#define REFORM_JULIAN 2147483647         /* never */

static const int mdays[2][13] = {
    { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
    { 0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
};

typedef struct {
    int  reform;
    int  num_months, span, months_in_row, weekstart, weektype, gutter;
    int  day_width, week_width;
    bool julian, header_year, header_hint, vertical, color;
    int  req_day, req_month, req_year, req_week, start_month;
    const char *month_name[12];
    const char *weekday[7];
    char headings[128];
} ctl_t;

typedef struct month {
    int days[MAXDAYS];
    int weeks[MAXDAYS / 7];
    int month, year;
    struct month *next;
} month_t;

static ctl_t C;

/* ── Output, buffered: a year is 2 KB and should be one write. ───── */

static char obuf[65536];
static size_t olen;
static void out_flush(void) { if (olen) lp_write(1, obuf, olen); olen = 0; }
static void outs(const char *s) { size_t n = strlen(s); if (olen + n > sizeof obuf) out_flush(); memcpy(obuf + olen, s, n); olen += n; }
static void outc(char c) { if (olen + 1 > sizeof obuf) out_flush(); obuf[olen++] = c; }
static void spaces(int n) { while (n-- > 0) outc(' '); }
static void outnum(int width, int v)
{
    char b[16];
    snprintf(b, sizeof b, "%*d", width, v);
    outs(b);
}

/* ── The calendar ─────────────────────────────────────────────────── */

static int leap(int year)
{
    if (year <= C.reform) return year % 4 == 0;
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int day_in_year(int day, int month, int year)
{
    int l = leap(year);
    for (int i = 1; i < month; i++) day += mdays[l][i];
    return day;
}

/* 0 = Sunday, for 1 January 1 to 31 December 9999 (and beyond); the
 * eleven days the reform removed have no weekday. The constants are the
 * days left over, modulo 7, from the start of the year to each month -
 * for the months after February, less one, since the year used for them
 * is the previous one and so already counted its leap day. */
static int day_in_week(int day, int month, int year)
{
    static const int greg[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    static const int jul[]  = { 5, 1, 0, 3, 5, 1, 3, 6, 2, 4, 0, 2 };
    long long y = year;
    if (C.reform != REFORM_JULIAN && year == C.reform + 1) y -= (month < 3) + 14;
    else y -= month < 3;
    if (C.reform < y || (y == C.reform && (9 < month || (month == 9 && 13 < day))))
        return (int)(((y + y / 4 - y / 100 + y / 400 + greg[month - 1] + day) % 7 + 7) % 7);
    if (y < C.reform || (y == C.reform && (month < 9 || (month == 9 && day < 3))))
        return (int)(((y + y / 4 + jul[month - 1] + day) % 7 + 7) % 7);
    return NONEDAY;
}

static int week_number(int day, int month, int year)
{
    int wday = day_in_week(1, 1, year);
    int fday = (C.weektype & WEEK_NUM_ISO) ? wday + (wday >= 5 ? -2 : 5) : wday + 6;
    if (day > 31) month = 1;
    int yday = day_in_year(day, month, year);
    if (year == C.reform && yday >= YDAY_AFTER_MISSING) fday -= MISSING;
    if (yday + fday < 7) return week_number(31, 12, year - 1);
    if (C.weektype == WEEK_NUM_ISO && yday >= 363) {
        int d = day_in_week(day, month, year), e = day_in_week(31, 12, year);
        if (d >= 1 && d <= 3 && e >= 1 && e <= 3) return week_number(1, 1, year + 1);
    }
    return (yday + fday) / 7;
}

static int week_to_day(void)
{
    int wday = day_in_week(1, 1, C.req_year);
    int yday = C.req_week * 7 - wday;
    if (C.req_year == C.reform && yday >= YDAY_AFTER_MISSING) yday += MISSING;
    if (C.weektype & WEEK_NUM_ISO) yday -= (wday >= 5 ? -2 : 5);
    else yday -= 6;
    return yday <= 0 ? 1 : yday;
}

static void fill_month(month_t *m)
{
    int first = day_in_week(1, m->month, m->year);
    int j = C.julian ? day_in_year(1, m->month, m->year) : 1;
    int last = j + mdays[leap(m->year)][m->month];
    int weeklines = 0;
    if (C.weekstart) {
        first -= C.weekstart;
        if (first < 0) first = 7 - C.weekstart;
        last += C.weekstart - 1;
    }
    for (int i = 0; i < MAXDAYS; i++) {
        if (first > 0) { m->days[i] = SPACE; first--; continue; }
        if (j < last) {
            if (m->year == C.reform && m->month == 9 && (j == 3 || j == 247)) j += MISSING;
            m->days[i] = j++;
            continue;
        }
        m->days[i] = SPACE;
        weeklines++;
    }
    if (C.weektype) {
        int wn = week_number(1, m->month, m->year);
        weeklines = MAXDAYS / 7 - weeklines / 7;
        for (int i = 0; i < MAXDAYS / 7; i++) {
            if (weeklines > 0) {
                if (wn > 52) wn = week_number(m->days[i * 7], m->month, m->year);
                m->weeks[i] = wn++;
            } else {
                m->weeks[i] = SPACE;
            }
            weeklines--;
        }
    }
}

/* ── Text in columns ──────────────────────────────────────────────── */

/* The first `width` columns of s (whole characters only) and how many
 * columns that is. */
static size_t fit(const char *s, int width, int *cols)
{
    size_t len = strlen(s), i = 0;
    int w = 0;
    while (i < len) {
        size_t n = utf8_next(s, len, i);
        int cw = (int)utf8_str_width(s + i, n - i);
        if (w + cw > width) break;
        w += cw;
        i = n;
    }
    *cols = w;
    return i;
}

/* Centred in width columns; the extra space, when there is one, goes on
 * the left - util-linux's mbsalign does it that way round. */
static void center(const char *s, int width, int gutter)
{
    int cols;
    size_t n = fit(s, width, &cols);
    int pad = width - cols;
    spaces((pad + 1) / 2);
    char tmp[256];
    if (n >= sizeof tmp) n = sizeof tmp - 1;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    outs(tmp);
    spaces(pad / 2);
    spaces(gutter);
}

static void left(const char *s, int width, int gutter)
{
    int cols;
    size_t n = fit(s, width, &cols);
    char tmp[256];
    if (n >= sizeof tmp) n = sizeof tmp - 1;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    outs(tmp);
    spaces(width - cols);
    spaces(gutter);
}

static void init_names(lp_lang_t lang)
{
    for (int i = 0; i < 12; i++) C.month_name[i] = lp_month_name(i + 1, true, lang);
    for (int i = 0; i < 7; i++) C.weekday[i] = lp_day_name((i + C.weekstart) % 7, false, lang);
    char *h = C.headings;
    for (int i = 0; i < 7; i++) {
        if (i) *h++ = ' ';
        int cols;
        size_t n = fit(C.weekday[i], C.day_width - 1, &cols);
        int pad = C.day_width - 1 - cols;
        for (int k = 0; k < (pad + 1) / 2; k++) *h++ = ' ';
        memcpy(h, C.weekday[i], n);
        h += n;
        for (int k = 0; k < pad / 2; k++) *h++ = ' ';
    }
    *h = '\0';
    char y[16];
    int yl = snprintf(y, sizeof y, "%04d", C.req_year);
    for (int i = 0; i < 12; i++)
        if (C.week_width < (int)strlen(C.month_name[i]) + yl) C.header_hint = true;
}

static void output_header(month_t *m)
{
    char s[128];
    if (C.header_hint || C.header_year) {
        for (month_t *i = m; i; i = i->next) center(C.month_name[i->month - 1], C.week_width, i->next ? C.gutter : 0);
        if (!C.header_year) {
            outc('\n');
            for (month_t *i = m; i; i = i->next) {
                snprintf(s, sizeof s, "%04d", i->year);
                center(s, C.week_width, i->next ? C.gutter : 0);
            }
        }
    } else {
        for (month_t *i = m; i; i = i->next) {
            snprintf(s, sizeof s, "%s %04d", C.month_name[i->month - 1], i->year);
            center(s, C.week_width, i->next ? C.gutter : 0);
        }
    }
    outc('\n');
    for (month_t *i = m; i; i = i->next) {
        if (C.weektype) spaces(C.julian ? C.day_width - 1 : C.day_width);
        outs(C.headings);
        if (i->next) spaces(C.gutter);
    }
    outc('\n');
}

static int requested_day(const month_t *m)
{
    if (m->month != C.req_month || m->year != C.req_year) return 0;
    return C.julian ? C.req_day : C.req_day + 1 - day_in_year(1, m->month, m->year);
}

static void output_months(month_t *m)
{
    const char *rev = "\033[7m", *reset = "\033[0m";
    for (int line = 0; line < MAXDAYS / 7; line++) {
        month_t *i;
        for (i = m; i; i = i->next) {
            int req = requested_day(i);
            int skip;
            if (C.weektype) {
                if (i->weeks[line] > 0) {
                    if ((C.weektype & WEEK_NUM_MASK) == i->weeks[line]) {
                        outs(rev); outnum(2, i->weeks[line]); outs(reset);
                    } else {
                        outnum(2, i->weeks[line]);
                    }
                } else {
                    spaces(2);
                }
                skip = C.day_width;
            } else {
                skip = C.day_width - 1;
            }
            for (int d = 7 * line; d < 7 * line + 7; d++) {
                if (i->days[d] > 0) {
                    if (req == i->days[d]) {
                        int w = C.julian ? 3 : 2;
                        spaces(skip - w);
                        outs(rev); outnum(w, i->days[d]); outs(reset);
                    } else {
                        outnum(skip, i->days[d]);
                    }
                } else {
                    spaces(skip);
                }
                if (skip < C.day_width) skip++;
            }
            if (i->next) spaces(C.gutter);
        }
        outc('\n');
    }
}

static void output_vertical(month_t *m)
{
    const char *rev = "\033[7m", *reset = "\033[0m";
    int month_width = C.day_width * (MAXDAYS / 7);
    char s[128];
    spaces(C.day_width + 1);
    if (C.header_hint || C.header_year) {
        for (month_t *i = m; i; i = i->next) left(C.month_name[i->month - 1], month_width, C.gutter);
        if (!C.header_year) {
            outc('\n');
            spaces(C.day_width + 1);
            for (month_t *i = m; i; i = i->next) {
                snprintf(s, sizeof s, "%04d", i->year);
                left(s, month_width, C.gutter);
            }
        }
    } else {
        for (month_t *i = m; i; i = i->next) {
            snprintf(s, sizeof s, "%s %04d", C.month_name[i->month - 1], i->year);
            left(s, month_width, C.gutter);
        }
    }
    outc('\n');

    int skip = C.day_width;
    for (int wd = 0; wd < 7; wd++) {
        left(C.weekday[wd], C.day_width - 1, 0);
        for (month_t *i = m; i; i = i->next) {
            int req = requested_day(i);
            for (int week = 0; week < MAXDAYS / 7; week++) {
                int d = wd + 7 * week;
                if (i->days[d] > 0) {
                    if (req == i->days[d]) {
                        int w = C.julian ? 3 : 2;
                        spaces(skip - w);
                        outs(rev); outnum(w, i->days[d]); outs(reset);
                    } else {
                        outnum(skip, i->days[d]);
                    }
                } else {
                    spaces(skip);
                }
                skip = C.day_width;
            }
            if (i->next) spaces(C.gutter);
        }
        outc('\n');
    }
    if (!C.weektype) return;
    spaces(C.day_width - 1);
    for (month_t *i = m; i; i = i->next) {
        for (int week = 0; week < MAXDAYS / 7; week++) {
            if (i->weeks[week] > 0) {
                if ((C.weektype & WEEK_NUM_MASK) == i->weeks[week]) {
                    outs(rev); outnum(skip - (C.julian ? 3 : 2), i->weeks[week]); outs(reset);
                } else {
                    outnum(skip, i->weeks[week]);
                }
            } else {
                spaces(skip);
            }
        }
        if (i->next) spaces(C.gutter);
    }
    outc('\n');
}

static void monthly(void)
{
    int month = C.start_month ? C.start_month : C.req_month;
    int year = C.req_year;
    if (C.span) {
        int nm = month - C.num_months / 2;
        if (nm < 1) {
            nm = -nm;
            year -= nm / 12 + 1;
            if (nm > 12) nm %= 12;
            month = 12 - nm;
        } else {
            month = nm;
        }
    }
    month_t *ms = calloc((size_t)C.months_in_row, sizeof *ms);
    if (!ms) lp_exit(1);
    for (int i = 0; i < C.months_in_row - 1; i++) ms[i].next = &ms[i + 1];
    int rows = (C.num_months - 1) / C.months_in_row;
    for (int r = 0; r < rows + 1; r++) {
        if (r == rows && C.num_months % C.months_in_row > 0)
            for (int n = C.num_months % C.months_in_row - 1; n < C.months_in_row; n++) ms[n].next = NULL;
        for (month_t *m = ms; m; m = m->next) {
            m->month = month++;
            m->year = year;
            if (month > 12) { year++; month = 1; }
            fill_month(m);
        }
        if (C.vertical) {
            if (r > 0) outc('\n');
            output_vertical(ms);
        } else {
            output_header(ms);
            output_months(ms);
        }
    }
    free(ms);
}

/* ── Arguments ────────────────────────────────────────────────────── */

static void fail(const char *msg)
{
    out_flush();
    dprintf(2, "cal: %s\n", msg);
    lp_exit(1);
}

static bool all_digits(const char *s)
{
    if (!*s) return false;
    for (; *s; s++) if (*s < '0' || *s > '9') return false;
    return true;
}

static long number(const char *s, const char *what)
{
    char msg[96];
    const char *p = s;
    if (*p == '-' || *p == '+') p++;
    if (!all_digits(p) || strlen(p) > 9) {
        snprintf(msg, sizeof msg, "%s: '%s'", what, s);
        fail(msg);
    }
    return strtol(s, NULL, 10);
}

static int month_by_name(const char *name)
{
    for (int lang = 0; lang < 2; lang++)
        for (int full = 1; full >= 0; full--)
            for (int i = 1; i <= 12; i++) {
                const char *m = lp_month_name(i, full, (lp_lang_t)lang);
                const char *a = m, *b = name;
                while (*a && *b) {
                    char x = *a, y = *b;
                    if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
                    if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
                    if (x != y) break;
                    a++; b++;
                }
                if (!*a && !*b) return i;
            }
    return -1;
}

static void usage(void)
{
    printf("\nUsage:\n"
           " cal [options] [[[day] month] year]\n"
           " cal [options] <timestamp|monthname>\n\n"
           "Display a calendar, or some part of it.\n"
           "Without any arguments, display the current month.\n\n"
           "Options:\n"
           " -1, --one             show only a single month (default)\n"
           " -3, --three           show three months spanning the date\n"
           " -n, --months <num>    show num months starting with date's month\n"
           " -S, --span            span the date when displaying multiple months\n"
           " -s, --sunday          Sunday as first day of week\n"
           " -m, --monday          Monday as first day of week\n"
           " -j, --julian          use day-of-year for all calendars\n"
           "     --reform <val>    Gregorian reform date (1752|gregorian|iso|julian)\n"
           "     --iso             alias for --reform=iso\n"
           " -y, --year            show the whole year\n"
           " -Y, --twelve          show the next twelve months\n"
           " -w, --week[=<num>]    show US or ISO-8601 week numbers\n"
           " -v, --vertical        show day vertically instead of line\n"
           " -c, --columns <width> amount of columns to use\n"
           "     --color[=<when>]  colorize messages (auto, always or never)\n\n"
           " -h, --help            display this help\n"
           " -V, --version         display version\n");
}

int main(int argc, char **argv)
{
    memset(&C, 0, sizeof C);
    C.reform = REFORM_1752;
    C.weekstart = SUNDAY;
    C.day_width = 3;
    C.gutter = 2;
    int color_mode = 0;              /* 0 auto, 1 always, 2 never */
    bool yflag = false, Yflag = false;
    int cols = -1;                   /* -1 at most three, -2 auto */

    char *ops[16];
    int nops = 0;
    bool only_ops = false;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (only_ops || a[0] != '-' || a[1] == '\0') {
            if (nops < 16) ops[nops++] = a;
            continue;
        }
        if (!strcmp(a, "--")) { only_ops = true; continue; }
        if (a[1] == '-') {
            const char *n = a + 2, *eq = strchr(n, '=');
            size_t nl = eq ? (size_t)(eq - n) : strlen(n);
#define IS(s) (nl == strlen(s) && !strncmp(n, s, nl))
            if (IS("one")) C.num_months = 1;
            else if (IS("three")) { C.num_months = 3; C.span = 1; }
            else if (IS("sunday")) C.weekstart = SUNDAY;
            else if (IS("monday")) C.weekstart = MONDAY;
            else if (IS("julian")) { C.julian = true; C.day_width = 4; }
            else if (IS("span")) C.span = 1;
            else if (IS("year")) yflag = true;
            else if (IS("twelve")) Yflag = true;
            else if (IS("vertical")) C.vertical = true;
            else if (IS("iso")) C.reform = REFORM_GREG;
            else if (IS("help")) { usage(); return 0; }
            else if (IS("version")) { printf("cal from LP (util-linux 2.39 compatible)\n"); return 0; }
            else if (IS("week")) { C.weektype = WEEK_NUM_US; if (eq) C.req_week = (int)number(eq + 1, "invalid week argument"); }
            else if (IS("color")) {
                const char *v = eq ? eq + 1 : "auto";
                color_mode = !strcmp(v, "always") ? 1 : !strcmp(v, "never") ? 2 : !strcmp(v, "auto") ? 0 : -1;
                if (color_mode < 0) fail("unsupported color mode");
            } else if (IS("months") || IS("reform") || IS("columns")) {
                const char *v = eq ? eq + 1 : (i + 1 < argc ? argv[++i] : NULL);
                if (!v) fail("option requires an argument");
                if (IS("months")) C.num_months = (int)number(v, "invalid month argument");
                else if (IS("columns")) cols = !strcmp(v, "auto") ? -2 : (int)number(v, "failed to parse columns");
                else if (!strcmp(v, "1752")) C.reform = REFORM_1752;
                else if (!strcmp(v, "gregorian") || !strcmp(v, "iso")) C.reform = REFORM_GREG;
                else if (!strcmp(v, "julian")) C.reform = REFORM_JULIAN;
                else { char m[96]; snprintf(m, sizeof m, "invalid --reform value: '%s'", v); fail(m); }
            } else {
                dprintf(2, "cal: unrecognized option '%s'\nTry 'cal --help' for more information.\n", a);
                return 1;
            }
#undef IS
            continue;
        }
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
            case '1': C.num_months = 1; break;
            case '3': C.num_months = 3; C.span = 1; break;
            case 's': C.weekstart = SUNDAY; break;
            case 'm': C.weekstart = MONDAY; break;
            case 'j': C.julian = true; C.day_width = 4; break;
            case 'S': C.span = 1; break;
            case 'y': yflag = true; break;
            case 'Y': Yflag = true; break;
            case 'v': C.vertical = true; break;
            case 'w': C.weektype = WEEK_NUM_US; break;
            case 'h': usage(); return 0;
            case 'V': printf("cal from LP (util-linux 2.39 compatible)\n"); return 0;
            case 'n': case 'c': {
                const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                if (!v) { dprintf(2, "cal: option requires an argument -- '%c'\n", *p); return 1; }
                if (*p == 'n') C.num_months = (int)number(v, "invalid month argument");
                else cols = !strcmp(v, "auto") ? -2 : (int)number(v, "failed to parse columns");
                p += strlen(p) - 1;
                break;
            }
            default:
                dprintf(2, "cal: invalid option -- '%c'\nTry 'cal --help' for more information.\n", *p);
                return 1;
            }
        }
    }
    if (yflag && Yflag) fail("mutually exclusive arguments: --twelve --year");
    if (C.req_week && (C.req_week < 1 || C.req_week > 54)) fail("illegal week value: use 1-54");

    if (C.weektype) {
        C.weektype = (C.req_week & WEEK_NUM_MASK) | (C.weekstart == MONDAY ? WEEK_NUM_ISO : WEEK_NUM_US);
        C.week_width = C.day_width * 7 + 3;
    } else {
        C.week_width = C.day_width * 7;
    }
    C.week_width -= 1;

    lp_tm_t now;
    lp_localtime(lp_time(), &now);

    if (nops == 1 && !all_digits(ops[0])) {
        /* A month name, or a date. */
        int m = month_by_name(ops[0]);
        const char *s = ops[0];
        if (m > 0) {
            C.req_month = m;
        } else if (!strcmp(s, "now") || !strcmp(s, "today") || !strcmp(s, "yesterday") || !strcmp(s, "tomorrow")) {
            s64 t = lp_time() + (!strcmp(s, "yesterday") ? -86400 : !strcmp(s, "tomorrow") ? 86400 : 0);
            lp_localtime(t, &now);
        } else if (strlen(s) >= 8 && s[4] == '-' && s[7] == '-') {
            now.year = (int)strtol(s, NULL, 10);
            now.mon = (int)strtol(s + 5, NULL, 10);
            now.day = (int)strtol(s + 8, NULL, 10);
            if (now.mon < 1 || now.mon > 12 || now.day < 1 || now.day > 31) goto bad_ts;
            now.yday = (int)(lp_days_from_civil(now.year, now.mon, now.day) - lp_days_from_civil(now.year, 1, 1));
        } else {
        bad_ts:;
            char msg[200];
            snprintf(msg, sizeof msg, "failed to parse timestamp or unknown month name: %s", ops[0]);
            fail(msg);
        }
        nops = 0;
    }

    switch (nops) {
    case 3:
    case 2:
    case 1: {
        int k = 0;
        if (nops == 3) {
            C.req_day = (int)number(ops[k++], "illegal day value");
            if (C.req_day < 1 || C.req_day > 31) fail("illegal day value: use 1-31");
        }
        if (nops >= 2) {
            if (ops[k][0] >= '0' && ops[k][0] <= '9') C.req_month = (int)number(ops[k], "illegal month value: use 1-12");
            else {
                C.req_month = month_by_name(ops[k]);
                if (C.req_month < 0) { char m[128]; snprintf(m, sizeof m, "unknown month name: %s", ops[k]); fail(m); }
            }
            k++;
            if (C.req_month < 1 || C.req_month > 12) fail("illegal month value: use 1-12");
        }
        C.req_year = (int)number(ops[k], "illegal year value");
        if (C.req_year < 1) fail("illegal year value: use positive integer");
        if (C.req_day) {
            int dm = mdays[leap(C.req_year)][C.req_month];
            if (C.req_day > dm) { char m[64]; snprintf(m, sizeof m, "illegal day value: use 1-%d", dm); fail(m); }
            C.req_day = day_in_year(C.req_day, C.req_month, C.req_year);
        } else if (now.year == C.req_year) {
            C.req_day = now.yday + 1;
        }
        if (!C.req_month && !C.req_week) {
            C.req_month = now.mon;
            if (!C.num_months) yflag = true;
        }
        break;
    }
    case 0:
        C.req_day = now.yday + 1;
        C.req_year = now.year;
        if (!C.req_month) C.req_month = now.mon;
        break;
    default:
        dprintf(2, "cal: bad usage\nTry 'cal --help' for more information.\n");
        return 1;
    }

    if (C.req_week > 0) {
        int yday = week_to_day();
        int l = leap(C.req_year), m = 1;
        if (yday < 1) fail("illegal week value");
        while (m <= 12 && yday > mdays[l][m]) yday -= mdays[l][m++];
        if (m > 12 && (C.weektype & WEEK_NUM_ISO) && C.req_week != week_number(31, 12, C.req_year - 1)) {
            char msg[96];
            snprintf(msg, sizeof msg, "illegal week value: year %d doesn't have week %d", C.req_year, C.req_week);
            fail(msg);
        }
        if (!C.req_month) C.req_month = m > 12 ? 1 : m;
    }

    lp_lang_t lang = lp_time_lang();
    init_names(lang);

    C.color = color_mode == 1 || (color_mode == 0 && lp_isatty(1));
    if (!C.color) {
        C.req_day = 0;
        C.weektype &= ~WEEK_NUM_MASK;
    }

    if (yflag || Yflag) {
        C.gutter = 3;
        if (!C.num_months) C.num_months = 12;
        if (yflag) { C.start_month = 1; C.header_year = true; }
    }
    if (C.vertical) C.gutter = 1;

    if (C.num_months > 1 && C.months_in_row == 0) {
        C.months_in_row = 3;
        if (cols > 0) C.months_in_row = cols;
        else if (lp_isatty(1)) {
            int rows, w = 80;
            if (lp_term_size(1, &rows, &w) < 0 || w <= 0) w = 80;
            int mw = C.julian ? 27 : 20;
            if (w < mw) w = mw;
            int extra = (w / mw - 1) * C.gutter;
            int n = (w - extra) / mw;
            if (cols == -1) { if (n < 3) C.months_in_row = n > 0 ? n : 1; }
            else C.months_in_row = n > 0 ? n : 1;
        }
    } else if (!C.months_in_row) {
        C.months_in_row = 1;
    }
    if (!C.num_months) C.num_months = 1;

    if (yflag || Yflag) {
        if (C.header_year) {
            int yw = C.months_in_row * C.week_width + (C.months_in_row - 1) * C.gutter;
            char y[16];
            snprintf(y, sizeof y, "%04d", C.req_year);
            center(y, yw, 0);
            outs("\n\n");
        }
    }
    monthly();
    out_flush();
    return 0;
}
