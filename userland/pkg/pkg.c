/* pkg - install and remove packages.
 *
 *   pkg list                 what is installed
 *   pkg info <name>          what one package is, and what it put where
 *   pkg add <file.tar>       install from a file you already have
 *   pkg remove <name>        take it out again
 *   pkg repo [url]           show or set where packages come from
 *   pkg update               fetch the repository's index
 *   pkg search <text>        what the repository has
 *   pkg install <name>...    fetch, check and install
 *
 * ── The format ──
 * A package is a plain uncompressed tar. Not a format of our own: tar is
 * made by every machine on earth, so a package can be built with one
 * command and inspected with another, and there is nothing to learn.
 * Uncompressed because the only decompressor on this system is inside
 * the kernel and not reachable from here - the download is larger, the
 * code that has to be right is a third of the size, and that is the
 * better trade for something that runs as root.
 *
 * Paths in the archive are relative and land under /data. A package
 * holding bin/foo installs /data/bin/foo, which is on PATH already. An
 * absolute path, or one containing "..", is refused outright: an archive
 * is untrusted input, and that is exactly how an archive escapes the
 * directory it was meant to stay in.
 *
 * ── What the checking does and does not cover ──
 * Everything downloaded is checked against the SHA-256 in the index, so
 * a corrupted transfer or a mirror serving the wrong bytes is caught.
 *
 * Whether that is worth anything depends on how the index arrived. Over
 * plain HTTP it is worth very little: someone able to rewrite your
 * traffic serves their own index and their own packages, and the hashes
 * agree with each other perfectly. Over HTTPS the index is authenticated
 * as coming from the server it claims to, and then the hashes mean what
 * they look like they mean.
 *
 * So the repository is https, and the default one is. The TLS is in
 * our own libc (BearSSL, with Mozilla's roots compiled in), so there is
 * nothing to install first and no certificate file that can be missing.
 * That was not always true: this used to shell out to python3 on the
 * data partition, and python3 could only be installed by downloading
 * it - over TLS. A package manager that needs a package manager to work
 * is one that never works, and on a freshly written card it did not.
 *
 * An http:// repository still works, and pkg says so out loud when the
 * repository is one, because the difference is not cosmetic.
 *
 * What this still does not do is prove that the index was written by
 * anyone in particular - there are no package signatures. A server that
 * is compromised serves bad packages over a perfectly valid certificate.
 * For something that matters: fetch it yourself, check it, and use
 * "pkg add", which never touches the network at all.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "net.h"

#define ROOT      "/data"
#define DB_DIR    "/data/pkg/db"
#define REPO_FILE "/data/pkg/repo"
#define INDEX     "/data/pkg/index"
#define TMP_FILE  "/data/pkg/.download"

/* ── Which packages this machine can run ─────────────────────────────
 *
 * A package holds compiled programs, so an index that mixes the three
 * machines this system runs on would hand an aarch64 binary to a Pi
 * Zero W. It would install perfectly and then fail at exec with
 * "cannot execute binary file", which reads like a corrupt download.
 *
 * So the repository has one index per architecture, and the name is
 * decided here at compile time - the binary asking for the index is
 * itself proof of what the machine is. */
#if defined(__x86_64__)
#  define PKG_ARCH "amd64"
#elif defined(__aarch64__)
#  define PKG_ARCH "arm64"
#elif defined(__arm__)
#  define PKG_ARCH "armv6"
#else
#  error "pkg has no repository name for this architecture"
#endif

/* Where packages come from when nobody has said otherwise: given when
 * building (make LP_PKG_REPO=<url>), because only the copy of this
 * source that publishes repo/ knows where that is. On GitHub that is
 * raw.githubusercontent.com/<owner>/<repo>/main/repo rather than a
 * release page: a plain static file served over TLS, with no API, no
 * token and no redirect to a signed URL that expires.
 *
 * Without one, `pkg repo <url>` chooses - a directory of files on any
 * web server will do, and tools/mkpkg.sh builds one. */
#ifndef LP_PKG_REPO
#  define LP_PKG_REPO ""
#endif
#define DEFAULT_REPO LP_PKG_REPO

/* SHA-256 is in the libc - sha256sum wants it too, and one copy of a
 * hash function is the right number. See lp_sha256_file in unistd.h. */

