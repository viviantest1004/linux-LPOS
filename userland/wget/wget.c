/* wget - download a file.
 *
 *   wget <url> [file]
 *   wget -O <file> <url>
 *
 * http:// and https:// both work, in this process. The TLS is BearSSL
 * compiled into our libc and the root certificates are compiled in with
 * it, so there is nothing to install and no certificate file to be
 * missing - see libc/src/tls.c.
 *
 * Redirects are followed, up to five.
 *
 * With no file name given, the last part of the URL path is used.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "net.h"

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0) {
        printf("usage: wget <url> [file]\n");
        printf("       wget -O <file> <url>\n");
        printf("       wget -O- <url>          to standard output\n");
        printf("  https:// works, and the certificate is checked\n");
        printf("  against the root list built into this system.\n");
        printf("  Redirects are followed.\n");
        return argc < 2 ? 2 : 0;
    }

    const char *url  = NULL;
    const char *dest = NULL;
    bool quiet = false;

    /* -O is accepted because it is what everybody types.
     *
     * This only ever took its arguments positionally, and `wget -O out
     * url` therefore treated "-O" as the URL and printed "-O: this is
     * not an http:// or https:// URL" - which reads as the URL being
     * rejected rather than the option not existing. Every other wget on
     * earth spells it this way; refusing it taught nobody anything. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-O-") == 0 || strcmp(argv[i], "-qO-") == 0) {
            /* 붙여 쓴 꼴. `wget -O- url | grep` 이 이 도구로 하는 일의
             * 거의 전부이고, 그때 아무도 빈칸을 넣지 않는다. */
            dest = "-";
        } else if (strcmp(argv[i], "-O") == 0) {
            if (i + 1 >= argc) {
                dprintf(STDERR_FILENO, "wget: -O needs a file name after it\n");
                return 2;
            }
            dest = argv[++i];
        } else if (strcmp(argv[i], "-q") == 0) {
            quiet = true;
        } else if (!url) {
            url = argv[i];
        } else if (!dest) {
            dest = argv[i];
        }
    }

    if (!url) {
        dprintf(STDERR_FILENO, "wget: no URL given\n");
        return 2;
    }

    char name[256];
    if (!dest) {
        /* The last path component, or index.html when there is none. */
        const char *slash = strrchr(url, '/');
        if (slash && slash[1])
            strlcpy(name, slash + 1, sizeof(name));
        else
            strlcpy(name, "index.html", sizeof(name));
        dest = name;
    }

    /* 표준출력으로 보낼 때는 아무 말도 하지 않는다. 진행 상황 한 줄이
     * 파이프 저쪽의 첫 줄이 되어 버리기 때문이다. */
    bool to_stdout = dest[0] == '-' && dest[1] == '\0';
    if (!quiet && !to_stdout)
        printf("wget: %s -> %s\n", url, dest);

    long n = net_http_get(url, dest);
    if (n < 0)
        return 1;

    if (quiet || to_stdout)
        return 0;

    if (n >= 1048576)
        printf("wget: %lld MB\n", (long long)(n / 1048576));
    else if (n >= 1024)
        printf("wget: %lld KB\n", (long long)(n / 1024));
    else
        printf("wget: %lld bytes\n", (long long)n);
    return 0;
}
