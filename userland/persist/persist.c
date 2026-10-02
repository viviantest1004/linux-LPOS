/* persist - keep what gets installed into the root filesystem
 *
 * The root filesystem of this machine is a cpio archive inside the
 * kernel image, unpacked into RAM at every boot. That is what makes the
 * system recover from anything: whatever a bad day did to /bin, the next
 * boot has the original back. It also means that anything installed
 * there is gone at the next power cut, which is the wrong answer to
 * `cp mytool /bin/` - the command succeeds, the file is there, and a
 * reboot silently undoes it.
 *
 * `pkg` sidesteps this by installing to /data/bin, and python packages
 * go to /data/python. But a program that was built somewhere else, or a
 * `make install` that writes to /usr/local, or a file copied into /bin
 * by hand, all landed in RAM.
 *
 * So each of those directories becomes an overlay: the copy in the
 * kernel image underneath, a directory on /data on top. Reads see both,
 * writes land on /data, and the next boot puts the overlay back. Nothing
 * has to know about it - `cp`, `make install`, a tarball unpacked with
 * tar, they all just work and the result is still there tomorrow.
 *
 * What this deliberately does NOT cover:
 *
 *   /etc  - the boot reads /etc/services and /etc/rc before /data is
 *           even mounted, and a stale copy of either shadowing a
 *           system update is how a board stops booting. Configuration
 *           that is meant to last already has its own files on /data.
 *   /tmp  - it is supposed to be lost. That is what it is for.
 *   /var  - runtime state. Writing service pids to the card every time
 *           a service restarts is wear for nothing.
 *
 * The overlays are mounted nosuid and nodev, for the same reason /data
 * itself is: the upper directories are on a card that can be taken to
 * another machine, and without those flags a setuid-root file written
 * there from a PC would be setuid-root here. Nothing in this system is
 * setuid, so nothing legitimate is lost.
 *
 * One consequence worth knowing. If a system update ships a new
 * /bin/foo and an older /bin/foo is in the upper layer, the old one
 * wins - that is what "on top" means. `persist` on its own lists what
 * is up there, and `persist forget <name>` removes one file so the
 * image's copy shows through again.
 *
 * ── `persist on` is not a boot-time-only command any more ──
 *
 * It used to be run exactly once, from /etc/rc, and everything here
 * could assume a machine that had just started. It cannot any more.
 *
 * The upper layer of all six of these overlays is on the SD card. Pull
 * the card out of a running board and every one of them is a mount
 * whose disk has gone - which is to say /bin, /sbin, /lib, /usr, /opt
 * and /srv are all dead, and there is no command left on the machine to
 * run. automount(8) is the daemon that handles that: it is already
 * running, its own pages come from the RAM root, and it detaches the
 * six with umount2(MNT_DETACH) so that /bin is the system image again
 * and the board is usable. When the card comes back it mounts /data
 * again and runs `persist on` a second time to rebuild them.
 *
 * So this has to work on a live system with processes running, not only
 * on a freshly booted one. Two things follow, and both are handled
 * below rather than assumed:
 *
 *   Some of the six may already be overlaid and some not, if the card
 *   was pulled halfway through something. Each is looked at on its own
 *   and one that is already up is left alone.
 *
 *   A mount can be refused with EBUSY, which does not happen at boot
 *   and is not obvious when it does. overlayfs marks the upper and work
 *   directories in use for as long as the overlay's superblock exists,
 *   and a superblock outlives umount2(MNT_DETACH) until the last
 *   process holding a file in it goes away - which, for /bin, is every
 *   process on the machine, because each has its own executable open
 *   there. When the card was physically removed this does not arise:
 *   the filesystem underneath is destroyed with the device and comes
 *   back as a new one with new inodes. When the card never left - ext4
 *   went read-only under it and was remounted - it can. mount_one says
 *   so in those words rather than printing a number.
 */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "disk.h"

/* getdents64 records: reclen at byte 16, the name at 19. */
#define DIRENT_RECLEN  16
#define DIRENT_NAME    19

#define UPPER_ROOT  "/data/persist"
#define WORK_ROOT   "/data/persist/.work"

/* The directories a person or an installer writes programs into. */
static const char *DIRS[] = {
    "/bin", "/sbin", "/lib", "/usr", "/opt", "/srv", NULL
};

static bool ensure_dir(const char *path)
{
    if (lp_is_dir(path))
        return true;
    if (lp_mkdir(path, 0755) == 0)
        return true;
    return lp_is_dir(path);
}

