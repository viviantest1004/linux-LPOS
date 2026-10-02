/* ls - list a directory.
 *
 *   ls [-l] [-a] [-h] [-t] [-S] [-r] [-R] [-d] [-1] [path]...
 *
 *   -l  one per line, with permissions, owner, size and date
 *   -a  include the names beginning with a dot
 *   -h  sizes as 4K and 1M rather than as digits
 *   -t  newest first
 *   -S  largest first
 *   -r  the other way round
 *   -R  and everything underneath
 *   -d  the directory itself, not what is in it
 *   -1  one name per line without the rest of -l
 *
 * getdents64 fills the buffer with variable-length records, back to back:
 *   struct linux_dirent64 {
 *       u64  d_ino;      offset 0
 *       s64  d_off;      offset 8
 *       u16  d_reclen;   offset 16   <- bytes to the next record
 *       u8   d_type;     offset 18
 *       char d_name[];   offset 19   NUL terminated
 *   };
 * We read by offset rather than declaring a struct, so nothing depends on
 * the compiler's padding rules.
 *
 * The names are collected before anything is printed, because sorting
 * needs all of them and because two passes are what lets the columns
 * line up. That puts a ceiling on how many entries one directory can
 * have - a real ls streams - and the ceiling is stated rather than
 * silently truncating.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define BUF_SIZE       8192
#define DIRENT_RECLEN  16
#define DIRENT_TYPE    18
#define DIRENT_NAME    19

#define MAX_ENTRIES  1024
#define NAME_MAX      256

/* d_type values */
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK     10
#define DT_SOCK    12

static bool opt_long, opt_all, opt_human, opt_time, opt_size;
static bool opt_reverse, opt_recurse, opt_dironly, opt_one;
/* -F / -p: 이름 뒤에 종류를 알리는 한 글자. */
static bool opt_classify;
static int  failures;

typedef struct {
    char name[NAME_MAX];
    u8   type;
    lp_stat_t st;
    bool have_stat;
} entry_t;

static entry_t entries[MAX_ENTRIES];

static char type_suffix(u8 t)
{
    switch (t) {
    case DT_DIR:  return '/';
    case DT_LNK:  return '@';
    case DT_FIFO: return '|';
    case DT_SOCK: return '=';
    default:      return '\0';
    }
}

static u8 type_from_mode(u32 mode)
{
    switch (mode & LP_S_IFMT) {
    case LP_S_IFDIR:  return DT_DIR;
    case LP_S_IFLNK:  return DT_LNK;
    case LP_S_IFCHR:  return DT_CHR;
    case LP_S_IFBLK:  return DT_BLK;
    case LP_S_IFIFO:  return DT_FIFO;
    case LP_S_IFSOCK: return DT_SOCK;
    default:          return DT_REG;
    }
}

/* "drwxr-xr-x" */
static void mode_string(u32 mode, char *out)
{
    switch (mode & LP_S_IFMT) {
    case LP_S_IFDIR:  out[0] = 'd'; break;
    case LP_S_IFLNK:  out[0] = 'l'; break;
    case LP_S_IFCHR:  out[0] = 'c'; break;
    case LP_S_IFBLK:  out[0] = 'b'; break;
    case LP_S_IFIFO:  out[0] = 'p'; break;
    case LP_S_IFSOCK: out[0] = 's'; break;
    default:          out[0] = '-'; break;
    }
    static const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        out[1 + i] = (mode & (1 << (8 - i))) ? rwx[i] : '-';

    /* setuid, setgid and the sticky bit sit on top of the x they
     * modify, which is where everyone expects to read them. */
    if (mode & 04000) out[3] = (out[3] == 'x') ? 's' : 'S';
    if (mode & 02000) out[6] = (out[6] == 'x') ? 's' : 'S';
    if (mode & 01000) out[9] = (out[9] == 'x') ? 't' : 'T';
    out[10] = '\0';
}

