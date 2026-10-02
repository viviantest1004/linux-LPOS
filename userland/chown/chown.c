/* chown - change who owns a file.
 *
 *   chown [-R] <user>[:<group>] <file>...
 *   chgrp [-R] <group> <file>...
 *
 * The same binary answers to both names; chgrp is chown with the user
 * left out, which is exactly what the kernel call takes.
 *
 * Names are looked up in /etc/passwd and /etc/group. A number is taken
 * as a number, so a user that does not exist in those files can still
 * be given a file - which matters when the owner was created on another
 * machine and only the number came with the disk.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static bool recursive = false;
static bool quiet     = false;
static int  failures  = 0;

static bool all_digits(const char *s)
{
    if (!*s) return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return false;
    return true;
}

/* dirent, as getdents64 lays it out */
#define DIRENT_RECLEN 16
#define DIRENT_TYPE   18
#define DIRENT_NAME   19
#define DT_DIR         4

static void apply(const char *path, uid_t uid, gid_t gid)
{
    long rc = lp_chown(path, uid, gid);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "chown: %s: %s (%ld)\n", path,
                -rc == 1  ? "not permitted" :
                -rc == 2  ? "no such file" : "failed", -rc);
        failures++;
        return;
    }
    if (!quiet)
        printf("%s\n", path);

    if (!recursive || !lp_is_dir(path))
        return;

    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;

    char buf[4096];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (long off = 0; off < n; ) {
            char *rec  = buf + off;
            u16   len  = *(u16 *)(rec + DIRENT_RECLEN);
            char *name = rec + DIRENT_NAME;
            if (len == 0)
                break;
            off += len;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            char child[512];
            snprintf(child, sizeof child, "%s/%s", path, name);
            apply(child, uid, gid);
        }
    }
    lp_close((int)fd);
}

int main(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    bool as_chgrp = (strcmp(base, "chgrp") == 0);

    int first = 1;
    for (; first < argc; first++) {
        if (strcmp(argv[first], "-R") == 0)      recursive = true;
        else if (strcmp(argv[first], "-q") == 0) quiet = true;
        else if (strcmp(argv[first], "-h") == 0) {
            printf("usage: %s [-R] [-q] %s <file>...\n", base,
                   as_chgrp ? "<group>" : "<user>[:<group>]");
            printf("  -R  everything underneath as well\n");
            printf("  -q  do not list what was changed\n\n");
            printf("Names come from /etc/passwd and /etc/group. A number\n");
            printf("is used as it stands, so a file can be given to an\n");
            printf("owner this machine has never heard of - which is what\n");
            printf("a disk from somewhere else needs.\n");
            return 0;
        }
        else break;
    }

    if (argc - first < 2) {
        dprintf(STDERR_FILENO, "usage: %s [-R] %s <file>...\n", base,
                as_chgrp ? "<group>" : "<user>[:<group>]");
        return 2;
    }

    /* Who to. -1 means "leave this one alone", which is what the kernel
     * takes and what makes chgrp the same call. */
    uid_t uid = (uid_t)-1;
    gid_t gid = (gid_t)-1;

    char spec[96];
    strlcpy(spec, argv[first], sizeof spec);
    char *group = strchr(spec, ':');
    if (group) *group++ = '\0';

    if (as_chgrp) {
        group = spec;
    } else if (spec[0]) {
        if (all_digits(spec)) {
            uid = (uid_t)atoi(spec);
        } else {
            lp_user_t u;
            if (!lp_user_by_name(spec, &u)) {
                dprintf(STDERR_FILENO,
                        "chown: there is no user called \"%s\".\n"
                        "       'id %s' or /etc/passwd says who there is.\n",
                        spec, spec);
                return 1;
            }
            uid = u.uid;
            /* "chown alex file" with no group also moves the group to
             * that user's own, which is what people expect and what the
             * name-only form means everywhere else. */
            if (!group) gid = u.gid;
        }
    }

    if (group && group[0]) {
        if (all_digits(group)) {
            gid = (gid_t)atoi(group);
        } else if (!lp_group_by_name(group, &gid)) {
            dprintf(STDERR_FILENO,
                    "%s: there is no group called \"%s\".\n"
                    "%s: /etc/group says which there are.\n",
                    base, group, base);
            return 1;
        }
    }

    for (int i = first + 1; i < argc; i++)
        apply(argv[i], uid, gid);

    return failures ? 1 : 0;
}
