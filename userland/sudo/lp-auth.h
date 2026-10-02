/* lp-auth.h - what sudo, su, login and visudo share: who somebody is,
 * which groups they are in, reading a password with the echo off,
 * checking it, becoming another user, and the auth log.
 *
 * It is a header of static functions rather than part of the libc on
 * purpose. These four programs are the only ones on the machine that
 * turn a typed password into root, and keeping the code that does it in
 * one directory, owned by one track and read as one piece, is worth
 * more than saving a few kilobytes of duplicated object code. Each
 * program includes it as "../sudo/lp-auth.h"; nothing else should.
 *
 * ── Why not the libc's helpers ──
 *
 * lp_user_by_name reads /etc/passwd through a 256-byte line buffer and
 * atoi()s the uid, so a long GECOS field or a garbage uid field is read
 * as something rather than refused. lp_getgid calls the arm64 syscall
 * number on every machine. Neither matters for `ls -l`; both matter for
 * a setuid program, so the few calls needed here are made directly, per
 * architecture, and every field is checked before it is believed.
 *
 * The 32-bit ARM calls are the *32 variants (getuid32, setresuid32...).
 * The plain ones there are the 16-bit legacy calls, which would silently
 * truncate a uid above 65535.
 */
#ifndef LP_AUTH_H
#define LP_AUTH_H

#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"
#include "crypt6.h"

#define A_UNUSED __attribute__((unused))

#if defined(__x86_64__)
#  define A_getuid      102
#  define A_getgid      104
#  define A_geteuid     107
#  define A_setgroups   116
#  define A_setresuid   117
#  define A_getresuid   118
#  define A_setresgid   119
#  define A_getresgid   120
#  define A_umask        95
#  define A_fchown       93
#  define A_fchmod       91
#  define A_O_NOFOLLOW  0400000
#  define A_O_DIRECTORY 0200000
#elif defined(__aarch64__)
#  define A_getuid      174
#  define A_getgid      176
#  define A_geteuid     175
#  define A_setgroups   159
#  define A_setresuid   147
#  define A_getresuid   148
#  define A_setresgid   149
#  define A_getresgid   150
#  define A_umask       166
#  define A_fchown       55
#  define A_fchmod       52
#  define A_O_NOFOLLOW  0100000
#  define A_O_DIRECTORY 040000
#elif defined(__arm__)
#  define A_getuid      199
#  define A_getgid      200
#  define A_geteuid     201
#  define A_setgroups   206
#  define A_setresuid   208
#  define A_getresuid   209
#  define A_setresgid   210
#  define A_getresgid   211
#  define A_umask        60
#  define A_fchown      207
#  define A_fchmod       94
#  define A_O_NOFOLLOW  0100000
#  define A_O_DIRECTORY 040000
#endif
#define A_CLOCK_BOOTTIME 7
#define A_AT_EMPTY_PATH  0x1000

static A_UNUSED int  a_getuid(void)  { return (int)sys_call0(A_getuid); }
static A_UNUSED int  a_getgid(void)  { return (int)sys_call0(A_getgid); }
static A_UNUSED int  a_geteuid(void) { return (int)sys_call0(A_geteuid); }
static A_UNUSED long a_umask(long m) { return sys_call1(A_umask, m); }
static A_UNUSED long a_fchown(int fd, long u, long g) { return sys_call3(A_fchown, fd, u, g); }
static A_UNUSED long a_fchmod(int fd, long m) { return sys_call2(A_fchmod, fd, m); }
static A_UNUSED long a_setgroups(int n, const gid_t *g)
{
    return sys_call2(A_setgroups, (long)n, (long)g);
}
static A_UNUSED long a_setresuid(long r, long e, long s) { return sys_call3(A_setresuid, r, e, s); }
static A_UNUSED long a_setresgid(long r, long e, long s) { return sys_call3(A_setresgid, r, e, s); }
static A_UNUSED long a_getresuid(u32 *r, u32 *e, u32 *s)
{
    return sys_call3(A_getresuid, (long)r, (long)e, (long)s);
}
static A_UNUSED long a_getresgid(u32 *r, u32 *e, u32 *s)
{
    return sys_call3(A_getresgid, (long)r, (long)e, (long)s);
}

/* The time since boot, suspend included, in milliseconds. A sudo
 * timestamp measured on the wall clock could be kept alive for ever by
 * `date -s` - which the timestamp itself lets you run. */
static A_UNUSED s64 a_boottime_ms(void)
{
    s64 ts[2] = { 0, 0 };
    if (sys_call2(SYS_clock_gettime, A_CLOCK_BOOTTIME, (long)ts) < 0)
        return -1;
    return ts[0] * 1000 + ts[1] / 1000000;
}