/* Bytes unless -h was asked for, which is what GNU does. Printing
 * human sizes by default reads better and teaches somebody that `ls -l`
 * gives you "98K"; it does not, and a script that assumed so would be
 * wrong everywhere but here. */
static void human_size(u64 n, char *out, size_t size)
{
    if (!opt_human) {
        /* %llu, not %lu: a long is four bytes on a 32-bit machine and
         * a 5GB file then listed as 1GB. */
        snprintf(out, size, "%llu", (unsigned long long)n);
        return;
    }
    static const char *unit[] = { "", "K", "M", "G", "T" };
    int u = 0;
    while (n >= 10240 && u < 4) { n /= 1024; u++; }
    snprintf(out, size, "%llu%s", (unsigned long long)n, unit[u]);
}

/* The date column.
 *
 * This used to print "2026-09-03 08:15" always, with a note saying a
 * date nobody has to decode beats the one ls usually prints. That was
 * true and it was still the wrong call: somebody learning here would
 * write a script that cuts fields out of `ls -l` and find it broken on
 * every other machine. GNU's shape is the one that has to be learned,
 * so it is the one printed - recent files get the time, anything older
 * than six months gets the year instead, and the year has two spaces in
 * front of it so both forms are the same width.
 *
 * The old format is still here under `voice lp`.
 *
 * Local time, not UTC. `date` and `ls` disagreeing about what time it is
 * was the thing that made the zone worth putting in the libc. */
