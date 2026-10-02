/* usage - memory and disk, at a glance.
 *
 * free tells you about memory. df tells you about disks. Neither
 * answers the question people actually have, which is "am I about to
 * run out of anything" - and answering it meant running two commands
 * and comparing two sets of numbers in your head.
 *
 * So: one screen, one bar per thing, everything in the same units.
 *
 * The bars are '#' and '.' rather than block-drawing characters. Those
 * look better, but the console font on a screen carries 256 glyphs and
 * which ones depends on the font that was built in - and a bar that
 * comes out as a row of question marks on the one display this board
 * has is worse than a plain one that always works.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define BAR_WIDTH 24

/* Colour, but only when someone is looking at it. Piped into a file,
 * escape codes are just noise in the middle of the numbers. */
static bool colour = false;

static const char *shade(long percent)
{
    if (!colour)          return "";
    if (percent >= 90)    return "\x1b[31m";     /* red */
    if (percent >= 75)    return "\x1b[33m";     /* yellow */
    return "\x1b[32m";                           /* green */
}

static const char *plain(void) { return colour ? "\x1b[0m" : ""; }

static void bar(long percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    int filled = (int)(percent * BAR_WIDTH / 100);
    /* Anything in use at all gets at least one mark: an empty bar next
     * to "2%" reads as a bug rather than as "almost nothing". */
    if (filled == 0 && percent > 0)
        filled = 1;

    char buf[BAR_WIDTH + 1];
    for (int i = 0; i < BAR_WIDTH; i++)
        buf[i] = (i < filled) ? '#' : '.';
    buf[BAR_WIDTH] = '\0';

    printf("[%s%s%s]", shade(percent), buf, plain());
}

/* One line: label, bar, used of total, percent, and a remark. */
static void row(const char *label, u64 used_mb, u64 total_mb,
                const char *note)
{
    long pct = total_mb ? (long)(used_mb * 100 / total_mb) : 0;

    /* Keep the columns lined up even when a mount point is long: a
     * table that shifts sideways halfway down is harder to read than
     * one with a name cut short. */
    char shown[11];
    strlcpy(shown, label, sizeof(shown));
    printf("  %-10s ", shown);
    bar(pct);
    printf(" %5llu / %-5llu MB  %3ld%%",
           (unsigned long long)used_mb, (unsigned long long)total_mb, pct);
    if (note && note[0])
        printf("   %s", note);
    printf("\n");
}

static long kv(const char *text, const char *key)
{
    long v = proc_find_kv(text, key);
    return v < 0 ? 0 : v;
}

static bool worth_showing(const char *type)
{
    static const char *skip[] = {
        "proc", "sysfs", "devtmpfs", "devpts", "cgroup", "cgroup2",
        "debugfs", "tracefs", "securityfs", "pstore", "bpf", "rootfs", NULL
    };
    for (int i = 0; skip[i]; i++)
        if (strcmp(type, skip[i]) == 0)
            return false;
    return true;
}

/* zram stores pages compressed, so the swap that "fits" in 256MB of
 * swap space costs far less than 256MB of memory. Worth saying, because
 * otherwise the swap line looks like memory we do not have. */
static void zram_note(char *out, size_t size)
{
    out[0] = '\0';

    char orig[64], comp[64];
    if (proc_read("/sys/block/zram0/orig_data_size", orig, sizeof(orig)) <= 0)
        return;
    if (proc_read("/sys/block/zram0/compr_data_size", comp, sizeof(comp)) <= 0)
        return;

    long o = strtol(orig, NULL, 10);
    long c = strtol(comp, NULL, 10);
    if (o <= 0 || c <= 0) {
        strlcpy(out, "compressed, in RAM", size);
        return;
    }
    snprintf(out, size, "compressed %ld%% (%ldKB in RAM)",
             (long)(c * 100 / o), c / 1024);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            printf("usage: usage\n");
            printf("  memory and disk on one screen. No options -\n");
            printf("  'free' and 'df' are there for the detail.\n");
            return 0;
        }
    }

    int rows = 0, cols = 0;
    colour = (lp_term_size(STDOUT_FILENO, &rows, &cols) == 0);

    static char mem[8192];
    if (proc_read("/proc/meminfo", mem, sizeof(mem)) <= 0) {
        dprintf(STDERR_FILENO, "usage: cannot read /proc/meminfo\n");
        return 1;
    }

    long total = kv(mem, "MemTotal");
    long avail = kv(mem, "MemAvailable");
    long stot  = kv(mem, "SwapTotal");
    long sfree = kv(mem, "SwapFree");

    printf("\n");

    if (total > 0) {
        char note[64];
        snprintf(note, sizeof(note), "%ld MB free to use", avail / 1024);
        row("memory", (u64)((total - avail) / 1024), (u64)(total / 1024), note);
    }

    if (stot > 0) {
        char note[80];
        zram_note(note, sizeof(note));
        row("swap", (u64)((stot - sfree) / 1024), (u64)(stot / 1024), note);
    }

    printf("\n");

    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "usage: cannot read /proc/mounts\n");
        return 1;
    }

    char line[512];
    char seen[16][64];
    int  nseen = 0;

    while (readline((int)fd, line, sizeof(line)) >= 0) {
        char *sp1 = strchr(line, ' ');
        if (!sp1) continue;
        *sp1 = '\0';
        char *dir = sp1 + 1;
        char *sp2 = strchr(dir, ' ');
        if (!sp2) continue;
        *sp2 = '\0';
        char *type = sp2 + 1;
        char *sp3 = strchr(type, ' ');
        if (sp3) *sp3 = '\0';

        if (!worth_showing(type))
            continue;

        /* /root is a bind mount of a directory on /data: the same disk,
         * the same numbers, listed twice. Once is enough. */
        bool dup = false;
        for (int i = 0; i < nseen; i++)
            if (strcmp(seen[i], dir) == 0) { dup = true; break; }
        if (dup || nseen >= 16)
            continue;

        u64 freeb = 0, totalb = 0;
        if (lp_fs_space(dir, &freeb, &totalb) < 0 || totalb == 0)
            continue;

        /* Two mount points on one device - /data and /root - report the
         * same size. Show the first and skip the rest. */
        bool same = false;
        for (int i = 0; i < nseen; i++) {
            u64 f2 = 0, t2 = 0;
            if (lp_fs_space(seen[i], &f2, &t2) == 0 &&
                t2 == totalb && f2 == freeb) { same = true; break; }
        }
        if (same)
            continue;

        strlcpy(seen[nseen++], dir, 64);

        char note[64];
        snprintf(note, sizeof(note), "%llu MB free",
                 (unsigned long long)(freeb / 1048576ULL));

        const char *label = dir;
        if (strcmp(dir, "/") == 0)
            label = "/ (RAM)";

        row(label, (totalb - freeb) / 1048576ULL, totalb / 1048576ULL, note);
    }
    lp_close((int)fd);

    printf("\n");
    return 0;
}
