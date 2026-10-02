/* getconf - the numbers the system will not budge on.
 *
 *   getconf PAGE_SIZE           one value
 *   getconf NAME_MAX /data      a value that depends on the filesystem
 *   getconf -a                  everything this system can answer
 *
 * Configure scripts and shell scripts ask this before they size a
 * buffer or split a command line, so the answers have to be the real
 * ones, measured here, not numbers copied out of somebody's header:
 *
 *   PAGE_SIZE, CLK_TCK      from the auxiliary vector the kernel puts on
 *                           our own stack at exec - the same place the C
 *                           library reads them, so they cannot disagree
 *                           with the kernel that is actually running.
 *   ARG_MAX                 a quarter of the stack limit, floored at
 *                           128K and capped at 6M. That is not a
 *                           constant: raise or lower `ulimit -s` and the
 *                           length of command line execve will accept
 *                           moves with it, which is exactly why xargs
 *                           asks.
 *   OPEN_MAX                the RLIMIT_NOFILE soft limit, likewise.
 *   NAME_MAX                from statfs on the path given, because 255
 *                           on ext4 and 255 on vfat happen to agree but
 *                           are not the same fact.
 *   _NPROCESSORS_*          the cpu lists in /sys/devices/system/cpu.
 *
 * The variables this does not know print GNU's "Unrecognized variable"
 * and exit 2. That list is long: glibc's getconf -a prints some three
 * hundred lines, most of them compile-time answers about glibc itself -
 * which POSIX option groups it claims, which flags to hand a compiler
 * for a 64-bit off_t. This libc is not glibc and does not have those
 * answers to give, so it does not invent them. The ones it does print
 * are the ones it can point at something for, and they match Ubuntu's
 * value for value. The LFS_* group is the one exception, and it is not
 * an invention: off_t is 64 bits here already, so "no extra flags
 * needed" is the true answer and an empty line is how getconf says it.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"

/* Where a value comes from. */
enum {
    K_CONST = 0,    /* a number fixed by the machine's C ABI */
    K_UCONST,       /* the same, but too big for a signed long */
    K_EMPTY,        /* defined, and defined to be nothing */
    K_ARG_MAX,
    K_OPEN_MAX,
    K_CLK_TCK,
    K_PAGESIZE,
    K_NAME_MAX,
    K_NPROC_CONF,
    K_NPROC_ONLN
};

typedef struct {
    const char *name;
    int         how;
    bool        wants_path;   /* a pathconf value: needs a file to ask about */
    u64         k;
} var_t;

/* The order is glibc's -a order, so that the two listings can be read
 * side by side. */
