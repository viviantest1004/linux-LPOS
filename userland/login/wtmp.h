/* wtmp.h - login records: /var/run/utmp (who is logged in now) and
 * /var/log/wtmp (every login ever), in the kernel-era struct utmp that
 * who, last, users and w all read.
 *
 * Shared by login, which writes them, and lp-motd, which reads the
 * last one back for "Last login: ... on tty1". The layout is written at
 * fixed byte offsets instead of through a C struct, as users.c reads
 * it: the offsets are the ABI, and a compiler's padding is not. The
 * record is the same 384 bytes on x86-64, arm64 and 32-bit ARM (glibc
 * keeps ut_tv two 32-bit fields there for exactly this reason).
 */
#ifndef LP_WTMP_H
#define LP_WTMP_H

#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define UT_RECLEN      384
#define UT_TYPE          0      /* short */
#define UT_PID           4      /* int */
#define UT_LINE          8      /* char[32] */
#define UT_ID           40      /* char[4] */
#define UT_USER         44      /* char[32] */
#define UT_HOST         76      /* char[256] */
#define UT_SESSION     336      /* int */
#define UT_TV_SEC      340      /* int32 */
#define UT_TV_USEC     344      /* int32 */
#define UT_USER_PROCESS  7
#define UT_DEAD_PROCESS  8

#define UTMP_PATH "/var/run/utmp"
#define WTMP_PATH "/var/log/wtmp"

typedef struct {
    s64  when;
    char line[33];
    char host[257];
} wtmp_last_t;

static __attribute__((unused)) void ut_field(const u8 *rec, int off, int len, char *out)
{
    memcpy(out, rec + off, (size_t)len);
    out[len] = '\0';
}

/* The newest USER_PROCESS record for `user` in wtmp. The file is read
 * from the end backwards a block at a time, because it only grows and
 * the answer is almost always in the last few records. */
static __attribute__((unused)) bool wtmp_last(const char *user, wtmp_last_t *out)
{
    long fd = lp_open(WTMP_PATH, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return false;
    s64 size = lp_lseek((int)fd, 0, 2);
    s64 pos = size - size % UT_RECLEN;
    bool found = false;
    u8 buf[UT_RECLEN * 32];
    while (pos > 0 && !found) {
        s64 start = pos >= (s64)sizeof buf ? pos - (s64)sizeof buf : 0;
        size_t want = (size_t)(pos - start);
        if (lp_lseek((int)fd, start, 0) != start) break;
        long got = 0, r;
        while ((size_t)got < want && (r = lp_read((int)fd, buf + got, want - (size_t)got)) > 0) got += r;
        if ((size_t)got != want) break;
        for (s64 off = (s64)want - UT_RECLEN; off >= 0; off -= UT_RECLEN) {
            const u8 *rec = buf + off;
            short type;
            memcpy(&type, rec + UT_TYPE, sizeof type);
            char name[33];
            ut_field(rec, UT_USER, 32, name);
            if (type == UT_USER_PROCESS && strcmp(name, user) == 0) {
                s32 sec;
                memcpy(&sec, rec + UT_TV_SEC, sizeof sec);
                out->when = (s64)(u32)sec;
                ut_field(rec, UT_LINE, 32, out->line);
                ut_field(rec, UT_HOST, 256, out->host);
                found = true;
                break;
            }
        }
        pos = start;
    }
    lp_close((int)fd);
    return found;
}

#endif /* LP_WTMP_H */
