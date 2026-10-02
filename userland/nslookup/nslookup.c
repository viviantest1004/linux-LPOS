/* nslookup - what address does this name have.
 *
 *   nslookup <name>...
 *
 * One A record from the nameserver in /etc/resolv.conf. No MX, no TXT,
 * no reverse lookup: the question this answers is "is DNS working, and
 * where does this name point", and that is the whole of it.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "net.h"

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0) {
        printf("usage: nslookup <name>...\n");
        printf("  asks the nameserver in /etc/resolv.conf\n");
        return argc < 2 ? 2 : 0;
    }

    /* Say which nameserver, because "it does not resolve" nearly always
     * means there is no nameserver rather than no such name. */
    char conf[512];
    if (proc_read("/etc/resolv.conf", conf, sizeof(conf)) > 0) {
        char *ns = strstr(conf, "nameserver");
        if (ns) {
            char *nl = strchr(ns, '\n');
            if (nl) *nl = '\0';
            printf("using %s\n\n", ns);
        }
    } else {
        printf("no /etc/resolv.conf - run dhcp first\n\n");
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        u32 addr = net_resolve(argv[i]);
        if (addr == 0) {
            printf("%-30s not found\n", argv[i]);
            rc = 1;
            continue;
        }
        char buf[16];
        ipv4_format(addr, buf);
        printf("%-30s %s\n", argv[i], buf);
    }
    return rc;
}
