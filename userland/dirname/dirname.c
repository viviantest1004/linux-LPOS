/* dirname - the path without its last part.
 *
 *   dirname path...
 *
 * A path with no slash gives ".", because that is the directory the
 * name is in and scripts concatenate the answer with a slash.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

static void one(const char *path)
{
    char buf[1024];
    strlcpy(buf, path, sizeof buf);

    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/') buf[--n] = '\0';

    char *slash = strrchr(buf, '/');
    if (!slash)          { printf(".\n");  return; }
    if (slash == buf)    { printf("/\n");  return; }

    *slash = '\0';
    /* Collapse the trailing slashes the cut may have exposed. */
    n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/') buf[--n] = '\0';
    printf("%s\n", buf);
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0) {
        printf("usage: dirname path...\n");
        return argc < 2 ? 2 : 0;
    }
    for (int i = 1; i < argc; i++) one(argv[i]);
    return 0;
}
