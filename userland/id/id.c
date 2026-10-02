/* id - which user this is, and which groups.
 *
 *   id            uid=0(root) gid=0(root) groups=0(root)
 *   id -u         just the number, which is what scripts ask for
 *   id -un        just the name
 *   id <user>     somebody in /etc/passwd
 *   groups        just the group names
 *
 * There is normally one user here and it is root. That is not a reason
 * to leave this out: a program dropped to another user with dropprivs,
 * or a script checking whether it is root before writing somewhere,
 * needs an answer, and "run whoami and hope" is not one.
 */
#include "types.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"
#include "unistd.h"

static void usage(int fd, const char *base)
{
    dprintf(fd, "Usage: %s [OPTION]... [USER]...\n"
                "Print user and group information for each specified USER,\n"
                "or (when USER omitted) for the current user.\n\n"
                "  -g, --group     print only the effective group ID\n"
                "  -G, --groups    print all group IDs\n"
                "  -n, --name      print a name instead of a number, for -ugG\n"
                "  -r, --real      print the real ID instead of the effective ID\n"
                "  -u, --user      print only the effective user ID\n"
                "  -z, --zero      delimit entries with NUL characters\n"
                "      --help      display this help and exit\n", base);
}

/* ",27(sudo),29(audio)" for each line of /etc/group whose member list
 * names `user`, skipping its primary group (already printed). */
static void print_member_groups(const char *user, gid_t primary)
{
    static char buf[16384];
    long fd = lp_open("/etc/group", O_RDONLY, 0);
    if (fd < 0)
        return;
    long n = lp_read((int)fd, buf, sizeof buf - 1);
    lp_close((int)fd);
    if (n <= 0)
        return;
    buf[n] = '\0';
    size_t ulen = strlen(user);
    for (char *line = buf; *line; ) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        /* name:x:gid:member,member */
        char *f[4] = { line, 0, 0, 0 };
        int k = 1;
        for (char *p = line; *p && k < 4; p++)
            if (*p == ':') {
                *p = '\0';
                f[k++] = p + 1;
            }
        if (k == 4 && f[2][0] && (gid_t)atoi(f[2]) != primary) {
            for (char *m = f[3]; *m; ) {
                char *c = strchr(m, ',');
                size_t len = c ? (size_t)(c - m) : strlen(m);
                if (len == ulen && !strncmp(m, user, len)) {
                    printf(",%d(%s)", atoi(f[2]), f[0]);
                    break;
                }
                if (!c)
                    break;
                m = c + 1;
            }
        }
        if (!nl)
            break;
        line = nl + 1;
    }
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "group", 0, 'g' }, { "groups", 0, 'G' }, { "name", 0, 'n' },
        { "real", 0, 'r' }, { "user", 0, 'u' }, { "zero", 0, 'z' },
        { "context", 0, 'Z' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    bool as_groups = (strcmp(base, "groups") == 0);

    bool want_u = false, want_g = false, want_G = false;
    bool names = false;
    char eol = '\n';

    lp_getopt_t o;
    lp_getopt_init(&o, argc, argv, "agGnruzZ", lo);
    for (int c; (c = lp_getopt(&o)) != -1; ) {
        switch (c) {
        case 'u': want_u = true; break;
        case 'g': want_g = true; break;
        case 'G': want_G = true; break;
        case 'n': names = true; break;
        case 'r': break;          /* no setuid here: real == effective */
        case 'a': case 'Z': break;
        case 'z': eol = '\0'; break;
        case 'H': usage(STDOUT_FILENO, base); return 0;
        default: lp_getopt_err(base, &o); return 1;
        }
    }

    if ((int)want_u + (int)want_g + (int)want_G > 1) {
        dprintf(STDERR_FILENO,
                "%s: cannot print \"only\" of more than one choice\n", base);
        return 1;
    }
    if (names && !(want_u || want_g || want_G) && !as_groups) {
        dprintf(STDERR_FILENO,
                "%s: cannot print only names or real IDs in default format\n", base);
        return 1;
    }

    lp_user_t u;
    bool known;

    if (o.ind < argc) {
        known = lp_user_by_name(argv[o.ind], &u);
        if (!known) {
            if (lp_voice() == LP_VOICE_GNU)
                dprintf(STDERR_FILENO, "%s: '%s': no such user\n", base, argv[o.ind]);
            else
                dprintf(STDERR_FILENO,
                        "%s: there is no user called \"%s\"\n", base, argv[o.ind]);
            return 1;
        }
    } else {
        uid_t me = (uid_t)lp_getuid();
        known = lp_user_by_uid(me, &u);
        if (!known) {
            /* Running as a uid with no line in /etc/passwd. Say the
             * number rather than nothing - that is the situation after
             * dropprivs to an id nobody created. */
            u.uid = me;
            u.gid = (gid_t)lp_getgid();
            u.name[0] = '\0';
        }
    }

    char gname[32];
    lp_group_name(u.gid, gname, sizeof gname);

    if (as_groups) { printf("%s%c", gname, eol); return 0; }

    if (want_u) {
        if (names && u.name[0]) printf("%s%c", u.name, eol);
        else                    printf("%d%c", (int)u.uid, eol);
        return 0;
    }
    if (want_g || want_G) {
        if (names) printf("%s%c", gname, eol);
        else       printf("%d%c", (int)u.gid, eol);
        return 0;
    }

    if (u.name[0])
        printf("uid=%d(%s) gid=%d(%s) groups=%d(%s)",
               (int)u.uid, u.name, (int)u.gid, gname, (int)u.gid, gname);
    else
        printf("uid=%d gid=%d(%s) groups=%d(%s)",
               (int)u.uid, (int)u.gid, gname, (int)u.gid, gname);
    /* Then every group /etc/group lists the account in, as GNU id does.
     * Printing the primary group alone made an administrator look as if
     * they were in nothing - not in sudo, not in audio - and sent people
     * looking for a fault in the session that was not there. */
    if (u.name[0])
        print_member_groups(u.name, u.gid);
    printf("\n");
    return 0;
}
