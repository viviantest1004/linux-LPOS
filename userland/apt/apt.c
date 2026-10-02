/* apt - Debian's package manager, on this system.
 *
 *   apt install <package>...     what you came for
 *   apt remove <package>...
 *   apt update                   refresh the package lists
 *   apt search <text>
 *   apt setup                    fetch the Debian base (first run does this)
 *   apt shell                    a shell inside the Debian tree
 *   apt status                   what is set up, and how much room it takes
 *
 * ── Why this is not just a binary we ship ──
 *
 * apt is not one program. It is apt, apt-get, dpkg, a database under
 * /var/lib/dpkg, maintainer scripts that expect /bin/sh and coreutils,
 * and a filesystem laid out the way Debian lays one out. dpkg has those
 * paths compiled in - it does not take a --root that means what you
 * would want it to mean. Dropping the binaries onto this system would
 * produce something that starts and then cannot install anything.
 *
 * So the whole Debian userland goes in one directory on /data, and apt
 * runs inside it with that directory as its root. Everything it
 * installs lands there too, which means:
 *
 *   - the system image is untouched, and stays the size it is
 *   - `apt remove` cannot break this machine's own commands, because
 *     they are not in there
 *   - deleting /data/debian undoes all of it, completely
 *
 * ── Why it downloads instead of shipping ──
 *
 * A minimal Debian is about 120MB unpacked. The whole point of this
 * system is that it is 11-23MB, so it cannot carry one. The first run
 * of `apt` fetches it, and says so before it starts.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "disk.h"
#include "net.h"

#define ROOT        "/data/debian"
#define MARKER      ROOT "/etc/debian_version"
/* Where the list of mirrors goes.
 *
 * The deb822 file under sources.list.d, not the one-line
 * /etc/apt/sources.list. Debian moved to this format in trixie and the
 * base image ships one already; writing the old file as well left every
 * suite configured twice, and apt then printed six "is configured
 * multiple times" warnings before and after every single command. It
 * still worked, and it looked broken, which for a package manager is
 * nearly as bad. */
#define SOURCES     ROOT "/etc/apt/sources.list.d/debian.sources"
#define OLD_SOURCES ROOT "/etc/apt/sources.list"
#define RESOLV      ROOT "/etc/resolv.conf"

/* Which Debian.
 *
 * trixie - Debian 13, the current stable release. A stable release is
 * the point: this is a board that gets left alone for months and its
 * package versions should stop moving.
 *
 * It is also the only suite all three architectures have. The armel
 * branch carries trixie and stable and no bookworm at all, so pinning
 * bookworm - which is what this said before - would have left the Pi
 * Zero W with nothing to install even after everything else was fixed. */
#define SUITE       "trixie"

/* http, not https, and deliberately.
 *
 * What protects a package here is its signature: apt checks the Release
 * file against the Debian archive keys that came with the base
 * filesystem, in /etc/apt/trusted.gpg.d, and refuses anything that does
 * not verify. TLS adds nothing to that - it is why Debian's own
 * sources.list has always been http, and why every official container
 * image ships it that way.
 *
 * What TLS does add here is failure. This board has no real-time clock:
 * the time comes from ntp, and until that answers every certificate on
 * earth looks expired or not yet valid. Behind a network that inspects
 * TLS - a school, an office - the certificate is signed by a CA the
 * Debian tree has never heard of, and every line of sources.list fails
 * with "the certificate issuer is unknown". Both of those are a board
 * that cannot install anything, for no gain in what an attacker could
 * actually do.
 *
 * The cost is that whoever carries the packets can see which packages
 * are being fetched. Anybody who minds can change the two lines in
 * /data/debian/etc/apt/sources.list to https - the base image carries a
 * CA store, so it works - and the file itself says so. */
#define MIRROR      "http://deb.debian.org/debian"

/* ── The base filesystem, and where it comes from ────────────────────
 *
 * debuerreotype builds these: they are what the official Debian
 * container images are made from, one per architecture, and each is a
 * plain gzipped tar of a working Debian root. They are committed to a
 * git branch, so raw.githubusercontent.com serves them as static files
 * over TLS - no API, no token, no expiring signed URL.
 *
 * The "slim" variant, which is the same tree with the documentation
 * and locales left out: 28MB instead of 47MB to fetch and 98MB instead
 * of 142MB on the card, with apt and dpkg both present. On a board
 * whose whole system image is 11MB that difference is worth having.
 *
 * This used to go through a Python script to images.linuxcontainers.org,
 * which fetched a .tar.xz and unpacked it with Python's tarfile. Two
 * problems with that, and the second is fatal: there is no Python on
 * this image, and the only 32-bit ARM build that server has is armhf,
 * which is ARMv7 and will not execute one instruction on a Pi Zero W.
 * Now the download is net_http_get, the decompression is our gzip and
 * the unpacking is our tar, and nothing else has to be installed first.
 */

