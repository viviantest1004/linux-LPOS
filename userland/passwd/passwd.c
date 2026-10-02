/* passwd - change a password.
 *
 *   passwd                 your own
 *   passwd bob             somebody else's (root only)
 *   passwd -S bob          what state their password is in
 *   passwd -l / -u / -d    lock, unlock, remove it
 *
 * ── The thing that matters more than everything else here ──
 * The account file is replaced by writing a temporary file next to it
 * and renaming that over the original. Never by opening the real file
 * with O_TRUNC.
 *
 * A passwd that is interrupted halfway - the board loses power, the
 * process is killed, the filesystem fills up - and leaves a truncated
 * password file has locked every account on the machine out of it,
 * including the one that could fix it. rename() is atomic: either the
 * old file is there in full or the new one is, and there is no third
 * state. That single property is why this program is written the way it
 * is, and it is the first thing to preserve in any change to it.
 *
 * ── Where the hashes live ──
 * /data/shadow, falling back to /etc/shadow for one an image ships. Same
 * rule as cron's table and for the same reason: /etc is unpacked from
 * the kernel image at every boot, so a hash written there is gone by
 * morning, and /data is the only writable half that survives. A first
 * write copies /etc/shadow forward into /data/shadow so nothing shipped
 * is lost.
 *
 * The format is shadow(5) exactly - name, hash, then last-change, min,
 * max, warn, inactive and expire in days - and the hash is real SHA-512
 * crypt, $6$, computed here from lp_digest. Not a lookalike: a line
 * written by this program verifies on Ubuntu and a line copied from
 * Ubuntu verifies here. A made-up hash format that merely looked like
 * $6$ would be the kind of false thing that is worse than a missing
 * feature.
 *
 * ── What checks it, today ──
 * Nothing yet. SSH here is public-key only with password authentication
 * compiled out of the server, there is no login program on the console,
 * and su is enforced by the kernel rather than by a password - useradd
 * and su both say so, and they are still right. What passwd does check
 * against the file is the current password it asks a non-root user for
 * before letting them change their own; that check is real. The file is
 * kept so that the account is the same account on both machines and so
 * that the first thing here that does want a password has something
 * true to ask.
 *
 * ── Reading the password ──
 * From standard input, with the terminal's echo turned off around it
 * (lp_term_cbreak, then lp_term_restore on every way out, including the
 * Ctrl-C one). GNU's passwd reads /dev/tty instead, so that a password
 * cannot be piped in; here it can, and that is deliberate - there is no
 * chpasswd on this system and setting a password from a provisioning
 * script would otherwise be impossible.
 *
 * Not here: -k, -i, -n, -w, -x (the aging fields), and -r. -R is a path
 * prefix rather than a real chroot(), which needs no privilege and has
 * the same effect on the only files this program touches.
 */
#include "crypt6.h"
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define PW_MAX       256
#define FILE_MAX     65536
#define DAY          86400

static const char *prog = "passwd";
static const char *rootdir = "";
static bool opt_quiet;

static char fbuf[FILE_MAX + 2];      /* the shadow file, as read */
static long flen;
static char obuf[FILE_MAX + 1024];   /* the one being written */
static long olen;

/* SHA-512 crypt is libc/src/crypt6.c, shared with sudo, login and the
 * administrator daemons. */

/* ── The files ────────────────────────────────────────────────────── */

static void under_root(char *buf, size_t n, const char *path)
{
    snprintf(buf, n, "%s%s", rootdir, path);
}

static long read_file(const char *path, char *buf, long max)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return fd;
    long used = 0;
    for (;;) {
        long r = lp_read((int)fd, buf + used, (size_t)(max - used));
        if (r <= 0) break;
        used += r;
        if (used >= max) break;
    }
    lp_close((int)fd);
    buf[used] = '\0';
    return used;
}

/* /data/shadow when there is one, otherwise /etc/shadow. */
static void shadow_read_path(char *buf, size_t n)
{
    char a[256], b[256];
    under_root(a, sizeof a, "/data/shadow");
    under_root(b, sizeof b, "/etc/shadow");
    strlcpy(buf, lp_exists(a) ? a : b, n);
}

/* Where a change goes. The writable half wins, so that a password set
 * on the machine is still set after the next boot. */
