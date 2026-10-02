/* storage - what survives a reboot, and how to add to it.
 *
 *   storage                     every place that persists, and how full
 *   storage adopt <dev> [name]  give a drive a fixed path, kept at boot
 *   storage forget <name>       stop doing that
 *   storage format <dev> [name] wipe it, make it ext4, then adopt it
 *
 * ── Why this exists ──
 * Three commands already answered pieces of this. `disk` says what
 * hardware is attached, `part` edits partition tables, `datadisk` moves
 * /data. None of them answered the question people actually have, which
 * is "where can I put something so it is still there tomorrow, and how
 * much room is left".
 *
 * On this system the answer used to be one word: /data. That is one
 * partition on one card, and on a Zero 2 W it is usually a small card.
 * Plug in a 1TB drive and it would appear under /media with a name that
 * depended on how it was formatted, mounted with whatever flags, and
 * gone again after a reboot until somebody plugged it in and mounted it
 * by hand. Useful for copying files off, useless for anything that has
 * to survive.
 *
 * ── Adopting ──
 * Adopting a drive means three things: it gets a name you chose, it
 * gets mounted at /mnt/<name> every boot, and it is mounted the same
 * careful way /data is.
 *
 * The record is kept by filesystem label, not by device name. sdb1
 * becomes sdc1 the moment somebody plugs in a second drive first, and a
 * system that loses its storage because of the order things were
 * plugged in is not one you can leave alone for months.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "disk.h"

#define CONF        "/data/storage.conf"
#define MNT_ROOT    "/mnt"
#define MAX_ENTRIES 16

typedef struct {
    char name[32];      /* what we call it, and the directory under /mnt */
    char label[24];     /* the filesystem label we match on */
} adopted_t;

/* ── the record ──────────────────────────────────────────────────── */

/* One line per drive: "<name> <label>". Comments and blanks ignored.
 *
 * A text file rather than anything cleverer, because it lives on /data
 * and /data is the thing most likely to be broken when you need to read
 * it. A person with the card in another machine can fix this with any
 * text editor. */
