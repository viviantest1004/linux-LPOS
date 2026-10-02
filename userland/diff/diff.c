/* diff - show what changed between two files.
 *
 *   diff [-u] [-q] [-i] [-w] [-U n] a b
 *   diff [-r] [-q] [-i] [-w] [-U n] dir1 dir2
 *
 *   -u  unified output, three lines of context - the default
 *   -q  only say whether they differ
 *   -r  walk two directory trees
 *   -i  ignore case          -w  ignore whitespace
 *   -U  n lines of context instead of three
 *
 * Exit status: 0 the same, 1 different, 2 trouble. Scripts split on
 * that, so a file that cannot be opened must not look like a difference.
 *
 * Unified output only. Every other diff also prints the classic ed-style
 * form, and it exists so that patch(1) can eat it. There is no patch on
 * this system, so a second output format would be bytes spent on nobody.
 *
 * ── How the difference is found, and what it costs ──
 *
 * A minimal diff is a longest common subsequence, and the textbook table
 * for that has (lines of a) x (lines of b) cells: two files of 100000
 * lines would ask for ten billion of them. This board has 512MB. So the
 * work is done in three stages, each of which exists to keep the last
 * one small.
 *
 *   1. The equal lines at the start and at the end are trimmed off
 *      first. On a real edit that is nearly all of both files, and it
 *      costs one pass and no memory.
 *   2. What is left goes through Myers' algorithm, which walks the edit
 *      graph in order of edit distance d and stops the moment it reaches
 *      the far corner. Its cost is O((n+m)*d) - proportional to how much
 *      changed, not to how big the files are. Lines are compared by a
 *      precomputed hash first, so the usual comparison is one integer.
 *   3. Turning that walk back into a script needs every step kept, and
 *      that is the one thing here which grows quickly: d*d integers.
 *      MAX_D caps it at 400, which is 640KB. Reaching the cap on two
 *      50000-line files takes a few milliseconds here, so the cap is
 *      about memory rather than about patience.
 *
 * Past that cap the answer is "they differ, and here is why I am not
 * showing you how". Widening the search until the board runs out of
 * memory is not an option, and neither is printing a non-minimal diff:
 * that looks right when it is not, which is worse than no answer. cmp
 * is the tool for files that far apart.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

/* The budget. Two files at the limit plus the trace is about 7MB, and
 * only a pathological pair gets anywhere near it. */
#define MAX_BYTES (2L * 1024 * 1024)   /* per file */
#define MAX_LINES 100000L              /* per file */
#define MAX_D     400L                 /* differing lines we will lay out */
#define MAX_DEPTH 20                   /* -r, against a tree that loops */

static bool icase, iwhite;

/* ── A file, read in and cut into lines ─────────────────────────────── */

typedef struct {
    const char *path;
    char       *data;      /* the whole file; each newline turned into NUL */
    long        size;
    char      **line;      /* line[i] points into data */
    u32        *hash;      /* line[i] hashed under -i and -w */
    long        nlines;
    bool        no_final_nl;
    bool        binary;
} file_t;

static u32 hash_line(const char *s)
{
    u32 h = 2166136261u;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (iwhite && (c == ' ' || c == '\t' || c == '\r'))
            continue;
        if (icase && c >= 'A' && c <= 'Z')
            c = (unsigned char)(c + 32);
        h = (h ^ c) * 16777619u;
    }
    return h;
}

static bool same_text(const char *x, const char *y)
{
    for (;;) {
        if (iwhite) {
            while (*x == ' ' || *x == '\t' || *x == '\r') x++;
            while (*y == ' ' || *y == '\t' || *y == '\r') y++;
        }
        unsigned char cx = (unsigned char)*x, cy = (unsigned char)*y;
        if (icase) {
            if (cx >= 'A' && cx <= 'Z') cx = (unsigned char)(cx + 32);
            if (cy >= 'A' && cy <= 'Z') cy = (unsigned char)(cy + 32);
        }
        if (cx != cy)
            return false;
        if (!cx)
            return true;
        x++;
        y++;
    }
}

static file_t FA, FB;

/* Line i of a against line j of b.
 *
 * The final-newline test lives here rather than in the printing: a file
 * ending "x" and one ending "x\n" hold different bytes, and if their
 * last lines compared equal the prefix trimming would swallow the very
 * difference we were asked about. */
