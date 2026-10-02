/* tar - make and open archives.
 *
 *   tar -c <archive> <path>...     create
 *   tar -t <archive>               list what is inside
 *   tar -x <archive> [dir]         extract (into dir, default here)
 *
 * Uncompressed ustar, which is what pkg uses: a package is a tar and
 * this is how one gets made or looked inside on the machine itself.
 * Without it, packages could only be built somewhere else and only
 * inspected by installing them.
 *
 * No compression here. `gzip -dc` is a separate program and does the
 * whole of DEFLATE in a 32KB window, so the pipeline is two commands
 * rather than one program with a decompressor bolted inside it.
 *
 * ── What extraction restores, and why it has to ──
 *
 * This started as the smallest thing that could open a package: names,
 * directories, symlinks, contents. That is enough for a package we
 * built ourselves and nowhere near enough for a Debian root filesystem,
 * which `apt` unpacks with this. Measured against GNU tar on a real
 * Debian trixie base, the old version got 5,440 of 5,440 entries'
 * timestamps wrong, dropped the setuid bit from su, mount, passwd and
 * five others, turned /tmp and /var/tmp from 1777 into 755, opened
 * /root from 0700 to 0755, gave /etc/shadow away from group shadow, and
 * wrote a zero-byte file where perl's hard link should have been - and
 * exited 0 with nothing on stderr.
 *
 * So it now carries: hard links, device nodes and FIFOs, the setuid,
 * setgid and sticky bits, owner and group, and mtime. And it refuses a
 * header type it does not implement instead of writing the pseudo-entry
 * out as a real file, which is what turned a GNU long-name header into
 * a file called ././@LongLink.
 *
 * Extraction refuses absolute paths and any component that is "..".
 * That is how an archive escapes the directory it was meant to stay in,
 * and it has been used in anger for thirty years.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "stdlib.h"
#include "syscall.h"

#define BLOCK        512
#define NAME_OFF       0
#define MODE_OFF     100
#define UID_OFF      108
#define GID_OFF      116
#define SIZE_OFF     124
#define MTIME_OFF    136
#define CHKSUM_OFF   148
#define TYPE_OFF     156
#define LINK_OFF     157
#define MAGIC_OFF    257
#define DEVMAJOR_OFF 329
#define DEVMINOR_OFF 337
#define PREFIX_OFF   345

#define DIRENT_RECLEN 16
#define DIRENT_TYPE   18
#define DIRENT_NAME   19
#define DT_DIR        4

static u8 block[BLOCK];

/* The data buffer is separate from the header block and much bigger.
 * See the copy loop in walk() for why 32KB rather than 512 bytes. */
static u8 databuf[32768];

static const char *type_name(char type)
{
    switch (type) {
    case '5': return "dir ";
    case '2': return "link";
    case '1': return "hard";
    case '3': return "chr ";
    case '4': return "blk ";
    case '6': return "fifo";
    default:  return "file";
    }
}

static u64 from_octal(const u8 *field, int len)
{
    u64 v = 0;
    for (int i = 0; i < len; i++) {
        if (field[i] < '0' || field[i] > '7')
            continue;
        v = v * 8 + (u64)(field[i] - '0');
    }
    return v;
}

static void to_octal(u8 *field, int len, u64 v)
{
    /* Right-aligned, zero-padded, with a NUL in the last position -
     * which is what every tar since 1979 expects to read. */
    for (int i = len - 2; i >= 0; i--) {
        field[i] = (u8)('0' + (v & 7));
        v >>= 3;
    }
    field[len - 1] = '\0';
}

/* The header checksum is the sum of every byte with the checksum field
 * itself treated as spaces. */
static void set_checksum(u8 *hdr)
{
    memset(hdr + CHKSUM_OFF, ' ', 8);

    u32 sum = 0;
    for (int i = 0; i < BLOCK; i++)
        sum += hdr[i];

    to_octal(hdr + CHKSUM_OFF, 7, sum);
    hdr[CHKSUM_OFF + 7] = ' ';
}

static bool path_is_safe(const char *p)
{
    if (p[0] == '/')
        return false;
    for (const char *q = p; *q; ) {
        if (q[0] == '.' && q[1] == '.' && (q[2] == '/' || q[2] == '\0'))
            return false;
        while (*q && *q != '/') q++;
        while (*q == '/') q++;
    }
    return true;
}