/* fstat through statx(fd, "", AT_EMPTY_PATH): one layout on every
 * machine, and a check made on the open file rather than on a path that
 * can be swapped between the check and the open. */
typedef struct { u32 mode, uid, gid; u64 size, ino, dev, rdev; s64 mtime; } a_stat_t;

static A_UNUSED long a_fstat(int fd, a_stat_t *st)
{
    u8 b[256];
    memset(b, 0, sizeof b);
    long r = sys_call5(SYS_statx, fd, (long)"", A_AT_EMPTY_PATH, 0x7ff, (long)b);
    if (r < 0) return r;
    st->uid   = *(u32 *)(b + 20);
    st->gid   = *(u32 *)(b + 24);
    st->mode  = *(u16 *)(b + 28);
    st->ino   = *(u64 *)(b + 32);
    st->size  = *(u64 *)(b + 40);
    st->mtime = *(s64 *)(b + 112);
    st->rdev  = ((u64)*(u32 *)(b + 128) << 32) | *(u32 *)(b + 132);
    st->dev   = ((u64)*(u32 *)(b + 136) << 32) | *(u32 *)(b + 140);
    return 0;
}

/* Zero a buffer in a way the compiler may not drop as a dead store. */
static A_UNUSED void a_wipe(void *p, size_t n)
{
    volatile char *v = (volatile char *)p;
    while (n--) *v++ = 0;
}

/* ── Accounts ─────────────────────────────────────────────────────── */

typedef struct {
    char  name[64];
    uid_t uid;
    gid_t gid;
    char  gecos[128];
    char  home[256];
    char  shell[256];
} a_pw_t;

/* A decimal id that is all digits and fits: anything else is not an id. */
static A_UNUSED bool a_parse_id(const char *s, u32 *out)
{
    if (!*s) return false;
    u64 v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (u64)(*s - '0');
        if (v > 0xfffffffeULL) return false;
    }
    *out = (u32)v;
    return true;
}

/* Split a line on ':' into at most max fields; the count found. */
static A_UNUSED int a_split(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    for (;;) {
        if (n == max) return n + 1;          /* too many: not this format */
        f[n++] = p;
        char *c = strchr(p, ':');
        if (!c) return n;
        *c = '\0';
        p = c + 1;
    }
}

/* The whole of a small system file, or NULL. */
static A_UNUSED char *a_slurp(const char *path, long max, long *len)
{
    long fd = lp_open(path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return NULL;
    char *buf = malloc((size_t)max + 1);
    if (!buf) { lp_close((int)fd); return NULL; }
    long n = 0, r;
    while (n < max && (r = lp_read((int)fd, buf + n, (size_t)(max - n))) > 0)
        n += r;
    lp_close((int)fd);
    buf[n] = '\0';
    if (len) *len = n;
    return buf;
}

/* By name (name != NULL) or by uid. Fields are copied only when they
 * fit; a line that does not parse is skipped, never half-believed. */
static A_UNUSED bool a_getpw(const char *name, uid_t uid, a_pw_t *out)
{
    char *buf = a_slurp("/etc/passwd", 1 << 20, NULL);
    if (!buf) return false;
    bool found = false;
    for (char *line = buf; line && *line && !found; ) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        char *f[8];
        if (line[0] != '#' && a_split(line, f, 7) == 7) {
            u32 u, g;
            bool match = name ? strcmp(f[0], name) == 0 : true;
            if (match && a_parse_id(f[2], &u) && a_parse_id(f[3], &g) &&
                (name || u == uid) && f[0][0] &&
                strlen(f[0]) < sizeof out->name && strlen(f[4]) < sizeof out->gecos &&
                strlen(f[5]) < sizeof out->home && strlen(f[6]) < sizeof out->shell) {
                strlcpy(out->name, f[0], sizeof out->name);
                out->uid = u;
                out->gid = g;
                strlcpy(out->gecos, f[4], sizeof out->gecos);
                strlcpy(out->home, f[5], sizeof out->home);
                strlcpy(out->shell, f[6][0] ? f[6] : "/bin/sh", sizeof out->shell);
                found = true;
            }
        }
        line = next;
    }
    free(buf);
    return found;
}