/* /bin -> bin, for building the paths under /data. */
static const char *leaf(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static bool already_overlaid(const char *dir)
{
    char buf[8192];
    if (proc_read("/proc/mounts", buf, sizeof buf) <= 0)
        return false;

    for (const char *p = buf; *p; ) {
        /* "overlay /bin overlay rw,... 0 0" */
        if (strncmp(p, "overlay ", 8) == 0) {
            const char *m = p + 8;
            size_t n = strlen(dir);
            if (strncmp(m, dir, n) == 0 && m[n] == ' ')
                return true;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return false;
}

static bool mount_one(const char *dir, bool loud)
{
    char upper[128], work[128], opts[512];

    if (!ensure_dir(dir)) {
        if (loud)
            dprintf(STDERR_FILENO,
                    "persist: %s is not there and cannot be created\n", dir);
        return false;
    }

    snprintf(upper, sizeof upper, "%s/%s", UPPER_ROOT, leaf(dir));
    snprintf(work,  sizeof work,  "%s/%s", WORK_ROOT,  leaf(dir));

    if (!ensure_dir(upper) || !ensure_dir(work)) {
        if (loud)
            dprintf(STDERR_FILENO,
                    "persist: cannot make %s on the data partition -"
                    " is it mounted and writable?\n", upper);
        return false;
    }

    /* index=off and xino=off, and they are not optional here.
     *
     * Both features record the identity of a file in the lower layer -
     * an inode number, a file handle - and check it on the next mount.
     * That is right when the lower layer is a filesystem on a disk. Ours
     * is a cpio archive unpacked into RAM at every boot, so every inode
     * number is new every time, verification fails, and the mount is
     * refused with ESTALE. Which is exactly what happened: the first
     * boot mounted cleanly and the second refused all six, so the files
     * installed on the first boot were invisible - the failure this
     * program exists to prevent, arriving one boot later.
     *
     * redirect_dir stays on: it records a path, and paths do not change
     * between boots. */
    snprintf(opts, sizeof opts,
             "lowerdir=%s,upperdir=%s,workdir=%s,index=off,xino=off",
             dir, upper, work);

    long rc = lp_mount("overlay", dir, "overlay",
                       MS_NOSUID | MS_NODEV, opts);
    if (rc < 0) {
        if (loud) {
            dprintf(STDERR_FILENO,
                    "persist: cannot keep %s (%ld) - files written there"
                    " will be lost at the next boot\n", dir, -rc);
            /* EBUSY here means one thing and it is not "the directory is
             * busy". overlayfs refuses to use an upper or work directory
             * that another overlay superblock still claims, and a
             * superblock detached with MNT_DETACH lives on until the
             * last process with a file open in it exits. That is every
             * process on the machine where /bin is concerned. It cannot
             * be waited out and there is nothing to unmount - the mount
             * is already gone from the tree - so say what it is and what
             * clears it. */
            if (-rc == 16 /* EBUSY */)
                dprintf(STDERR_FILENO,
                        "persist:   %s is still claimed by the overlay"
                        " that was taken down, and will be until every\n"
                        "persist:   process started before then has"
                        " exited. A reboot is the reliable way back.\n",
                        upper);
        }
        return false;
    }
    return true;
}

/* Count the files a directory tree holds, and their bytes. */
static void measure(const char *path, int *files, long *bytes, int depth)
{
    if (depth > 8)
        return;

    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;

    char buf[4096];
    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0)
            break;

        for (long off = 0; off < got; ) {
            char *rec  = buf + off;
            u16   rlen = *(u16 *)(rec + DIRENT_RECLEN);
            char *name = rec + DIRENT_NAME;
            if (rlen == 0)
                break;
            off += rlen;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            char sub[1024];
            snprintf(sub, sizeof sub, "%s/%s", path, name);

            lp_stat_t st;
            if (lp_stat(sub, &st, false) < 0)
                continue;

            if ((st.mode & LP_S_IFMT) == LP_S_IFDIR) {
                measure(sub, files, bytes, depth + 1);
            } else {
                (*files)++;
                *bytes += (long)st.size;
            }
        }
    }
    lp_close((int)fd);
}

static void show(void)
{
    if (root_is_persistent()) {
        printf("The root filesystem is on disk, not in RAM.\n\n"
               "  Anything written to /bin, /usr, /etc or anywhere else"
               " is simply there\n"
               "  after a reboot. There is no overlay, and nothing for"
               " this command to\n"
               "  set up or take away.\n\n"
               "  df /   shows how much room is left.\n");
        return;
    }

    printf("Files installed into the system are kept on the data"
           " partition,\nso they are still there after a reboot.\n\n");

    int  total_files = 0;
    long total_bytes = 0;

    for (int i = 0; DIRS[i]; i++) {
        char upper[128];
        snprintf(upper, sizeof upper, "%s/%s", UPPER_ROOT, leaf(DIRS[i]));

        int  files = 0;
        long bytes = 0;
        measure(upper, &files, &bytes, 0);
        total_files += files;
        total_bytes += bytes;

        printf("  %-8s %-6s %d file%s", DIRS[i],
               already_overlaid(DIRS[i]) ? "kept" : "**",
               files, files == 1 ? "" : "s");
        if (bytes >= 1024 * 1024)
            printf("  %ld MB", bytes / (1024 * 1024));
        else if (bytes > 0)
            printf("  %ld KB", (bytes + 1023) / 1024);
        printf("\n");
    }

    printf("\n  %d file%s in all, %ld KB on the card.\n",
           total_files, total_files == 1 ? "" : "s",
           (total_bytes + 1023) / 1024);

    printf("\n  /etc is not in that list - the boot reads part of it before"
           " /data\n"
           "  exists. Individual files can still be kept:"
           " `persist etc`.\n");

    for (int i = 0; DIRS[i]; i++) {
        if (!already_overlaid(DIRS[i])) {
            printf("\n  ** is a directory that is NOT being kept: anything"
                   " written there\n"
                   "     is in RAM and goes at the next boot. That happens"
                   " when the data\n"
                   "     partition is missing or read-only - `disk` and"
                   " `fsck` say which.\n");
            break;
        }
    }
}


/* ── keeping a file from /etc ─────────────────────────────────────
 *
 * /etc deliberately has no overlay. /etc/rc and /etc/services are read
 * BEFORE /data is mounted, so an overlay could not be in place in time
 * anyway, and a stale copy of either shadowing a system update is
 * exactly how a board stops booting.
 *
 * But that reasoning only applies to the files the boot itself reads.
 * Everything else in /etc - a hosts file, a timezone, a message of the
 * day, a configuration file something installed through apt expects -
 * is simply lost at every reboot, and the answer "put it in
 * /data/rc.local" is not an answer for a file.
 *
 * So: a named file can be kept. `persist etc keep /etc/hosts` copies it
 * to /data/etc, and /etc/rc copies /data/etc back over /etc once /data
 * is mounted - after the boot has already read the two files it must
 * read from the image. A copy, not a mount: /etc is small, the copy
 * costs nothing, and it cannot leave /etc half-shadowed if /data goes
 * away mid-boot.
 *
 * The refused list is short and each entry has a reason, because a
 * silent refusal reads as a bug. */
#define ETC_KEEP "/data/etc"

static bool etc_refused(const char *name, const char **why)
{
    if (strcmp(name, "rc") == 0 || strcmp(name, "services") == 0) {
        *why = "the boot reads it before /data is mounted, so a kept copy"
               " could never be in place in time - and an old one shadowing"
               " a system update is how a board stops booting";
        return true;
    }
    if (strcmp(name, "passwd") == 0 || strcmp(name, "shadow") == 0 ||
        strcmp(name, "group") == 0) {
        *why = "users are already kept, in /data/users, and merged in at"
               " boot. Keeping the whole file would let a line with uid 0"
               " in it arrive from a card written on another machine";
        return true;
    }
    return false;
}

static bool copy_file(const char *from, const char *to, mode_t mode)
{
    long in = lp_open(from, O_RDONLY, 0);
    if (in < 0)
        return false;
    long out = lp_open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out < 0) { lp_close((int)in); return false; }

    char buf[8192];
    bool ok = true;
    for (;;) {
        long n = lp_read((int)in, buf, sizeof buf);
        if (n < 0)  { ok = false; break; }
        if (n == 0) break;
        if (lp_write((int)out, buf, (size_t)n) != n) { ok = false; break; }
    }
    lp_close((int)in);
    lp_close((int)out);
    if (!ok) lp_unlink(to);
    return ok;
}