static void shadow_write_path(char *buf, size_t n)
{
    char datadir[256], a[256], b[256];
    under_root(datadir, sizeof datadir, "/data");
    under_root(a, sizeof a, "/data/shadow");
    under_root(b, sizeof b, "/etc/shadow");
    strlcpy(buf, (lp_exists(a) || lp_is_dir(datadir)) ? a : b, n);
}

typedef struct {
    char name[64];
    char pwfield[128];       /* field 2 of /etc/passwd */
    uid_t uid;
    bool found;
} pwent_t;

/* /etc/passwd read directly rather than through lp_user_by_name: this
 * needs field 2, which lp_user_t does not carry, and -R has to be able
 * to look in a different tree. lp_user_by_name is still the fallback
 * when there is no -R, so a user libc can see is never missed. */
static pwent_t pw_lookup(const char *name, int uid)
{
    pwent_t e;
    memset(&e, 0, sizeof e);

    char path[256];
    under_root(path, sizeof path, "/etc/passwd");
    static char pw[FILE_MAX + 2];
    long n = read_file(path, pw, FILE_MAX);

    for (long i = 0; i < n; ) {
        long j = i;
        while (j < n && pw[j] != '\n') j++;
        char line[512];
        long len = j - i;
        if (len > 0 && len < (long)sizeof line) {
            memcpy(line, pw + i, (size_t)len);
            line[len] = '\0';
            char *f[4] = { line, NULL, NULL, NULL };
            int k = 1;
            for (char *p = line; *p && k < 4; p++)
                if (*p == ':') { *p = '\0'; f[k++] = p + 1; }
            if (k >= 3 && f[1] && f[2]) {
                int this_uid = atoi(f[2]);
                if ((name && strcmp(f[0], name) == 0) ||
                    (!name && this_uid == uid)) {
                    strlcpy(e.name, f[0], sizeof e.name);
                    strlcpy(e.pwfield, f[1], sizeof e.pwfield);
                    e.uid = (uid_t)this_uid;
                    e.found = true;
                    return e;
                }
            }
        }
        i = j + 1;
    }

    if (!rootdir[0]) {
        lp_user_t u;
        bool ok = name ? lp_user_by_name(name, &u)
                       : lp_user_by_uid((uid_t)uid, &u);
        if (ok) {
            strlcpy(e.name, u.name, sizeof e.name);
            strlcpy(e.pwfield, "x", sizeof e.pwfield);
            e.uid = u.uid;
            e.found = true;
        }
    }
    return e;
}

/* One shadow line, split. Missing fields read as "". */
typedef struct { char *f[9]; char buf[1024]; } spent_t;

static void sp_split(spent_t *sp, const char *line)
{
    strlcpy(sp->buf, line, sizeof sp->buf);
    for (int i = 0; i < 9; i++)
        sp->f[i] = (char *)"";
    int k = 0;
    sp->f[k++] = sp->buf;
    for (char *p = sp->buf; *p && k < 9; p++)
        if (*p == ':') { *p = '\0'; sp->f[k++] = p + 1; }
}

/* Find the target's line in fbuf. Returns its start offset, or -1. */
static long sp_find(const char *name, long *end)
{
    size_t nlen = strlen(name);
    for (long i = 0; i < flen; ) {
        long j = i;
        while (j < flen && fbuf[j] != '\n') j++;
        if ((long)nlen < j - i && strncmp(fbuf + i, name, nlen) == 0 &&
            fbuf[i + (long)nlen] == ':') {
            *end = j;
            return i;
        }
        i = j + 1;
    }
    return -1;
}

static long days_now(void) { return lp_time() / DAY; }

/* ── Writing it, the part that must not go wrong ──
 *
 * A temporary file in the same directory, then rename over the target.
 * Same directory because rename only works within one filesystem, and a
 * copy-then-delete would reintroduce the window this exists to close. */
static bool write_atomically(const char *target)
{
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp%d", target, (int)lp_getpid());
    lp_unlink(tmp);                        /* a leftover from a crash */

    long fd = lp_open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        lp_diag(prog, "cannot create", NULL, "cannot create", tmp, (int)-fd);
        return false;
    }
    long off = 0;
    while (off < olen) {
        long w = lp_write((int)fd, obuf + off, (size_t)(olen - off));
        if (w <= 0) {
            lp_close((int)fd);
            lp_unlink(tmp);
            lp_diag(prog, "cannot write", NULL, "cannot write", tmp,
                    w < 0 ? (int)-w : 28 /* ENOSPC */);
            return false;
        }
        off += w;
    }
    lp_close((int)fd);

    /* The hashes reach the disk before anything points at them. Without
     * this a power cut just after the rename can leave the new name
     * attached to an empty file. */
    lp_sync();
    lp_chmod(tmp, 0600);

    long r = lp_rename(tmp, target);
    if (r < 0) {
        lp_unlink(tmp);
        lp_diag(prog, "cannot rename", NULL, "cannot rename", tmp, (int)-r);
        return false;
    }
    lp_sync();
    return true;
}