/* ═══════════════════════════════════════════════════════════════════
 * Reading a tar
 * ═══════════════════════════════════════════════════════════════════ */
#define TAR_BLOCK      512
#define TAR_NAME         0
#define TAR_MODE       100
#define TAR_SIZE       124
#define TAR_TYPE       156
#define TAR_LINKNAME   157
#define TAR_PREFIX     345

/* Octal fields, which tar pads with spaces or NULs. */
static u64 tar_octal(const u8 *field, int len)
{
    u64 v = 0;
    for (int i = 0; i < len; i++) {
        if (field[i] < '0' || field[i] > '7')
            continue;
        v = v * 8 + (u64)(field[i] - '0');
    }
    return v;
}

/* Is this path safe to write under our root?
 *
 * An archive is something someone else made. Two things in a path let it
 * write outside the directory we intended, and both have been used in
 * anger for thirty years: an absolute path, and "..". */
static bool path_is_safe(const char *p)
{
    if (p[0] == '/')
        return false;

    for (const char *q = p; *q; ) {
        if (q[0] == '.' && q[1] == '.' &&
            (q[2] == '/' || q[2] == '\0'))
            return false;
        while (*q && *q != '/') q++;
        while (*q == '/') q++;
    }
    return true;
}

/* Make the directories leading up to a file. */
static void make_parents(const char *path)
{
    char work[1024];
    strlcpy(work, path, sizeof(work));

    for (char *p = work + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (!lp_is_dir(work))
            lp_mkdir(work, 0755);
        *p = '/';
    }
}

/* Unpack one tar into ROOT, writing each path installed to `list_fd`.
 * Returns the number of files, or -1. */
