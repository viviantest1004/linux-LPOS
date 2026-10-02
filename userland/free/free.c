/* free - how much memory is left.
 *
 *   free [-b]      -b prints bytes-ish numbers (KB) instead of MB
 *
 * "free" in the kernel's sense is memory nothing is using at all, which
 * on a healthy system is close to zero - the page cache takes whatever
 * is going and gives it back on demand. The number worth reading is
 * "available": what a new program could get without anything being
 * pushed to swap. That is the one guard watches, so it is the one shown
 * first here.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

static long kv(const char *text, const char *key)
{
    long v = proc_find_kv(text, key);
    return v < 0 ? 0 : v;
}

int main(int argc, char **argv)
{
    bool in_kb = (argc > 1 && strcmp(argv[1], "-b") == 0);

    if (argc > 1 && strcmp(argv[1], "-h") == 0) {
        printf("usage: free [-b]\n");
        printf("  -b  show KB instead of MB\n");
        return 0;
    }

    static char mem[8192];
    if (proc_read("/proc/meminfo", mem, sizeof(mem)) <= 0) {
        dprintf(STDERR_FILENO, "free: cannot read /proc/meminfo\n");
        return 1;
    }

    long total  = kv(mem, "MemTotal");
    long avail  = kv(mem, "MemAvailable");
    long fre    = kv(mem, "MemFree");
    long cached = kv(mem, "Cached") + kv(mem, "Buffers");
    long stot   = kv(mem, "SwapTotal");
    long sfree  = kv(mem, "SwapFree");

    long div = in_kb ? 1 : 1024;
    const char *unit = in_kb ? "KB" : "MB";

    printf("             total      used      free    cached   (%s)\n", unit);
    printf("memory  %10ld%10ld%10ld%10ld\n",
           total / div, (total - avail) / div, fre / div, cached / div);
    if (stot > 0)
        printf("swap    %10ld%10ld%10ld\n",
               stot / div, (stot - sfree) / div, sfree / div);

    printf("\navailable %8ld %s - what a new program can have\n",
           avail / div, unit);
    return 0;
}