static bool eq(long i, long j)
{
    if (FA.hash[i] != FB.hash[j])
        return false;
    if (i == FA.nlines - 1 && j == FB.nlines - 1 &&
        FA.no_final_nl != FB.no_final_nl)
        return false;
    return same_text(FA.line[i], FB.line[j]);
}

static void unload(file_t *f)
{
    free(f->data);
    free(f->line);
    free(f->hash);
    memset(f, 0, sizeof *f);
}

/* 0 read, 2 trouble. The message is printed here, where the reason is
 * still known. */
static int load(const char *path, file_t *f)
{
    memset(f, 0, sizeof *f);
    f->path = path;

    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "diff: %s: cannot open\n", path);
        return 2;
    }

    /* The size from stat is only a hint: /proc reports 0 for files that
     * have plenty in them, so the read loop grows the buffer when the
     * hint turns out to be short. */
    long cap = 65536;
    lp_stat_t st;
    if (lp_stat(path, &st, true) == 0 && st.size > 0)
        cap = (long)st.size + 1;
    if (cap > MAX_BYTES) {
        dprintf(STDERR_FILENO,
                "diff: %s is bigger than the %ldMB diff can hold\n"
                "diff:   cmp finds the first difference in a file of any size\n",
                path, MAX_BYTES / (1024 * 1024));
        lp_close((int)fd);
        return 2;
    }

    char *buf = malloc((size_t)cap + 1);
    long  len = 0;
    while (buf) {
        if (len == cap) {
            if (cap >= MAX_BYTES) {
                dprintf(STDERR_FILENO,
                        "diff: %s is bigger than the %ldMB diff can hold\n"
                        "diff:   try cmp, which reads a file of any size\n",
                        path, MAX_BYTES / (1024 * 1024));
                free(buf);
                lp_close((int)fd);
                return 2;
            }
            long ncap = cap * 2;
            if (ncap > MAX_BYTES) ncap = MAX_BYTES;
            char *nb = realloc(buf, (size_t)ncap + 1);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap = ncap;
        }
        long n = lp_read((int)fd, buf + len, (size_t)(cap - len));
        if (n < 0) {
            dprintf(STDERR_FILENO, "diff: %s: cannot read\n", path);
            free(buf);
            lp_close((int)fd);
            return 2;
        }
        if (n == 0)
            break;
        len += n;
    }
    lp_close((int)fd);
    if (!buf) {
        dprintf(STDERR_FILENO, "diff: out of memory reading %s\n", path);
        return 2;
    }
    buf[len] = '\0';
    f->data = buf;
    f->size = len;

    long nl = 0;
    for (long i = 0; i < len; i++) {
        if (buf[i] == '\n')      nl++;
        else if (buf[i] == '\0') f->binary = true;
    }
    if (f->binary)
        return 0;              /* the caller only asks whether it matches */

    f->no_final_nl = (len > 0 && buf[len - 1] != '\n');
    f->nlines = nl + (f->no_final_nl ? 1 : 0);
    if (f->nlines > MAX_LINES) {
        dprintf(STDERR_FILENO,
                "diff: %s has more than %ld lines, more than diff lays out\n"
                "diff:   diff -q says whether it changed; cmp says where\n",
                path, MAX_LINES);
        unload(f);
        return 2;
    }
    if (f->nlines == 0)
        return 0;

    f->line = malloc((size_t)f->nlines * sizeof(char *));
    f->hash = malloc((size_t)f->nlines * sizeof(u32));
    if (!f->line || !f->hash) {
        dprintf(STDERR_FILENO, "diff: out of memory reading %s\n", path);
        unload(f);
        return 2;
    }

    long  k = 0;
    char *start = buf;
    for (long i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            buf[i] = '\0';
            f->line[k++] = start;
            start = buf + i + 1;
        }
    }
    if (f->no_final_nl)
        f->line[k++] = start;
    for (long i = 0; i < f->nlines; i++)
        f->hash[i] = hash_line(f->line[i]);
    return 0;
}

/* ── -q: differ or not, without holding either file ──────────────────
 *
 * This one streams, because "are these two 200MB logs the same" is
 * exactly the question -q is asked, and it must not be the question
 * that runs the board out of memory. -i and -w become a filter on the
 * byte stream: dropping every space and folding case as the bytes go
 * past compares the same thing the line comparison above does. */

typedef struct { int fd; long len, pos; unsigned char buf[4096]; } rd_t;