static int load_conf(adopted_t *out, int max)
{
    long fd = lp_open(CONF, O_RDONLY, 0);
    if (fd < 0)
        return 0;

    char buf[2048];
    long got = lp_read((int)fd, buf, sizeof buf - 1);
    lp_close((int)fd);
    if (got <= 0)
        return 0;
    buf[got] = '\0';

    int n = 0;
    for (char *line = buf; line && *line && n < max; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        while (*line == ' ' || *line == '\t') line++;
        if (*line && *line != '#') {
            char *sp = strchr(line, ' ');
            if (sp) {
                *sp = '\0';
                strlcpy(out[n].name, line, sizeof out[n].name);
                strlcpy(out[n].label, sp + 1, sizeof out[n].label);
                if (out[n].name[0] && out[n].label[0])
                    n++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    return n;
}

static bool save_conf(const adopted_t *list, int n)
{
    long fd = lp_open(CONF, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "storage: cannot write %s - is /data mounted?\n", CONF);
        return false;
    }

    const char *head =
        "# storage.conf - drives this system keeps.\n"
        "#\n"
        "# One per line: <name> <filesystem label>\n"
        "# Each is mounted at /mnt/<name> at every boot.\n"
        "#\n"
        "# Matched by label, not by device name, because sdb1 becomes\n"
        "# sdc1 as soon as something else is plugged in first.\n";
    lp_write((int)fd, head, strlen(head));

    for (int i = 0; i < n; i++) {
        char line[80];
        int len = snprintf(line, sizeof line, "%s %s\n",
                           list[i].name, list[i].label);
        lp_write((int)fd, line, (size_t)len);
    }
    lp_close((int)fd);
    lp_sync();
    return true;
}

/* ── finding things ──────────────────────────────────────────────── */

/* Every partition on every disk, plus disks with no partition table. */
static int all_volumes(blk_t *out, int max)
{
    blk_t disks[DISK_MAX];
    int nd = disk_list(disks, DISK_MAX);
    int n = 0;

    for (int i = 0; i < nd && n < max; i++) {
        blk_t parts[DISK_PARTS];
        int np = disk_parts(disks[i].path, parts, DISK_PARTS);
        if (np <= 0) {
            out[n++] = disks[i];
            continue;
        }
        for (int j = 0; j < np && n < max; j++)
            out[n++] = parts[j];
    }
    return n;
}

static bool find_by_label(const char *label, blk_t *out)
{
    blk_t v[DISK_MAX * DISK_PARTS];
    int n = all_volumes(v, DISK_MAX * DISK_PARTS);
    for (int i = 0; i < n; i++)
        if (v[i].label[0] && strcmp(v[i].label, label) == 0) {
            *out = v[i];
            return true;
        }
    return false;
}

static bool find_by_name(const char *dev, blk_t *out)
{
    char want[48];
    if (dev[0] == '/')
        strlcpy(want, dev, sizeof want);
    else
        snprintf(want, sizeof want, "/dev/%s", dev);

    blk_t v[DISK_MAX * DISK_PARTS];
    int n = all_volumes(v, DISK_MAX * DISK_PARTS);
    for (int i = 0; i < n; i++)
        if (strcmp(v[i].path, want) == 0) {
            *out = v[i];
            return true;
        }
    return false;
}

/* ── how full ────────────────────────────────────────────────────── */

/* lp_fs_space is what df uses: free bytes and total bytes for whatever
 * filesystem holds this path. "Free" is what an ordinary process may
 * still use, which is the number people actually want. */
static bool space_of(const char *path, u64 *total, u64 *avail)
{
    u64 freeb = 0, totalb = 0;
    if (lp_fs_space(path, &freeb, &totalb) < 0 || totalb == 0)
        return false;
    *total = totalb;
    *avail = freeb;
    return true;
}

static void print_place(const char *what, const char *where, const char *dev)
{
    u64 total = 0, avail = 0;
    if (!space_of(where, &total, &avail)) {
        printf("  %-10s %-14s %s\n", what, where, "(not mounted)");
        return;
    }

    char t[12], a[12];
    disk_human(total, t, sizeof t);
    disk_human(avail, a, sizeof a);

    int pct = total ? (int)(((total - avail) * 100) / total) : 0;
    printf("  %-10s %-14s %6s free of %-6s  %3d%% used   %s\n",
           what, where, a, t, pct, dev ? dev : "");
}

/* ── the overview ────────────────────────────────────────────────── */

static void show(void)
{
    char boot_dev[48] = "", data_dev[48] = "";

    /* Which device is behind /boot and /data. */
    blk_t v[DISK_MAX * DISK_PARTS];
    int n = all_volumes(v, DISK_MAX * DISK_PARTS);
    for (int i = 0; i < n; i++) {
        if (strcmp(v[i].mount, "/boot") == 0) strlcpy(boot_dev, v[i].path, sizeof boot_dev);
        if (strcmp(v[i].mount, "/data") == 0) strlcpy(data_dev, v[i].path, sizeof data_dev);
    }

    printf("what survives a reboot\n");
    print_place("boot", "/boot", boot_dev[0] ? boot_dev : "(none)");
    print_place("data", "/data", data_dev[0] ? data_dev : "(none)");

    adopted_t list[MAX_ENTRIES];
    int na = load_conf(list, MAX_ENTRIES);
    for (int i = 0; i < na; i++) {
        char point[64];
        snprintf(point, sizeof point, "%s/%s", MNT_ROOT, list[i].name);

        blk_t b;
        bool here = find_by_label(list[i].label, &b);
        print_place(list[i].name, point, here ? b.path : "(drive not attached)");
    }

    /* Anything mounted under /media is a drive we found but nobody has
     * asked us to keep. Saying so is the whole point - otherwise the
     * only way to learn that adopting is possible is to read the help. */
    long fd = lp_open("/proc/mounts", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[4096];
        long got = lp_read((int)fd, buf, sizeof buf - 1);
        lp_close((int)fd);
        if (got > 0) {
            buf[got] = '\0';
            bool said = false;
            for (char *line = buf; line && *line; ) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = '\0';
                if (strncmp(line, "/dev/", 5) == 0 && strstr(line, "/media/")) {
                    if (!said) {
                        printf("\nplugged in, but not kept across reboots\n");
                        said = true;
                    }
                    char *sp = strchr(line, ' ');
                    if (sp) {
                        *sp = '\0';
                        char *sp2 = strchr(sp + 1, ' ');
                        if (sp2) *sp2 = '\0';
                        print_place("", sp + 1, line);
                    }
                }
                line = nl ? nl + 1 : NULL;
            }
            if (said)
                printf("\n  `storage adopt <device> <name>` keeps one of"
                       " these at /mnt/<name>\n");
        }
    }
}

/* ── adopting ────────────────────────────────────────────────────── */

static bool name_is_sane(const char *s)
{
    if (!s || !s[0] || strlen(s) > 24)
        return false;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

/* Give the filesystem a label, so we can find it again wherever it is
 * plugged in. Without one there is nothing stable to match on. */
static bool set_label(const blk_t *b, const char *label)
{
    if (strcmp(b->fs, "ext4") != 0 && strcmp(b->fs, "ext3") != 0 &&
        strcmp(b->fs, "ext2") != 0) {
        /* Labelling vfat/exfat needs a different tool for each. Rather
         * than write three, we match those on whatever label they came
         * with, and say so. */
        return false;
    }

    long fd = lp_open(b->path, O_WRONLY, 0);
    if (fd < 0)
        return false;

    /* The ext superblock is at byte 1024; s_volume_name is at +0x78 and
     * is 16 bytes, padded with NULs rather than terminated. */
    char field[16];
    memset(field, 0, sizeof field);
    for (int i = 0; i < 16 && label[i]; i++)
        field[i] = label[i];

    if (lp_lseek((int)fd, 1024 + 0x78, SEEK_SET) < 0) {
        lp_close((int)fd);
        return false;
    }
    bool ok = lp_write((int)fd, field, sizeof field) == (long)sizeof field;
    lp_close((int)fd);
    lp_sync();
    return ok;
}

static int cmd_adopt(const char *dev, const char *name)
{
    blk_t b;
    if (!find_by_name(dev, &b)) {
        dprintf(STDERR_FILENO, "storage: no such volume: %s\n", dev);
        dprintf(STDERR_FILENO, "  `disk` lists what is attached\n");
        return 1;
    }

    if (!name)
        name = b.label[0] ? b.label : b.name;
    if (!name_is_sane(name)) {
        dprintf(STDERR_FILENO,
                "storage: \"%s\" will not do as a name -"
                " letters, digits, - and _ only\n", name);
        return 1;
    }

    char whole[40];
    if (disk_whole(b.path, whole, sizeof whole)) {
        blk_t sysv[DISK_MAX * DISK_PARTS];
        int n = all_volumes(sysv, DISK_MAX * DISK_PARTS);
        for (int i = 0; i < n; i++) {
            char w2[40];
            if ((strcmp(sysv[i].mount, "/data") == 0 ||
                 strcmp(sysv[i].mount, "/boot") == 0) &&
                disk_whole(sysv[i].path, w2, sizeof w2) &&
                strcmp(w2, whole) == 0) {
                dprintf(STDERR_FILENO,
                        "storage: %s is on the disk this system booted"
                        " from - refusing\n", b.path);
                dprintf(STDERR_FILENO,
                        "  use `datadisk` to move /data instead\n");
                return 1;
            }
        }
    }

    if (!b.fs[0]) {
        dprintf(STDERR_FILENO,
                "storage: no filesystem on %s\n", b.path);
        dprintf(STDERR_FILENO,
                "  `storage format %s %s` would make one"
                " (this erases the drive)\n", dev, name);
        return 1;
    }

    /* Label it so we can find it again in a different port. */
    char label[24];
    if (b.label[0]) {
        strlcpy(label, b.label, sizeof label);
    } else if (set_label(&b, name)) {
        strlcpy(label, name, sizeof label);
        printf("labelled %s as \"%s\"\n", b.path, label);
    } else {
        dprintf(STDERR_FILENO,
                "storage: %s has no label and this tool can only label"
                " ext filesystems\n", b.path);
        dprintf(STDERR_FILENO,
                "  label it elsewhere, or `storage format` it here\n");
        return 1;
    }

    adopted_t list[MAX_ENTRIES];
    int n = load_conf(list, MAX_ENTRIES);
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].name, name) == 0) {
            dprintf(STDERR_FILENO,
                    "storage: \"%s\" is already taken (label %s)\n",
                    name, list[i].label);
            return 1;
        }
        if (strcmp(list[i].label, label) == 0) {
            dprintf(STDERR_FILENO,
                    "storage: that drive is already kept, as \"%s\"\n",
                    list[i].name);
            return 1;
        }
    }
    if (n >= MAX_ENTRIES) {
        dprintf(STDERR_FILENO, "storage: no room for another drive\n");
        return 1;
    }

    strlcpy(list[n].name, name, sizeof list[n].name);
    strlcpy(list[n].label, label, sizeof list[n].label);
    if (!save_conf(list, n + 1))
        return 1;

    /* Mount it now, at the place it will live from here on. */
    char point[64];
    snprintf(point, sizeof point, "%s/%s", MNT_ROOT, name);
    lp_mkdir(MNT_ROOT, 0755);
    lp_mkdir(point, 0755);

    char here[64];
    if (disk_mountpoint(b.path, here, sizeof here))
        lp_umount(here, MNT_DETACH);

    if (lp_mount(b.path, point, b.fs, MS_NOSUID | MS_NODEV, NULL) < 0) {
        printf("kept as \"%s\", but it could not be mounted now -"
               " it will be tried again at the next boot\n", name);
        return 0;
    }

    printf("%s is now \"%s\", at %s\n", b.path, name, point);
    printf("  it will be mounted there at every boot, wherever it is"
           " plugged in\n");
    return 0;
}