static long tar_extract(const char *archive, int list_fd, bool dry_run)
{
    long fd = lp_open(archive, O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "pkg: %s: cannot open\n", archive);
        return -1;
    }

    static u8 hdr[TAR_BLOCK];
    static u8 data[TAR_BLOCK];
    long count = 0;
    int  empty = 0;

    for (;;) {
        long n = lp_read((int)fd, hdr, TAR_BLOCK);
        if (n <= 0)
            break;
        if (n < TAR_BLOCK) {
            dprintf(STDERR_FILENO, "pkg: %s: truncated\n", archive);
            lp_close((int)fd);
            return -1;
        }

        /* Two blocks of zeroes end the archive. */
        bool all_zero = true;
        for (int i = 0; i < TAR_BLOCK; i++)
            if (hdr[i]) { all_zero = false; break; }
        if (all_zero) {
            if (++empty >= 2)
                break;
            continue;
        }
        empty = 0;

        /* The name and prefix fields are fixed width and only NUL
         * terminated when the name is shorter than the field, so they
         * are copied out by length rather than read as strings. */
        char base[101], prefix[156], name[300];
        memcpy(base, hdr + TAR_NAME, 100);
        base[100] = '\0';
        memcpy(prefix, hdr + TAR_PREFIX, 155);
        prefix[155] = '\0';

        if (prefix[0])
            snprintf(name, sizeof(name), "%s/%s", prefix, base);
        else
            strlcpy(name, base, sizeof(name));

        /* "tar -cf x.tar ." names everything ./bin/foo, which extracts
         * to the right place but is recorded as /data/./bin/foo - and
         * that is what `pkg info` then shows somebody. Strip it here
         * rather than in the tool that builds packages, because the
         * archive can have been made by anyone's tar. Repeated, since
         * "././bin" is legal too, and path_is_safe still sees whatever
         * is left. */
        while (name[0] == '.' && name[1] == '/')
            memmove(name, name + 2, strlen(name + 2) + 1);
        if (!name[0] || strcmp(name, ".") == 0)
            continue;                             /* the archive root */

        u64  size = tar_octal(hdr + TAR_SIZE, 12);
        u32  mode = (u32)tar_octal(hdr + TAR_MODE, 8);
        char type = (char)hdr[TAR_TYPE];
        u64  blocks = (size + TAR_BLOCK - 1) / TAR_BLOCK;

        if (!path_is_safe(name)) {
            dprintf(STDERR_FILENO,
                    "pkg: %s: refusing '%s' - it points outside %s\n",
                    archive, name, ROOT);
            lp_close((int)fd);
            return -1;
        }

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", ROOT, name);

        if (type == '5') {                        /* a directory */
            if (!dry_run) {
                make_parents(full);
                if (!lp_is_dir(full))
                    lp_mkdir(full, mode ? mode : 0755);
            }
            continue;                             /* not recorded: shared */
        }

        if (type == '2') {                        /* a symlink */
            char target[101];
            memcpy(target, hdr + TAR_LINKNAME, 100);
            target[100] = '\0';

            /* The target has to pass the same test as the name.
             *
             * It did not, and that was the third way out of /data - the
             * header above names absolute paths and ".." and says both
             * are refused, and it is the link target that neither of
             * them was ever applied to. Two entries were enough:
             *
             *     bin/x                  symlink -> /root/.ssh
             *     bin/x/authorized_keys  regular file
             *
             * The second name is relative and has no "..", so it
             * passes; make_parents stats bin/x, follows the link, finds
             * a directory and creates nothing; and the file is then
             * opened O_CREAT|O_TRUNC through the link, as root,
             * anywhere on the machine. /root/.ssh/authorized_keys,
             * /data/rc.local and /etc/passwd were all one archive
             * away. */
            if (!path_is_safe(target)) {
                dprintf(STDERR_FILENO,
                        "pkg: %s points at %s, which is outside %s -"
                        " refusing the archive\n", name, target, ROOT);
                return -1;
            }
            if (!dry_run) {
                make_parents(full);
                lp_unlink(full);
                lp_symlink(target, full);
                if (list_fd >= 0) {
                    dprintf(list_fd, "%s\n", full);
                }
            }
            count++;
            continue;
        }

        if (type != '0' && type != '\0') {
            /* Character devices, fifos, hard links. Skipping them beats
             * half-supporting them. */
            dprintf(STDERR_FILENO, "pkg:   skipping %s (type %c)\n",
                    name, type ? type : '0');
            for (u64 b = 0; b < blocks; b++)
                lp_read((int)fd, data, TAR_BLOCK);
            continue;
        }

        /* A regular file. */
        long out = -1;
        if (!dry_run) {
            make_parents(full);
            out = lp_open(full, O_WRONLY | O_CREAT | O_TRUNC,
                          mode ? (mode & 0777) : 0644);
            if (out < 0) {
                dprintf(STDERR_FILENO, "pkg: cannot write %s\n", full);
                lp_close((int)fd);
                return -1;
            }
        }

        u64 left = size;
        for (u64 b = 0; b < blocks; b++) {
            if (lp_read((int)fd, data, TAR_BLOCK) < TAR_BLOCK) {
                dprintf(STDERR_FILENO, "pkg: %s: truncated inside %s\n",
                        archive, name);
                if (out >= 0) lp_close((int)out);
                lp_close((int)fd);
                return -1;
            }
            size_t want = left < TAR_BLOCK ? (size_t)left : TAR_BLOCK;
            if (out >= 0)
                lp_write((int)out, data, want);
            left -= want;
        }

        if (out >= 0) {
            lp_close((int)out);
            /* The mode again: O_CREAT only applies it to a new file, and
             * an upgrade overwrites an existing one. */
            if (mode)
                lp_chmod(full, mode & 0777);
            if (list_fd >= 0)
                dprintf(list_fd, "%s\n", full);
        }
        count++;
    }

    lp_close((int)fd);
    return count;
}

/* The HTTP client lives in the libc: pkg is not the only thing that
 * needs to fetch a file, and one copy of "read past the headers" is
 * enough. See net_http_get in net.h. */

/* ═══════════════════════════════════════════════════════════════════
 * The package database
 * ═══════════════════════════════════════════════════════════════════ */

static void ensure_dirs(void)
{
    lp_mkdir("/data/pkg", 0755);
    lp_mkdir(DB_DIR, 0755);
}

static void db_path(char *out, size_t size, const char *name, const char *ext)
{
    snprintf(out, size, "%s/%s.%s", DB_DIR, name, ext);
}

static bool is_installed(const char *name)
{
    char p[256];
    db_path(p, sizeof(p), name, "list");
    return lp_exists(p);
}