/* Is this a real directory, not a symlink pointing at one?
 *
 * lp_is_dir() answers the second question as yes, because it stats
 * through the link. That is the right answer almost everywhere and the
 * wrong one here - see make_parents below. */
static bool is_real_dir(const char *path)
{
    lp_stat_t st;
    if (lp_stat(path, &st, false) < 0)
        return false;
    return (st.mode & LP_S_IFMT) == LP_S_IFDIR;
}

/* Create every directory above `path`, but not `path` itself.
 *
 * The "but not itself" is load-bearing. Every directory entry in a tar
 * carries a trailing slash, so with the name as it comes out of the
 * header this loop created the directory too - at 0755 - and the branch
 * below that would have made it with the right mode then found it
 * already there and left it alone. Every directory in every archive
 * came out 0755: /root world-readable, /tmp not sticky. The names are
 * stripped of their trailing slash before they get here now, and this
 * says so out loud in case they stop being.
 *
 * ── Why a symlink in the path is removed ──
 *
 * path_is_safe() stops "../.." and "/etc/passwd". It does not stop this,
 * which is the same attack by a different road:
 *
 *     lrwxrwxrwx  evil -> /etc
 *     -rw-r--r--  evil/passwd
 *
 * Both entries are relative and neither contains "..". Extract them in
 * order and the symlink is created, and then the second entry is opened
 * through it - writing /etc/passwd on the machine doing the unpacking,
 * as root, from an archive off the network. The check that used to be
 * here asked lp_is_dir(), which stats through the link and answers yes,
 * so nothing was created and nothing was in the way.
 *
 * So: a component that exists and is not a real directory is removed
 * and replaced with one. That loses nothing - a tar that puts a file
 * and then a directory at the same name is self-contradictory - and it
 * is what GNU tar does.
 */
static void make_parents(const char *path)
{
    char work[1024];
    strlcpy(work, path, sizeof(work));
    size_t len = strlen(work);
    while (len > 1 && work[len - 1] == '/')
        work[--len] = '\0';
    for (char *p = work + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (!is_real_dir(work)) {
            lp_unlink(work);
            lp_mkdir(work, 0755);
        }
        *p = '/';
    }
}

/* ── Putting back what the header says ───────────────────────────────
 *
 * Owner, then permissions, then time, in that order and not another.
 *
 * chown clears the setuid and setgid bits - the kernel does that on
 * purpose, so that giving a file away cannot hand somebody a program
 * that runs as its old owner. So the chmod has to come second or the
 * bits are lost again, which is exactly what happens if these two are
 * written in the order they read naturally.
 *
 * And the time last, because the other two are writes and a write
 * updates ctime; mtime is what anything looking at the tree will read.
 */
#define AT_SYMLINK_NOFOLLOW 0x100

/* utimensat with two timespecs: access time then modification time.
 * Both get the header's mtime - ustar carries no atime, and "now" would
 * be a worse answer than the only one we have. */
static void set_times(const char *path, u64 mtime, int flags)
{
    s64 times[4] = { (s64)mtime, 0, (s64)mtime, 0 };
    sys_call4(SYS_utimensat, AT_FDCWD, (long)path, (long)times, flags);
}

static void restore_meta(const char *path, u32 mode, u32 uid, u32 gid,
                         u64 mtime)
{
    lp_chown(path, uid, gid);
    lp_chmod(path, mode & 07777);
    set_times(path, mtime, 0);
}

/* ── Directory times, held back to the end ───────────────────────────
 *
 * A directory's mtime changes every time something is created inside
 * it, so setting it when the directory is made is setting it before
 * everything that will undo it. Unpacking a Debian base left 526 of
 * 3,263 entries dated "now" for this reason.
 *
 * So directories get their owner and mode at once - both are needed
 * before anything can be written inside - and their time at the end,
 * after nothing more will be written. This is what GNU tar does, for
 * the same reason.
 */
typedef struct dirtime {
    struct dirtime *next;
    u64  mtime;
    char path[];
} dirtime_t;

static dirtime_t *dirtimes;

static void remember_dirtime(const char *path, u64 mtime)
{
    size_t n = strlen(path) + 1;
    dirtime_t *d = malloc(sizeof *d + n);
    if (!d)
        return;         /* the tree is right; only its dates are not */
    memcpy(d->path, path, n);
    d->mtime = mtime;
    d->next  = dirtimes;
    dirtimes = d;
}

