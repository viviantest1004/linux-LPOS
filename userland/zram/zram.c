/* zram - compressed swap inside RAM.
 *
 * Why zram and not swap on the SD card:
 *   The Zero 2 W's card does about 25MB/s - some 400x slower than RAM.
 *   Swapping to it thrashes: the machine spends its time moving pages in
 *   and out and looks hung, and the card's write endurance drains fast.
 *   zram keeps swapped pages in RAM, compressed - typically about 3:1.
 *   Usable memory grows and not one byte reaches the card. This is what
 *   Android and ChromeOS do on low-memory devices.
 *
 *   It is not free - zram itself uses RAM, so it only pays off on data
 *   that compresses. Even so it beats card swap on every count.
 *
 * Usage:
 *   zram on [sizeMB]    default 256MB
 *   zram off
 *   zram status
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define ZRAM_DEV      "/dev/zram0"
#define ZRAM_SYS      "/sys/block/zram0"
/* A quarter of RAM, rather than a fixed 256MB.
 *
 * 256 was chosen for a 512MB board, where it is half the machine and
 * about right. The same number on a 4GB virtual machine is 6% of RAM -
 * technically working, practically pointless, and misleading to anyone
 * reading `free`. Compressed swap is only ever worth a fraction of RAM,
 * so make it a fraction. */
/* Half of RAM since the desktop's minimum became 4GB: with Firefox and
 * LibreOffice open a 4GB machine lives in its swap, and compressed at
 * about 3:1 half of RAM costs a sixth of it when full. */
#define DEFAULT_FRACTION 2      /* RAM / 2 */
#define DEFAULT_MB_MIN   64
#define DEFAULT_MB_MAX   8192
#define PAGE_SIZE     4096

/* Swap header v1_2 layout, inside the first page.
 *   0..1023      boot area (left alone)
 *   1024         version      u32 = 1
 *   1028         last_page    u32 = page count - 1
 *   1032         nr_badpages  u32 = 0
 *   PAGE-10      "SWAPSPACE2" magic
 */
#define SWAP_VERSION_OFF    1024
#define SWAP_LASTPAGE_OFF   1028
#define SWAP_BADPAGES_OFF   1032
#define SWAP_MAGIC_OFF      (PAGE_SIZE - 10)
#define SWAP_MAGIC          "SWAPSPACE2"

static long sysfs_write(const char *path, const char *val)
{
    long fd = lp_open(path, O_WRONLY, 0);
    if (fd < 0)
        return fd;

    long rc = lp_write((int)fd, val, strlen(val));
    lp_close((int)fd);
    return rc;
}

static long sysfs_read_num(const char *path)
{
    char buf[64];
    long n = proc_read(path, buf, sizeof(buf));
    if (n < 0)
        return n;

    long v = 0;
    for (const char *p = buf; *p >= '0' && *p <= '9'; p++)
        v = v * 10 + (*p - '0');
    return v;
}

/* Write the swap header into the first page - what mkswap does. */
static int write_swap_header(const char *dev, u64 bytes)
{
    long fd = lp_open(dev, O_RDWR, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "zram: cannot open %s (%ld)\n", dev, -fd);
        return 1;
    }

    static u8 page[PAGE_SIZE];
    memset(page, 0, sizeof(page));

    u32 nr_pages = (u32)(bytes / PAGE_SIZE);
    if (nr_pages < 2) {
        dprintf(STDERR_FILENO, "zram: size is too small\n");
        lp_close((int)fd);
        return 1;
    }

    *(u32 *)(page + SWAP_VERSION_OFF)   = 1;
    *(u32 *)(page + SWAP_LASTPAGE_OFF)  = nr_pages - 1;
    *(u32 *)(page + SWAP_BADPAGES_OFF)  = 0;
    memcpy(page + SWAP_MAGIC_OFF, SWAP_MAGIC, 10);

    long n = lp_write((int)fd, page, sizeof(page));
    lp_close((int)fd);

    if (n != (long)sizeof(page)) {
        dprintf(STDERR_FILENO, "zram: writing the header failed (%ld)\n", n);
        return 1;
    }
    return 0;
}

