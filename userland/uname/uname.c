/* uname - what this system is.
 *
 *   uname            the kernel name
 *   uname -a         everything
 *   uname -r         the release
 *   uname -m         the machine
 *
 * From the uname syscall, which fills a struct of six fixed-size fields
 * of 65 bytes each: sysname, nodename, release, version, machine and
 * domainname.
 */
#include "types.h"
#include "osname.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define FIELD 65
#define SYSNAME  (0 * FIELD)
#define NODENAME (1 * FIELD)
#define RELEASE  (2 * FIELD)
#define VERSION  (3 * FIELD)
#define MACHINE  (4 * FIELD)

int main(int argc, char **argv)
{
    char uts[FIELD * 6];
    memset(uts, 0, sizeof(uts));

    if (lp_uname(uts) < 0) {
        dprintf(STDERR_FILENO, "uname: failed\n");
        return 1;
    }

    bool all = false, rel = false, mach = false, node = false, ver = false;
    bool sys = false, osname = false;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') continue;
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
            case 'a': all = true; break;
            case 's': sys = true; break;
            case 'r': rel = true; break;
            case 'm': mach = true; break;
            case 'n': node = true; break;
            case 'v': ver = true; break;
            case 'o': osname = true; break;
            case 'h':
                printf("usage: uname [-asrmnvo]\n");
                printf("  -a all          -s kernel name (the default)\n");
                printf("  -r release      -m machine\n");
                printf("  -n hostname     -v version\n");
                printf("  -o operating system\n");
                return 0;
            default:
                dprintf(STDERR_FILENO, "uname: unknown option -%c\n", *o);
                return 2;
            }
        }
    }

    if (all) {
        printf("%s %s %s %s %s\n",
               uts + SYSNAME, uts + NODENAME, uts + RELEASE,
               uts + VERSION, uts + MACHINE);
        return 0;
    }

    if (!sys && !rel && !mach && !node && !ver && !osname) {
        printf("%s\n", uts + SYSNAME);
        return 0;
    }

    /* The order is the one uname(1) prints in, not the order the flags
     * were typed. Anything reading this output - and things do; pip's
     * vendored distro module runs "uname -rs" - expects that order. */
    bool first = true;
    if (sys)  { printf("%s%s", first ? "" : " ", uts + SYSNAME);  first = false; }
    if (node) { printf("%s%s", first ? "" : " ", uts + NODENAME); first = false; }
    if (rel)  { printf("%s%s", first ? "" : " ", uts + RELEASE);  first = false; }
    if (ver)  { printf("%s%s", first ? "" : " ", uts + VERSION);  first = false; }
    if (mach) { printf("%s%s", first ? "" : " ", uts + MACHINE);  first = false; }
    /* There is no field for this in the kernel's struct; it is what the
     * userland is, and this one is not GNU. */
    if (osname) { printf("%s%s", first ? "" : " ", LP_OS_NAME); first = false; }
    printf("\n");
    return 0;
}