/* Deepest first, which is the order they were pushed in reverse - and
 * the order that matters, because setting a child's time writes to the
 * parent and would undo the parent's. */
static void apply_dirtimes(void)
{
    while (dirtimes) {
        dirtime_t *d = dirtimes;
        dirtimes = d->next;
        set_times(d->path, d->mtime, 0);
        free(d);
    }
}

/* ── Creating ─────────────────────────────────────────────────────── */

static bool write_header(int out, const char *name, u64 size,
                         u32 mode, char type)
{
    memset(block, 0, BLOCK);

    if (strlen(name) > 99) {
        dprintf(STDERR_FILENO, "tar: name too long: %s\n", name);
        return false;
    }
    strlcpy((char *)block + NAME_OFF, name, 100);

    to_octal(block + MODE_OFF, 8, mode & 07777);
    to_octal(block + MODE_OFF + 8, 8, 0);          /* uid */
    to_octal(block + MODE_OFF + 16, 8, 0);         /* gid */
    to_octal(block + SIZE_OFF, 12, size);
    to_octal(block + MTIME_OFF, 12, (u64)lp_time());
    block[TYPE_OFF] = (u8)type;
    memcpy(block + MAGIC_OFF, "ustar\00000", 8);

    set_checksum(block);
    return lp_write(out, block, BLOCK) == BLOCK;
}

static bool add_path(int out, const char *path);

static bool add_dir(int out, const char *path)
{
    if (!write_header(out, path, 0, 0755, '5'))
        return false;

    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return true;

    char buf[8192];
    bool ok = true;

    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (long off = 0; off < n; ) {
            char       *rec  = buf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            char child[1024];
            snprintf(child, sizeof(child), "%s/%s", path, name);
            if (!add_path(out, child))
                ok = false;
        }
    }
    lp_close((int)fd);
    return ok;
}

static bool add_file(int out, const char *path, const lp_stat_t *st)
{
    long in = lp_open(path, O_RDONLY, 0);
    if (in < 0) {
        dprintf(STDERR_FILENO, "tar: %s: cannot read\n", path);
        return false;
    }

    if (!write_header(out, path, st->size, st->mode, '0')) {
        lp_close((int)in);
        return false;
    }

    u64 left = st->size;
    while (left > 0) {
        memset(block, 0, BLOCK);
        size_t want = left < BLOCK ? (size_t)left : BLOCK;
        long   got  = lp_read((int)in, block, want);
        if (got <= 0)
            break;
        lp_write(out, block, BLOCK);        /* always a whole block */
        left -= (u64)got;
    }
    lp_close((int)in);

    /* The file shrank while we were reading it: pad out so the archive
     * still lines up with the size in the header. */
    while (left > 0) {
        memset(block, 0, BLOCK);
        lp_write(out, block, BLOCK);
        left = left > BLOCK ? left - BLOCK : 0;
    }
    return true;
}

static bool add_path(int out, const char *path)
{
    lp_stat_t st;
    if (lp_stat(path, &st, false) < 0) {
        dprintf(STDERR_FILENO, "tar: %s: not there\n", path);
        return false;
    }

    if ((st.mode & LP_S_IFMT) == LP_S_IFLNK) {
        char target[128];
        long n = lp_readlink(path, target, sizeof(target) - 1);
        if (n <= 0)
            return false;
        target[n] = '\0';

        memset(block, 0, BLOCK);
        strlcpy((char *)block + NAME_OFF, path, 100);
        strlcpy((char *)block + LINK_OFF, target, 100);
        to_octal(block + MODE_OFF, 8, 0777);
        to_octal(block + MODE_OFF + 8, 8, 0);
        to_octal(block + MODE_OFF + 16, 8, 0);
        to_octal(block + SIZE_OFF, 12, 0);
        to_octal(block + MTIME_OFF, 12, (u64)lp_time());
        block[TYPE_OFF] = '2';
        memcpy(block + MAGIC_OFF, "ustar\00000", 8);
        set_checksum(block);
        return lp_write(out, block, BLOCK) == BLOCK;
    }

    if ((st.mode & LP_S_IFMT) == LP_S_IFDIR)
        return add_dir(out, path);

    return add_file(out, path, &st);
}

