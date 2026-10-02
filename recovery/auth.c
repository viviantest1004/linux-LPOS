/* auth.c - who may open the recovery shell.
 *
 * The recovery shell is a root shell with the installed system mounted
 * read-write under it: everything on the machine, every account's files,
 * the password hashes. So it opens only for somebody who proves they
 * administer this computer - the rule macOS Recovery uses, and the one
 * COMMON.md "Administrator rights" sets:
 *
 *   - the password of an account in group `sudo` of the INSTALLED system
 *     (LP-ROOT, mounted read-only for the check), verified against that
 *     system's /etc/shadow with crypt6 - the same SHA-512 crypt sudo,
 *     login and lp-privd use; or
 *   - when the installed system cannot give us such an account - LP-ROOT
 *     does not mount, its /etc/group or /etc/shadow cannot be read, or it
 *     lists no sudo account whose password recovery can check - the
 *     recovery password: a $6$ hash the installer leaves in
 *     /etc/lp/recovery-password on LP-RECOVERY, root-owned, mode 0600.
 *     It is refused if anybody but root could read or change it.
 *
 * There is no third way in. An unauthenticated recovery shell is exactly
 * the thing a lost laptop must not have (an earlier design was stopped
 * for it).
 *
 * Every attempt is logged on LP-RECOVERY (account and result, never the
 * password). Three wrong passwords in a row cost a 30-second wait; the
 * count and the time it ends are kept on disk, so restarting the menu or
 * the machine does not reset them.
 */
#include "recovery.h"
#include "crypt6.h"

#define AUTH_STATE STATE_DIR "/auth-state"
#define TRIES      3
#define WAIT_S     30

static int fails;
static s64 until;

static void state_load(void)
{
    char buf[128];
    fails = 0;
    until = 0;
    if (!file_read(AUTH_STATE, buf, sizeof buf))
        return;
    char *p = strstr(buf, "fails=");
    if (p)
        fails = atoi(p + 6);
    p = strstr(buf, "until=");
    if (p)
        until = strtoll(p + 6, NULL, 10);
}

static void state_save(void)
{
    char buf[128];
    int n = snprintf(buf, sizeof buf, "fails=%d\nuntil=%lld\n", fails, (long long)until);
    file_write_atomic(AUTH_STATE, buf, (size_t)n, 0600);
}

int auth_lockout_left(void)
{
    state_load();
    s64 now = lp_time();
    if (until > now) {
        /* A clock that jumped backwards must not lock somebody out for
         * a day. */
        if (until - now > WAIT_S)
            until = now + WAIT_S;
        return (int)(until - now);
    }
    return 0;
}

int auth_tries_left(void)
{
    state_load();
    return TRIES - fails % TRIES;
}

/* Field `n` (0-based) of a colon-separated line, into out. */
static void field(const char *line, int n, char *out, size_t cap)
{
    while (n-- > 0) {
        line = strchr(line, ':');
        if (!line) {
            out[0] = 0;
            return;
        }
        line++;
    }
    size_t k = 0;
    while (line[k] && line[k] != ':' && line[k] != '\n' && k < cap - 1) {
        out[k] = line[k];
        k++;
    }
    out[k] = 0;
}

static void add_user(auth_info_t *ai, const char *name)
{
    if (!name[0] || ai->nusers >= AUTH_MAX_USERS)
        return;
    for (int i = 0; i < ai->nusers; i++)
        if (!strcmp(ai->users[i], name))
            return;
    strlcpy(ai->users[ai->nusers++], name, sizeof ai->users[0]);
}

/* The account's hash field in the installed system's shadow file. */
static bool shadow_hash(const char *shadow, const char *user, char *out, size_t cap)
{
    size_t ul = strlen(user);
    for (const char *l = shadow; *l; ) {
        if (!strncmp(l, user, ul) && l[ul] == ':') {
            field(l, 1, out, cap);
            return true;
        }
        const char *nl = strchr(l, '\n');
        if (!nl)
            break;
        l = nl + 1;
    }
    out[0] = 0;
    return false;
}

static bool recovery_pw_ok(char *hash, size_t cap)
{
    lp_stat_t st;
    if (lp_stat(RECOVERY_PW, &st, false) < 0)
        return false;
    if ((st.mode & LP_S_IFMT) != LP_S_IFREG || st.uid != 0 || (st.mode & 077)) {
        rlog("auth: %s ignored: must be a root-owned file with mode 0600", RECOVERY_PW);
        return false;
    }
    if (!file_read(RECOVERY_PW, hash, cap))
        return false;
    char *nl = strchr(hash, '\n');
    if (nl)
        *nl = 0;
    return !strncmp(hash, "$6$", 3);
}