static const char *MON[13] = { "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void time_string(s64 t, char *out, size_t size)
{
    if (t <= 0) { strlcpy(out, "               -", size); return; }
    lp_tm_t tm;
    lp_localtime(t, &tm);

    if (lp_voice() == LP_VOICE_LP) {
        snprintf(out, size, "%04d-%02d-%02d %02d:%02d",
                 tm.year, tm.mon, tm.day, tm.hour, tm.min);
        return;
    }

    /* Six months, as GNU counts it. */
    s64 age = lp_time() - t;
    if (age > 15552000LL || age < -3600LL)
        snprintf(out, size, "%s %2d  %04d", MON[tm.mon], tm.day, tm.year);
    else
        snprintf(out, size, "%s %2d %02d:%02d",
                 MON[tm.mon], tm.day, tm.hour, tm.min);
}

static int compare(const entry_t *a, const entry_t *b)
{
    int r;
    if (opt_time)      r = (a->st.mtime < b->st.mtime) ? 1 :
                           (a->st.mtime > b->st.mtime) ? -1 : 0;
    else if (opt_size) r = (a->st.size < b->st.size) ? 1 :
                           (a->st.size > b->st.size) ? -1 : 0;
    else               r = strcmp(a->name, b->name);

    if (r == 0 && (opt_time || opt_size))
        r = strcmp(a->name, b->name);      /* a stable tie-break */
    return opt_reverse ? -r : r;
}

static void sort_entries(int n)
{
    /* Insertion sort. A directory big enough for this to matter is one
     * where reading it costs far more than ordering it. */
    for (int i = 1; i < n; i++) {
        entry_t key = entries[i];
        int j = i - 1;
        while (j >= 0 && compare(&entries[j], &key) > 0) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }
}

/* Column widths for a whole listing.
 *
 * GNU sizes each column to the widest value in the listing and puts one
 * space between them. Fixed widths - what this used to do - line up
 * until a name is longer or a size is bigger than the guess, and then
 * the columns jump. Worse for a learner: the output does not match the
 * one they will cut fields out of on any other machine.
 *
 * Computed once per directory, before anything is printed, which is why
 * the names are collected first. */
typedef struct { int nlink, owner, group, size; } widths_t;
static widths_t W = { 1, 1, 1, 1 };

static int textlen(const char *s) { return (int)strlen(s); }

static void entry_columns(const entry_t *e, char *owner, size_t osz,
                          char *group, size_t gsz, char *size, size_t ssz)
{
    strlcpy(owner, "?", osz);
    strlcpy(group, "?", gsz);
    strlcpy(size,  "?", ssz);
    if (!e->have_stat)
        return;

    lp_user_t u;
    if (lp_user_by_uid(e->st.uid, &u)) strlcpy(owner, u.name, osz);
    else snprintf(owner, osz, "%d", (int)e->st.uid);
    lp_group_name(e->st.gid, group, gsz);
    human_size(e->st.size, size, ssz);
}

static void measure(int n)
{
    W.nlink = W.owner = W.group = W.size = 1;
    for (int i = 0; i < n; i++) {
        char o[32], g[32], z[24], nl[16];
        entry_columns(&entries[i], o, sizeof o, g, sizeof g, z, sizeof z);
        snprintf(nl, sizeof nl, "%u",
                 (unsigned)(entries[i].have_stat ? entries[i].st.nlink : 1));
        if (textlen(nl) > W.nlink) W.nlink = textlen(nl);
        if (textlen(o)  > W.owner) W.owner = textlen(o);
        if (textlen(g)  > W.group) W.group = textlen(g);
        if (textlen(z)  > W.size)  W.size  = textlen(z);
    }
}

static void print_entry(const char *dir, const entry_t *e)
{
    if (!opt_long) {
        /* GNU 는 -F 나 -p 를 줬을 때만 꼬리표를 붙인다. 늘 붙이면
         * `ls | while read d` 가 "sub/" 를 받고, 그 이름으로 만든
         * 경로가 다른 기계에서만 맞는다. 배우는 사람이 여기서 익힌
         * 것이 저기서 틀리는 그 자리다. */
        char suffix = (opt_classify && !opt_one) ? type_suffix(e->type) : 0;
        if (suffix) printf("%s%c\n", e->name, suffix);
        else        printf("%s\n", e->name);
        return;
    }

    char modes[12] = "----------";
    char size[16]  = "?";
    char when[24]  = "               -";
    char owner[32] = "?", group[32] = "?";

    if (e->have_stat) {
        mode_string(e->st.mode, modes);
        human_size(e->st.size, size, sizeof size);
        time_string(e->st.mtime, when, sizeof when);

        lp_user_t u;
        if (lp_user_by_uid(e->st.uid, &u))
            strlcpy(owner, u.name, sizeof owner);
        else
            snprintf(owner, sizeof owner, "%d", (int)e->st.uid);
        lp_group_name(e->st.gid, group, sizeof group);
    }

    char link[256] = "";
    if (e->type == DT_LNK) {
        char full[512];
        snprintf(full, sizeof full, "%s/%s", dir, e->name);
        long n = lp_readlink(full, link, sizeof link - 1);
        if (n > 0) link[n] = '\0';
        else       link[0] = '\0';
    }

    if (lp_voice() == LP_VOICE_LP) {
        printf("%s %3u %-8s %-8s %8s  %s  %s%s%s\n",
               modes, (unsigned)(e->have_stat ? e->st.nlink : 1),
               owner, group, size, when, e->name,
               link[0] ? " -> " : "", link);
        return;
    }

    printf("%s %*u %-*s %-*s %*s %s %s%s%s\n",
           modes,
           W.nlink, (unsigned)(e->have_stat ? e->st.nlink : 1),
           W.owner, owner,
           W.group, group,
           W.size,  size,
           when, e->name,
           link[0] ? " -> " : "", link);
}

static int list_dir(const char *path, bool show_header);

static int list_one(const char *path)
{
    /* -d, or a path that is not a directory: the name is the answer. */
    lp_stat_t st;
    bool have = (lp_stat(path, &st, false) == 0);

    if (opt_dironly || (have && (st.mode & LP_S_IFMT) != LP_S_IFDIR)) {
        if (!have) {
            lp_diag("ls", "cannot access", NULL, "no such file", path, 2);
            return 1;
        }
        entry_t e;
        memset(&e, 0, sizeof e);
        strlcpy(e.name, path, sizeof e.name);
        e.st = st;
        e.have_stat = true;
        e.type = type_from_mode(st.mode);

        /* The directory the name lives in, for resolving a symlink. */
        char dir[512];
        strlcpy(dir, path, sizeof dir);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0'; else strlcpy(dir, ".", sizeof dir);

        print_entry(dir, &e);
        return 0;
    }

    return list_dir(path, false);
}

static int list_dir(const char *path, bool show_header)
{
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        if (fd == -20 /* ENOTDIR */ || lp_exists(path)) {
            printf("%s\n", path);
            return 0;
        }
        lp_diag("ls", "cannot access", NULL, "cannot open",
                path, (int)-fd);
        return 1;
    }

    if (show_header)
        printf("\n%s:\n", path);

    int n = 0;
    bool full = false;
    char buf[BUF_SIZE];

    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got == 0)
            break;
        if (got < 0) {
            dprintf(STDERR_FILENO, "ls: %s: read failed (%ld)\n", path, -got);
            lp_close((int)fd);
            return 1;
        }

        for (long off = 0; off < got; ) {
            char *rec  = buf + off;
            u16   len  = *(u16 *)(rec + DIRENT_RECLEN);
            u8    type = *(u8 *)(rec + DIRENT_TYPE);
            char *name = rec + DIRENT_NAME;
            if (len == 0)
                break;
            off += len;

            if (!opt_all && name[0] == '.')
                continue;
            if (n >= MAX_ENTRIES) { full = true; continue; }

            entry_t *e = &entries[n];
            memset(e, 0, sizeof *e);
            strlcpy(e->name, name, sizeof e->name);
            e->type = type;

            /* Only stat when something needs it. On a directory of a
             * few thousand names that is the difference between instant
             * and noticeably slow. */
            if (opt_long || opt_time || opt_size) {
                char full_path[768];
                snprintf(full_path, sizeof full_path, "%s/%s", path, name);
                e->have_stat = (lp_stat(full_path, &e->st, false) == 0);
                if (e->have_stat && type == DT_UNKNOWN)
                    e->type = type_from_mode(e->st.mode);
            }
            n++;
        }
    }
    lp_close((int)fd);

    if (full)
        dprintf(STDERR_FILENO,
                "ls: %s has more than %d entries - only the first %d are\n"
                "ls:   shown. 'find %s' walks it without holding it all.\n",
                path, MAX_ENTRIES, MAX_ENTRIES, path);

    sort_entries(n);
    measure(n);

    if (opt_long) {
        /* GNU's "total" is the 512-byte blocks the files actually take,
         * expressed in 1K units. Rounding the size up to the next
         * kilobyte - what this did - is a different number whenever a
         * file is sparse or the filesystem's block is not 1K, and
         * "different from every other ls" is the one thing this output
         * must not be. */
        u64 blocks = 0;
        for (int i = 0; i < n; i++)
            if (entries[i].have_stat)
                blocks += entries[i].st.blocks;
        if (lp_voice() == LP_VOICE_LP) {
            char t[16];
            human_size(blocks * 512, t, sizeof t);
            printf("total %s\n", t);
        } else {
            printf("total %lu\n", (unsigned long)(blocks / 2));
        }
    }

    for (int i = 0; i < n; i++)
        print_entry(path, &entries[i]);

    if (!opt_recurse)
        return 0;

    /* The names are copied out before recursing: the shared table is
     * about to be overwritten by the directory below. */
    static char subdirs[64][NAME_MAX];
    int nsub = 0;
    for (int i = 0; i < n && nsub < 64; i++) {
        if (entries[i].type != DT_DIR)
            continue;
        if (strcmp(entries[i].name, ".") == 0 ||
            strcmp(entries[i].name, "..") == 0)
            continue;
        strlcpy(subdirs[nsub++], entries[i].name, NAME_MAX);
    }

    for (int i = 0; i < nsub; i++) {
        char child[768];
        snprintf(child, sizeof child, "%s/%s", path, subdirs[i]);
        failures |= list_dir(child, true);
    }
    return 0;
}