static int create(const char *archive, char **paths, int npaths)
{
    long out = lp_open(archive, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        dprintf(STDERR_FILENO, "tar: %s: cannot create\n", archive);
        return 1;
    }

    int rc = 0;
    for (int i = 0; i < npaths; i++)
        if (!add_path((int)out, paths[i]))
            rc = 1;

    /* Two empty blocks mark the end. */
    memset(block, 0, BLOCK);
    lp_write((int)out, block, BLOCK);
    lp_write((int)out, block, BLOCK);

    lp_close((int)out);
    return rc;
}

/* ── Listing and extracting ───────────────────────────────────────── */

/* Step over an entry's data without looking at it. */
static void skip_data(long fd, u64 blocks)
{
    u64 remaining = blocks * BLOCK;
    while (remaining) {
        size_t want = remaining < sizeof databuf
                    ? (size_t)remaining : sizeof databuf;
        long got = lp_read((int)fd, databuf, want);
        if (got <= 0)
            return;
        remaining -= (u64)got;
    }
}

static int walk(const char *archive, const char *into, bool extract)
{
    long fd = lp_open(archive, O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "tar: %s: cannot open\n", archive);
        return 1;
    }

    int empty = 0;
    int rc = 0;

    for (;;) {
        long n = lp_read((int)fd, block, BLOCK);
        if (n <= 0)
            break;
        if (n < BLOCK) {
            dprintf(STDERR_FILENO, "tar: %s: truncated\n", archive);
            rc = 1;
            break;
        }

        bool zero = true;
        for (int i = 0; i < BLOCK; i++)
            if (block[i]) { zero = false; break; }
        if (zero) {
            if (++empty >= 2) break;
            continue;
        }
        empty = 0;

        char base[101], prefix[156], name[300];
        memcpy(base, block + NAME_OFF, 100);   base[100] = '\0';
        memcpy(prefix, block + PREFIX_OFF, 155); prefix[155] = '\0';
        if (prefix[0]) snprintf(name, sizeof(name), "%s/%s", prefix, base);
        else           strlcpy(name, base, sizeof(name));

        u64  size   = from_octal(block + SIZE_OFF, 12);
        u32  mode   = (u32)from_octal(block + MODE_OFF, 8);
        u32  uid    = (u32)from_octal(block + UID_OFF, 8);
        u32  gid    = (u32)from_octal(block + GID_OFF, 8);
        u64  mtime  = from_octal(block + MTIME_OFF, 12);
        char type   = (char)block[TYPE_OFF];
        u64  blocks = (size + BLOCK - 1) / BLOCK;
        if (mode == 0)
            mode = (type == '5') ? 0755 : 0644;

        /* The link name, wanted by both '2' and '1'. ustar gives it 100
         * bytes with no terminator when it fills them. */
        char target[101];
        memcpy(target, block + LINK_OFF, 100);
        target[100] = '\0';

        /* A directory entry carries a trailing slash. Take it off once,
         * here, so that every path below is the name of the thing and
         * not the name of the thing plus a separator. */
        {
            size_t nl = strlen(name);
            while (nl > 1 && name[nl - 1] == '/')
                name[--nl] = '\0';
        }

        if (!extract) {
            const char *what = type_name(type);
            if (type == '2' || type == '1')
                printf("%s %8llu  %s -> %s\n", what,
                       (unsigned long long)size, name, target);
            else
                printf("%s %8llu  %s\n", what,
                       (unsigned long long)size, name);
            skip_data(fd, blocks);
            continue;
        }

        if (!path_is_safe(name)) {
            dprintf(STDERR_FILENO,
                    "tar: refusing '%s' - it points outside %s\n", name, into);
            lp_close((int)fd);
            return 1;
        }

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", into, name);

        switch (type) {
        case '5':
            make_parents(full);
            if (!is_real_dir(full)) {
                /* A symlink here would make everything unpacked inside
                 * it land wherever it points. Same reason as
                 * make_parents: lp_is_dir() would have said yes. */
                lp_unlink(full);
                lp_mkdir(full, mode & 07777);
            }
            /* Unconditionally, not only when we just made it: a
             * directory that already exists still has to end up with
             * the mode the archive says. */
            lp_chown(full, uid, gid);
            lp_chmod(full, mode & 07777);
            remember_dirtime(full, mtime);
            continue;

        case '2':
            make_parents(full);
            lp_unlink(full);
            if (lp_symlink(target, full) < 0) {
                dprintf(STDERR_FILENO, "tar: cannot link %s -> %s\n",
                        full, target);
                rc = 1;
            }
            /* No chmod or chown here. Both follow the link, so they
             * would change whatever it points at - which in a Debian
             * root is usually a real file somewhere else in the same
             * tree. A symlink's own mode means nothing on Linux.
             *
             * The time does have to be the link's own, though, which is
             * what AT_SYMLINK_NOFOLLOW is for. Without it this dated
             * the target instead, and /etc/alternatives - which is
             * nothing but symlinks - came out entirely wrong. */
            set_times(full, mtime, AT_SYMLINK_NOFOLLOW);
            continue;

        case '1': {
            /* A hard link. Its target is a path inside the archive, so
             * it is relative to where we are unpacking - not to the
             * machine's own root, which is what makes this two lines
             * instead of one.
             *
             * There is exactly one in a Debian base: perl5.40.1 to
             * perl. Falling through to the regular-file branch, which
             * is what used to happen, wrote a zero-byte perl - and
             * quite a lot of Debian is maintainer scripts in perl. */
            char to[1024];
            snprintf(to, sizeof(to), "%s/%s", into, target);
            make_parents(full);
            lp_unlink(full);
            if (lp_link(to, full) < 0) {
                dprintf(STDERR_FILENO,
                        "tar: cannot hard-link %s to %s\n", full, to);
                rc = 1;
            }
            continue;
        }

        case '3': case '4': case '6': {
            /* Character device, block device, FIFO. A Debian base
             * carries none of these - its /dev is deliberately empty -
             * but an archive of a running system does, and writing one
             * out as an empty regular file is the kind of wrong that is
             * only noticed much later. */
            u32 maj = (u32)from_octal(block + DEVMAJOR_OFF, 8);
            u32 min = (u32)from_octal(block + DEVMINOR_OFF, 8);
            mode_t kind = type == '3' ? LP_S_IFCHR
                        : type == '4' ? LP_S_IFBLK : LP_S_IFIFO;
            make_parents(full);
            lp_unlink(full);
            /* The device number, in the layout the kernel wants:
             * 12 bits of major and 20 of minor, interleaved. Not
             * (major << 8 | minor), which is the ancient 16-bit form
             * and silently wrong for anything with a minor over 255. */
            u64 dev = ((u64)(maj & 0xfff) << 8) | (min & 0xff)
                    | ((u64)(min & ~0xffu) << 12);
            if (lp_mknod(full, kind | (mode & 07777), dev) < 0) {
                dprintf(STDERR_FILENO, "tar: cannot create %s\n", full);
                rc = 1;
                continue;
            }
            restore_meta(full, mode, uid, gid, mtime);
            continue;
        }

        case '0': case '\0': case '7':
            break;              /* a regular file - handled below */

        default:
            /* Anything else is a header this tar does not implement:
             * GNU's 'L' and 'K' long names, PAX 'x' and 'g' records,
             * GNU sparse and volume headers. They used to fall through
             * to the regular-file branch, which turned a long-name
             * header into a real file called ././@LongLink holding the
             * path, and then wrote the actual entry under its truncated
             * name. Stopping is the only honest answer: the tree would
             * be wrong in a way nothing downstream could detect. */
            dprintf(STDERR_FILENO,
                    "tar: %s: header type '%c' is not supported\n",
                    name, type ? type : '0');
            lp_close((int)fd);
            return 1;
        }

        make_parents(full);
        /* Remove whatever is there first. O_CREAT on an existing
         * symlink follows it, so without this an archive that puts a
         * link before a file of the same name writes through the link -
         * the same escape make_parents describes, one level down. */
        lp_unlink(full);
        long out = lp_open(full, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
        if (out < 0) {
            dprintf(STDERR_FILENO, "tar: cannot write %s\n", full);
            rc = 1;
            skip_data(fd, blocks);
            continue;
        }

        /* 32KB at a time, not 512 bytes. A Debian base is 138MB in
         * 269,820 blocks, and one read and one write per block is over
         * half a million system calls - on a 1GHz ARM1176 with an SD
         * card that is the difference between a couple of minutes and
         * long enough that people conclude it has hung. */
        u64 left = size;
        u64 remaining = blocks * BLOCK;
        bool short_read = false;
        while (remaining) {
            size_t want = remaining < sizeof databuf
                        ? (size_t)remaining : sizeof databuf;
            long got = lp_read((int)fd, databuf, want);
            if (got < (long)want) { short_read = true; break; }
            remaining -= (u64)got;
            size_t use = left < (u64)got ? (size_t)left : (size_t)got;
            if (use)
                lp_write((int)out, databuf, use);
            left -= use;
        }
        lp_close((int)out);
        if (short_read) {
            dprintf(STDERR_FILENO, "tar: %s: truncated\n", archive);
            lp_close((int)fd);
            return 1;
        }
        restore_meta(full, mode, uid, gid, mtime);
    }

    lp_close((int)fd);
    if (extract)
        apply_dirtimes();
    return rc;
}