/* Walk /data/etc, calling back for each plain file in it. */
static int etc_each(void (*fn)(const char *name, void *ctx), void *ctx)
{
    long fd = lp_open(ETC_KEEP, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 0;

    int seen = 0;
    char buf[4096];
    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0)
            break;
        for (long off = 0; off < got; ) {
            char *rec  = buf + off;
            u16   rlen = *(u16 *)(rec + DIRENT_RECLEN);
            char *name = rec + DIRENT_NAME;
            if (rlen == 0) break;
            off += rlen;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

            char full[512];
            snprintf(full, sizeof full, "%s/%s", ETC_KEEP, name);
            lp_stat_t st;
            if (lp_stat(full, &st, false) < 0) continue;
            if ((st.mode & LP_S_IFMT) != LP_S_IFREG) continue;

            seen++;
            if (fn) fn(name, ctx);
        }
    }
    lp_close((int)fd);
    return seen;
}

static void etc_print(const char *name, void *ctx)
{
    (void)ctx;
    char full[512];
    snprintf(full, sizeof full, "%s/%s", ETC_KEEP, name);
    lp_stat_t st;
    long size = (lp_stat(full, &st, false) == 0) ? (long)st.size : 0;
    printf("    /etc/%-24s %6ld bytes\n", name, size);
}