/* Names given on the command line, in the listing's own order: newest
 * first with -t, largest with -S, by name otherwise. `ls -t a b` printed
 * them as given, so `ls -t report-* | head -1` was not the newest. */
static void sort_operands(const char **p, int n)
{
    static entry_t a, b;
    for (int i = 1; i < n; i++) {
        const char *key = p[i];
        memset(&a, 0, sizeof a);
        strlcpy(a.name, key, sizeof a.name);
        a.have_stat = lp_stat(key, &a.st, false) == 0;
        int j = i - 1;
        for (; j >= 0; j--) {
            memset(&b, 0, sizeof b);
            strlcpy(b.name, p[j], sizeof b.name);
            b.have_stat = lp_stat(p[j], &b.st, false) == 0;
            if (compare(&b, &a) <= 0)
                break;
            p[j + 1] = p[j];
        }
        p[j + 1] = key;
    }
}

static void usage(void)
{
    printf("usage: ls [-lahtSrRd1] [path]...\n\n");
    printf("  -l  permissions, owner, size and date\n");
    printf("  -a  the names beginning with a dot too\n");
    printf("  -h  sizes as 4K and 1M\n");
    printf("  -t  newest first\n");
    printf("  -S  largest first\n");
    printf("  -r  the other way round\n");
    printf("  -R  and everything underneath\n");
    printf("  -d  the directory itself, not what is in it\n");
    printf("  -1  one name per line\n");
}