/* GNU tar 로 넘길 것인가.
 *
 * 이 tar 는 pkg 가 쓰는 ustar 부분집합만 안다. 그것으로 충분한
 * 이유는 이 OS 의 패키지가 그 형식이기 때문이고, 충분하지 않은
 * 이유는 이 기계에 데비안도 같이 있기 때문이다: dpkg-deb 는
 * `tar --warning=no-timestamp -xf -` 처럼 부르고, PATH 가 /bin 을
 * 먼저 보므로 그 tar 가 우리 것이 된다. 우리 것은 그 인자를 모르고,
 * dpkg 는 "tar subprocess returned error exit status 1" 만 남긴다 -
 * 무엇이 없는지 아무도 알 수 없는 실패다.
 *
 * 그래서 우리가 아는 꼴이 아니면 GNU tar 에게 넘긴다. 우리 것이
 * 앞에 있다는 사실은 그대로 두면서(사람이 `tar -t pkg.tar` 를 치면
 * 우리 것이 답한다) 기계가 우분투처럼 동작하게 하는 방법이다.
 * GNU tar 가 없는 기계에서는 예전처럼 우리가 답한다. */
static void hand_to_gnu_tar(int argc, char **argv)
{
    static const char *GNU = "/usr/bin/tar";
    if (lp_access(GNU, 1) != 0)
        return;                        /* 데비안이 깔리지 않은 이미지 */

    (void)argc;
    lp_execve(GNU, argv, environ);
    /* 넘기지 못하면 그냥 우리가 계속한다. */
}