static int rd_next(rd_t *r)
{
    for (;;) {
        if (r->pos >= r->len) {
            long n = lp_read(r->fd, r->buf, sizeof r->buf);
            if (n <= 0)
                return -1;
            r->len = n;
            r->pos = 0;
        }
        unsigned char c = r->buf[r->pos++];
        if (iwhite && (c == ' ' || c == '\t' || c == '\r'))
            continue;
        if (icase && c >= 'A' && c <= 'Z')
            c = (unsigned char)(c + 32);
        return (int)c;
    }
}

/* 0 the same, 1 different, 2 trouble. */
static int quick(const char *pa, const char *pb)
{
    static rd_t ra, rb;          /* 8KB of buffers; one pair is enough */

    long fa = lp_open(pa, O_RDONLY, 0);
    if (fa < 0) {
        dprintf(STDERR_FILENO, "diff: %s: cannot open\n", pa);
        return 2;
    }
    long fb = lp_open(pb, O_RDONLY, 0);
    if (fb < 0) {
        dprintf(STDERR_FILENO, "diff: %s: cannot open\n", pb);
        lp_close((int)fa);
        return 2;
    }

    ra.fd = (int)fa; ra.len = ra.pos = 0;
    rb.fd = (int)fb; rb.len = rb.pos = 0;

    int rc = 0;
    for (;;) {
        int x = rd_next(&ra), y = rd_next(&rb);
        if (x != y) { rc = 1; break; }
        if (x < 0)  break;
    }
    lp_close((int)fa);
    lp_close((int)fb);
    return rc;
}

/* ── Myers ───────────────────────────────────────────────────────────
 *
 * blocks[] holds the answer: one entry per run of removed and added
 * lines, in file order, with the indices of the whole file rather than
 * of the trimmed middle. A block always has at least one step behind
 * it, so there can never be more of them than MAX_D. */

typedef struct { long a0, alen, b0, blen; } block_t;

static block_t  blocks[MAX_D];
static block_t *first_block;
static long     nblocks;

static s32 *trace[MAX_D + 1];          /* the walk, one row per step */
static s32  vwork[2 * MAX_D + 3];
#define VOFF (MAX_D + 1)

/* 0 laid out, 1 needs more than MAX_D steps, 2 out of memory. */
static int myers(long alo, long ahi, long blo, long bhi)
{
    long n = ahi - alo, m = bhi - blo;
    long maxd = n + m;
    if (maxd > MAX_D) maxd = MAX_D;

    s32 *v = vwork + VOFF;
    v[1] = 0;

    long ndone = -1;   /* the step that reached the corner */
    long nrows = 0;    /* rows of trace[] allocated */
    int  rc = 1;

    for (long d = 0; d <= maxd; d++) {
        bool reached = false;

        for (long k = -d; k <= d; k += 2) {
            long x;
            if (k == -d || (k != d && v[k - 1] < v[k + 1]))
                x = v[k + 1];                 /* down: a line was added */
            else
                x = v[k - 1] + 1;             /* right: a line was removed */
            long y = x - k;
            while (x < n && y < m && eq(alo + x, blo + y)) { x++; y++; }
            v[k] = (s32)x;
            if (x >= n && y >= m) { reached = true; break; }
        }
        if (reached) { ndone = d; rc = 0; break; }

        /* Kept for the walk back. The last step is not kept: the walk
         * starts at the corner and only ever looks one step behind. */
        trace[d] = malloc((size_t)(2 * d + 1) * sizeof(s32));
        if (!trace[d]) { rc = 2; break; }
        for (long k = -d; k <= d; k += 2)
            trace[d][k + d] = v[k];
        nrows++;
    }

    if (rc == 0) {
        /* Back from the corner, gathering steps into blocks. Going
         * backwards means a block's first line is written several times
         * and the leftmost write is the one that stands. */
        long x = n, y = m;
        long slot = MAX_D;
        bool open = false;
        block_t cur = { 0, 0, 0, 0 };

        for (long d = ndone; d > 0; d--) {
            const s32 *row = trace[d - 1];
            long k = x - y;
            long prev_k = (k == -d ||
                           (k != d && row[(k - 1) + (d - 1)] <
                                      row[(k + 1) + (d - 1)]))
                          ? k + 1 : k - 1;
            long px = row[prev_k + (d - 1)];
            long py = px - prev_k;
            bool removed = (prev_k == k - 1);

            /* Equal lines between this step and the one to its right
             * end the block that was being gathered. */
            if (open && x > px + (removed ? 1 : 0)) {
                blocks[--slot] = cur;
                open = false;
            }
            if (!open) { cur.alen = 0; cur.blen = 0; open = true; }
            if (removed) cur.alen++; else cur.blen++;
            cur.a0 = alo + px;
            cur.b0 = blo + py;

            x = px;
            y = py;
        }
        if (open)
            blocks[--slot] = cur;

        first_block = blocks + slot;
        nblocks = MAX_D - slot;
    }

    for (long d = 0; d < nrows; d++) {
        free(trace[d]);
        trace[d] = NULL;
    }
    return rc;
}