/* Debian's name for the machine, and debuerreotype's name for the
 * branch, which are not the same string - the branch is named after the
 * Docker platform (arm64v8, arm32v5) and the port is named after the
 * ABI (arm64, armel).
 *
 * armel and not armhf for the Pi Zero W. armhf has an ARMv7 baseline;
 * the ARM1176 in a Zero W is ARMv6, and every armhf binary that uses a
 * movw or a Thumb-2 encoding - which is most of them - dies with SIGILL
 * on it. armel's baseline is ARMv5TE, which an ARM1176 runs. This is
 * the same distinction that made the rest of this port use an armel
 * cross compiler; see tools/build-thirdparty.sh. */
#if defined(__x86_64__)
#  define DEB_ARCH   "amd64"
#  define DEB_BRANCH "amd64"
#elif defined(__aarch64__)
#  define DEB_ARCH   "arm64"
#  define DEB_BRANCH "arm64v8"
#elif defined(__arm__)
#  define DEB_ARCH   "armel"
#  define DEB_BRANCH "arm32v5"
#else
#  error "apt has no Debian architecture for this machine"
#endif

#define BASE_URL \
    "https://raw.githubusercontent.com/debuerreotype/" \
    "docker-debian-artifacts/dist-" DEB_BRANCH "/" SUITE \
    "/slim/oci/blobs/rootfs.tar.gz"

/* Where the download and the intermediate tar live while unpacking. On
 * /data, not /tmp: /tmp is in RAM here and this is a hundred megabytes. */
#define TARGZ  "/data/.debian-base.tar.gz"
#define TARBALL "/data/.debian-base.tar"

static const char *me = "apt";

/* ── small helpers ───────────────────────────────────────────────── */

static bool exists(const char *p) { return lp_access(p, F_OK) == 0; }

static int run_wait(const char *path, char *const argv[])
{
    pid_t pid = lp_fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        extern char **environ;
        lp_execve(path, argv, environ);
        lp_exit(127);
    }
    int status = 0;
    lp_waitpid(pid, &status, 0);
    return LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : -1;
}

/* Write a file into the Debian tree, replacing whatever is there.
 *
 * The unlink first is not tidiness. A Debian base image ships
 * /etc/resolv.conf as a symlink to /run/systemd/resolve/stub-resolv.conf
 * - a file that exists only once systemd-resolved is running, which here
 * it never is. Opening that path with O_CREAT follows the link and fails
 * with ENOENT, because the directory it points into does not exist. The
 * write silently does nothing, the tree ends up with no name servers,
 * and `apt update` answers "Ign:1 ... InRelease" for every line in
 * sources.list with no explanation of why. That cost a while to find,
 * so: remove the name first, then create it. */
static bool write_file(const char *path, const char *text)
{
    lp_unlink(path);
    long fd = lp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    long n = lp_write((int)fd, text, strlen(text));
    lp_close((int)fd);
    return n == (long)strlen(text);
}

static void mkdirs(const char *path)
{
    char buf[256];
    strlcpy(buf, path, sizeof buf);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        lp_mkdir(buf, 0755);
        *p = '/';
    }
    lp_mkdir(buf, 0755);
}

/* Name resolution for the tree, refreshed every time apt runs.
 *
 * Not once at setup: DHCP hands out a different name server when the
 * board moves to another network, and a tree pinned to the one that was
 * current in March is a tree that cannot reach the mirror in April.
 * Writing it costs a syscall or two against a download of megabytes. */