static int cmd_forget(const char *name)
{
    adopted_t list[MAX_ENTRIES];
    int n = load_conf(list, MAX_ENTRIES);

    int found = -1;
    for (int i = 0; i < n; i++)
        if (strcmp(list[i].name, name) == 0)
            found = i;

    if (found < 0) {
        dprintf(STDERR_FILENO, "storage: nothing called \"%s\"\n", name);
        return 1;
    }

    for (int i = found; i < n - 1; i++)
        list[i] = list[i + 1];
    if (!save_conf(list, n - 1))
        return 1;

    char point[64];
    snprintf(point, sizeof point, "%s/%s", MNT_ROOT, name);
    lp_umount(point, MNT_DETACH);
    lp_rmdir(point);

    printf("\"%s\" is no longer kept. The drive and everything on it is"
           " untouched.\n", name);
    return 0;
}

/* ── formatting ──────────────────────────────────────────────────── */

static int cmd_format(const char *dev, const char *name)
{
    blk_t b;
    if (!find_by_name(dev, &b)) {
        dprintf(STDERR_FILENO, "storage: no such volume: %s\n", dev);
        return 1;
    }

    char here[64];
    if (disk_mountpoint(b.path, here, sizeof here)) {
        dprintf(STDERR_FILENO,
                "storage: %s is mounted at %s - unmount it first\n",
                b.path, here);
        return 1;
    }

    char whole[40];
    if (disk_whole(b.path, whole, sizeof whole)) {
        blk_t sysv[DISK_MAX * DISK_PARTS];
        int n = all_volumes(sysv, DISK_MAX * DISK_PARTS);
        for (int i = 0; i < n; i++) {
            char w2[40];
            if (sysv[i].mount[0] &&
                disk_whole(sysv[i].path, w2, sizeof w2) &&
                strcmp(w2, whole) == 0) {
                dprintf(STDERR_FILENO,
                        "storage: something on that disk is mounted"
                        " (%s at %s) - refusing\n",
                        sysv[i].path, sysv[i].mount);
                return 1;
            }
        }
    }

    char size[12];
    disk_human(b.bytes, size, sizeof size);
    printf("This erases everything on %s (%s%s%s).\n",
           b.path, size,
           b.label[0] ? ", labelled " : "", b.label[0] ? b.label : "");
    printf("Type the device name again to go ahead: ");

    char answer[64];
    long got = lp_read(STDIN_FILENO, answer, sizeof answer - 1);
    if (got <= 0) {
        printf("\nnot done\n");
        return 1;
    }
    answer[got] = '\0';
    for (long i = 0; i < got; i++)
        if (answer[i] == '\n' || answer[i] == '\r')
            answer[i] = '\0';

    if (strcmp(answer, dev) != 0) {
        printf("that did not match - nothing was changed\n");
        return 1;
    }

    char tool[64];
    if (!boot_tool("mke2fs", tool, sizeof tool))
        return 1;

    const char *label = name ? name : (b.label[0] ? b.label : "STORAGE");
    printf("making an ext4 filesystem on %s...\n", b.path);

    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "storage: cannot start mke2fs\n");
        return 1;
    }
    if (pid == 0) {
        char *args[] = { (char *)tool, (char *)"-t", (char *)"ext4",
                         (char *)"-F", (char *)"-L", (char *)label,
                         (char *)b.path, NULL };
        char *envp[] = { NULL };
        lp_execve(tool, args, envp);
        lp_exit(127);
    }
    int status = 0;
    lp_waitpid(pid, &status, 0);
    int rc = LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : -1;
    if (rc != 0) {
        dprintf(STDERR_FILENO, "storage: mke2fs failed (%d)\n", rc);
        return 1;
    }

    lp_sync();
    printf("done\n");
    return cmd_adopt(dev, name);
}

/* ── entry ───────────────────────────────────────────────────────── */

static void usage(void)
{
    printf("usage:\n");
    printf("  storage                      what survives a reboot\n");
    printf("  storage adopt <dev> [name]   keep this drive at /mnt/<name>\n");
    printf("  storage forget <name>        stop keeping it\n");
    printf("  storage format <dev> [name]  erase it, make ext4, then keep it\n");
    printf("\n");
    printf("An adopted drive is found by its filesystem label, so it works\n");
    printf("in any port. `disk` lists what is attached; `automount -l` shows\n");
    printf("what was plugged in but not kept.\n");
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        show();
        return 0;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0) {
        usage();
        return 0;
    }
    if (strcmp(argv[1], "adopt") == 0 && argc >= 3)
        return cmd_adopt(argv[2], argc >= 4 ? argv[3] : NULL);
    if (strcmp(argv[1], "forget") == 0 && argc >= 3)
        return cmd_forget(argv[2]);
    if (strcmp(argv[1], "format") == 0 && argc >= 3)
        return cmd_format(argv[2], argc >= 4 ? argv[3] : NULL);

    dprintf(STDERR_FILENO, "storage: do not understand \"%s\"\n", argv[1]);
    usage();
    return 2;
}