/* ── Printing ────────────────────────────────────────────────────── */

static void put_line(char tag, const file_t *f, long i)
{
    printf("%c%s\n", tag, f->line[i]);
    /* The marker patch(1) would read is "\ No newline at end of file".
     * Nothing here parses it, so it is spelled the way everything else
     * on this system speaks. */
    if (i == f->nlines - 1 && f->no_final_nl)
        printf("\\ no newline at end of file\n");
}

static void put_range(long start, long count)
{
    if (count == 1)
        printf("%ld", start + 1);
    else
        printf("%ld,%ld", count ? start + 1 : start, count);
}

static void emit(long ctx)
{
    printf("--- %s\n", FA.path);
    printf("+++ %s\n", FB.path);

    long i = 0;
    while (i < nblocks) {
        const block_t *bl = first_block;

        /* Two changes closer than twice the context share a hunk;
         * further apart, their context would not have met anyway. */
        long j = i;
        while (j + 1 < nblocks &&
               bl[j + 1].a0 - (bl[j].a0 + bl[j].alen) <= 2 * ctx)
            j++;

        /* The same number of lines is taken on both sides, or the two
         * halves of the hunk drift apart and the counts in the header
         * stop matching what follows. */
        long pre = ctx;
        if (pre > bl[i].a0) pre = bl[i].a0;
        if (pre > bl[i].b0) pre = bl[i].b0;
        long post = ctx;
        if (post > FA.nlines - (bl[j].a0 + bl[j].alen))
            post = FA.nlines - (bl[j].a0 + bl[j].alen);
        if (post > FB.nlines - (bl[j].b0 + bl[j].blen))
            post = FB.nlines - (bl[j].b0 + bl[j].blen);

        long as = bl[i].a0 - pre, ae = bl[j].a0 + bl[j].alen + post;
        long bs = bl[i].b0 - pre, be = bl[j].b0 + bl[j].blen + post;

        fputs("@@ -", STDOUT_FILENO);
        put_range(as, ae - as);
        fputs(" +", STDOUT_FILENO);
        put_range(bs, be - bs);
        printf(" @@\n");

        long ax = as, bx = bs;
        for (long k = i; k <= j; k++) {
            while (ax < bl[k].a0) { put_line(' ', &FA, ax); ax++; bx++; }
            for (long t = 0; t < bl[k].alen; t++) put_line('-', &FA, ax++);
            for (long t = 0; t < bl[k].blen; t++) put_line('+', &FB, bx++);
        }
        while (ax < ae) { put_line(' ', &FA, ax); ax++; bx++; }

        i = j + 1;
    }
}

/* ── One pair of files ───────────────────────────────────────────── */

static int diff_files(const char *pa, const char *pb, long ctx, bool quiet)
{
    if (quiet) {
        int rc = quick(pa, pb);
        if (rc == 1)
            printf("files %s and %s differ\n", pa, pb);
        return rc;
    }

    if (load(pa, &FA) != 0)
        return 2;
    if (load(pb, &FB) != 0) { unload(&FA); return 2; }

    int rc;
    if (FA.binary || FB.binary) {
        /* Lines mean nothing here, and printing the bytes would spray
         * control characters over the terminal. */
        bool same = (FA.size == FB.size) &&
                    memcmp(FA.data, FB.data, (size_t)FA.size) == 0;
        if (!same)
            printf("binary files %s and %s differ\n", pa, pb);
        rc = same ? 0 : 1;
        goto done;
    }

    long alo = 0, ahi = FA.nlines, blo = 0, bhi = FB.nlines;
    while (alo < ahi && blo < bhi && eq(alo, blo)) { alo++; blo++; }
    while (ahi > alo && bhi > blo && eq(ahi - 1, bhi - 1)) { ahi--; bhi--; }

    if (alo == ahi && blo == bhi) { rc = 0; goto done; }

    switch (myers(alo, ahi, blo, bhi)) {
    case 0:
        emit(ctx);
        rc = 1;
        break;
    case 1:
        dprintf(STDERR_FILENO,
                "diff: %s and %s differ by more than %ld lines, too far\n"
                "diff:   apart to lay out as one edit. cmp -l lists every\n"
                "diff:   differing byte, and diff -q just confirms it.\n",
                pa, pb, MAX_D);
        rc = 1;
        break;
    default:
        dprintf(STDERR_FILENO,
                "diff: out of memory comparing %s and %s\n", pa, pb);
        rc = 2;
        break;
    }

done:
    unload(&FA);
    unload(&FB);
    return rc;
}