/* A group's id by name, or its name by id (whichever is asked). */
static A_UNUSED bool a_getgr(const char *name, gid_t gid, gid_t *gid_out,
                             char *name_out, size_t cap)
{
    char *buf = a_slurp("/etc/group", 1 << 20, NULL);
    if (!buf) return false;
    bool found = false;
    for (char *line = buf; line && *line && !found; ) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        char *f[5];
        u32 g;
        if (line[0] != '#' && a_split(line, f, 4) == 4 && a_parse_id(f[2], &g) &&
            (name ? strcmp(f[0], name) == 0 : g == gid)) {
            if (gid_out) *gid_out = g;
            if (name_out) strlcpy(name_out, f[0], cap);
            found = true;
        }
        line = next;
    }
    free(buf);
    return found;
}

/* The groups a user is in: their primary group first, then every group
 * in /etc/group that lists them, duplicates dropped. Membership is read
 * from the file each time, so taking somebody out of group sudo takes
 * effect at once rather than at their next login - lp-privd reads it
 * the same way, and the two must never disagree about who is an
 * administrator. */
static A_UNUSED int a_groups(const char *user, gid_t primary, gid_t *out, int max)
{
    int n = 0;
    out[n++] = primary;
    char *buf = a_slurp("/etc/group", 1 << 20, NULL);
    if (!buf) return n;
    for (char *line = buf; line && *line; ) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        char *f[5];
        u32 g;
        if (line[0] != '#' && a_split(line, f, 4) == 4 && a_parse_id(f[2], &g)) {
            for (char *m = f[3]; m && *m; ) {
                char *comma = strchr(m, ',');
                if (comma) *comma = '\0';
                if (strcmp(m, user) == 0) {
                    bool dup = false;
                    for (int i = 0; i < n; i++) if (out[i] == g) dup = true;
                    if (!dup && n < max) out[n++] = g;
                }
                m = comma ? comma + 1 : NULL;
            }
        }
        line = next;
    }
    free(buf);
    return n;
}

static A_UNUSED bool a_in_group(const char *user, gid_t primary, const char *group)
{
    gid_t want;
    if (!a_getgr(group, 0, &want, NULL, 0)) return false;
    gid_t g[256];
    int n = a_groups(user, primary, g, 256);
    for (int i = 0; i < n; i++) if (g[i] == want) return true;
    return false;
}

/* ── Passwords ────────────────────────────────────────────────────── */

/* /data/shadow on a Pi whose /etc is rebuilt at every boot (passwd
 * writes there), /etc/shadow everywhere else. Either one is believed
 * only when root owns it and nobody else can write it. */
static A_UNUSED const char *a_shadow_path(void)
{
    static const char *const paths[2] = { "/data/shadow", "/etc/shadow" };
    for (int i = 0; i < 2; i++) {
        lp_stat_t st;
        if (lp_stat(paths[i], &st, false) < 0) continue;
        if ((st.mode & LP_S_IFMT) != LP_S_IFREG || st.uid != 0 || (st.mode & 022))
            continue;
        return paths[i];
    }
    return "/etc/shadow";
}

/* What to say when lp_shadow_check says no for a reason other than a
 * wrong password. NULL for LP_SHADOW_OK and LP_SHADOW_WRONG. */
static A_UNUSED const char *a_shadow_why(int r)
{
    switch (r) {
    case LP_SHADOW_LOCKED:      return "this account's password is locked";
    case LP_SHADOW_EMPTY:       return "this account has no password; set one with passwd first";
    case LP_SHADOW_NOUSER:      return "this account has no entry in the shadow file";
    case LP_SHADOW_UNSUPPORTED: return "this account's password is not a SHA-512 ($6$) hash;"
                                       " set it again with passwd";
    case LP_SHADOW_NOFILE:      return "cannot read the shadow file";
    default:                    return NULL;
    }
}

/* An account's password state without checking a password: what
 * lp_shadow_check would say before it got as far as the hash, and
 * LP_SHADOW_OK when there is a $6$ hash there to check against. su uses
 * it to say "root login is disabled" before asking for a password that
 * could never be right. */
static A_UNUSED int a_shadow_state(const char *user)
{
    long len;
    char *buf = a_slurp(a_shadow_path(), 1 << 20, &len);
    if (!buf) return LP_SHADOW_NOFILE;
    int res = LP_SHADOW_NOUSER;
    size_t ul = strlen(user);
    for (char *line = buf; line && *line; ) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (strncmp(line, user, ul) == 0 && line[ul] == ':') {
            char *hash = line + ul + 1;
            char *end = strchr(hash, ':');
            if (end) *end = '\0';
            res = !*hash ? LP_SHADOW_EMPTY
                : (*hash == '!' || *hash == '*') ? LP_SHADOW_LOCKED
                : strncmp(hash, "$6$", 3) != 0 ? LP_SHADOW_UNSUPPORTED
                : LP_SHADOW_OK;
            break;
        }
        line = next;
    }
    a_wipe(buf, (size_t)len);
    free(buf);
    return res;
}