/* 우리가 다룰 수 있는 꼴인가: -c/-t/-x 하나에 파일 이름. */
static bool ours(int argc, char **argv)
{
    if (argc < 3)
        return false;
    if (strcmp(argv[1], "-c") != 0 && strcmp(argv[1], "-t") != 0 &&
        strcmp(argv[1], "-x") != 0)
        return false;
    /* 긴 옵션이 하나라도 있으면 GNU 것이다. '-' (표준 입력) 도. */
    for (int i = 2; i < argc; i++)
        if (argv[i][0] == '-' && argv[i][1])
            return false;
    if (strcmp(argv[2], "-") == 0)
        return false;
    return true;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !ours(argc, argv) && strcmp(argv[1], "-h") != 0)
        hand_to_gnu_tar(argc, argv);

    if (argc < 3 || strcmp(argv[1], "-h") == 0) {
        printf("usage:\n");
        printf("  tar -c <archive> <path>...   create\n");
        printf("  tar -t <archive>             list\n");
        printf("  tar -x <archive> [dir]       extract\n\n");
        printf("Uncompressed ustar - the format pkg uses, so this is how\n");
        printf("a package is made or looked inside on the machine.\n");
        return argc < 3 ? 2 : 0;
    }

    const char *mode    = argv[1];
    const char *archive = argv[2];

    if (strcmp(mode, "-c") == 0) {
        if (argc < 4) {
            dprintf(STDERR_FILENO, "tar: nothing to put in it\n");
            return 2;
        }
        return create(archive, argv + 3, argc - 3);
    }

    if (strcmp(mode, "-t") == 0)
        return walk(archive, ".", false);

    if (strcmp(mode, "-x") == 0)
        return walk(archive, argc > 3 ? argv[3] : ".", true);

    dprintf(STDERR_FILENO, "tar: -c, -t or -x\n");
    return 2;
}