/* ── -r ──────────────────────────────────────────────────────────── */

static int worse(int a, int b) { return a > b ? a : b; }

/* Entries are visited in the order the filesystem hands them over,
 * which is not alphabetical. Sorting would mean holding every name in a
 * directory in memory at every level of the recursion, and the output
 * is grouped by file either way. */
static int diff_dirs(const char *pa, const char *pb, long ctx, bool quiet,
                     bool recurse, int depth)
{
    if (depth >= MAX_DEPTH) {
        dprintf(STDERR_FILENO,
                "diff: %s is more than %d directories deep - stopping.\n"
                "diff:   a mount that contains its own parent looks like this\n",
                pa, MAX_DEPTH);
        return 2;
    }

    int rc = 0;

    for (int pass = 0; pass < 2; pass++) {
        /* Pass 0 walks a and compares; pass 1 walks b and only reports
         * what a did not have. */
        const char *dir = pass ? pb : pa;

        long fd = lp_open(dir, O_RDONLY | O_DIRECTORY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "diff: %s: cannot read the directory\n",
                    dir);
            return 2;
        }

        char buf[2048];
        for (;;) {
            long got = sys_getdents((int)fd, buf, sizeof buf);
            if (got <= 0)
                break;

            for (long off = 0; off < got; ) {
                char *rec  = buf + off;
                u16   reclen = *(u16 *)(rec + DIRENT_RECLEN);
                char *name = rec + DIRENT_NAME;
                if (reclen == 0)
                    break;
                off += reclen;
                if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                    continue;

                char ca[768], cb[768];
                snprintf(ca, sizeof ca, "%s/%s", pa, name);
                snprintf(cb, sizeof cb, "%s/%s", pb, name);

                /* Not lp_exists: that follows the link, so a symlink
                 * pointing at something absent - a device that is not
                 * plugged in, a path on an unmounted card - would be
                 * reported as missing from a directory it is sitting
                 * in. The link itself is what is being compared. */
                lp_stat_t sa, sb;
                bool has_a = (lp_stat(ca, &sa, false) == 0);
                bool has_b = (lp_stat(cb, &sb, false) == 0);

                if (pass) {
                    if (!has_a) {
                        printf("only in %s: %s\n", pb, name);
                        rc = worse(rc, 1);
                    }
                    continue;
                }
                if (!has_a)
                    continue;              /* it went away as we walked */
                if (!has_b) {
                    printf("only in %s: %s\n", pa, name);
                    rc = worse(rc, 1);
                    continue;
                }

                /* A symlink is compared as a link rather than as what
                 * it points at. Descending through one is how a walk
                 * ends up going round for ever - a link back to its own
                 * parent is enough - and "points at b" becoming "points
                 * at c" is a change worth reporting in its own right. */
                bool a_link = (sa.mode & LP_S_IFMT) == LP_S_IFLNK;
                bool b_link = (sb.mode & LP_S_IFMT) == LP_S_IFLNK;
                if (a_link || b_link) {
                    char ta[256], tb[256];
                    long la = a_link ? lp_readlink(ca, ta, sizeof ta - 1) : 0;
                    long lb = b_link ? lp_readlink(cb, tb, sizeof tb - 1) : 0;
                    ta[la > 0 ? la : 0] = '\0';
                    tb[lb > 0 ? lb : 0] = '\0';
                    if (a_link != b_link)
                        printf("%s is a symlink and %s is not\n",
                               a_link ? ca : cb, a_link ? cb : ca);
                    else if (strcmp(ta, tb) != 0)
                        printf("%s points at %s, %s at %s\n",
                               ca, ta, cb, tb);
                    else
                        continue;
                    rc = worse(rc, 1);
                    continue;
                }

                bool a_dir = (sa.mode & LP_S_IFMT) == LP_S_IFDIR;
                bool b_dir = (sb.mode & LP_S_IFMT) == LP_S_IFDIR;
                if (a_dir != b_dir) {
                    printf("%s is a directory and %s is a file\n",
                           a_dir ? ca : cb, a_dir ? cb : ca);
                    rc = worse(rc, 1);
                    continue;
                }
                if (a_dir) {
                    if (recurse)
                        rc = worse(rc, diff_dirs(ca, cb, ctx, quiet,
                                                 recurse, depth + 1));
                    continue;
                }
                rc = worse(rc, diff_files(ca, cb, ctx, quiet));
            }
        }
        lp_close((int)fd);
    }
    return rc;
}

