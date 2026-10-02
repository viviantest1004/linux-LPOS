/* basename - the last part of a path.
 *
 *   basename path [suffix]
 *   basename -a path...
 *   basename -s .txt path...
 *
 * Trailing slashes are removed first, so basename /data/logs/ is logs
 * and not an empty line - which is what scripts that build filenames
 * out of it depend on.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

static void one(const char *path, const char *suffix)
{
    char buf[1024];
    strlcpy(buf, path, sizeof buf);

    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/') buf[--n] = '\0';

    /* "/" and "//" are their own answer; there is no last component to
     * take. */
    const char *base = buf;
    char *slash = strrchr(buf, '/');
    if (slash && slash[1]) base = slash + 1;
    else if (slash && slash == buf && !slash[1]) base = "/";

    if (suffix && *suffix) {
        size_t bl = strlen(base), sl = strlen(suffix);
        /* Never strip the whole name: basename .txt .txt is .txt. */
        if (bl > sl && strcmp(base + bl - sl, suffix) == 0) {
            char cut[1024];
            strlcpy(cut, base, bl - sl + 1);
            printf("%s\n", cut);
            return;
        }
    }
    printf("%s\n", base);
}

int main(int argc, char **argv)
{
    bool many = false;
    const char *suffix = NULL;
    int i = 1;

    for (; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) many = true;
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            suffix = argv[++i];
            many = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: basename path [suffix]\n");
            printf("       basename -a path...\n");
            printf("       basename -s .txt path...\n");
            return 0;
        } else break;
    }

    if (i >= argc) {
        dprintf(STDERR_FILENO, "usage: basename path [suffix]\n");
        return 2;
    }

    if (many) {
        for (; i < argc; i++) one(argv[i], suffix);
    } else {
        one(argv[i], (i + 1 < argc) ? argv[i + 1] : NULL);
    }
    return 0;
}
