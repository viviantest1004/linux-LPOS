/* which - where a command would come from.
 *
 *   which [-a] name...
 *
 *   -a  every match on PATH, not just the first
 *
 * The question this answers is "why did that run the wrong thing", which
 * comes up the moment /bin has an overlay on it: the image's copy and a
 * copy installed onto /data both exist, and only one of them wins. It
 * prints the winner, so `which` and the shell always agree.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static bool runnable(const char *path)
{
    return lp_access(path, X_OK) == 0;
}

static bool look(const char *name, bool all)
{
    /* A name with a slash in it is a path already - PATH is not
     * consulted, exactly as the shell does not consult it. */
    if (strchr(name, '/')) {
        if (runnable(name)) { printf("%s\n", name); return true; }
        return false;
    }

    const char *path = getenv("PATH");
    if (!path || !*path)
        path = "/bin:/sbin:/usr/bin:/usr/sbin:/data/bin";

    bool found = false;
    const char *p = path;
    while (*p) {
        const char *end = strchr(p, ':');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        char full[512];
        if (len == 0) { strlcpy(full, ".", sizeof full); len = 1; }
        else {
            if (len >= sizeof full - 2) len = sizeof full - 2;
            memcpy(full, p, len);
            full[len] = '\0';
        }
        strlcat(full, "/", sizeof full);
        strlcat(full, name, sizeof full);

        if (runnable(full)) {
            printf("%s\n", full);
            found = true;
            if (!all) return true;
        }
        if (!end) break;
        p = end + 1;
    }
    return found;
}

int main(int argc, char **argv)
{
    bool all = false;
    int  first = 1;

    for (; first < argc; first++) {
        if (strcmp(argv[first], "-a") == 0) all = true;
        else if (strcmp(argv[first], "-h") == 0) {
            printf("usage: which [-a] name...\n");
            printf("  -a  show every match on PATH, not just the first\n");
            return 0;
        }
        else break;
    }

    if (first >= argc) {
        dprintf(STDERR_FILENO, "usage: which [-a] name...\n");
        return 2;
    }

    int missing = 0;
    for (int i = first; i < argc; i++)
        if (!look(argv[i], all)) {
            dprintf(STDERR_FILENO, "which: %s: not on PATH\n", argv[i]);
            missing++;
        }
    return missing ? 1 : 0;
}
