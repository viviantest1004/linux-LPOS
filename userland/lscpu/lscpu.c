/* lscpu - what the processor is.
 *
 * Everything here comes from /proc/cpuinfo and /sys/devices/system/cpu,
 * which is where lscpu gets it too. On a Pi Zero 2 W the answer is four
 * Cortex-A53 cores; on EC2 it is whatever Amazon gave you, and that is
 * the number you need before choosing -j for a build.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static char cpuinfo[131072];

/* The value of "key\t: value" in /proc/cpuinfo, first occurrence. */
static bool field(const char *key, char *out, size_t n)
{
    size_t klen = strlen(key);
    for (char *p = cpuinfo; *p; ) {
        char *nl = strchr(p, '\n');
        size_t linelen = nl ? (size_t)(nl - p) : strlen(p);
        if (strncmp(p, key, klen) == 0 &&
            (p[klen] == ' ' || p[klen] == '\t' || p[klen] == ':')) {
            char *colon = NULL;
            for (size_t k = 0; k < linelen; k++)
                if (p[k] == ':') { colon = p + k; break; }
            if (colon) {
                colon++;
                while (*colon == ' ' || *colon == '\t') colon++;
                size_t vlen = (size_t)(p + linelen - colon);
                if (vlen >= n) vlen = n - 1;
                memcpy(out, colon, vlen);
                out[vlen] = '\0';
                return true;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}

static int count_key(const char *key)
{
    int n = 0;
    size_t klen = strlen(key);
    for (char *p = cpuinfo; *p; ) {
        if ((p == cpuinfo || p[-1] == '\n') && strncmp(p, key, klen) == 0) n++;
        char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return n;
}

static bool sysfile(const char *path, char *out, size_t n)
{
    long r = proc_read(path, out, n);
    if (r <= 0) return false;
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    return out[0] != '\0';
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: lscpu [OPTION]...\n"
                   "Display information about the CPU architecture.\n\n"
                   "      --help     display this help and exit\n");
            return 0;
        }

    if (proc_read("/proc/cpuinfo", cpuinfo, sizeof cpuinfo) <= 0) {
        dprintf(STDERR_FILENO, "lscpu: cannot read /proc/cpuinfo\n");
        return 1;
    }

    char v[512];

    int cpus = count_key("processor");
    if (cpus == 0) cpus = 1;

#if defined(__x86_64__)
    const char *arch = "x86_64";
    const char *modes = "32-bit, 64-bit";
    const char *endian = "Little Endian";
#else
    const char *arch = "aarch64";
    const char *modes = "32-bit, 64-bit";
    const char *endian = "Little Endian";
#endif

    printf("Architecture:            %s\n", arch);
    printf("  CPU op-mode(s):        %s\n", modes);
    printf("  Byte Order:            %s\n", endian);
    printf("CPU(s):                  %d\n", cpus);
    printf("  On-line CPU(s) list:   0-%d\n", cpus - 1);

    if (field("model name", v, sizeof v))
        printf("Model name:              %s\n", v);
    else if (field("Model", v, sizeof v))
        printf("Model name:              %s\n", v);
    else if (field("CPU part", v, sizeof v)) {
        /* ARM reports a part number rather than a name. The common ones
         * are worth spelling out; the rest print as the number. */
        const char *name = NULL;
        if (strcmp(v, "0xd03") == 0) name = "Cortex-A53";
        else if (strcmp(v, "0xd08") == 0) name = "Cortex-A72";
        else if (strcmp(v, "0xd0b") == 0) name = "Cortex-A76";
        printf("Model name:              %s\n", name ? name : v);
    }

    if (field("vendor_id", v, sizeof v))     printf("  Vendor ID:             %s\n", v);
    if (field("cpu family", v, sizeof v))    printf("  CPU family:            %s\n", v);
    if (field("model", v, sizeof v))         printf("  Model:                 %s\n", v);
    if (field("stepping", v, sizeof v))      printf("  Stepping:              %s\n", v);
    if (field("cpu MHz", v, sizeof v))       printf("  CPU MHz:               %s\n", v);
    if (field("BogoMIPS", v, sizeof v))      printf("  BogoMIPS:              %s\n", v);

    if (sysfile("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", v, sizeof v))
        printf("  CPU max MHz:           %ld\n", strtol(v, NULL, 10) / 1000);
    if (sysfile("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq", v, sizeof v))
        printf("  CPU min MHz:           %ld\n", strtol(v, NULL, 10) / 1000);

    if (field("flags", v, sizeof v) || field("Features", v, sizeof v))
        printf("  Flags:                 %s\n", v);

    static const char *caches[] = {
        "/sys/devices/system/cpu/cpu0/cache/index0/size",
        "/sys/devices/system/cpu/cpu0/cache/index1/size",
        "/sys/devices/system/cpu/cpu0/cache/index2/size",
        "/sys/devices/system/cpu/cpu0/cache/index3/size",
    };
    static const char *labels[] = { "L1d", "L1i", "L2 ", "L3 " };
    for (int i = 0; i < 4; i++)
        if (sysfile(caches[i], v, sizeof v))
            printf("Caches:  %s                   %s\n", labels[i], v);

    return 0;
}