/* The kernel's struct termios (TCGETS), the same 36 bytes on x86-64,
 * arm64 and arm; only the echo bits are touched. */
typedef struct { u32 iflag, oflag, cflag, lflag; u8 line, cc[19]; } a_termios_t;
#define A_TCGETS  0x5401
#define A_TCSETSF 0x5404
#define A_ECHO    0000010
#define A_ECHONL  0000100

static int         a_tty_fd = -1;
static a_termios_t a_tty_saved;
static bool        a_tty_changed;

static A_UNUSED void a_tty_restore(void)
{
    if (a_tty_changed && a_tty_fd >= 0) {
        lp_ioctl(a_tty_fd, A_TCSETSF, &a_tty_saved);
        a_tty_changed = false;
    }
}

static void a_on_signal(int sig)
{
    a_tty_restore();
    if (a_tty_fd >= 0) lp_write(a_tty_fd, "\n", 1);
    lp_exit(128 + sig);
}

/* Read a password: from the terminal with echo off, or from standard
 * input when from_stdin (sudo -S). The prompt goes to the terminal, or
 * to stderr for -S. Ctrl-C and friends put the echo back before dying -
 * a shell left with echo off after a cancelled sudo looks broken, and
 * people reboot machines over less. false when there was nothing to
 * read (no terminal, end of input). */
static A_UNUSED bool a_read_password(const char *prompt, bool from_stdin,
                                     char *pw, size_t cap)
{
    int fd;
    if (from_stdin) {
        fd = STDIN_FILENO;
    } else {
        long t = lp_open("/dev/tty", O_RDWR | O_CLOEXEC, 0);
        if (t < 0) return false;
        fd = (int)t;
    }
    a_tty_fd = fd;
    lp_signal_handler(SIGINT, a_on_signal);
    lp_signal_handler(SIGQUIT, a_on_signal);
    lp_signal_handler(SIGTERM, a_on_signal);
    lp_signal_handler(SIGHUP, a_on_signal);
    lp_signal_handler(SIGTSTP, a_on_signal);

    a_termios_t t;
    bool is_tty = lp_ioctl(fd, A_TCGETS, &t) == 0;
    if (is_tty) {
        a_tty_saved = t;
        t.lflag &= ~(u32)A_ECHO;
        t.lflag |= A_ECHONL;
        if (lp_ioctl(fd, A_TCSETSF, &t) == 0) a_tty_changed = true;
    }
    int out = from_stdin ? STDERR_FILENO : fd;
    lp_write(out, prompt, strlen(prompt));

    size_t n = 0;
    bool got = false;
    for (;;) {
        char c;
        long r = lp_read(fd, &c, 1);
        if (r <= 0) break;
        got = true;
        if (c == '\n' || c == '\r') break;
        if (n + 1 < cap) pw[n++] = c;
    }
    pw[n] = '\0';
    a_tty_restore();
    if (is_tty && !a_tty_changed && !from_stdin) {
        /* ECHONL printed the newline; nothing else to do. */
    }
    lp_signal_default(SIGINT);
    lp_signal_default(SIGQUIT);
    lp_signal_default(SIGTERM);
    lp_signal_default(SIGHUP);
    lp_signal_default(SIGTSTP);
    if (!from_stdin) lp_close(fd);
    a_tty_fd = -1;
    return got;
}

/* ── Becoming somebody ────────────────────────────────────────────── */

/* Groups, then gid, then uid - in that order, because after the uid
 * changes the process can no longer change the others - and then read
 * back and compared, because a setresuid that quietly did less than
 * asked would leave a "user" shell running as root. */
static A_UNUSED bool a_become(uid_t uid, gid_t gid, const gid_t *groups, int ngroups)
{
    if (a_setgroups(ngroups, groups) < 0) return false;
    if (a_setresgid(gid, gid, gid) < 0) return false;
    if (a_setresuid(uid, uid, uid) < 0) return false;
    u32 r, e, s;
    if (a_getresuid(&r, &e, &s) < 0 || r != uid || e != uid || s != uid) return false;
    if (a_getresgid(&r, &e, &s) < 0 || r != gid || e != gid || s != gid) return false;
    /* Past this point a setuid(0) must fail, or the drop did not happen. */
    if (uid != 0 && a_setresuid(0, 0, 0) == 0) return false;
    return true;
}

/* ── The controlling terminal ─────────────────────────────────────── */