/* Replace the target's line with `newline`, or add it when there is
 * none, and put the result in place. */
static bool store_line(const char *name, const char *newline)
{
    char wpath[256];
    shadow_write_path(wpath, sizeof wpath);

    /* fbuf holds whatever shadow_read_path found, so when the first
     * change on a machine moves the file from /etc to /data every other
     * account moves with it. What is left behind in /etc/shadow is a
     * stale copy that the next boot removes along with the rest of /etc,
     * and nothing reads it again while /data/shadow exists. */
    olen = 0;
    bool replaced = false;
    for (long i = 0; i < flen; ) {
        long j = i;
        while (j < flen && fbuf[j] != '\n') j++;
        long len = j - i;
        size_t nlen = strlen(name);
        bool mine = (long)nlen < len && strncmp(fbuf + i, name, nlen) == 0 &&
                    fbuf[i + (long)nlen] == ':';
        if (mine) {
            olen += snprintf(obuf + olen, sizeof obuf - (size_t)olen,
                             "%s\n", newline);
            replaced = true;
        } else if (len > 0) {
            if (olen + len + 1 >= (long)sizeof obuf)
                return false;
            memcpy(obuf + olen, fbuf + i, (size_t)len);
            olen += len;
            obuf[olen++] = '\n';
        }
        i = j + 1;
    }
    if (!replaced)
        olen += snprintf(obuf + olen, sizeof obuf - (size_t)olen,
                         "%s\n", newline);

    return write_atomically(wpath);
}

/* Is the file this would write even writable? Worth asking before a
 * password is typed twice for nothing.
 *
 * On this machine the answer for anybody but root is no, and it is not a
 * misconfiguration: nothing here is setuid, and /etc/rc mounts /data
 * nosuid on purpose, so there is no way for an ordinary user's passwd to
 * hold the file open for writing. A user's password is changed by root
 * doing it for them. */
static bool shadow_writable(void)
{
    char path[256];
    shadow_write_path(path, sizeof path);
    char dir[256];
    strlcpy(dir, path, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) *slash = '\0';
    else strlcpy(dir, "/", sizeof dir);
    return lp_access(dir, W_OK) == 0;
}

static int store_failed(void)
{
    dprintf(STDERR_FILENO, "passwd: password unchanged\n");
    return 10;
}

/* ── Asking for a password ────────────────────────────────────────── */

static bool echo_off;
static lp_termios_t saved_term;

static void echo_stop(void)
{
    if (echo_off) {
        lp_term_restore(STDIN_FILENO, &saved_term);
        echo_off = false;
    }
}

/* Read one line with nothing shown. Returns false when the input ended
 * or Ctrl-C was pressed - the terminal is put back either way, because
 * a passwd that exits with echo still off leaves a shell nobody can
 * type into. */
static bool ask(const char *prompt, char *out, size_t n)
{
    dprintf(STDERR_FILENO, "%s", prompt);

    bool tty = lp_isatty(STDIN_FILENO);
    if (tty && lp_term_cbreak(STDIN_FILENO, &saved_term) == 0)
        echo_off = true;

    size_t used = 0;
    bool ok = true;
    for (;;) {
        char ch;
        long r = lp_read(STDIN_FILENO, &ch, 1);
        if (r <= 0) { ok = used > 0; break; }
        if (ch == '\n' || ch == '\r')
            break;
        if (ch == 3) {                     /* Ctrl-C: ISIG is off in cbreak */
            ok = false;
            break;
        }
        if (ch == 0x7F || ch == '\b') {
            if (used) used--;
            continue;
        }
        if (used < n - 1)
            out[used++] = ch;
    }
    out[used] = '\0';

    echo_stop();
    /* The Enter was not echoed either, so the next line would start in
     * the middle of the prompt. */
    if (tty)
        dprintf(STDERR_FILENO, "\n");
    return ok;
}

/* ── Reporting ────────────────────────────────────────────────────── */