void auth_prepare(auth_info_t *ai)
{
    static char group[16384], passwd[32768], shadow[32768];
    memset(ai, 0, sizeof *ai);
    auth_info_t all;
    memset(&all, 0, sizeof all);
    bool readable = sys_root_mounted() &&
                    file_read(MNT_ROOT "/etc/group", group, sizeof group) &&
                    file_read(MNT_ROOT "/etc/shadow", shadow, sizeof shadow);
    if (readable) {
        /* Members of sudo, and accounts whose primary group it is. */
        char gid[16] = "";
        for (const char *l = group; *l; ) {
            if (!strncmp(l, "sudo:", 5)) {
                char members[1024];
                field(l, 2, gid, sizeof gid);
                field(l, 3, members, sizeof members);
                char *m = members;
                while (*m) {
                    char *c = strchr(m, ',');
                    if (c)
                        *c = 0;
                    add_user(&all, m);
                    if (!c)
                        break;
                    m = c + 1;
                }
            }
            const char *nl = strchr(l, '\n');
            if (!nl)
                break;
            l = nl + 1;
        }
        if (gid[0] && file_read(MNT_ROOT "/etc/passwd", passwd, sizeof passwd)) {
            for (const char *l = passwd; *l; ) {
                char name[32], g[16];
                field(l, 0, name, sizeof name);
                field(l, 3, g, sizeof g);
                if (!strcmp(g, gid))
                    add_user(&all, name);
                const char *nl = strchr(l, '\n');
                if (!nl)
                    break;
                l = nl + 1;
            }
        }
        for (int i = 0; i < all.nusers; i++) {
            char h[256];
            if (shadow_hash(shadow, all.users[i], h, sizeof h) && !strncmp(h, "$6$", 3))
                add_user(ai, all.users[i]);
        }
    }
    char h[256];
    if (ai->nusers > 0)
        ai->mode = AUTH_ADMINS;
    else if (recovery_pw_ok(h, sizeof h))
        ai->mode = AUTH_RECOVERY;
    else if (all.nusers > 0) {
        /* Administrators exist but none has a password recovery can
         * check: show them, so the check can say why. */
        *ai = all;
        ai->mode = AUTH_ADMINS;
    } else
        ai->mode = AUTH_NOBODY;
    rlog("auth: installed system %s, %d administrator(s), mode %s",
         readable ? "readable" : "not readable", ai->nusers,
         ai->mode == AUTH_ADMINS ? "administrators" :
         ai->mode == AUTH_RECOVERY ? "recovery password" : "none");
}

int auth_check(const auth_info_t *ai, int user, const char *pw, int *wait_s,
               const char **why)
{
    *wait_s = auth_lockout_left();
    if (*wait_s > 0) {
        rlog("auth: attempt during the wait, refused");
        return 2;
    }
    const char *who = ai->mode == AUTH_RECOVERY ? "(recovery password)" :
                      (user >= 0 && user < ai->nusers) ? ai->users[user] : "?";
    int r;
    if (ai->mode == AUTH_RECOVERY) {
        char h[256];
        r = recovery_pw_ok(h, sizeof h) && lp_crypt6_verify(pw, h) ? LP_SHADOW_OK : LP_SHADOW_WRONG;
    } else if (ai->mode == AUTH_ADMINS && user >= 0 && user < ai->nusers)
        r = lp_shadow_check(MNT_ROOT "/etc/shadow", ai->users[user], pw);
    else
        r = LP_SHADOW_NOFILE;

    if (r == LP_SHADOW_UNSUPPORTED || r == LP_SHADOW_LOCKED || r == LP_SHADOW_EMPTY) {
        /* Not a wrong guess - the account cannot be checked at all - so it
         * does not count towards the wait. */
        *why = r == LP_SHADOW_UNSUPPORTED ? "not $6$" : "";
        rlog("auth: user=%s result=%s", who,
             r == LP_SHADOW_UNSUPPORTED ? "unsupported-hash" : "no-password");
        return r == LP_SHADOW_UNSUPPORTED ? 3 : 4;
    }
    state_load();
    if (r == LP_SHADOW_OK) {
        rlog("auth: user=%s result=ok", who);
        fails = 0;
        until = 0;
        state_save();
        return 0;
    }
    fails++;
    if (fails % TRIES == 0)
        until = lp_time() + WAIT_S;
    state_save();
    rlog("auth: user=%s result=wrong (%d in a row)", who, fails);
    *wait_s = fails % TRIES == 0 ? WAIT_S : 0;
    return 1;
}