static int cmd_on(u64 mb)
{
    /* 1) Set the size. A missing sysfs entry means no CONFIG_ZRAM. */
    char size_str[32];
    snprintf(size_str, sizeof(size_str), "%luM", (unsigned long)mb);

    long rc = sysfs_write(ZRAM_SYS "/disksize", size_str);
    if (rc < 0) {
        dprintf(STDERR_FILENO,
                "zram: cannot write %s/disksize (%ld)\n"
                "      does the kernel have CONFIG_ZRAM?\n", ZRAM_SYS, -rc);
        return 1;
    }

    /* 2) Swap header */
    if (write_swap_header(ZRAM_DEV, mb * 1024 * 1024) != 0)
        return 1;

    /* 3) Enable it, at a high priority so it is used before any other swap. */
    rc = lp_swapon(ZRAM_DEV, 0);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "zram: swapon failed (%ld)\n", -rc);
        return 1;
    }

    printf("zram: %luMB of compressed swap created in RAM\n", (unsigned long)mb);
    return 0;
}

static int cmd_off(void)
{
    long rc = lp_swapoff(ZRAM_DEV);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "zram: swapoff failed (%ld)\n", -rc);
        return 1;
    }
    sysfs_write(ZRAM_SYS "/reset", "1");
    printf("zram: disabled\n");
    return 0;
}

static int cmd_status(void)
{
    long disksize = sysfs_read_num(ZRAM_SYS "/disksize");
    if (disksize <= 0) {
        printf("zram: not configured\n");
        return 0;
    }

    char mem[4096];
    long swap_total = -1, swap_free = -1;
    if (proc_read("/proc/meminfo", mem, sizeof(mem)) > 0) {
        swap_total = proc_find_kv(mem, "SwapTotal");
        swap_free  = proc_find_kv(mem, "SwapFree");
    }

    printf("device size    : %ld MB\n", disksize / 1024 / 1024);
    if (swap_total >= 0)
        printf("swap total     : %ld MB\n", swap_total / 1024);
    if (swap_total > 0 && swap_free >= 0)
        printf("swap used      : %ld MB\n", (swap_total - swap_free) / 1024);

    /* How well it compresses: orig_data_size in, compr_data_size out. */
    long orig  = sysfs_read_num(ZRAM_SYS "/orig_data_size");
    long compr = sysfs_read_num(ZRAM_SYS "/compr_data_size");
    if (orig > 0 && compr > 0)
        printf("compression    : %ld%% (%ld KB -> %ld KB)\n",
               compr * 100 / orig, orig / 1024, compr / 1024);

    return 0;
}

/* A quarter of what this machine actually has, within reason. */
static u64 default_mb(void)
{
    char buf[2048];
    long total_kb = 0;

    if (proc_read("/proc/meminfo", buf, sizeof buf) > 0) {
        const char *p = strstr(buf, "MemTotal:");
        if (p)
            total_kb = strtol(p + 9, NULL, 10);
    }
    if (total_kb <= 0)
        return 256;                       /* cannot tell; the old default */

    u64 mb = (u64)total_kb / 1024 / DEFAULT_FRACTION;
    if (mb < DEFAULT_MB_MIN) mb = DEFAULT_MB_MIN;
    if (mb > DEFAULT_MB_MAX) mb = DEFAULT_MB_MAX;
    return mb;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: zram on [sizeMB] | zram off | zram status\n");
        return 2;
    }

    if (strcmp(argv[1], "on") == 0) {
        u64 mb = default_mb();
        if (argc > 2) {
            long v = strtol(argv[2], NULL, 10);
            if (v <= 0) {
                dprintf(STDERR_FILENO, "zram: bad size\n");
                return 2;
            }
            mb = (u64)v;
        }
        return cmd_on(mb);
    }
    if (strcmp(argv[1], "off") == 0)     return cmd_off();
    if (strcmp(argv[1], "status") == 0)  return cmd_status();

    dprintf(STDERR_FILENO, "zram: unknown command '%s'\n", argv[1]);
    return 2;
}
