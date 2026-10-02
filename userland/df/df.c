/* df - how full each filesystem is.
 *
 *   df                 1K blocks, the way every other df prints it
 *   df -h              sizes a person can read
 *   df -T              with the filesystem type
 *   df /path           just the one holding that path
 *
 * The old version printed columns of its own ("filesystem on total MB
 * used free use%"), which nothing could parse and nobody recognised.
 * The header below is GNU's, and so is the arithmetic: "Available" is
 * what an ordinary process may still use, and Use% is measured against
 * used+available rather than the total - ext4 keeps a few percent back
 * for root, and counting that reserve as free would say 95% on a disk
 * that is already refusing writes.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static bool opt_human, opt_si, opt_type, opt_all, opt_inodes;
static bool opt_portable;
static const char *only_type = NULL, *skip_type = NULL;
static u64  block_size = 1024;
static char block_suffix = '\0';   /* -BM prints "258020M", not "258020" */
static const char *prog = "df";

typedef struct {
    char dev[128], dir[256], type[32];
    char size[32], used[32], avail[32], pcent[16], itype[32];
} row_t;

#define MAX_ROWS 128
static row_t rows[MAX_ROWS];
static int   nrows;

/* The filesystems df calls "dummy" and leaves out unless asked. This is
 * coreutils 9's list, letter for letter - guessing at it is how you end
 * up showing four lines of zeroes above the two that matter, or hiding
 * something a person went looking for. Everything else with no blocks
 * at all (cgroup, pstore, tracefs) is dropped by the zero-blocks test
 * instead, which is the same rule df uses. */
static bool virtual_fs(const char *type)
{
    static const char *skip[] = {
        "autofs", "devtmpfs", "fuse.portal", "proc", "squashfs", "subfs",
        "debugfs", "devpts", "fusectl", "mqueue", "rpc_pipefs", "sysfs",
        "devfs", "kernfs", "ignore", "none", NULL
    };
    for (int i = 0; skip[i]; i++)
        if (strcmp(type, skip[i]) == 0)
            return true;
    return false;
}

/* "252G", "1.5M", "16K" - one decimal below 10, none above, which is
 * what GNU prints and what makes the column line up. */
static void human(char *out, size_t cap, u64 bytes, u64 base)
{
    static const char *suf = "BKMGTPE";
    if (bytes == 0) { snprintf(out, cap, "0"); return; }

    int i = 0;
    u64 v = bytes, rem = 0;
    while (v >= base && i < 6) { rem = v % base; v /= base; i++; }
    if (i == 0) { snprintf(out, cap, "%llu", (unsigned long long)v); return; }

    /* Round up, the way df does - 1.01G must not print as 1.0G when the
     * point of the column is "will it fit". */
    u64 tenths = (rem * 10 + base - 1) / base;
    if (tenths >= 10) { v++; tenths = 0; if (v >= base) { v /= base; i++; tenths = 0; } }
    /* Below ten there is a decimal even when it is zero - "1.0M", not
     * "1M". That is what makes the column the same width all the way
     * down, and it is what df prints. */
    if (v < 10)
        snprintf(out, cap, "%llu.%llu%c", (unsigned long long)v,
                 (unsigned long long)tenths, suf[i]);
    else
        snprintf(out, cap, "%llu%c", (unsigned long long)(v + (tenths ? 1 : 0)), suf[i]);
}

static void amount(char *out, size_t cap, u64 bytes)
{
    if (opt_human)
        human(out, cap, bytes, opt_si ? 1000 : 1024);
    else if (block_suffix)
        snprintf(out, cap, "%llu%c",
                 (unsigned long long)((bytes + block_size - 1) / block_size),
                 block_suffix);
    else
        snprintf(out, cap, "%llu",
                 (unsigned long long)((bytes + block_size - 1) / block_size));
}

static u64 seen_dev[MAX_ROWS];
static int nseen;
static bool dedup = true;      /* only for the automatic list */
static int  failed = 0;