typedef struct {
    u64  tty;            /* device number, 0 when there is none */
    int  sid;            /* session */
    int  ppid;
    u64  sid_start;      /* the session leader's start time, in ticks */
    u64  ppid_start;
    char name[32];       /* "pts/3", "tty1", "unknown" */
} a_tty_t;

/* Field n (1 = pid) of /proc/<pid>/stat, counted after the command name,
 * which may itself contain spaces and parentheses. */
static A_UNUSED bool a_proc_stat(int pid, int field, u64 *out)
{
    char path[48], buf[1024];
    if (pid <= 0) snprintf(path, sizeof path, "/proc/self/stat");
    else snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (proc_read(path, buf, sizeof buf) <= 0) return false;
    char *p = strrchr(buf, ')');
    if (!p) return false;
    p++;
    for (int f = 3; f <= field; f++) {
        while (*p == ' ') p++;
        if (!*p) return false;
        if (f == field) {
            bool neg = *p == '-';
            if (neg) p++;
            u64 v = 0;
            while (*p >= '0' && *p <= '9') v = v * 10 + (u64)(*p++ - '0');
            *out = neg ? (u64)0 - v : v;
            return true;
        }
        while (*p && *p != ' ') p++;
    }
    return false;
}

static A_UNUSED void a_tty_name(u64 dev, char *out, size_t cap)
{
    unsigned major = (unsigned)(((dev >> 8) & 0xfff) | ((dev >> 32) & ~0xfffu));
    unsigned minor = (unsigned)((dev & 0xff) | ((dev >> 12) & ~0xffu));
    if (dev == 0)                           snprintf(out, cap, "unknown");
    else if (major >= 136 && major <= 143)  snprintf(out, cap, "pts/%u", (major - 136) * 256 + minor);
    else if (major == 4 && minor < 64)      snprintf(out, cap, "tty%u", minor);
    else if (major == 4)                    snprintf(out, cap, "ttyS%u", minor - 64);
    else if (major == 204 && minor >= 64)   snprintf(out, cap, "ttyAMA%u", minor - 64);
    else if (major == 5 && minor == 1)      snprintf(out, cap, "console");
    else                                    snprintf(out, cap, "tty%u:%u", major, minor);
}

static A_UNUSED void a_tty_info(a_tty_t *t)
{
    memset(t, 0, sizeof *t);
    u64 v;
    if (a_proc_stat(0, 7, &v)) t->tty = v & 0xffffffffu;
    if (a_proc_stat(0, 6, &v)) t->sid = (int)v;
    if (a_proc_stat(0, 4, &v)) t->ppid = (int)v;
    if (t->sid > 0 && a_proc_stat(t->sid, 22, &v)) t->sid_start = v;
    if (t->ppid > 0 && a_proc_stat(t->ppid, 22, &v)) t->ppid_start = v;
    a_tty_name(t->tty, t->name, sizeof t->name);
}

/* ── The auth log ─────────────────────────────────────────────────── */

/* host name for log lines and prompts */
static A_UNUSED void a_hostname(char *out, size_t cap)
{
    char buf[128];
    if (proc_read("/proc/sys/kernel/hostname", buf, sizeof buf) > 0) {
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
        strlcpy(out, buf, cap);
    } else {
        strlcpy(out, "localhost", cap);
    }
}

/* One line in /var/log/auth.log, in the shape Debian's rsyslog writes
 * it, and the same line into the kernel log, where logd collects it on
 * a machine that has no /var/log. The file is opened O_NOFOLLOW and
 * created 0640 so a user cannot read who typed a wrong password. */
static A_UNUSED void a_authlog(const char *prog, const char *msg)
{
    char host[64], ts[64], line[1024];
    a_hostname(host, sizeof host);
    long ns;
    s64 now = lp_time_ns(&ns);
    lp_tm_t tm;
    lp_localtime(now, &tm);
    lp_strftime_lang(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S.%6N%:z", &tm, ns, LP_LANG_C);
    int n = snprintf(line, sizeof line, "%s %s %s[%d]: %s\n", ts, host, prog,
                     (int)lp_getpid(), msg);
    if (n > (int)sizeof line - 1) { n = (int)sizeof line - 1; line[n - 1] = '\n'; }
    long fd = lp_open("/var/log/auth.log",
                      O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | A_O_NOFOLLOW, 0640);
    if (fd >= 0) {
        a_stat_t st;
        if (a_fstat((int)fd, &st) == 0 && (st.mode & LP_S_IFMT) == LP_S_IFREG)
            lp_write((int)fd, line, (size_t)n);
        lp_close((int)fd);
    }
    lp_log(prog, msg);
}

#endif /* LP_AUTH_H */