/* ═══════════════════════════════════════════════════════════════════
 * Keeping integrity(1) in step
 * ═══════════════════════════════════════════════════════════════════
 *
 * integrity(1) watches the handful of things on /data that survive a
 * reboot, and three of them are directories this program writes to:
 * /data/pkg/db, where the record of what is installed lives, and
 * /data/bin and python's site-packages, where the files in a package
 * land. So `pkg install jq` - a completely ordinary thing to do - made
 * the next integrity run announce that something which survives a
 * reboot had changed, and defend(1) turned that into a security
 * finding. The board reported itself for working.
 *
 * The record therefore follows the tool that is allowed to change it:
 * once an install or a remove has actually succeeded, we hand integrity
 * the watched directories WE wrote to and it re-records exactly those.
 *
 * Two things this deliberately does not do:
 *
 *   - it does not run `integrity -u`, which would accept everything.
 *     A package installed in the same minute as somebody editing
 *     /data/rc.local would then have that edit accepted too, silently,
 *     and rc.local is the whole reason integrity exists.
 *   - it names only the directories the archive actually touched.
 *     Installing something that puts nothing in site-packages leaves
 *     the record for site-packages alone, so a file appearing there
 *     while we were busy is still reported.
 *
 * The list below has to agree with WATCHED[] in integrity.c, which is
 * the authority; integrity refuses a path it does not watch and says
 * so, which is what makes a disagreement visible rather than silent.
 * The files integrity watches which are NOT here - rc.local, users,
 * groups, authorized_keys - are the ones that decide what runs and who
 * gets in, and no package has any business writing them: an archive
 * that does gets reported, which is the point.
 */
static const char *ACCEPTABLE[] = {
    DB_DIR,
    "/data/bin",
    "/data/python/lib/python3.12/site-packages",
    NULL
};
#define MAX_ACCEPTABLE 3

static bool touched[MAX_ACCEPTABLE];

static void note_touched(const char *path)
{
    for (int i = 0; ACCEPTABLE[i]; i++) {
        size_t n = strlen(ACCEPTABLE[i]);
        if (strncmp(path, ACCEPTABLE[i], n) == 0 &&
            (path[n] == '/' || path[n] == '\0'))
            touched[i] = true;
    }
}

/* Every path in a package's file list, so that an install or a remove
 * can say which watched directories it changed. */
static void note_list_file(const char *listp)
{
    long fd = lp_open(listp, O_RDONLY, 0);
    if (fd < 0)
        return;
    char line[1024];
    while (readline((int)fd, line, sizeof(line)) >= 0)
        if (line[0])
            note_touched(line);
    lp_close((int)fd);
}

static void accept_touched(void)
{
    char *argv[MAX_ACCEPTABLE + 3];
    int   n = 0;

    argv[n++] = (char *)"integrity";
    argv[n++] = (char *)"-a";
    for (int i = 0; ACCEPTABLE[i]; i++)
        if (touched[i])
            argv[n++] = (char *)ACCEPTABLE[i];
    argv[n] = NULL;

    if (n == 2)
        return;                 /* nothing integrity watches was written */

    /* An image built without it. Nothing is watching those paths, so
     * there is nothing to keep in step and nothing to say. */
    if (!lp_exists("/bin/integrity"))
        return;

    pid_t pid = lp_fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        lp_execve("/bin/integrity", argv, environ);
        lp_exit(127);
    }
    int status = 0;
    lp_waitpid(pid, &status, 0);
    /* integrity prints its own reason on stderr if it refused - a path
     * it does not watch, or a record it could not write. Repeating it
     * here would only make the same complaint twice. */
}

static int cmd_list(void)
{
    long fd = lp_open(DB_DIR, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        printf("nothing installed\n");
        return 0;
    }

    char buf[8192];
    int  found = 0;

    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (long off = 0; off < n; ) {
            char       *rec  = buf + off;
            u16         len  = *(u16 *)(rec + 16);
            const char *name = rec + 19;
            off += len;

            size_t l = strlen(name);
            if (l < 6 || strcmp(name + l - 5, ".list") != 0)
                continue;

            char base[128];
            strlcpy(base, name, l - 4 > sizeof(base) ? sizeof(base) : l - 4);

            char info[256], text[256];
            db_path(info, sizeof(info), base, "info");
            if (proc_read(info, text, sizeof(text)) > 0) {
                char *nl = strchr(text, '\n');
                if (nl) *nl = '\0';
                printf("  %-20s %s\n", base, text);
            } else {
                printf("  %s\n", base);
            }
            found++;
        }
    }
    lp_close((int)fd);

    if (!found)
        printf("nothing installed\n");
    return 0;
}