static bool sync_resolv(void)
{
    char resolv[512] = "nameserver 1.1.1.1\nnameserver 8.8.8.8\n";

    long fd = lp_open("/etc/resolv.conf", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[512];
        long n = lp_read((int)fd, buf, sizeof buf - 1);
        lp_close((int)fd);
        if (n > 0) {
            buf[n] = '\0';
            /* An empty or comment-only file means DHCP has not answered
             * yet. The public resolvers above are a worse answer than the
             * local one and a much better answer than none. */
            if (strstr(buf, "nameserver"))
                strlcpy(resolv, buf, sizeof resolv);
        }
    }

    mkdirs(ROOT "/etc");
    if (!write_file(RESOLV, resolv)) {
        dprintf(STDERR_FILENO,
                "%s: cannot write %s - the tree has no name servers and\n"
                "%s:   every mirror lookup inside it will fail.\n", me,
                RESOLV, me);
        return false;
    }
    return true;
}

/* ── is it there ─────────────────────────────────────────────────── */

static bool is_set_up(void)
{
    return exists(MARKER) && exists(ROOT "/usr/bin/apt");
}

/* ── the kernel filesystems apt needs inside the tree ────────────── */

/* dpkg's maintainer scripts read /proc, and anything that touches the
 * network needs /dev. These are bind mounts, so nothing is copied and
 * unmounting them leaves the tree exactly as it was.
 *
 * Put up when apt starts and taken down when it finishes, so that
 * between runs /data holds nothing but files. A tree with /proc missing
 * fails in ways that read as "this package is broken" rather than "the
 * mount is missing", so they do have to be here while apt runs. */
static void mount_kernel_fs(void)
{
    static const struct { const char *src, *dir, *type; unsigned long flags; }
    binds[] = {
        { "proc",  ROOT "/proc", "proc",     0 },
        { "sysfs", ROOT "/sys",  "sysfs",    0 },
        { "/dev",  ROOT "/dev",  NULL,       MS_BIND },
    };

    for (unsigned i = 0; i < sizeof binds / sizeof *binds; i++) {
        mkdirs(binds[i].dir);
        /* Already mounted is not an error worth reporting. */
        lp_mount(binds[i].src, binds[i].dir, binds[i].type,
                 binds[i].flags, NULL);
    }
}

static void unmount_kernel_fs(void)
{
    lp_umount(ROOT "/dev", MNT_DETACH);
    lp_umount(ROOT "/sys", MNT_DETACH);
    lp_umount(ROOT "/proc", MNT_DETACH);
}

/* ── setup ───────────────────────────────────────────────────────── */