static void add_row(const char *dev, const char *dir, const char *type)
{
    if (nrows == MAX_ROWS) return;
    lp_statfs_t fs;
    if (lp_statfs(dir, &fs) < 0) return;
    if (fs.blocks == 0 && !opt_all) return;

    /* /proc/mounts lists the same filesystem more than once - a mount
     * made twice, or an overmount. df shows it once. */
    lp_stat_t dst;
    if (dedup && lp_stat(dir, &dst, true) == 0) {
        for (int i = 0; i < nseen; i++)
            if (seen_dev[i] == dst.dev) return;
        if (nseen < MAX_ROWS) seen_dev[nseen++] = dst.dev;
    }

    row_t *r = &rows[nrows];
    strlcpy(r->dev, dev, sizeof r->dev);
    strlcpy(r->dir, dir, sizeof r->dir);
    strlcpy(r->type, type, sizeof r->type);

    if (opt_inodes) {
        u64 used = fs.files - fs.ffree;
        snprintf(r->size,  sizeof r->size,  "%llu", (unsigned long long)fs.files);
        snprintf(r->used,  sizeof r->used,  "%llu", (unsigned long long)used);
        snprintf(r->avail, sizeof r->avail, "%llu", (unsigned long long)fs.ffree);
        u64 denom = used + fs.ffree;
        if (denom == 0) snprintf(r->pcent, sizeof r->pcent, "-");
        else snprintf(r->pcent, sizeof r->pcent, "%llu%%",
                      (unsigned long long)((used * 100 + denom - 1) / denom));
    } else {
        u64 total = fs.blocks * fs.frsize;
        u64 avail = fs.bavail * fs.frsize;
        u64 used  = (fs.blocks - fs.bfree) * fs.frsize;
        amount(r->size,  sizeof r->size,  total);
        amount(r->used,  sizeof r->used,  used);
        amount(r->avail, sizeof r->avail, avail);
        u64 denom = used + avail;
        if (denom == 0) snprintf(r->pcent, sizeof r->pcent, "-");
        else snprintf(r->pcent, sizeof r->pcent, "%llu%%",
                      (unsigned long long)((used * 100 + denom - 1) / denom));
    }
    nrows++;
}

/* An absolute path, with . and .. taken out.
 *
 * The mount points in /proc/mounts are absolute, so a relative path
 * matches none of them: `df .` answered "not on any mounted
 * filesystem", which is a strange thing to be told about the directory
 * you are standing in. This does not resolve symlinks - the longest
 * matching mount point is still the answer for the name as written,
 * which is what df reports. */
static bool abspath(const char *path, char *out, size_t cap)
{
    char buf[1024];
    if (path[0] == '/') {
        strlcpy(buf, path, sizeof buf);
    } else {
        char cwd[768];
        if (lp_getcwd(cwd, sizeof cwd) < 0)
            return false;
        if (strcmp(path, ".") == 0)
            strlcpy(buf, cwd, sizeof buf);
        else
            snprintf(buf, sizeof buf, "%s/%s", cwd, path);
    }

    /* Walk the components, dropping "." and popping on "..". */
    size_t n = 0;
    out[0] = '\0';
    const char *p = buf;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - seg);
        if (len == 1 && seg[0] == '.')
            continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (n > 0 && out[n - 1] != '/') n--;
            if (n > 1) n--;              /* drop the slash, keep the root */
            out[n] = '\0';
            continue;
        }
        if (n + len + 2 > cap) return false;
        out[n++] = '/';
        memcpy(out + n, seg, len);
        n += len;
        out[n] = '\0';
    }
    if (n == 0) { strlcpy(out, "/", cap); }
    return true;
}

/* Which mount a path belongs to: the longest mount point it starts at. */
static void row_for_path(const char *path, char mounts[][3][256], int nm)
{
    int best = -1;
    size_t bestlen = 0;
    char real[1024];
    if (!abspath(path, real, sizeof real))
        strlcpy(real, path, sizeof real);

    for (int i = 0; i < nm; i++) {
        size_t l = strlen(mounts[i][1]);
        if (strncmp(real, mounts[i][1], l) != 0) continue;
        if (l > 1 && real[l] && real[l] != '/') continue;
        if (l >= bestlen) { bestlen = l; best = i; }
    }
    if (best < 0) {
        lp_diag(prog, NULL, NULL, "not on any mounted filesystem", path, 2);
        failed = 1;
        return;
    }
    add_row(mounts[best][0], mounts[best][1], mounts[best][2]);
}