static const var_t vars[] = {
    { "NAME_MAX",                K_NAME_MAX, true,  0 },
    { "_POSIX_NAME_MAX",         K_NAME_MAX, true,  0 },
    { "PATH_MAX",                K_CONST,    true,  4096 },
    { "_POSIX_PATH_MAX",         K_CONST,    true,  4096 },
    { "PIPE_BUF",                K_CONST,    true,  4096 },
    { "_POSIX_PIPE_BUF",         K_CONST,    true,  4096 },
    { "_POSIX_CHOWN_RESTRICTED", K_CONST,    true,  1 },
    { "_POSIX_NO_TRUNC",         K_CONST,    true,  1 },
    { "_POSIX_VDISABLE",         K_CONST,    true,  0 },
    { "ARG_MAX",                 K_ARG_MAX,  false, 0 },
    { "CHAR_BIT",                K_CONST,    false, 8 },
    { "CHAR_MAX",                K_CONST,    false, 127 },
    { "CHAR_MIN",                K_CONST,    false, (u64)-128 },
    { "CLK_TCK",                 K_CLK_TCK,  false, 0 },
    { "INT_MAX",                 K_CONST,    false, 2147483647 },
    { "INT_MIN",                 K_CONST,    false, (u64)-2147483648L },
    { "LONG_BIT",                K_CONST,    false, 64 },
    { "OPEN_MAX",                K_OPEN_MAX, false, 0 },
    { "PAGESIZE",                K_PAGESIZE, false, 0 },
    { "PAGE_SIZE",               K_PAGESIZE, false, 0 },
    { "SCHAR_MAX",               K_CONST,    false, 127 },
    { "SCHAR_MIN",               K_CONST,    false, (u64)-128 },
    { "SHRT_MAX",                K_CONST,    false, 32767 },
    { "SHRT_MIN",                K_CONST,    false, (u64)-32768 },
    { "UCHAR_MAX",               K_CONST,    false, 255 },
    { "UINT_MAX",                K_UCONST,   false, 4294967295u },
    { "ULONG_MAX",               K_UCONST,   false, 18446744073709551615ull },
    { "USHRT_MAX",               K_CONST,    false, 65535 },
    { "WORD_BIT",                K_CONST,    false, 32 },
    { "_NPROCESSORS_CONF",       K_NPROC_CONF, false, 0 },
    { "_NPROCESSORS_ONLN",       K_NPROC_ONLN, false, 0 },
    { "LFS_CFLAGS",              K_EMPTY,    false, 0 },
    { "LFS_LDFLAGS",             K_EMPTY,    false, 0 },
    { "LFS_LIBS",                K_EMPTY,    false, 0 },
    { "LFS_LINTFLAGS",           K_EMPTY,    false, 0 },
};
#define NVARS ((int)(sizeof vars / sizeof vars[0]))

/* One entry out of the auxiliary vector the kernel left for us.
 * /proc/self/auxv is pairs of 8-byte words, ending at a type of 0. */
static u64 auxv(u64 type, u64 dflt)
{
    static char raw[1024];
    static long len = -1;
    if (len < 0)
        len = proc_read("/proc/self/auxv", raw, sizeof raw);

    for (long off = 0; off + 16 <= len; off += 16) {
        u64 t, v;
        memcpy(&t, raw + off, sizeof t);
        memcpy(&v, raw + off + 8, sizeof v);
        if (t == 0)
            break;
        if (t == type)
            return v;
    }
    return dflt;
}

/* The soft limit for a resource. prlimit64 with a NULL "new" only
 * reads; there is no getrlimit wrapper in this libc because until now
 * nothing wanted to read one back. */
static u64 soft_limit(int resource, u64 dflt)
{
    u64 lim[2] = { 0, 0 };
    if (sys_call4(SYS_prlimit64, 0, resource, 0, (long)lim) < 0)
        return dflt;
    return lim[0];
}

/* "0-3", "0,2-3": how the cpu lists in sysfs are written. */
static int count_cpu_list(const char *path)
{
    char buf[256];
    if (proc_read(path, buf, sizeof buf) <= 0)
        return 0;

    int n = 0;
    for (const char *p = buf; *p; ) {
        if (*p < '0' || *p > '9') { p++; continue; }
        char *end;
        long lo = strtol(p, &end, 10);
        long hi = lo;
        if (*end == '-') {
            p = end + 1;
            hi = strtol(p, &end, 10);
        }
        if (hi >= lo)
            n += (int)(hi - lo + 1);
        p = end;
    }
    return n;
}

static int cpus(bool online)
{
    int n = count_cpu_list(online ? "/sys/devices/system/cpu/online"
                                  : "/sys/devices/system/cpu/present");
    if (n > 0)
        return n;

    /* No sysfs: fall back to counting the processor blocks the same way
     * nproc does. */
    static char cpuinfo[65536];
    long len = proc_read("/proc/cpuinfo", cpuinfo, sizeof cpuinfo);
    if (len <= 0)
        return 1;
    int count = 0;
    for (char *p = cpuinfo; (p = strstr(p, "processor")); p++)
        if (p == cpuinfo || p[-1] == '\n')
            count++;
    return count ? count : 1;
}

static u64 name_max(const char *path)
{
    lp_statfs_t fs;
    if (lp_statfs(path, &fs) < 0 || fs.namelen == 0)
        return 255;
    return fs.namelen;
}