static int cmd_setup(bool quiet)
{
    if (is_set_up()) {
        if (!quiet)
            printf("%s: Debian is already set up in %s\n", me, ROOT);
        return 0;
    }

    printf("%s: setting up Debian %s (%s) under %s\n",
           me, SUITE, DEB_ARCH, ROOT);
    printf("%s:   this downloads about 28MB and unpacks to about 98MB.\n",
           me);
    printf("%s:   Measured, not estimated - a Debian base is not small,\n",
           me);
    printf("%s:   which is most of why this system does not ship one.\n",
           me);
    printf("%s:   It goes on /data and nowhere else - `rm -rf %s`"
           " undoes all of it.\n", me, ROOT);
    printf("\n");

    /* Room to work: the download, the tar it decompresses to, the tree
     * that comes out of it, and slack for the first apt update. About
     * 230MB at the peak; 500MB leaves room to install something
     * afterwards, which is the point of setting this up at all.
     *
     * Checking now beats filling the data partition and finding out
     * when something else fails - a tree that is there but half
     * unpacked is harder to recover from than one that never started. */
    u64 freeb = 0, totalb = 0;
    if (lp_fs_space("/data", &freeb, &totalb) == 0) {
        u64 need = 500ULL * 1024 * 1024;
        if (freeb < need) {
            dprintf(STDERR_FILENO,
                    "%s: /data has %lu MB free and this needs about %lu MB.\n",
                    me, (unsigned long)(freeb / 1048576),
                    (unsigned long)(need / 1048576));
            dprintf(STDERR_FILENO,
                    "%s:   `expandfs` grows /data to fill the card;"
                    " `storage` can add a drive.\n", me);
            return 1;
        }
    }

    mkdirs(ROOT);

    /* ── download, decompress, unpack ────────────────────────────────
     *
     * Three of our own programs and nothing else. net_http_get does TLS
     * with the roots compiled into the libc and follows the redirect
     * GitHub answers with; gzip does the whole of DEFLATE in a 32KB
     * window, so a 95MB stream costs the same memory as a 4KB one; tar
     * restores hard links, device nodes, setuid bits, ownership and
     * times, which a Debian root needs all of - without them the tree
     * unpacks and then cannot su, cannot mount and cannot passwd. */
    lp_unlink(TARGZ);
    lp_unlink(TARBALL);

    printf("%s: fetching %s\n", me, BASE_URL);
    long got = net_http_get(BASE_URL, TARGZ);
    if (got < 0) {
        dprintf(STDERR_FILENO,
                "%s: the download failed.\n"
                "%s:   `net` says whether this machine can reach anything,"
                " and\n"
                "%s:   HTTPS fails outright when the clock is far wrong -"
                " try `ntp`.\n", me, me, me);
        lp_unlink(TARGZ);
        return 1;
    }
    printf("%s: %ld MB downloaded, decompressing\n", me, got / 1048576);

    /* gzip -d leaves TARBALL where TARGZ was and removes TARGZ, so the
     * two never both exist at full size. */
    char *gz_args[] = { (char *)"gzip", (char *)"-d", (char *)TARGZ, NULL };
    if (run_wait("/bin/gzip", gz_args) != 0 || !exists(TARBALL)) {
        dprintf(STDERR_FILENO,
                "%s: could not decompress the download - it was not"
                " gzip, or the transfer was damaged\n", me);
        lp_unlink(TARGZ);
        lp_unlink(TARBALL);
        return 1;
    }

    printf("%s: unpacking into %s\n", me, ROOT);
    char *tar_args[] = { (char *)"tar", (char *)"-x",
                         (char *)TARBALL, (char *)ROOT, NULL };
    int trc = run_wait("/bin/tar", tar_args);
    lp_unlink(TARBALL);
    if (trc != 0) {
        dprintf(STDERR_FILENO,
                "%s: unpacking failed. A half-unpacked tree is worse than"
                " none -\n"
                "%s:   `rm -rf %s` and try again.\n", me, me, ROOT);
        return 1;
    }

    if (!exists(MARKER)) {
        dprintf(STDERR_FILENO,
                "%s: unpacked, but %s is not there - the tarball was not"
                " a Debian root\n", me, MARKER);
        return 1;
    }

    /* Where packages come from. The base tarball ships a sources.list
     * pointing at a snapshot, which is right for a container built to
     * be reproducible and wrong for a machine that wants updates. */
    mkdirs(ROOT "/etc/apt/sources.list.d");

    /* The base image ships a one-line sources.list too, and anything
     * left in it is a second copy of every suite below. */
    lp_unlink(OLD_SOURCES);

    if (!write_file(SOURCES,
        "# Written by `apt setup` on linux-LP.\n"
        "#\n"
        "# http, not https: what makes a package trustworthy here is its\n"
        "# signature, checked against the Debian archive keys named in\n"
        "# Signed-By below, and apt refuses anything that does not\n"
        "# verify. TLS would add a second thing that can fail - a board\n"
        "# with no clock, or a network that inspects TLS - without adding\n"
        "# anything an attacker could otherwise do.\n"
        "#\n"
        "# Change http to https below if you would rather the network\n"
        "# could not see which packages you install. It works; the base\n"
        "# image carries a CA store.\n"
        "\n"
        "Types: deb\n"
        "URIs: " MIRROR "\n"
        "Suites: " SUITE " " SUITE "-updates\n"
        "Components: main contrib non-free-firmware\n"
        "Signed-By: /usr/share/keyrings/debian-archive-keyring.pgp\n"
        "\n"
        "Types: deb\n"
        "URIs: " MIRROR "-security\n"
        "Suites: " SUITE "-security\n"
        "Components: main contrib non-free-firmware\n"
        "Signed-By: /usr/share/keyrings/debian-archive-keyring.pgp\n")) {
        dprintf(STDERR_FILENO,
                "%s: cannot write %s - apt would have nowhere to fetch from\n",
                me, SOURCES);
        return 1;
    }

    /* The tree has its own /etc, so it needs its own name servers. */
    if (!sync_resolv())
        return 1;

    /* Keep apt from asking questions nobody is there to answer. A board
     * that stops mid-install waiting for a keypress is a board that has
     * hung, as far as anyone can tell from outside. */
    mkdirs(ROOT "/etc/apt/apt.conf.d");
    write_file(ROOT "/etc/apt/apt.conf.d/90linux-lp",
        "// Written by `apt setup` on linux-LP.\n"
        "APT::Get::Assume-Yes \"true\";\n"
        "APT::Get::Fix-Broken \"true\";\n"
        "Dpkg::Use-Pty \"false\";\n"
        "Acquire::Retries \"3\";\n");

    printf("\n%s: Debian is set up. `apt update` next, then `apt install`.\n",
           me);
    return 0;
}

