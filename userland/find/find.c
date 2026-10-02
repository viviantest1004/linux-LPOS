/* find - walk a directory tree.
 *
 *   find [path] [-name pattern] [-type f|d] [-maxdepth n]
 *
 * With no path it starts where you are. The pattern is the same * and ?
 * matching the shell does, and it is matched against the file's own name
 * rather than the whole path - which is what people mean by -name.
 *
 * Quote the pattern, or the shell expands it first and find never sees
 * it: find . -name "*.log"
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define DIRENT_RECLEN 16
#define DIRENT_TYPE   18
#define DIRENT_NAME   19
#define DT_DIR        4
#define DT_REG        8

static const char *want_name = NULL;
static char        want_type = 0;      /* 'f', 'd', or 0 for anything */
static int         max_depth = 64;

static bool match(const char *pat, const char *name)
{
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat)
                return true;
            for (const char *n = name; ; n++) {
                if (match(pat, n))
                    return true;
                if (!*n)
                    return false;
            }
        }
        if (!*name)
            return false;
        if (*pat != '?' && *pat != *name)
            return false;
        pat++;
        name++;
    }
    return *name == '\0';
}

static void consider(const char *path, const char *name, bool is_dir)
{
    if (want_type == 'f' && is_dir)  return;
    if (want_type == 'd' && !is_dir) return;
    if (want_name && !match(want_name, name)) return;
    printf("%s\n", path);
}

static void walk(const char *path, int depth)
{
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;                       /* not a directory, or not readable */

    char buf[8192];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;

        for (long off = 0; off < n; ) {
            char       *rec  = buf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            u8          type = *(u8 *)(rec + DIRENT_TYPE);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            char child[1024];
            snprintf(child, sizeof(child), "%s%s%s",
                     path, strcmp(path, "/") == 0 ? "" : "/", name);

            bool is_dir = (type == DT_DIR);
            /* Some filesystems answer DT_UNKNOWN; ask properly then. */
            if (type != DT_DIR && type != DT_REG)
                is_dir = lp_is_dir(child);

            consider(child, name, is_dir);

            if (is_dir && depth + 1 < max_depth)
                walk(child, depth + 1);
        }
    }
    lp_close((int)fd);
}

int main(int argc, char **argv)
{
    const char *root = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            want_name = argv[++i];
        } else if (strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
            want_type = argv[++i][0];
        } else if (strcmp(argv[i], "-maxdepth") == 0 && i + 1 < argc) {
            max_depth = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: find [path] [-name pattern] [-type f|d]"
                   " [-maxdepth n]\n");
            printf("  quote the pattern, or the shell expands it first:\n");
            printf("    find /data -name \"*.log\"\n");
            return 0;
        } else if (!root && argv[i][0] != '-') {
            root = argv[i];
        }
    }

    if (!root)
        root = ".";

    /* The starting point counts as a result too, the way find does. */
    const char *base = strrchr(root, '/');
    consider(root, base ? base + 1 : root, lp_is_dir(root));

    if (lp_is_dir(root))
        walk(root, 0);
    return 0;
}
