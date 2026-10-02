/* du - how much space a directory takes.
 *
 *   du [-s] [-b] [path]...
 *
 *   -s  the total only, no line per subdirectory
 *   -b  count bytes rather than MB
 *
 * This adds up the blocks each file actually occupies, which is what
 * every other du does. It used to add up file sizes instead, and the
 * numbers came out different from `du` on any other machine: a sparse
 * file counted for more than it takes, and a directory full of tiny
 * files for far less. On a machine somebody is learning on, a number
 * that disagrees with every book is worse than no number.
 *
 * (The old note read: this adds up file sizes, not the blocks on disk.
 * The two differ - a
 * file smaller than a block still occupies one, and a sparse file
 * occupies less than its size - but "how much is in here" is nearly
 * always the question being asked, and it is the one that matches what
 * copying it somewhere else would cost.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define DIRENT_RECLEN 16
#define DIRENT_TYPE   18
#define DIRENT_NAME   19
#define DT_DIR        4

static bool summary_only = false;
static bool in_bytes     = false;
static bool human        = false;
static bool failed       = false;

/* GNU prints 1K blocks and a tab, with no padding: "8\tsub".
 *
 * This printed megabytes in a padded column, which reads better and is
 * useless to a script - `du -s x | cut -f1` gets "       0 MB" here and
 * a number everywhere else. Worse for somebody learning, anything under
 * a megabyte showed as 0, so a directory with real files in it looked
 * empty. -b still gives bytes, -h human sizes, and `voice lp` keeps the
 * old column. */
static void show(const char *path, u64 bytes)
{
    if (lp_voice() == LP_VOICE_LP) {
        if (in_bytes)
            printf("%12llu  %s\n", (unsigned long long)bytes, path);
        else
            printf("%8llu MB  %s\n",
                   (unsigned long long)(bytes / 1048576ULL), path);
        return;
    }

    if (in_bytes) {
        printf("%llu\t%s\n", (unsigned long long)bytes, path);
        return;
    }
    if (human) {
        /* GNU 꼴: 10 미만이면 소수 한 자리("8.0K"), 그 이상이면 정수
         * ("30M"). 소수를 빼면 4K 와 8K 가 둘 다 "0M" 이 되어, 작은
         * 디렉터리끼리 비교가 안 된다. */
        static const char *unit[] = { "", "K", "M", "G", "T" };
        u64 n = bytes;
        int u = 0;
        while (n >= 1024 && u < 4) { n /= 1024; u++; }
        u64 whole = bytes;
        for (int k = 0; k < u; k++) whole /= 1024;
        u64 rem = bytes;
        for (int k = 0; k + 1 < u + 1 && k < u; k++) rem /= 1024;
        int tenth = u ? (int)(((rem % 1024) * 10 + 512) / 1024) : 0;
        if (tenth > 9) tenth = 9;

        char h[24];
        if (u == 0)
            snprintf(h, sizeof h, "%llu", (unsigned long long)bytes);
        else if (whole < 10)
            snprintf(h, sizeof h, "%llu.%d%s",
                     (unsigned long long)whole, tenth, unit[u]);
        else
            snprintf(h, sizeof h, "%llu%s",
                     (unsigned long long)whole, unit[u]);
        printf("%s\t%s\n", h, path);
        return;
    }
    /* 1K blocks, rounded up - GNU's default. */
    printf("%llu\t%s\n",
           (unsigned long long)((bytes + 1023) / 1024), path);
}

/* Returns the total under `path`, printing a line per directory unless
 * we were asked for the summary alone. */
static u64 walk(const char *path, int depth)
{
    u64 total = 0;

    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        lp_stat_t st;
        if (lp_stat(path, &st, true) != 0) {
            if (depth == 0) {
                lp_diag("du", "cannot access", NULL, "no such file", path, 2);
                failed = true;
            }
            return 0;
        }
        total = in_bytes ? st.size : st.blocks * 512;
        /* A file named on the command line gets its line, as in GNU:
         * `du -h report.tar.gz` printed nothing at all. */
        if (depth == 0 && !summary_only)
            show(path, total);
        return total;
    }

    /* The directory's own blocks count too. GNU includes them, and a
     * tree of empty directories is not free - each one is a 4K block on
     * most filesystems. Leaving them out made `du -s` on a directory
     * report exactly half of what every other du reports. */
    {
        /* With -b (apparent size) GNU counts the directory as nothing.
         * A directory's "size" is an implementation detail of the
         * filesystem - 4096 on ext4, 0 on others - and adding it makes
         * the same tree measure differently on different disks, which
         * is the opposite of what apparent size is for. Without -b the
         * blocks are real and they count. */
        lp_stat_t dst;
        if (!in_bytes && lp_stat(path, &dst, false) == 0)
            total += dst.blocks * 512;
    }

    char buf[8192];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;

        for (long off = 0; off < n; ) {
            char       *rec  = buf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            u8          type = *(u8 *)(rec + DIRENT_TYPE);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            char child[1024];
            snprintf(child, sizeof(child), "%s%s%s",
                     path, strcmp(path, "/") == 0 ? "" : "/", name);

            if (type == DT_DIR) {
                total += walk(child, depth + 1);
            } else {
                lp_stat_t st;
                /* The link itself, not what it points at - otherwise a
                 * symlink into a big tree gets counted twice. */
                if (lp_stat(child, &st, false) == 0)
                    total += in_bytes ? st.size : st.blocks * 512;
            }
        }
    }
    lp_close((int)fd);

    if (!summary_only)
        show(path, total);
    return total;
}

int main(int argc, char **argv)
{
    int paths = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] && argv[i][1] != '-' &&
            strcmp(argv[i], "-h") != 0) {
            /* 붙여 쓴 짧은 옵션. `du -sh` 는 사람들이 실제로 치는
             * 유일한 꼴이고, 예전에는 그것이 옵션이 아니라 경로로
             * 읽혀서 "그런 파일 없음" 이 나왔다. */
            bool known = true;
            for (const char *c = argv[i] + 1; *c && known; c++) {
                if      (*c == 's') summary_only = true;
                else if (*c == 'b') in_bytes = true;
                else if (*c == 'h') human = true;
                else known = false;
            }
            if (known) continue;
        }
        if (strcmp(argv[i], "-s") == 0) summary_only = true;
        else if (strcmp(argv[i], "-b") == 0) in_bytes = true;
        /* -h is human sizes here, as it is in GNU. It used to be the
         * help, which meant `du -h` printed the manual instead of the
         * one thing everybody uses it for. --help is the help. */
        else if (strcmp(argv[i], "-h") == 0) human = true;
        else if (strcmp(argv[i], "-sh") == 0 ||
                 strcmp(argv[i], "-hs") == 0) { summary_only = true; human = true; }
        else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: du [-s] [-h] [-b] [path]...\n");
            printf("  -s  the total only\n");
            printf("  -h  human sizes (1K, 30M)\n");
            printf("  -b  bytes\n");
            printf("  with none of these: 1K blocks, as GNU prints them\n");
            return 0;
        }
        else paths++;
    }

    if (paths == 0) {
        u64 t = walk(".", 0);
        if (summary_only)
            show(".", t);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-')
            continue;
        bool was = failed;
        u64 t = walk(argv[i], 0);
        if (summary_only && failed == was)
            show(argv[i], t);
    }
    return failed ? 1 : 0;
}