static const char *pw_status(const char *hash)
{
    if (!hash || !hash[0])         return "NP";
    if (hash[0] == '!' || hash[0] == '*') return "L";
    return "P";
}

static long field_num(const char *s)
{
    if (!s || !s[0]) return -1;
    return strtol(s, NULL, 10);
}

static void print_status(const pwent_t *e)
{
    long end;
    long at = sp_find(e->name, &end);
    if (at < 0) {
        /* No shadow line: the status is whatever field 2 of /etc/passwd
         * says, which is what shadow-utils prints for the same file. */
        printf("%s %s\n", e->name, pw_status(e->pwfield));
        return;
    }
    char line[1024];
    long len = end - at;
    if (len >= (long)sizeof line) len = (long)sizeof line - 1;
    memcpy(line, fbuf + at, (size_t)len);
    line[len] = '\0';

    spent_t sp;
    sp_split(&sp, line);

    char date[16];
    long lstchg = field_num(sp.f[2]);
    if (lstchg < 0) {
        strlcpy(date, "never", sizeof date);
    } else {
        lp_tm_t t;
        lp_gmtime(lstchg * DAY, &t);
        snprintf(date, sizeof date, "%04d-%02d-%02d", t.year, t.mon, t.day);
    }

    printf("%s %s %s %ld %ld %ld %ld\n", e->name, pw_status(sp.f[1]), date,
           field_num(sp.f[3]), field_num(sp.f[4]),
           field_num(sp.f[5]), field_num(sp.f[6]));
}

static void usage(int fd)
{
    dprintf(fd, "Usage: passwd [options] [LOGIN]\n"
           "\n"
           "Options:\n"
           "  -a, --all                     report password status on all accounts\n"
           "  -d, --delete                  delete the password for the named account\n"
           "  -e, --expire                  force expire the password for the named account\n"
           "  -h, --help                    display this help message and exit\n"
           "  -l, --lock                    lock the password of the named account\n"
           "  -q, --quiet                   quiet mode\n"
           "  -R, --root CHROOT_DIR         directory to look in\n"
           "  -S, --status                  report password status on the named account\n"
           "  -u, --unlock                  unlock the password of the named account\n"
           "\n");
}

/* Rebuild a shadow line with a new hash. `lstchg` is the day to record
 * in field 3, or -1 to keep whatever is already there - lock, unlock and
 * delete leave that field alone, and only -e and a real change move it. */
static void rebuild(const char *name, const char *hash, long lstchg,
                    char *out, size_t n)
{
    long end;
    long at = sp_find(name, &end);
    spent_t sp;
    if (at < 0) {
        sp_split(&sp, name);               /* fields 2..9 come out empty */
    } else {
        char line[1024];
        long len = end - at;
        if (len >= (long)sizeof line) len = (long)sizeof line - 1;
        memcpy(line, fbuf + at, (size_t)len);
        line[len] = '\0';
        sp_split(&sp, line);
    }

    char days[24] = "";
    if (lstchg >= 0)
        snprintf(days, sizeof days, "%ld", lstchg);
    else if (lstchg == -1)
        strlcpy(days, sp.f[2], sizeof days);

    snprintf(out, n, "%s:%s:%s:%s:%s:%s:%s:%s:%s", name, hash, days,
             sp.f[3], sp.f[4], sp.f[5], sp.f[6], sp.f[7], sp.f[8]);
}