static int cmd_info(const char *name)
{
    if (!is_installed(name)) {
        dprintf(STDERR_FILENO, "pkg: %s is not installed\n", name);
        return 1;
    }

    char p[256], text[256];
    db_path(p, sizeof(p), name, "info");
    if (proc_read(p, text, sizeof(text)) > 0)
        printf("%s", text);

    printf("\nfiles:\n");
    db_path(p, sizeof(p), name, "list");
    long fd = lp_open(p, O_RDONLY, 0);
    if (fd < 0)
        return 1;

    char line[1024];
    long count = 0;
    while (readline((int)fd, line, sizeof(line)) >= 0) {
        printf("  %s\n", line);
        count++;
    }
    lp_close((int)fd);
    printf("(%ld)\n", count);
    return 0;
}

static int install_tar(const char *name, const char *version,
                       const char *archive)
{
    ensure_dirs();

    /* Walk it once without writing anything. A package that is going to
     * be refused halfway through should be refused before the first file
     * of it lands on the disk. */
    if (tar_extract(archive, -1, true) < 0)
        return 1;

    char listp[256];
    db_path(listp, sizeof(listp), name, "list");

    long list_fd = lp_open(listp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (list_fd < 0) {
        dprintf(STDERR_FILENO, "pkg: cannot write %s\n", listp);
        return 1;
    }

    long n = tar_extract(archive, (int)list_fd, false);
    lp_close((int)list_fd);

    if (n < 0) {
        lp_unlink(listp);
        return 1;
    }

    char infop[256];
    db_path(infop, sizeof(infop), name, "info");
    long info_fd = lp_open(infop, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (info_fd >= 0) {
        dprintf((int)info_fd, "%s\n", version ? version : "(unknown version)");
        lp_close((int)info_fd);
    }

    lp_sync();

    /* The database entry above is itself a change to a watched
     * directory, so it counts whatever the archive held. */
    note_touched(listp);
    note_list_file(listp);
    accept_touched();

    printf("pkg: installed %s (%ld files)\n", name, n);
    return 0;
}

/* "some/path/foo-1.2.tar" -> "foo" */
static void name_from_file(const char *path, char *out, size_t size)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    strlcpy(out, base, size);

    char *dot = strrchr(out, '.');
    if (dot && strcmp(dot, ".tar") == 0)
        *dot = '\0';

    char *dash = strrchr(out, '-');
    if (dash && dash[1] >= '0' && dash[1] <= '9')
        *dash = '\0';               /* the version, which we keep apart */
}

static int cmd_add(const char *file)
{
    char name[128];
    name_from_file(file, name, sizeof(name));

    const char *base = strrchr(file, '/');
    return install_tar(name, base ? base + 1 : file, file);
}

static int cmd_remove(const char *name)
{
    if (!is_installed(name)) {
        dprintf(STDERR_FILENO, "pkg: %s is not installed\n", name);
        return 1;
    }

    char listp[256];
    db_path(listp, sizeof(listp), name, "list");

    long fd = lp_open(listp, O_RDONLY, 0);
    if (fd < 0)
        return 1;

    char line[1024];
    long gone = 0, kept = 0;
    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (line[0] == '\0')
            continue;
        /* Noted before the unlink, because after it the list file is
         * gone too and there is nothing left to read the paths from.
         * A file removed from a watched directory changes its listing
         * exactly as much as one appearing there does. */
        note_touched(line);
        if (lp_unlink(line) == 0)
            gone++;
        else
            kept++;
    }
    lp_close((int)fd);

    lp_unlink(listp);
    char infop[256];
    db_path(infop, sizeof(infop), name, "info");
    lp_unlink(infop);

    lp_sync();

    note_touched(listp);        /* the database directory, always */
    accept_touched();

    printf("pkg: removed %s (%ld files", name, gone);
    if (kept)
        printf(", %ld were already gone", kept);
    printf(")\n");

    /* Directories are left behind on purpose: several packages share
     * bin/ and lib/, and there is no counting of who else is in them. */
    return 0;
}

/* The repository somebody chose, or the built-in one if there is one. */
static bool repo_base(char *out, size_t size)
{
    char buf[512];
    if (proc_read(REPO_FILE, buf, sizeof(buf)) > 0) {
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
        if (buf[0]) {
            strlcpy(out, buf, size);
            return true;
        }
    }
    if (!DEFAULT_REPO[0]) {
        dprintf(STDERR_FILENO,
                "pkg: no repository set - choose one with: pkg repo <url>\n");
        return false;
    }
    strlcpy(out, DEFAULT_REPO, size);
    return true;
}