/* The text getconf prints for one variable. */
static void value_of(const var_t *v, const char *path, char *out, size_t n)
{
    switch (v->how) {
    case K_EMPTY:
        out[0] = '\0';
        break;
    case K_UCONST:
        snprintf(out, n, "%llu", (unsigned long long)v->k);
        break;
    case K_ARG_MAX: {
        u64 stack = soft_limit(3 /* RLIMIT_STACK */, 8ull << 20);
        u64 arg   = stack / 4;
        if (stack == 0xFFFFFFFFFFFFFFFFull || arg > 6ull << 20) arg = 6ull << 20;
        if (arg < 128 << 10) arg = 128 << 10;
        snprintf(out, n, "%llu", (unsigned long long)arg);
        break;
    }
    case K_OPEN_MAX:
        snprintf(out, n, "%llu",
                 (unsigned long long)soft_limit(7 /* RLIMIT_NOFILE */, 1024));
        break;
    case K_CLK_TCK:
        snprintf(out, n, "%llu", (unsigned long long)auxv(17 /* AT_CLKTCK */, 100));
        break;
    case K_PAGESIZE:
        snprintf(out, n, "%llu", (unsigned long long)auxv(6 /* AT_PAGESZ */, 4096));
        break;
    case K_NAME_MAX:
        snprintf(out, n, "%llu", (unsigned long long)name_max(path));
        break;
    case K_NPROC_CONF:
        snprintf(out, n, "%d", cpus(false));
        break;
    case K_NPROC_ONLN:
        snprintf(out, n, "%d", cpus(true));
        break;
    default:
        snprintf(out, n, "%lld", (long long)v->k);
        break;
    }
}

static int usage(void)
{
    dprintf(STDERR_FILENO,
            "Usage: getconf [-v specification] variable_name [pathname]\n"
            "       getconf -a [pathname]\n");
    return 2;
}

int main(int argc, char **argv)
{
    int i = 1;

    if (i < argc && strcmp(argv[i], "--help") == 0) {
        printf("Usage: getconf [-v SPEC] VAR\n"
               "  or:  getconf [-v SPEC] PATH_VAR PATH\n"
               "  or:  getconf -a [PATH]\n\n"
               "Get the configuration value for variable VAR, or for "
               "variable PATH_VAR\nfor path PATH.\n");
        return 0;
    }

    /* -v names a compilation environment. There is one of those here,
     * the machine's own, so the specification is accepted and ignored -
     * which is what glibc's getconf does with it as well for every
     * variable that is not a compiler flag. */
    if (i < argc && strncmp(argv[i], "-v", 2) == 0) {
        if (argv[i][2] == '\0') {
            if (argc - i < 3)
                return usage();
            i += 2;
        } else {
            i++;
        }
    }

    bool all = false;
    if (i < argc && strcmp(argv[i], "-a") == 0) {
        all = true;
        i++;
    }

    const char *var = NULL;
    if (!all) {
        if (i >= argc)
            return usage();
        var = argv[i++];
    }

    const char *path = NULL;
    if (i < argc)
        path = argv[i++];
    if (i < argc)
        return usage();

    char value[64];

    if (all) {
        for (int v = 0; v < NVARS; v++) {
            value_of(&vars[v], path ? path : "/", value, sizeof value);
            printf("%-35s%s\n", vars[v].name, value);
        }
        return 0;
    }

    for (int v = 0; v < NVARS; v++) {
        if (strcmp(var, vars[v].name) != 0)
            continue;
        /* A value that depends on a filesystem needs to be told which
         * one, and a value that does not must not be given one - both
         * are a usage error, and GNU treats them as the same one. */
        if (vars[v].wants_path != (path != NULL))
            return usage();
        value_of(&vars[v], path, value, sizeof value);
        printf("%s\n", value);
        return 0;
    }

    dprintf(STDERR_FILENO, "getconf: Unrecognized variable `%s'\n", var);
    return 2;
}
