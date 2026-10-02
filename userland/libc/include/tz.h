/* tz.h - local time, time zones and strftime.
 *
 * The machine keeps its clock in UTC. Everything a person reads should be
 * in the zone they chose, and every program should agree on what that
 * zone is - `ls -l`, the log, cron, `date` and the desktop clock. So the
 * answer lives here, in one place, and nothing above the libc works out
 * an offset by itself.
 *
 * ── Where the zone comes from, in order ──
 *
 *   TZ=...            the environment, exactly as glibc reads it: a zone
 *                     name ("Asia/Seoul"), a file (":/path"), a POSIX
 *                     rule ("EST5EDT,M3.2.0,M11.1.0"), or empty for UTC.
 *   /data/timezone    the writable half of a RAM-rooted Pi. It may hold
 *                     a zone name or the older "<minutes> <label> [rule]
 *                     [summer label]" line this system used before it
 *                     read tzdata; both are understood.
 *   /etc/localtime    the Debian base's zone file, a TZif symlink into
 *                     /usr/share/zoneinfo. When it and /data/timezone
 *                     both exist the newer one wins, so neither
 *                     `timedatectl` nor Debian's own tools can be
 *                     silently overruled by a stale copy of the other.
 *   /etc/timezone     a zone name, or the older line, as a last resort.
 *   UTC
 *
 * ── Why tzdata now, when it was refused before ──
 *
 * This header used to say that full tzdata is tens of megabytes, which it
 * is not: the compiled zone files are four, and the desktop base carries
 * them already. What four hand-written daylight-saving rules could not
 * do was history - Seoul was +08:30 in 1955, Lord Howe moves by half an
 * hour, and a file dated before a rule change came out an hour wrong.
 * The zone files have all of that. The four rules survive only to read
 * the older one-line format on a Pi that has no zoneinfo directory.
 *
 * The zone is cached and looked at again every ten seconds, not once,
 * because cron, logd and the supervisor run for months: a zone changed
 * with `timedatectl` has to reach them too, or the machine keeps two
 * clocks and the logs are the ones that stay wrong.
 */
#ifndef _LP_TZ_H
#define _LP_TZ_H

#include "types.h"

/* Broken-down time. Unlike struct tm this holds what a person reads:
 * year is not offset from 1900 and mon starts at 1, so nothing to confuse.
 *
 * The last four fields say which zone the other seven are in. They were
 * added when the zone stopped being one offset for the whole machine and
 * became a table of transitions: "what offset was this?" is no longer a
 * question with one answer, so the answer travels with the time instead
 * of being asked again and getting the other side of a transition.
 * lp_gmtime and lp_localtime fill them; code that builds a time by hand
 * may leave them zero, and lp_strftime treats a zero zone as UTC. */
typedef struct {
    int year;    /* e.g. 2026 */
    int mon;     /* 1-12 */
    int day;     /* 1-31 */
    int hour;    /* 0-23 */
    int min;     /* 0-59 */
    int sec;     /* 0-60 */
    int wday;    /* 0 = Sunday */
    int yday;    /* 0-365, 0 = 1 January */
    int isdst;   /* 1 summer time, 0 not; -1 "unknown" as lp_mktime input */
    int gmtoff;  /* seconds east of UTC, e.g. 32400 for Korea */
    char zone[16];   /* "KST", "EDT", "+0530"; empty means UTC */
} lp_tm_t;

/* Unix seconds -> broken-down time, in UTC. */
void  lp_gmtime(s64 t, lp_tm_t *out);
/* Broken-down UTC time -> unix seconds. wday, yday and the zone fields
 * are ignored; a field out of range (month 13, day 0) is carried into
 * the next one, the way timegm(3) does. */
s64   lp_timegm(const lp_tm_t *tm);

/* The four daylight-saving rules of the older one-line format. Kept for
 * `date -z` on a machine without zone files, which writes them. */
typedef enum {
    LP_DST_NONE = 0,
    LP_DST_EU,      /* last Sun Mar 01:00 UTC -> last Sun Oct 01:00 UTC */
    LP_DST_US,      /* 2nd Sun Mar 02:00 local -> 1st Sun Nov 02:00 local */
    LP_DST_AU,      /* 1st Sun Oct -> 1st Sun Apr (southern) */
    LP_DST_NZ       /* last Sun Sep -> 1st Sun Apr (southern) */
} lp_dst_t;