/* That, with this machine's architecture on the end - which is the
 * directory the index and the packages are actually in. */
static bool repo_url(char *out, size_t size)
{
    char base[512];
    if (!repo_base(base, sizeof(base)))
        return false;
    snprintf(out, size, "%s/" PKG_ARCH, base);
    return true;
}

static int cmd_repo(const char *url)
{
    char cur[512];

    if (!url) {
        char base[512];
        if (!repo_base(base, sizeof(base)))
            return 1;
        repo_url(cur, sizeof(cur));
        printf("%s\n", base);
        if (strcmp(base, DEFAULT_REPO) == 0)
            printf("  (the built-in default)\n");
        printf("  packages for this machine come from %s\n", cur);
        return 0;
    }

    ensure_dirs();
    long fd = lp_open(REPO_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "pkg: cannot write %s\n", REPO_FILE);
        return 1;
    }
    dprintf((int)fd, "%s\n", url);
    lp_close((int)fd);
    printf("pkg: repository is %s\n", url);
    if (strncmp(url, "http://", 7) == 0)
        printf("pkg:   this is plain HTTP. Anyone between here and that\n"
               "pkg:   server can replace both the index and the packages,\n"
               "pkg:   and the checksums will still agree. https:// works.\n");
    printf("     run 'pkg update' to fetch its index\n");
    return 0;
}

static int cmd_update(void)
{
    char base[512];
    if (!repo_url(base, sizeof(base)))
        return 1;

    ensure_dirs();

    char url[640];
    snprintf(url, sizeof(url), "%s/index", base);

    long n = net_http_get(url, INDEX);
    if (n < 0)
        return 1;

    printf("pkg: index updated (%ld bytes)\n", n);
    return 0;
}

/* One line of the index: name version size sha256 file */
typedef struct {
    char name[64], version[32], sha[72], file[128];
    long size;
} entry_t;

static bool index_find(const char *name, entry_t *e)
{
    long fd = lp_open(INDEX, O_RDONLY, 0);
    if (fd < 0)
        return false;

    char line[512];
    bool found = false;

    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (line[0] == '#' || line[0] == '\0')
            continue;

        char *p = line;
        char *fields[5];
        int   nf = 0;

        while (*p && nf < 5) {
            while (*p == ' ') p++;
            if (!*p) break;
            fields[nf++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }
        if (nf < 5)
            continue;
        if (strcmp(fields[0], name) != 0)
            continue;

        strlcpy(e->name,    fields[0], sizeof(e->name));
        strlcpy(e->version, fields[1], sizeof(e->version));
        e->size = strtoll(fields[2], NULL, 10);
        strlcpy(e->sha,     fields[3], sizeof(e->sha));
        strlcpy(e->file,    fields[4], sizeof(e->file));
        found = true;
        break;
    }

    lp_close((int)fd);
    return found;
}

static int cmd_search(const char *text)
{
    long fd = lp_open(INDEX, O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO,
                "pkg: no index yet. Set a repository and run 'pkg update'\n");
        return 1;
    }

    char line[512];
    int  hits = 0;

    while (readline((int)fd, line, sizeof(line)) >= 0) {
        if (line[0] == '#' || line[0] == '\0')
            continue;
        if (text && !strstr(line, text))
            continue;

        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = '\0';
        char *ver = sp + 1;
        char *sp2 = strchr(ver, ' ');
        if (sp2) *sp2 = '\0';

        printf("  %-20s %-12s %s\n", line, ver,
               is_installed(line) ? "(installed)" : "");
        hits++;
    }
    lp_close((int)fd);

    if (!hits)
        printf("nothing matched\n");
    return 0;
}