static int fail_unchanged(const char *why)
{
    dprintf(STDERR_FILENO, "%s: %s\n", prog, why);
    dprintf(STDERR_FILENO, "passwd: password unchanged\n");
    return 10;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "all", 0, 'a' }, { "delete", 0, 'd' }, { "expire", 0, 'e' },
        { "help", 0, 'h' }, { "lock", 0, 'l' }, { "quiet", 0, 'q' },
        { "root", 1, 'R' }, { "status", 0, 'S' }, { "unlock", 0, 'u' },
        { 0, 0, 0 }
    };
    bool aflg = false, dflg = false, eflg = false;
    bool lflg = false, Sflg = false, uflg = false;

    /* Everything after "--" is held aside: lp_getopt shuffles argv
     * without shrinking argc, so a "--" leaves the last operand in the
     * list twice and passwd would see two logins. */
    char *args[64], *tail[8];
    int nargs = 0, ntail = 0;
    bool past = false;
    for (int i = 0; i < argc && nargs < 64 && ntail < 8; i++) {
        if (i && past)                      { tail[ntail++] = argv[i]; continue; }
        if (i && strcmp(argv[i], "--") == 0) { past = true;            continue; }
        args[nargs++] = argv[i];
    }
    argc = nargs;
    argv = args;

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "adehlqR:Su", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'a': aflg = true; break;
        case 'd': dflg = true; break;
        case 'e': eflg = true; break;
        case 'l': lflg = true; break;
        case 'q': opt_quiet = true; break;
        case 'R': rootdir = g.arg; break;
        case 'S': Sflg = true; break;
        case 'u': uflg = true; break;
        case 'h': usage(STDOUT_FILENO); return 0;
        default:  lp_getopt_err(prog, &g); return 6;
        }
    }

    const char *login = NULL;
    int operands = (argc - g.ind) + ntail;
    if (operands == 1)
        login = (argc - g.ind) ? argv[g.ind] : tail[0];
    if (operands > 1 || (aflg && !Sflg)) {
        usage(STDERR_FILENO);
        return 2;
    }

    int me = lp_getuid();
    bool amroot = (me == 0);

    /* Not "unless you are root": shadow-utils refuses for root too, and
     * a root with no line of its own in /etc/passwd is a broken file
     * that this program should say so about rather than write to. */
    pwent_t self = pw_lookup(NULL, me);
    if (!self.found) {
        dprintf(STDERR_FILENO, "%s: Cannot determine your user name.\n", prog);
        return 1;
    }

    /* Order matters and it is the shadow-utils order: what an
     * unprivileged caller is not allowed to do at all comes before
     * whether the account exists, so `passwd root` from a user does not
     * become a way to ask which accounts there are. */
    if (!amroot && (dflg || eflg || lflg || uflg || aflg)) {
        dprintf(STDERR_FILENO, "%s: Permission denied.\n", prog);
        return 1;
    }
    if (!amroot && login && strcmp(login, self.name) != 0) {
        dprintf(STDERR_FILENO,
                "%s: You may not view or modify password information for"
                " %s.\n", prog, login);
        return 1;
    }

    pwent_t target = login ? pw_lookup(login, -1) : self;
    if (!target.found) {
        dprintf(STDERR_FILENO, "%s: user '%s' does not exist\n", prog, login);
        return 1;
    }

    char rpath[256];
    shadow_read_path(rpath, sizeof rpath);
    flen = read_file(rpath, fbuf, FILE_MAX);
    if (flen < 0)
        flen = 0;                          /* no shadow file yet */
    /* Everything below rewrites the whole file from this buffer, so a
     * file that did not fit in it would be silently shortened - the
     * accounts past the cut would simply stop existing. Refusing is the
     * only safe answer; a partial rewrite is the failure this program
     * exists to avoid. */
    if (flen >= FILE_MAX) {
        dprintf(STDERR_FILENO,
                "%s: %s is larger than %d bytes and will not be rewritten"
                " here\n", prog, rpath, FILE_MAX);
        return 10;
    }

    if (aflg) {
        char path[256];
        under_root(path, sizeof path, "/etc/passwd");
        static char pw[FILE_MAX + 2];
        long n = read_file(path, pw, FILE_MAX);
        if (n < 0) n = 0;
        for (long i = 0; i < n; ) {
            long j = i;
            while (j < n && pw[j] != '\n') j++;
            long len = j - i;
            const char *line = pw + i;
            i = j + 1;
            if (len <= 0 || len >= 512 || line[0] == '#') continue;

            pwent_t e;
            memset(&e, 0, sizeof e);
            const char *c1 = line, *c2;
            while (c1 < line + len && *c1 != ':') c1++;
            if (c1 == line + len) continue;
            for (c2 = c1 + 1; c2 < line + len && *c2 != ':'; c2++) { }
            strlcpy(e.name, line, (size_t)(c1 - line) + 1);
            strlcpy(e.pwfield, c1 + 1, (size_t)(c2 - c1 - 1) + 1);
            e.found = true;
            print_status(&e);
        }
        return 0;
    }

    if (Sflg) {
        print_status(&target);
        return 0;
    }

    long end;
    long at = sp_find(target.name, &end);
    char cur[1024] = "";
    if (at >= 0) {
        long len = end - at;
        if (len >= (long)sizeof cur) len = (long)sizeof cur - 1;
        memcpy(cur, fbuf + at, (size_t)len);
        cur[len] = '\0';
    }
    spent_t sp;
    sp_split(&sp, at >= 0 ? cur : target.name);
    /* With no shadow line the old token is field 2 of /etc/passwd -
     * except for a plain "x", which is not a hash at all, only a note
     * saying the hash is in the shadow file. Carrying that across would
     * make `passwd -l` write "!x" and call it a locked password. */
    const char *oldhash = at >= 0 ? sp.f[1] : target.pwfield;
    if (at < 0 && strcmp(oldhash, "x") == 0)
        oldhash = "";

    char line[1024];

    if (dflg) {
        rebuild(target.name, "", -1, line, sizeof line);
        if (!store_line(target.name, line))
            return store_failed();
        if (!opt_quiet) printf("%s: password changed.\n", prog);
        return 0;
    }

    if (lflg) {
        char locked[512];
        if (oldhash[0] == '!')
            strlcpy(locked, oldhash, sizeof locked);
        else
            snprintf(locked, sizeof locked, "!%s", oldhash);
        rebuild(target.name, locked, -1, line, sizeof line);
        if (!store_line(target.name, line))
            return store_failed();
        if (!opt_quiet) printf("%s: password changed.\n", prog);
        return 0;
    }

    if (uflg) {
        const char *bare = oldhash;
        while (*bare == '!') bare++;
        if (!*bare) {
            dprintf(STDERR_FILENO,
                    "%s: unlocking the password would result in a"
                    " passwordless account.\n"
                    "You should set a password with usermod -p to unlock the"
                    " password of this account.\n", prog);
            return 3;
        }
        rebuild(target.name, bare, -1, line, sizeof line);
        if (!store_line(target.name, line))
            return store_failed();
        if (!opt_quiet) printf("%s: password changed.\n", prog);
        return 0;
    }

    if (eflg) {
        rebuild(target.name, oldhash, 0, line, sizeof line);
        if (!store_line(target.name, line))
            return store_failed();
        if (!opt_quiet) printf("%s: password changed.\n", prog);
        return 0;
    }

    /* ── setting a password ── */
    if (!shadow_writable()) {
        char path[256];
        shadow_write_path(path, sizeof path);
        dprintf(STDERR_FILENO,
                "%s: cannot write %s.\n", prog, path);
        if (!amroot)
            dprintf(STDERR_FILENO,
                    "%s:   Nothing on this machine is setuid and /data is"
                    " mounted nosuid,\n"
                    "%s:   so only root can change a password here.\n",
                    prog, prog);
        dprintf(STDERR_FILENO, "passwd: password unchanged\n");
        return 10;
    }

    if (!amroot) {
        if (!opt_quiet)
            printf("Changing password for %s.\n", target.name);
        char old[PW_MAX];
        if (!ask("Current password: ", old, sizeof old))
            return fail_unchanged("Authentication token manipulation error");
        if (!lp_crypt6_verify(old, oldhash))
            return fail_unchanged("Authentication token manipulation error");
    }

    char chosen[PW_MAX] = "";
    bool got = false;
    for (int try = 0; try < 3 && !got; try++) {
        char a[PW_MAX], b[PW_MAX];
        if (!ask("New password: ", a, sizeof a))
            return fail_unchanged("Authentication token manipulation error");
        if (!a[0]) {
            dprintf(STDERR_FILENO, "No password has been supplied.\n");
            continue;
        }
        /* Short passwords: root is warned and allowed, because root is
         * setting up a machine and knows. Anybody else is refused, which
         * is what the check is for. */
        if (strlen(a) < 8) {
            dprintf(STDERR_FILENO, "You must choose a longer password.\n");
            if (!amroot)
                continue;
        }
        if (!ask("Retype new password: ", b, sizeof b))
            return fail_unchanged("Authentication token manipulation error");
        if (strcmp(a, b) != 0) {
            dprintf(STDERR_FILENO, "Sorry, passwords do not match.\n");
            continue;
        }
        strlcpy(chosen, a, sizeof chosen);
        got = true;
    }
    if (!got)
        return fail_unchanged("Have exhausted maximum number of retries for"
                              " service");

    char hash[256];
    if (!lp_crypt6_new(chosen, hash, sizeof hash))
        return fail_unchanged("Authentication token manipulation error");
    rebuild(target.name, hash, days_now(), line, sizeof line);
    if (!store_line(target.name, line))
        return store_failed();

    if (!opt_quiet)
        printf("passwd: password updated successfully\n");
    return 0;
}