/* ── running something inside the tree ───────────────────────────── */

static int in_debian(char *const argv[]) __attribute__((noreturn));
static int in_debian(char *const argv[])
{
    mount_kernel_fs();

    if (lp_chroot(ROOT) < 0) {
        dprintf(STDERR_FILENO,
                "%s: cannot enter %s - are you root?\n", me, ROOT);
        lp_exit(1);
    }
    if (lp_chdir("/") < 0)
        lp_exit(1);

    /* Inside, PATH is Debian's. Ours means nothing here - none of our
     * commands exist in that tree. */
    char *envp[] = {
        (char *)"PATH=/usr/sbin:/usr/bin:/sbin:/bin",
        (char *)"HOME=/root",
        (char *)"TERM=linux",
        (char *)"LC_ALL=C",
        (char *)"DEBIAN_FRONTEND=noninteractive",
        NULL
    };

    /* execve takes a path, and `apt run rg` gives a name.
     *
     * This used to hand argv[0] straight to execve, so anything without
     * a slash in it failed - and failed with "rg is not in the Debian
     * tree", which is the wrong sentence entirely when the package is
     * installed and the binary is sitting in /usr/bin. The PATH in envp
     * above is for whatever gets run, not for this call; the walk has to
     * happen here. Same directories, same order. */
    if (strchr(argv[0], '/')) {
        lp_execve(argv[0], argv, envp);
    } else {
        static const char *dirs[] = {
            "/usr/sbin", "/usr/bin", "/sbin", "/bin", "/usr/local/bin", NULL
        };
        for (int i = 0; dirs[i]; i++) {
            char full[512];
            snprintf(full, sizeof full, "%s/%s", dirs[i], argv[0]);
            lp_execve(full, argv, envp);      /* returns only on failure */
        }
    }

    dprintf(STDERR_FILENO,
            "%s: there is no %s in the Debian tree.\n"
            "%s:   `apt install <package>` puts one there;"
            " `apt shell` then\n"
            "%s:   `which %s` says where it landed if the package used a\n"
            "%s:   different name for it.\n",
            me, argv[0], me, me, argv[0], me);
    lp_exit(127);
}

/* Fork, chroot in the child, wait. The parent keeps its own root, so
 * the mounts can be cleaned up afterwards and the shell that called us
 * is not left inside a chroot it cannot leave. */
static int call_in_debian(char *const argv[])
{
    /* Before anything else: the tree's name servers. Cheap, and it is
     * the difference between `apt update` working and answering "Ign"
     * for every line with no reason given. */
    sync_resolv();

    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "%s: cannot fork\n", me);
        return 1;
    }
    if (pid == 0)
        in_debian(argv);

    int status = 0;
    lp_waitpid(pid, &status, 0);

    /* Take the bind mounts back down.
     *
     * They used to be left up, on the grounds that mounting is cheap and
     * a missing mount reads as a broken package. The cost of leaving
     * them turned out to be worse: /data then contains a live copy of
     * /dev, /proc and /sys, so `du /data` walks the whole of /proc,
     * `tar` of the data partition tries to archive it, unmounting /data
     * at shutdown finds it busy, and - the one that matters - a person
     * who types `rm -rf /data/debian` deletes the real /dev through the
     * bind mount. Two syscalls per apt run is a cheap price for /data
     * being an ordinary directory again the moment apt is done. */
    unmount_kernel_fs();

    return LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : 1;
}

/* ── status ──────────────────────────────────────────────────────── */

static int cmd_status(void)
{
    if (!is_set_up()) {
        printf("Debian is not set up yet.\n");
        printf("  `apt install <package>` will set it up first,\n");
        printf("  or `apt setup` to do only that.\n");
        return 0;
    }

    char version[32] = "?";
    long fd = lp_open(MARKER, O_RDONLY, 0);
    if (fd >= 0) {
        long n = lp_read((int)fd, version, sizeof version - 1);
        lp_close((int)fd);
        if (n > 0) {
            version[n] = '\0';
            for (int i = 0; version[i]; i++)
                if (version[i] == '\n') version[i] = '\0';
        }
    }

    printf("Debian %s (%s) in %s\n", version, DEB_ARCH, ROOT);

    u64 freeb = 0, totalb = 0;
    if (lp_fs_space("/data", &freeb, &totalb) == 0) {
        char f[12];
        disk_human(freeb, f, sizeof f);
        printf("  /data has %s free\n", f);
    }

    printf("\nAnything installed lives in that directory and runs with\n");
    printf("`apt run <command>`. The system's own commands are untouched.\n");
    return 0;
}

