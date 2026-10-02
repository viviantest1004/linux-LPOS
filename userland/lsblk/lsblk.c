/* lsblk - the block devices, and what is on them.
 *
 * /sys/block has one directory per disk and one below it per partition,
 * with the size in 512-byte sectors and the mount points findable from
 * /proc/mounts. That is everything lsblk prints, so this prints it.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"

#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

static char mounts[16384];

static void human(char *out, size_t cap, u64 bytes)
{
    static const char *suf = "BKMGTPE";
    int i = 0;
    u64 v = bytes, rem = 0;
    while (v >= 1024 && i < 6) { rem = v % 1024; v /= 1024; i++; }
    if (i == 0) { snprintf(out, cap, "%lluB", (unsigned long long)v); return; }
    u64 tenths = (rem * 10 + 512) / 1024;
    if (tenths >= 10) { v++; tenths = 0; }
    if (v < 10) snprintf(out, cap, "%llu.%llu%c", (unsigned long long)v,
                         (unsigned long long)tenths, suf[i]);
    else        snprintf(out, cap, "%llu%c", (unsigned long long)v, suf[i]);
}

static bool sysval(const char *path, char *out, size_t n)
{
    long r = proc_read(path, out, n);
    if (r <= 0) return false;
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    return true;
}

static const char *mount_of(const char *dev)
{
    static char found[256];
    char want[64];
    snprintf(want, sizeof want, "/dev/%s ", dev);
    for (char *p = mounts; *p; ) {
        if (strncmp(p, want, strlen(want)) == 0) {
            char *sp = strchr(p, ' ');
            char *sp2 = sp ? strchr(sp + 1, ' ') : NULL;
            if (sp && sp2) {
                size_t n = (size_t)(sp2 - sp - 1);
                if (n >= sizeof found) n = sizeof found - 1;
                memcpy(found, sp + 1, n);
                found[n] = '\0';
                return found;
            }
        }
        char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return "";
}

static void row(const char *name, int depth, bool is_part)
{
    char path[512], v[128], size[32] = "0B", mm[32] = "";
    u64 sectors = 0;

    snprintf(path, sizeof path, "/sys/class/block/%s/size", name);
    if (sysval(path, v, sizeof v)) sectors = (u64)strtoll(v, NULL, 10);
    human(size, sizeof size, sectors * 512);

    snprintf(path, sizeof path, "/sys/class/block/%s/dev", name);
    if (sysval(path, v, sizeof v)) strlcpy(mm, v, sizeof mm);

    char ro[8] = "0";
    snprintf(path, sizeof path, "/sys/class/block/%s/ro", name);
    sysval(path, ro, sizeof ro);

    char label[64];
    snprintf(label, sizeof label, "%*s%s%s", depth * 2, "",
             depth ? "\xe2\x94\x94\xe2\x94\x80" : "", name);

    printf("%-12s %-7s %2s %6s %2s %-6s %s\n",
           label, mm, ro, size, "0", is_part ? "part" : "disk",
           mount_of(name));
}

int main(int argc, char **argv)
{
    bool show_all = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0)
            show_all = true;
        else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: lsblk [OPTION]...\n"
                   "List information about block devices.\n\n"
                   "  -a, --all      also list empty devices\n"
                   "      --help     display this help and exit\n");
            return 0;
        }
    }

    proc_read("/proc/mounts", mounts, sizeof mounts);

    long fd = lp_open("/sys/block", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "lsblk: /sys/block is not there - "
                               "is sysfs mounted?\n");
        return 1;
    }

    static char disks[128][64];
    int nd = 0;
    char buf[8192];
    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0) break;
        for (long off = 0; off < got && nd < 128; ) {
            char *rec = buf + off;
            u16 len = *(u16 *)(rec + DIRENT_RECLEN);
            char *name = rec + DIRENT_NAME;
            if (len == 0) break;
            off += len;
            if (name[0] == '.') continue;
            strlcpy(disks[nd++], name, 64);
        }
    }
    lp_close((int)fd);

    /* getdents hands them back in whatever order the filesystem likes,
     * which puts vdf above vda. Sort so two runs agree. */
    for (int i = 1; i < nd; i++) {
        char tmp[64];
        strlcpy(tmp, disks[i], sizeof tmp);
        int j = i - 1;
        while (j >= 0 && strcmp(disks[j], tmp) > 0) {
            strlcpy(disks[j + 1], disks[j], 64);
            j--;
        }
        strlcpy(disks[j + 1], tmp, 64);
    }

    printf("%-12s %-7s %2s %6s %2s %-6s %s\n",
           "NAME", "MAJ:MIN", "RM", "SIZE", "RO", "TYPE", "MOUNTPOINTS");

    for (int i = 0; i < nd; i++) {
        /* An empty loop device is a loop device nobody has attached
         * anything to. Listing eight of them hides the real disks. */
        if (!show_all) {
            char szpath[256], szv[64];
            snprintf(szpath, sizeof szpath, "/sys/class/block/%s/size", disks[i]);
            if (sysval(szpath, szv, sizeof szv) && strtol(szv, NULL, 10) == 0)
                continue;
        }
        row(disks[i], 0, false);

        /* Partitions are the subdirectories whose names start with the
         * disk's own name. */
        char dpath[256];
        snprintf(dpath, sizeof dpath, "/sys/block/%s", disks[i]);
        long d = lp_open(dpath, O_RDONLY | O_DIRECTORY, 0);
        if (d < 0) continue;
        char pbuf[8192];
        for (;;) {
            long got = sys_getdents((int)d, pbuf, sizeof pbuf);
            if (got <= 0) break;
            for (long off = 0; off < got; ) {
                char *rec = pbuf + off;
                u16 len = *(u16 *)(rec + DIRENT_RECLEN);
                char *name = rec + DIRENT_NAME;
                if (len == 0) break;
                off += len;
                if (name[0] == '.') continue;
                if (strncmp(name, disks[i], strlen(disks[i])) != 0) continue;
                row(name, 1, true);
            }
        }
        lp_close((int)d);
    }
    return 0;
}