static void etc_restore_one(const char *name, void *ctx)
{
    int *failed = (int *)ctx;
    char from[512], to[512];
    snprintf(from, sizeof from, "%s/%s", ETC_KEEP, name);
    snprintf(to,   sizeof to,   "/etc/%s", name);

    const char *why;
    if (etc_refused(name, &why))
        return;                        /* never, whatever is on the card */

    lp_stat_t st;
    mode_t mode = (lp_stat(from, &st, false) == 0) ? (st.mode & 07777) : 0644;
    if (!copy_file(from, to, mode))
        (*failed)++;
}

static int etc_cmd(int argc, char **argv)
{
    /* /etc is a real directory on a disk-rooted machine, so both halves
     * of this - copying files out to /data and putting them back at the
     * next boot - are answering a question that is not being asked.
     * Restore is a plain success; keep says so rather than making a
     * second copy that will drift from the file it came from. */
    if (root_is_persistent()) {
        if (argc > 2 && strcmp(argv[2], "restore") == 0)
            return 0;
        printf("persist: /etc is on disk here - it keeps what you write"
               " to it, with nothing to set up\n");
        return 0;
    }

    /* persist etc restore - what /etc/rc calls. Quiet unless it fails. */
    if (argc > 2 && strcmp(argv[2], "restore") == 0) {
        int failed = 0;
        etc_each(etc_restore_one, &failed);
        if (failed)
            dprintf(STDERR_FILENO,
                    "persist: %d kept /etc file%s could not be put back\n",
                    failed, failed == 1 ? "" : "s");
        return failed ? 1 : 0;
    }

    if (argc > 3 && strcmp(argv[2], "keep") == 0) {
        const char *arg = argv[3];
        const char *name = leaf(arg);

        /* Accept "hosts" and "/etc/hosts" alike, but refuse a path that
         * is not in /etc at all rather than quietly keeping the leaf of
         * something else. */
        if (strchr(arg, '/') && strncmp(arg, "/etc/", 5) != 0) {
            dprintf(STDERR_FILENO,
                    "persist: %s is not in /etc.\n"
                    "persist:   /bin /sbin /lib /usr /opt and /srv are"
                    " already kept whole - just write the file.\n", arg);
            return 1;
        }
        if (strchr(name, '/')) {
            dprintf(STDERR_FILENO,
                    "persist: only files directly in /etc can be kept,"
                    " not %s\n", arg);
            return 1;
        }

        const char *why;
        if (etc_refused(name, &why)) {
            dprintf(STDERR_FILENO,
                    "persist: /etc/%s is not kept, because %s.\n", name, why);
            return 1;
        }

        char src[512];
        snprintf(src, sizeof src, "/etc/%s", name);
        lp_stat_t st;
        if (lp_stat(src, &st, false) < 0) {
            dprintf(STDERR_FILENO, "persist: there is no %s to keep\n", src);
            return 1;
        }
        if ((st.mode & LP_S_IFMT) != LP_S_IFREG) {
            dprintf(STDERR_FILENO, "persist: %s is not a plain file\n", src);
            return 1;
        }

        if (!ensure_dir(ETC_KEEP)) {
            dprintf(STDERR_FILENO,
                    "persist: cannot create %s - is /data mounted?\n",
                    ETC_KEEP);
            return 1;
        }
        char dst[512];
        snprintf(dst, sizeof dst, "%s/%s", ETC_KEEP, name);
        if (!copy_file(src, dst, st.mode & 07777)) {
            dprintf(STDERR_FILENO, "persist: could not copy %s\n", src);
            return 1;
        }

        printf("persist: /etc/%s is kept, and put back at every boot.\n", name);
        printf("persist:   `persist etc keep /etc/%s` again after you edit"
               " it - the copy\n"
               "persist:   on the card is not updated by editing /etc.\n",
               name);
        return 0;
    }

    if (argc > 3 && strcmp(argv[2], "forget") == 0) {
        const char *name = leaf(argv[3]);
        char dst[512];
        snprintf(dst, sizeof dst, "%s/%s", ETC_KEEP, name);
        if (lp_unlink(dst) < 0) {
            dprintf(STDERR_FILENO, "persist: /etc/%s was not being kept\n",
                    name);
            return 1;
        }
        printf("persist: /etc/%s is no longer kept. The image's copy is back"
               " at the next reboot.\n", name);
        return 0;
    }

    /* persist etc - what is being kept */
    printf("/etc is rebuilt from the kernel image at every boot, so files"
           " kept here\nare copied back over it once /data is mounted.\n\n");
    int n = etc_each(etc_print, NULL);
    if (!n)
        printf("    nothing yet\n");
    printf("\n  persist etc keep /etc/hosts     keep one\n");
    printf("  persist etc forget /etc/hosts   stop\n\n");
    printf("/etc/rc and /etc/services cannot be kept: the boot reads them"
           " before\n/data exists. Put boot-time commands in"
           " /data/rc.local instead.\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "etc") == 0)
        return etc_cmd(argc, argv);

    if (argc > 1 && strcmp(argv[1], "on") == 0) {
        /* Called from /etc/rc, once, after /data is mounted. Quiet when
         * it works: the boot has enough to say already. */

        /* On a disk-rooted machine there is nothing to do, and doing it
         * anyway would be harmful: an overlay whose lower layer is the
         * real /bin and whose upper layer is a directory on that same
         * filesystem hides the real one behind a copy of itself, and
         * every later update writes to the copy. Success, because /bin
         * does keep what is written to it - which is what the caller
         * asked about. */
        if (root_is_persistent()) {
            printf("persist: the root is on disk and already keeps what"
                   " is installed into it\n");
            return 0;
        }

        if (!lp_is_dir("/data")) {
            dprintf(STDERR_FILENO,
                    "persist: no /data - nothing installed into the system"
                    " will survive a reboot\n");
            return 1;
        }
        if (!ensure_dir(UPPER_ROOT) || !ensure_dir(WORK_ROOT)) {
            dprintf(STDERR_FILENO,
                    "persist: cannot write to %s - nothing installed into"
                    " the system will survive a reboot\n", UPPER_ROOT);
            return 1;
        }

        int kept = 0, failed = 0;
        for (int i = 0; DIRS[i]; i++) {
            if (already_overlaid(DIRS[i])) { kept++; continue; }
            if (mount_one(DIRS[i], true)) kept++;
            else                          failed++;
        }
        if (failed)
            dprintf(STDERR_FILENO,
                    "persist: keeping %d of %d directories\n",
                    kept, kept + failed);
        return failed ? 1 : 0;
    }

    if (argc > 2 && strcmp(argv[1], "forget") == 0) {
        /* Remove one file from the upper layer, so the copy in the
         * kernel image shows through again. */
        const char *want = argv[2];
        int removed = 0;

        for (int i = 0; DIRS[i]; i++) {
            char p[512];
            snprintf(p, sizeof p, "%s/%s/%s",
                     UPPER_ROOT, leaf(DIRS[i]), leaf(want));
            lp_stat_t st;
            if (lp_stat(p, &st, false) < 0)
                continue;
            if (lp_unlink(p) == 0) {
                printf("persist: forgot %s/%s\n", DIRS[i], leaf(want));
                removed++;
            } else {
                dprintf(STDERR_FILENO,
                        "persist: cannot remove %s\n", p);
            }
        }

        if (!removed) {
            dprintf(STDERR_FILENO,
                    "persist: nothing called %s was added to the system\n",
                    leaf(want));
            return 1;
        }
        printf("persist:   the version in the system image is back."
               " It takes a reboot\n"
               "persist:   to see it, because the old one is still open.\n");
        return 0;
    }

    if (argc > 1) {
        printf("usage: persist            what is being kept\n");
        printf("       persist on         start keeping it (from /etc/rc)\n");
        printf("       persist forget <name>\n");
        printf("                          drop one added file, so the\n");
        printf("                          system's own copy is used again\n");
        return argc > 1 && strcmp(argv[1], "-h") == 0 ? 0 : 2;
    }

    show();
    return 0;
}
