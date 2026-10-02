/* hostid - the 32-bit number that is supposed to identify this machine.
 *
 * It is eight hex digits and it is older than most of the software that
 * still asks for it. Licence managers and a few daemons key on it, so it
 * has to exist and it has to give the same answer twice.
 *
 * Where the number comes from, in the order GNU's does:
 *
 *   /etc/hostid   four raw bytes, read in the machine's own byte order.
 *                 If that file is there it wins, and it is the only way
 *                 to give this machine an id that is actually its own.
 *   the hostname  looked up in /etc/hosts for an IPv4 address, whose two
 *                 16-bit halves are then swapped. That swap is not a
 *                 checksum or an obfuscation; it is what the original
 *                 implementation did, and every hostid since has copied
 *                 it byte for byte, which is why 127.0.0.1 prints as
 *                 007f0100 here and on Ubuntu.
 *   nothing       00000000, the same as GNU when the name cannot be
 *                 resolved.
 *
 * What this does NOT do is ask a nameserver. glibc's gethostid falls
 * through to DNS when /etc/hosts has no answer; a machine identifier
 * that changes depending on whether the network is up is worse than one
 * that says zero, and every machine that has a hostname worth using has
 * it in /etc/hosts.
 *
 * The half-swap is done on the address as it sits in memory, which makes
 * the printed value little-endian-specific. Both machines this system
 * builds for are little endian, and so is the Ubuntu it is compared
 * against, so the numbers line up.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "net.h"

static bool read_hostid_file(u32 *out)
{
    long fd = lp_open("/etc/hostid", O_RDONLY, 0);
    if (fd < 0)
        return false;
    u8 raw[4];
    long n = lp_read((int)fd, raw, sizeof raw);
    lp_close((int)fd);
    if (n != (long)sizeof raw)
        return false;
    /* glibc reads the file straight into an int32_t, so the bytes are in
     * the machine's order, not the network's. */
    *out = (u32)raw[0] | ((u32)raw[1] << 8) | ((u32)raw[2] << 16) |
           ((u32)raw[3] << 24);
    return true;
}

static bool same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb)
            return false;
    }
    return *a == '\0' && *b == '\0';
}

/* The address /etc/hosts gives this name, in network byte order. */
static bool hosts_lookup(const char *name, u32 *addr)
{
    long fd = lp_open("/etc/hosts", O_RDONLY, 0);
    if (fd < 0)
        return false;

    char line[512];
    bool found = false;
    while (!found && readrec((int)fd, line, sizeof line, '\n', NULL) >= 0) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char *p = line;
        char *field[16];
        int   nf = 0;
        while (*p && nf < 16) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            field[nf++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
        for (int i = 1; i < nf; i++) {
            if (!same_name(field[i], name))
                continue;
            /* An IPv6 line for the same name is not an error, just not
             * an answer this command can use. */
            if (ipv4_parse(field[0], addr))
                found = true;
            break;
        }
    }
    lp_close((int)fd);
    return found;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = { { "help", 0, 'H' }, { 0, 0, 0 } };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "", lo);

    for (int c; (c = lp_getopt(&g)) != -1; ) {
        if (c == 'H') {
            printf("Usage: hostid [OPTION]\n"
                   "Print the numeric identifier (in hexadecimal) for the "
                   "current host.\n\n"
                   "      --help     display this help and exit\n");
            return 0;
        }
        lp_getopt_err("hostid", &g);
        return 1;
    }

    if (g.ind < argc) {
        dprintf(STDERR_FILENO, "hostid: extra operand '%s'\n", argv[g.ind]);
        dprintf(STDERR_FILENO, "Try 'hostid --help' for more information.\n");
        return 1;
    }

    u32 id = 0;
    if (!read_hostid_file(&id)) {
        char host[256];
        long n = proc_read("/proc/sys/kernel/hostname", host, sizeof host);
        if (n > 0) {
            char *nl = strchr(host, '\n');
            if (nl) *nl = '\0';
            u32 addr = 0;
            if (host[0] && hosts_lookup(host, &addr))
                id = (addr << 16) | (addr >> 16);
        }
    }

    printf("%08x\n", id);
    return 0;
}
