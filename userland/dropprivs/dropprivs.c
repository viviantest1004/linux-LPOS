/* dropprivs - run something as somebody who is not root.
 *
 *   dropprivs <uid> <program> [arguments...]
 *   dropprivs <uid>:<gid> <program> [arguments...]
 *
 * ── Why ──
 * Everything on this machine runs as root, because there is one account
 * and no login. That is fine for the system's own daemons. It is not
 * fine for a script that reads things off the network: an HTTP response,
 * an MQTT message, a sensor feed. A mistake in that script is not a
 * mistake in a script, it is root on the machine.
 *
 * So run it as somebody else:
 *
 *   # /etc/services
 *   dropprivs 1000 /data/python/bin/python3.12 /data/app.py
 *
 * The user does not need to exist in /etc/passwd for this - the kernel
 * only deals in numbers - but there is a "user" account at 1000 in the
 * image so that ls -l has a name to print. /data/user is owned by it.
 *
 * ── The order matters ──
 * setgid before setuid, always. After setuid(1000) the process can no
 * longer change its groups, so doing it the other way round leaves the
 * program running with root's group - which on this system is root's
 * access to everything.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"

int main(int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[1], "-h") == 0) {
        printf("usage: dropprivs <uid>[:<gid>] <program> [arguments...]\n\n");
        printf("Runs the program as that user instead of as root.\n");
        printf("For anything that handles input from the network: a\n");
        printf("mistake in it then costs one account, not the machine.\n\n");
        printf("  dropprivs 1000 python3 /data/app.py\n");
        return argc < 3 ? 2 : 0;
    }

    char spec[64];
    strlcpy(spec, argv[1], sizeof(spec));

    int uid = 0, gid = 0;
    char *colon = strchr(spec, ':');
    if (colon) {
        *colon = '\0';
        gid = atoi(colon + 1);
    }
    uid = atoi(spec);

    if (uid <= 0) {
        dprintf(STDERR_FILENO,
                "dropprivs: %s is not a user id to drop to.\n"
                "           0 is root, which is what we are dropping from.\n",
                argv[1]);
        return 2;
    }
    if (!colon)
        gid = uid;

    /* Groups first: after setuid this process may not change them, and
     * keeping root's supplementary groups would undo the whole point. */
    if (sys_call3(SYS_setgroups, 0, 0, 0) < 0) {
        /* Not fatal on its own - report it and carry on, because the
         * two calls below are what actually matter. */
        dprintf(STDERR_FILENO, "dropprivs: could not clear the groups\n");
    }

    if (sys_call1(SYS_setgid, gid) < 0) {
        dprintf(STDERR_FILENO, "dropprivs: cannot become group %d\n", gid);
        return 1;
    }

    if (sys_call1(SYS_setuid, uid) < 0) {
        dprintf(STDERR_FILENO, "dropprivs: cannot become user %d\n", uid);
        return 1;
    }

    /* Make sure it actually happened. If setuid quietly failed we would
     * be about to run somebody's program as root while telling them it
     * is not - which is worse than refusing. */
    if (lp_getuid() != uid) {
        dprintf(STDERR_FILENO,
                "dropprivs: still uid %d after asking for %d - refusing\n",
                lp_getuid(), uid);
        return 1;
    }

    extern char **environ;
    char path[512];

    /* Find it on PATH, the way the shell would. */
    if (strchr(argv[2], '/')) {
        strlcpy(path, argv[2], sizeof(path));
    } else {
        const char *p = getenv("PATH");
        if (!p || !*p)
            p = "/bin:/data/bin:/sbin:/usr/bin:/usr/sbin";

        bool found = false;
        while (*p && !found) {
            const char *end = strchr(p, ':');
            size_t dlen = end ? (size_t)(end - p) : strlen(p);
            if (dlen && dlen + 2 + strlen(argv[2]) < sizeof(path)) {
                memcpy(path, p, dlen);
                path[dlen] = '/';
                strlcpy(path + dlen + 1, argv[2], sizeof(path) - dlen - 1);
                if (lp_exists(path))
                    found = true;
            }
            if (!end) break;
            p = end + 1;
        }
        if (!found)
            strlcpy(path, argv[2], sizeof(path));
    }

    lp_execve(path, argv + 2, environ);

    dprintf(STDERR_FILENO, "dropprivs: cannot run %s\n", path);
    return 127;
}