int main(int argc, char **argv)
{
    const char *paths[64];
    int npaths = 0;

    bool opts_done = false;
    for (int i = 1; i < argc; i++) {
        /* "--": everything after it is a name, even one that starts
         * with a dash. */
        if (!opts_done && strcmp(argv[i], "--") == 0) {
            opts_done = true;
            continue;
        }
        if (!opts_done && strcmp(argv[i], "--help") == 0) { usage(); return 0; }
        if (!opts_done && argv[i][0] == '-' && argv[i][1]) {
            for (const char *o = argv[i] + 1; *o; o++) {
                switch (*o) {
                case 'l': opt_long = true; break;
                case 'a': opt_all = true; break;
                case 'h': opt_human = true; break;
                case 't': opt_time = true; break;
                case 'S': opt_size = true; break;
                case 'r': opt_reverse = true; break;
                case 'R': opt_recurse = true; break;
                case 'd': opt_dironly = true; break;
                case '1': opt_one = true; break;
                case 'F': case 'p': opt_classify = true; break;
                default:
                    dprintf(STDERR_FILENO, "ls: unknown option -%c\n", *o);
                    usage();
                    return 2;
                }
            }
            continue;
        }
        if (npaths < 64)
            paths[npaths++] = argv[i];
    }

    if (npaths == 0)
        return list_one(".");

    /* As GNU: the names that are not there first (their errors), then
     * the ones that are not directories, then the directories, each
     * group in the listing's order. */
    if (npaths > 1) {
        const char *miss[64], *files[64], *dirs[64];
        int nm = 0, nf = 0, nd = 0;
        for (int i = 0; i < npaths; i++) {
            lp_stat_t st;
            if (lp_stat(paths[i], &st, false) != 0)      miss[nm++] = paths[i];
            else if (!opt_dironly && lp_is_dir(paths[i])) dirs[nd++] = paths[i];
            else                                          files[nf++] = paths[i];
        }
        sort_operands(files, nf);
        sort_operands(dirs, nd);
        npaths = 0;
        for (int i = 0; i < nm; i++) paths[npaths++] = miss[i];
        for (int i = 0; i < nf; i++) paths[npaths++] = files[i];
        for (int i = 0; i < nd; i++) paths[npaths++] = dirs[i];
    }

    for (int i = 0; i < npaths; i++) {
        if (npaths > 1 && !opt_dironly && lp_is_dir(paths[i]))
            printf("%s%s:\n", i ? "\n" : "", paths[i]);
        failures |= list_one(paths[i]);
    }
    return failures;
}