static void usage(int fd)
{
    dprintf(fd, "Usage: df [OPTION]... [FILE]...\n"
                "Show information about the file system on which each FILE resides,\n"
                "or all file systems by default.\n\n"
                "  -a, --all             include pseudo, duplicate, inaccessible file systems\n"
                "  -B, --block-size=SIZE scale sizes by SIZE before printing them\n"
                "  -h, --human-readable  print sizes in powers of 1024 (e.g., 1023M)\n"
                "  -H, --si              print sizes in powers of 1000 (e.g., 1.1G)\n"
                "  -i, --inodes          list inode information instead of block usage\n"
                "  -k                    like --block-size=1K\n"
                "  -P, --portability     use the POSIX output format\n"
                "  -T, --print-type      print file system type\n"
                "  -t, --type=TYPE       limit listing to file systems of type TYPE\n"
                "  -x, --exclude-type=TYPE   limit listing to file systems not of type TYPE\n"
                "      --help     display this help and exit\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "all", 0, 'a' }, { "block-size", 1, 'B' },
        { "human-readable", 0, 'h' }, { "si", 0, 'H' },
        { "inodes", 0, 'i' }, { "portability", 0, 'P' },
        { "print-type", 0, 'T' }, { "type", 1, 't' },
        { "exclude-type", 1, 'x' }, { "local", 0, 'l' },
        { "sync", 0, 1001 }, { "no-sync", 0, 1002 },
        { "help", 0, 1003 }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "aB:hHikPTt:x:lv", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'a': opt_all = true; break;
        case 'h': opt_human = true; opt_si = false; break;
        case 'H': opt_human = true; opt_si = true;  break;
        case 'i': opt_inodes = true; break;
        case 'k': block_size = 1024; opt_human = false; break;
        case 'P': opt_portable = true; break;
        case 'T': opt_type = true; break;
        case 't': only_type = g.arg; break;
        case 'x': skip_type = g.arg; break;
        case 'B': {
            char *end;
            u64 v = (u64)strtol(g.arg, &end, 10);
            if (v == 0) v = 1;
            block_suffix = '\0';
            if (*end == 'K')      { v *= 1024; block_suffix = 'K'; }
            else if (*end == 'M') { v *= 1048576; block_suffix = 'M'; }
            else if (*end == 'G') { v *= 1073741824ULL; block_suffix = 'G'; }
            else if (*end == 'T') { v *= 1099511627776ULL; block_suffix = 'T'; }
            block_size = v;
            opt_human = false;
            break;
        }
        case 'l': case 'v': case 1001: case 1002: break;
        case 1003: usage(STDOUT_FILENO); return 0;
        default: lp_getopt_err(prog, &g); return 1;
        }
    }

    /* -a means everything, duplicates included - that is the point of
     * it, and hiding the second mount of the same filesystem would make
     * -a a slightly longer default rather than the whole list. */
    if (opt_all) dedup = false;

    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "%s: cannot read /proc/mounts\n", prog);
        return 1;
    }

    static char mounts[MAX_ROWS][3][256];
    int nm = 0;
    char line[1024];
    while (readrec((int)fd, line, sizeof line, '\n', NULL) >= 0 && nm < MAX_ROWS) {
        /* device mountpoint type options dump pass */
        char *dev = line;
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

        strlcpy(mounts[nm][0], dev, 256);
        strlcpy(mounts[nm][1], dir, 256);
        strlcpy(mounts[nm][2], type, 256);
        nm++;
    }
    lp_close((int)fd);

    if (g.ind < argc) {
        dedup = false;          /* one row per path the caller named */
        for (int i = g.ind; i < argc; i++)
            row_for_path(argv[i], mounts, nm);
    } else {
        for (int i = 0; i < nm; i++) {
            if (!opt_all && virtual_fs(mounts[i][2])) continue;
            if (only_type && strcmp(mounts[i][2], only_type) != 0) continue;
            if (skip_type && strcmp(mounts[i][2], skip_type) == 0) continue;
            add_row(mounts[i][0], mounts[i][1], mounts[i][2]);
        }
    }

    if (nrows == 0) {
        if (g.ind >= argc)
            dprintf(STDERR_FILENO, "%s: no file systems processed\n", prog);
        return 1;
    }

    /* Column widths come from the widest cell, header included, so the
     * table lines up whatever the numbers turn out to be. Each column
     * also has a floor, which is why df's tables from two machines line
     * up against each other. */
    const char *h_size  = opt_inodes ? "Inodes" : (opt_human ? "Size" : "1K-blocks");
    char blkhdr[32];
    if (!opt_human && !opt_inodes && block_size != 1024) {
        if (block_suffix)
            snprintf(blkhdr, sizeof blkhdr, "1%c-blocks", block_suffix);
        else
            snprintf(blkhdr, sizeof blkhdr, "%llu-blocks", (unsigned long long)block_size);
        h_size = blkhdr;
    }
    const char *h_used  = opt_inodes ? "IUsed" : "Used";
    const char *h_avail = opt_inodes ? "IFree" : (opt_human ? "Avail" : "Available");
    const char *h_pcent = opt_inodes ? "IUse%" : "Use%";

    /* df gives the source column a floor of 14 so that short device
     * names still line the table up with the usual long ones. */
    size_t w_dev = 14, w_type = 4;
    size_t w_size = strlen(h_size), w_used = strlen(h_used);
    size_t w_avail = strlen(h_avail), w_pcent = strlen(h_pcent);
    if (w_size  < 5) w_size  = 5;
    if (w_used  < 5) w_used  = 5;
    if (w_avail < 5) w_avail = 5;
    if (w_pcent < 4) w_pcent = 4;
    for (int i = 0; i < nrows; i++) {
        if (strlen(rows[i].dev)   > w_dev)   w_dev   = strlen(rows[i].dev);
        if (strlen(rows[i].type)  > w_type)  w_type  = strlen(rows[i].type);
        if (strlen(rows[i].size)  > w_size)  w_size  = strlen(rows[i].size);
        if (strlen(rows[i].used)  > w_used)  w_used  = strlen(rows[i].used);
        if (strlen(rows[i].avail) > w_avail) w_avail = strlen(rows[i].avail);
        if (strlen(rows[i].pcent) > w_pcent) w_pcent = strlen(rows[i].pcent);
    }

    if (opt_type)
        printf("%-*s %-*s %*s %*s %*s %*s %s\n",
               (int)w_dev, "Filesystem", (int)w_type, "Type",
               (int)w_size, h_size, (int)w_used, h_used,
               (int)w_avail, h_avail, (int)w_pcent, h_pcent, "Mounted on");
    else
        printf("%-*s %*s %*s %*s %*s %s\n",
               (int)w_dev, "Filesystem", (int)w_size, h_size,
               (int)w_used, h_used, (int)w_avail, h_avail,
               (int)w_pcent, h_pcent, "Mounted on");

    for (int i = 0; i < nrows; i++) {
        if (opt_type)
            printf("%-*s %-*s %*s %*s %*s %*s %s\n",
                   (int)w_dev, rows[i].dev, (int)w_type, rows[i].type,
                   (int)w_size, rows[i].size, (int)w_used, rows[i].used,
                   (int)w_avail, rows[i].avail, (int)w_pcent, rows[i].pcent,
                   rows[i].dir);
        else
            printf("%-*s %*s %*s %*s %*s %s\n",
                   (int)w_dev, rows[i].dev, (int)w_size, rows[i].size,
                   (int)w_used, rows[i].used, (int)w_avail, rows[i].avail,
                   (int)w_pcent, rows[i].pcent, rows[i].dir);
    }
    (void)opt_portable;
    return failed;
}