static int cmd_install(const char *name)
{
    char base[512];
    if (!repo_url(base, sizeof(base)))
        return 1;

    entry_t e;
    if (!index_find(name, &e)) {
        /* No index yet, or an old one. Fetch it and look again rather
         * than telling somebody to type `pkg update` first: they have
         * already said what they want, and the machine knows how to
         * find out whether it can be had. Only once, though - a second
         * failure is a package that is not there. */
        if (!lp_exists(INDEX)) {
            if (cmd_update() != 0)
                return 1;
        }
        if (!index_find(name, &e)) {
            dprintf(STDERR_FILENO,
                    "pkg: %s is not in %s\n", name, base);
            dprintf(STDERR_FILENO,
                    "pkg:   `pkg update` refreshes the list,"
                    " `pkg search` shows what is in it\n");
            return 1;
        }
    }

    ensure_dirs();

    char url[700];
    snprintf(url, sizeof(url), "%s/%s", base, e.file);
    printf("pkg: fetching %s %s\n", e.name, e.version);

    long n = net_http_get(url, TMP_FILE);
    if (n < 0)
        return 1;

    if (e.size > 0 && n != e.size) {
        dprintf(STDERR_FILENO,
                "pkg: %s: got %ld bytes, the index says %ld\n",
                name, n, e.size);
        lp_unlink(TMP_FILE);
        return 1;
    }

    char hex[72];
    if (!lp_sha256_file(TMP_FILE, hex)) {
        lp_unlink(TMP_FILE);
        return 1;
    }

    if (strcmp(hex, e.sha) != 0) {
        dprintf(STDERR_FILENO,
                "pkg: %s: the checksum does not match. Not installing.\n"
                "     expected %s\n"
                "     got      %s\n", name, e.sha, hex);
        lp_unlink(TMP_FILE);
        return 1;
    }

    int rc = install_tar(e.name, e.version, TMP_FILE);
    lp_unlink(TMP_FILE);
    return rc;
}

static void usage(void)
{
    printf("usage: pkg <command>\n\n");
    printf("  list                 what is installed\n");
    printf("  info <name>          what it is and where its files went\n");
    printf("  add <file.tar>       install from a file you already have\n");
    printf("  remove <name>        take it out again\n");
    printf("  repo [url]           show or set where packages come from\n");
    printf("  update               fetch the repository's index\n");
    printf("  search [text]        what the repository has\n");
    printf("  install <name>...    fetch, check and install\n");
    printf("  sha256 <file>        the checksum of a file\n\n");
    printf("Packages are plain uncompressed tar files. Paths inside them\n");
    printf("are relative and land under %s, so bin/foo becomes\n", ROOT);
    printf("%s/bin/foo - which is already on PATH.\n\n", ROOT);
    printf("Everything downloaded is checked against the SHA-256 in the\n");
    printf("index, and the index itself comes over HTTPS with the\n");
    printf("certificate checked, so the hashes mean what they look like\n");
    printf("they mean. What is still missing is a signature: a server\n");
    printf("that has been broken into serves bad packages over a\n");
    printf("perfectly valid certificate. For something that matters,\n");
    printf("fetch the file yourself, check it, and use 'pkg add'.\n");
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0) {
        usage();
        return argc < 2 ? 2 : 0;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "list") == 0)
        return cmd_list();

    if (strcmp(cmd, "info") == 0 && argc > 2)
        return cmd_info(argv[2]);

    if (strcmp(cmd, "add") == 0 && argc > 2) {
        int rc = 0;
        for (int i = 2; i < argc; i++)
            rc |= cmd_add(argv[i]);
        return rc;
    }

    if (strcmp(cmd, "remove") == 0 && argc > 2) {
        int rc = 0;
        for (int i = 2; i < argc; i++)
            rc |= cmd_remove(argv[i]);
        return rc;
    }

    if (strcmp(cmd, "repo") == 0)
        return cmd_repo(argc > 2 ? argv[2] : NULL);

    if (strcmp(cmd, "update") == 0)
        return cmd_update();

    if (strcmp(cmd, "search") == 0)
        return cmd_search(argc > 2 ? argv[2] : NULL);

    if (strcmp(cmd, "install") == 0 && argc > 2) {
        int rc = 0;
        for (int i = 2; i < argc; i++)
            rc |= cmd_install(argv[i]);
        return rc;
    }

    if (strcmp(cmd, "sha256") == 0 && argc > 2) {
        char hex[72];
        if (!lp_sha256_file(argv[2], hex)) {
            dprintf(STDERR_FILENO, "pkg: %s: cannot read\n", argv[2]);
            return 1;
        }
        printf("%s  %s\n", hex, argv[2]);
        return 0;
    }

    usage();
    return 2;
}