static void usage(int fd)
{
    dprintf(fd, "usage: diff [-u] [-q] [-i] [-w] [-U n] a b\n");
    dprintf(fd, "       diff -r [...] dir1 dir2\n");
    dprintf(fd, "  -u  unified, three lines of context (the default)\n");
    dprintf(fd, "  -q  only say whether they differ\n");
    dprintf(fd, "  -r  walk two directory trees\n");
    dprintf(fd, "  -i  ignore case      -w  ignore whitespace\n");
    dprintf(fd, "  -U  n lines of context instead of three\n");
    dprintf(fd, "  status: 0 the same, 1 different, 2 trouble\n");
}

int main(int argc, char **argv)
{
    long ctx = 3;
    bool quiet = false, recurse = false, end_of_opts = false;
    const char *pa = NULL, *pb = NULL;

    /* One loop, and the value of -U is consumed inside it. Splitting the
     * options and the filenames into two passes is how the value of an
     * option ends up being opened as a file - that bug has shipped here
     * before. */
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];

        if (!end_of_opts && s[0] == '-' && s[1]) {
            if (strcmp(s, "--") == 0) { end_of_opts = true; continue; }

            for (int c = 1; s[c]; c++) {
                if (s[c] == 'u') continue;      /* the default; said aloud */
                if (s[c] == 'q') { quiet   = true; continue; }
                if (s[c] == 'r') { recurse = true; continue; }
                if (s[c] == 'i') { icase   = true; continue; }
                if (s[c] == 'w') { iwhite  = true; continue; }
                if (s[c] == 'h') { usage(STDOUT_FILENO); return 0; }
                if (s[c] == 'U') {
                    const char *num = s[c + 1] ? s + c + 1
                                    : (i + 1 < argc ? argv[++i] : NULL);
                    if (!num || num[0] < '0' || num[0] > '9') {
                        dprintf(STDERR_FILENO,
                                "diff: -U wants a line count, as in"
                                " diff -U 5 a b\n");
                        return 2;
                    }
                    ctx = atoi(num);
                    if (ctx > MAX_LINES) ctx = MAX_LINES;
                    break;                      /* the rest of s was the number */
                }
                dprintf(STDERR_FILENO,
                        "diff: unknown option -%c; diff -h lists them\n", s[c]);
                return 2;
            }
            continue;
        }

        if (!pa)      pa = s;
        else if (!pb) pb = s;
        else {
            dprintf(STDERR_FILENO,
                    "diff: %s: diff compares exactly two things\n", s);
            return 2;
        }
    }

    if (!pa || !pb) {
        usage(STDERR_FILENO);
        return 2;
    }

    bool da = lp_is_dir(pa), db = lp_is_dir(pb);
    if (da && db)
        return diff_dirs(pa, pb, ctx, quiet, recurse, 0);

    /* diff <dir> <file> means the file of that name inside the
     * directory, which is what people type when comparing an edited
     * copy against the original in place. */
    static char joined[768];
    if (da != db) {
        const char *file = da ? pb : pa;
        if (!lp_exists(file)) {
            /* Not "dir/thing: cannot open" - the name that is wrong is
             * the one the person typed. */
            dprintf(STDERR_FILENO, "diff: %s: cannot open\n", file);
            return 2;
        }
        const char *base = strrchr(file, '/');
        base = base ? base + 1 : file;
        snprintf(joined, sizeof joined, "%s/%s", da ? pa : pb, base);
        if (da) pa = joined; else pb = joined;
    }

    return diff_files(pa, pb, ctx, quiet);
}