/* ── The configured zone ── */

/* Seconds east of UTC in force at that instant, summer time included.
 * Ask about the time you are formatting, not about now - in March a
 * summer offset applied to a winter date is an hour wrong. */
int   lp_tz_offset_sec(s64 utc);
/* The same in whole minutes, for the callers written before offsets
 * could carry seconds (local mean time before 1900 did). */
int   lp_tz_offset(s64 utc);
/* The abbreviation to print next to a time ("KST", "BST", "+1030"). */
const char *lp_tz_label(s64 utc);
/* Drop the cached zone, so the next call reads the files again. For a
 * program that has just changed the zone and wants to print the result
 * through the same door everything else reads it by. */
void  lp_tz_forget(void);

/* The zone's name as a person picks it ("Asia/Seoul"), or "" when it has
 * none - a POSIX rule in TZ, or the older one-line format. */
const char *lp_tz_name(void);
/* Where the zone was read from: "TZ", "/etc/localtime", ... */
const char *lp_tz_source(void);
/* The zone file directory: $TZDIR, or /usr/share/zoneinfo. */
const char *lp_tz_dir(void);
/* Does this name a readable, well-formed zone file under lp_tz_dir()?
 * Refuses absolute paths and "..", so a name typed by a user cannot
 * point at an arbitrary file. */
bool  lp_tz_valid(const char *name);
/* The next change of offset strictly after t, if the zone has one
 * within the next 400 years. */
bool  lp_tz_next_change(s64 t, s64 *when);

/* Unix seconds -> broken-down local time, with the zone fields filled. */
void  lp_localtime(s64 t, lp_tm_t *out);
/* Broken-down local time -> unix seconds. Fields out of range are
 * carried; the zone fields and isdst are ignored. An hour that summer
 * time skipped comes back as the hour after it; an hour that happened
 * twice comes back as the first of the two. */
s64   lp_timelocal(const lp_tm_t *tm);
/* mktime(3): like lp_timelocal, but tm->isdst chooses between the two
 * readings of an hour that happened twice (-1: the first), and *tm is
 * rewritten normalised with every field filled in. */
s64   lp_mktime(lp_tm_t *tm);

/* The clock with its nanoseconds, for %N. */
s64   lp_time_ns(long *ns);

/* ── The calendar ── */
bool  lp_is_leap(long year);
int   lp_days_in_month(long year, int mon);        /* mon 1-12 */
/* Days since 1970-01-01 of a proleptic Gregorian date; mon may be out of
 * 1-12 and is carried into the year. */
s64   lp_days_from_civil(long year, int mon, int day);
void  lp_civil_from_days(s64 days, long *year, int *mon, int *day);

/* ── Names ──
 *
 * Two languages: the C locale's English and Korean. Which one a program
 * speaks is read from LC_ALL, LC_TIME and LANG, in that order, the way
 * glibc chooses; anything starting "ko" is Korean. */
typedef enum { LP_LANG_C = 0, LP_LANG_KO = 1 } lp_lang_t;
lp_lang_t   lp_time_lang(void);
const char *lp_day_name(int wday, bool full, lp_lang_t lang);   /* 0 = Sun */
const char *lp_month_name(int mon, bool full, lp_lang_t lang);  /* 1-12 */

/* ── strftime ──
 *
 * GNU's set, as coreutils' date implements it: the -_0^#+ flags, a field
 * width, %N with a digit count, %:z %::z %:::z, %q, %P, %s. An unknown
 * conversion is copied through as written.
 *
 * Unlike strftime(3) the return value is the length the whole result
 * needed, snprintf-style, and the buffer holds as much of it as fits,
 * always terminated. strftime(3) returns 0 on overflow, which cannot be
 * told apart from an empty result - and `date +%p` in some locales is
 * legitimately empty.
 *
 * lp_strftime speaks C; lp_strftime_lang takes the language, and
 * lp_strftime_ns the nanoseconds for %N as well. */
size_t lp_strftime(char *buf, size_t n, const char *fmt, const lp_tm_t *tm);
size_t lp_strftime_lang(char *buf, size_t n, const char *fmt,
                        const lp_tm_t *tm, long ns, lp_lang_t lang);

#endif /* _LP_TZ_H */
