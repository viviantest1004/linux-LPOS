/* blkid - the label, UUID and type of a filesystem.
 *
 *   blkid                 every partition the kernel knows about
 *   blkid /dev/mmcblk0p2  just that one
 *   blkid -s UUID /dev/x  just the UUID, for a script
 *
 * Read straight out of the superblock. Only the filesystems this
 * machine can actually meet are recognised - ext2/3/4, FAT and swap -
 * because a table of forty magic numbers that were never tested here
 * would be a table of forty guesses, and a wrong TYPE= in /etc/fstab is
 * a boot that stops.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"

#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

typedef struct {
    char type[16], label[80], uuid[40];
} info_t;

static void hexuuid(const u8 *p, char *out)
{
    static const char h[] = "0123456789abcdef";
    int k = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[k++] = '-';
        out[k++] = h[p[i] >> 4];
        out[k++] = h[p[i] & 15];
    }
    out[k] = '\0';
}

static void trim(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = '\0';
}

static bool probe(const char *dev, info_t *out)
{
    memset(out, 0, sizeof *out);

    long fd = lp_open(dev, O_RDONLY, 0);
    if (fd < 0) return false;

    static u8 page[4096];
    long n = lp_read((int)fd, page, sizeof page);
    lp_close((int)fd);
    if (n < 2048) return false;

    /* ext2/3/4: the superblock sits at 1024 and says 0xEF53 at +56. */
    u8 *e = page + 1024;
    if ((u16)(e[56] | (e[57] << 8)) == 0xEF53) {
        u32 compat   = *(u32 *)(e + 92);
        u32 incompat = *(u32 *)(e + 96);
        u32 ro       = *(u32 *)(e + 100);
        if      (incompat & 0x40) strlcpy(out->type, "ext4", 16);   /* EXTENTS */
        else if (ro & 0x8)        strlcpy(out->type, "ext4", 16);   /* HUGE_FILE */
        else if (compat & 0x4)    strlcpy(out->type, "ext3", 16);   /* HAS_JOURNAL */
        else                      strlcpy(out->type, "ext2", 16);
        memcpy(out->label, e + 120, 16);
        out->label[16] = '\0';
        hexuuid(e + 104, out->uuid);
        trim(out->label);
        return true;
    }

    /* swap: the signature is at the end of the first page. */
    if (n >= 4096 && memcmp(page + 4086, "SWAPSPACE2", 10) == 0) {
        strlcpy(out->type, "swap", 16);
        hexuuid(page + 1036, out->uuid);
        memcpy(out->label, page + 1052, 16);
        out->label[16] = '\0';
        trim(out->label);
        return true;
    }

    /* FAT: "FAT12"/"FAT16" at 54 for the old layout, "FAT32" at 82. */
    bool f32 = memcmp(page + 82, "FAT32", 5) == 0;
    if (f32 || memcmp(page + 54, "FAT", 3) == 0) {
        strlcpy(out->type, "vfat", 16);
        const u8 *lab = f32 ? page + 71 : page + 43;
        memcpy(out->label, lab, 11);
        out->label[11] = '\0';
        trim(out->label);
        if (strcmp(out->label, "NO NAME") == 0) out->label[0] = '\0';
        const u8 *id = f32 ? page + 67 : page + 39;
        snprintf(out->uuid, sizeof out->uuid, "%02X%02X-%02X%02X",
                 id[3], id[2], id[1], id[0]);
        return true;
    }

    return false;
}

static void print_one(const char *dev, const info_t *in, const char *only)
{
    if (only) {
        if (strcmp(only, "UUID") == 0 && in->uuid[0])  printf("%s\n", in->uuid);
        else if (strcmp(only, "LABEL") == 0 && in->label[0]) printf("%s\n", in->label);
        else if (strcmp(only, "TYPE") == 0 && in->type[0])   printf("%s\n", in->type);
        return;
    }
    printf("%s:", dev);
    if (in->label[0]) printf(" LABEL=\"%s\"", in->label);
    if (in->uuid[0])  printf(" UUID=\"%s\"", in->uuid);
    if (in->type[0])  printf(" TYPE=\"%s\"", in->type);
    printf("\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "match-tag", 1, 's' }, { "output", 1, 'o' },
        { "probe", 0, 'p' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    const char *only = NULL;

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "s:o:pc:", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 's': only = g.arg; break;
        case 'o': case 'p': case 'c': break;
        case 'H':
            printf("Usage: blkid [OPTION]... [DEVICE]...\n"
                   "Print the label, UUID and type of each filesystem.\n\n"
                   "  -s, --match-tag=TAG   show only this tag: LABEL, UUID or TYPE\n"
                   "      --help     display this help and exit\n\n"
                   "With no DEVICE, every block device under /sys/class/block is tried.\n"
                   "ext2, ext3, ext4, FAT and swap are recognised; anything else is\n"
                   "left out rather than guessed at.\n");
            return 0;
        default: lp_getopt_err("blkid", &g); return 1;
        }
    }

    if (g.ind < argc) {
        int found = 0;
        for (int i = g.ind; i < argc; i++) {
            info_t in;
            if (!probe(argv[i], &in)) continue;
            print_one(argv[i], &in, only);
            found++;
        }
        return found ? 0 : 2;
    }

    long fd = lp_open("/sys/class/block", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "blkid: /sys/class/block is not there - is sysfs mounted?\n");
        return 2;
    }

    /* The names are collected before probing: getdents' buffer is
     * reused, and opening a device in the middle of walking it would
     * hand back a half-read directory. */
    static char names[512][64];
    int nn = 0;
    char buf[8192];
    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0) break;
        for (long off = 0; off < got && nn < 512; ) {
            char *rec = buf + off;
            u16 len = *(u16 *)(rec + DIRENT_RECLEN);
            char *name = rec + DIRENT_NAME;
            if (len == 0) break;
            off += len;
            if (name[0] == '.') continue;
            strlcpy(names[nn++], name, 64);
        }
    }
    lp_close((int)fd);

    int found = 0;
    for (int i = 0; i < nn; i++) {
        char dev[128];
        snprintf(dev, sizeof dev, "/dev/%s", names[i]);
        info_t in;
        if (!probe(dev, &in)) continue;
        print_one(dev, &in, only);
        found++;
    }
    return found ? 0 : 2;
}