/* ── entry ───────────────────────────────────────────────────────── */

static void usage(void)
{
    printf("usage: apt <command> [arguments]\n\n");
    printf("  install <pkg>...   install packages from Debian\n");
    printf("  remove <pkg>...    take them away again\n");
    printf("  update             refresh the package lists\n");
    printf("  upgrade            update what is installed\n");
    printf("  search <text>      what Debian has\n");
    printf("  show <pkg>         details of one\n");
    printf("  list --installed   what is in there now\n");
    printf("\n");
    printf("  run <cmd> [args]   run something installed, inside the tree\n");
    printf("  shell              a shell in there\n");
    printf("  setup              fetch the Debian base (95MB download)\n");
    printf("  status             what is set up\n");
    printf("  purge-all          delete the whole thing\n");
    printf("\n");
    printf("Debian lives in %s and nothing else on this system is\n", ROOT);
    printf("touched by it. Removing that directory removes all of it.\n");
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0 ||
        strcmp(argv[1], "--help") == 0) {
        usage();
        return argc < 2 ? 2 : 0;
    }

    const char *cmd = argv[1];

    if (lp_getuid() != 0) {
        dprintf(STDERR_FILENO, "%s: only root can do this\n", me);
        return 1;
    }

    if (strcmp(cmd, "status") == 0)
        return cmd_status();

    if (strcmp(cmd, "setup") == 0)
        return cmd_setup(false);

    if (strcmp(cmd, "purge-all") == 0) {
        if (!is_set_up()) {
            printf("%s: nothing to remove\n", me);
            return 0;
        }
        printf("This deletes %s and everything installed in it.\n", ROOT);
        printf("Type yes to go ahead: ");
        char answer[16];
        long n = lp_read(STDIN_FILENO, answer, sizeof answer - 1);
        if (n <= 0) return 1;
        answer[n] = '\0';
        for (long i = 0; i < n; i++)
            if (answer[i] == '\n' || answer[i] == '\r') answer[i] = '\0';
        if (strcmp(answer, "yes") != 0) {
            printf("nothing was changed\n");
            return 1;
        }
        unmount_kernel_fs();
        char *rm_args[] = { (char *)"rm", (char *)"-rf",
                            (char *)ROOT, NULL };
        int rc = run_wait("/bin/rm", rm_args);
        printf(rc == 0 ? "%s: gone\n" : "%s: could not remove it all\n", me);
        return rc == 0 ? 0 : 1;
    }

    /* Everything else needs the tree. Setting it up on first use rather
     * than making people find `apt setup` first: the command they typed
     * still does what they asked, it just takes longer the first time. */
    if (!is_set_up()) {
        printf("%s: Debian is not here yet - setting it up first.\n\n", me);
        if (cmd_setup(false) != 0)
            return 1;
        printf("\n");
    }

    /* `apt run <cmd>` and `apt shell` are ours; the rest is Debian's
     * apt, with the arguments passed straight through. */
    static char *args[64];
    int n = 0;

    if (strcmp(cmd, "run") == 0) {
        if (argc < 3) {
            dprintf(STDERR_FILENO, "usage: apt run <command> [args]\n");
            return 2;
        }
        for (int i = 2; i < argc && n < 63; i++)
            args[n++] = argv[i];
    } else if (strcmp(cmd, "shell") == 0) {
        args[n++] = (char *)"/bin/bash";
        args[n++] = (char *)"-l";
    } else {
        args[n++] = (char *)"/usr/bin/apt";
        for (int i = 1; i < argc && n < 63; i++)
            args[n++] = argv[i];
    }
    args[n] = NULL;

    int rc = call_in_debian(args);

    /* apt install pulls in things that want to start at boot. Nothing
     * in that tree is wired into our init, and saying so once is
     * better than letting somebody wonder why their service is not
     * running after a reboot. */
    if (rc == 0 && strcmp(cmd, "install") == 0)
        printf("\n%s: installed. Run it with `apt run <command>`.\n"
               "%s:   Debian services are not started by this system's"
               " init.\n", me, me);
    return rc;
}
