/* install - put a file where it belongs, with the mode it needs.
 *
 *   install -m 755 prog /usr/bin/       one file, executable, into a directory
 *   install -d /var/lib/thing           make the directory and its parents
 *   install -D prog /usr/bin/prog       make the parents, then copy
 *
 * This is what the `install:` target of every Makefile runs, and the
 * reason it exists rather than `cp` plus `chmod` is that it does both in
 * one step and does them in the safe order: the destination is unlinked
 * before the new file is opened, never truncated in place. That matters
 * more than it looks. Overwriting a running program in place gives
 * ETXTBSY or, worse on some systems, corrupts the copy the kernel is
 * still paging in; unlinking first leaves the old inode alive for
 * whoever still has it open and puts a brand new one in the directory.
 * `cp` cannot do that without -f, and by then the mode has already been
 * lost.
 *
 * Directories are made 0755 whatever the umask says, and files get the
 * mode asked for exactly, because the point of an install rule is that
 * the result does not depend on the shell that ran it.
 *
 * What is missing, and why:
 *   -p   copies the source's timestamps. The call for that is
 *        utimensat(2) and this libc has no wrapper for it - the same
 *        gap `touch` documents. Rather than accept the flag and quietly
 *        do nothing, it is not here, so `install -p` says so.
 *   -s   runs strip(1) on the result. There is no strip on this system.
 *   -Z, --context, --preserve-context  are SELinux, which this kernel
 *        is not built with.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define EEXIST  17
#define ENOTDIR 20
#define ENOENT   2

static const char *prog = "install";

static bool   opt_dir      = false;   /* -d */
static bool   opt_parents  = false;   /* -D */
static bool   opt_verbose  = false;   /* -v */
static bool   opt_compare  = false;   /* -C */
static bool   opt_notarget = false;   /* -T */
static const char *target_dir = NULL; /* -t */
static mode_t file_mode   = 0755;
static uid_t  want_uid    = (uid_t)-1;
static gid_t  want_gid    = (gid_t)-1;
static int    failures    = 0;

/* --backup=CONTROL, and -b which is --backup with the control left to
 * the environment. "existing" is the default and means: keep making
 * numbered backups if numbered ones are already there, otherwise one
 * plain file~. */
enum { BK_NONE = 0, BK_SIMPLE, BK_NUMBERED, BK_EXISTING };
static int   backup_kind = BK_NONE;
static const char *simple_suffix = "~";

/* ── Naming a file in a message ──
 * coreutils always quotes an operand it is talking about, and reaches
 * for double quotes or a $'..' escape when the name itself contains a
 * quote or a byte that does not print. Getting this right is not
 * decoration: the name may contain a space, and "cannot stat my file"
 * reads as two files. */
static bool printable(unsigned char c) { return c >= 0x20 && c < 0x7f; }

static void quoteaf(char *out, size_t cap, const char *s)
{
    bool has_quote = false, has_special = false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '\'') has_quote = true;
        else if (!printable(*p) || *p == '"' || *p == '\\' || *p == '$' || *p == '`')
            has_special = true;
    }
    if (has_quote && !has_special) { snprintf(out, cap, "\"%s\"", s); return; }
    if (!has_quote && !has_special) { snprintf(out, cap, "'%s'", s); return; }

    size_t n = 0;
    bool inside = true;
    if (n + 1 < cap) out[n++] = '\'';
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (printable(*p) && *p != '\'') {
            if (!inside && n + 2 < cap) { out[n++] = '\''; out[n++] = '\''; inside = true; }
            if (n + 1 < cap) out[n++] = (char)*p;
            continue;
        }
        if (inside && n + 3 < cap) { out[n++] = '\''; out[n++] = '$'; out[n++] = '\''; inside = false; }
        if (*p == '\'') { if (n + 2 < cap) { out[n++] = '\\'; out[n++] = '\''; } }
        else if (*p == '\n') { if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 'n'; } }
        else if (*p == '\t') { if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 't'; } }
        else if (n + 4 < cap) {
            out[n++] = '\\';
            out[n++] = (char)('0' + (*p >> 6));
            out[n++] = (char)('0' + ((*p >> 3) & 7));
            out[n++] = (char)('0' + (*p & 7));
        }
    }
    if (n + 1 < cap) out[n++] = '\'';
    if (n < cap) out[n] = '\0'; else if (cap) out[cap - 1] = '\0';
}

static char qa[4096], qb[4096];

static void oops(const char *what, const char *path, int err)
{
    quoteaf(qa, sizeof qa, path);
    dprintf(STDERR_FILENO, "%s: %s %s: %s\n", prog, what, qa, lp_strerror(err));
    failures = 1;
}

static void try_help(void)
{
    dprintf(STDERR_FILENO, "Try '%s --help' for more information.\n", prog);
}

/* ── The mode ──
 * -m takes what chmod takes, but unlike chmod it starts from nothing:
 * `install -m u+x` gives 0100, not "whatever it was plus x". The umask
 * is not consulted either - install exists so the result does not depend
 * on the shell. */
static bool mode_parse(const char *spec, mode_t *out)
{
    if (*spec == '\0') return false;

    if (*spec >= '0' && *spec <= '7') {
        mode_t v = 0;
        for (const char *p = spec; *p; p++) {
            if (*p < '0' || *p > '7') return false;
            v = v * 8 + (mode_t)(*p - '0');
            if (v > 07777) return false;
        }
        *out = v;
        return true;
    }

    mode_t cur = 0;
    const char *p = spec;
    for (;;) {
        mode_t who = 0;
        for (; *p; p++) {
            if (*p == 'u')      who |= 04700;
            else if (*p == 'g') who |= 02070;
            else if (*p == 'o') who |= 00007;
            else if (*p == 'a') who |= 07777;
            else break;
        }
        if (who == 0) who = 07777;
        if (*p != '+' && *p != '-' && *p != '=') return false;

        while (*p == '+' || *p == '-' || *p == '=') {
            char op = *p++;
            mode_t bits = 0;
            for (; *p && *p != ',' && *p != '+' && *p != '-' && *p != '='; p++) {
                switch (*p) {
                case 'r': bits |= 0444; break;
                case 'w': bits |= 0222; break;
                case 'x': bits |= 0111; break;
                case 's': bits |= 06000; break;
                case 't': bits |= 01000; break;
                default:  return false;
                }
            }
            bits &= who;
            if (op == '+')      cur |= bits;
            else if (op == '-') cur &= ~bits;
            else                cur = (cur & ~who) | bits;
        }
        if (*p == '\0') break;
        if (*p != ',') return false;
        p++;
    }
    *out = cur;
    return true;
}

static const char *basename_of(const char *path)
{
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' && p[1] != '\0')
            last = p + 1;
    return last;
}

static bool join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlcpy(out, dir, cap);
    if (n >= cap) return false;
    if (n > 0 && out[n - 1] != '/') {
        if (n + 1 >= cap) return false;
        out[n++] = '/';
        out[n] = '\0';
    }
    return strlcat(out, name, cap) < cap;
}

static bool is_dir(const char *path)
{
    lp_stat_t st;
    return lp_stat(path, &st, true) == 0 && (st.mode & LP_S_IFMT) == LP_S_IFDIR;
}

/* ── Making the parents ──
 * `install -d a/b/c` and `install -D x a/b/c/x` both walk the path from
 * the left and make what is not there yet. A component that exists but
 * is not a directory is ENOTDIR, not EEXIST: the thing in the way is a
 * file, and saying "File exists" about a parent tells the reader
 * nothing about what to do. */
static bool make_parents(const char *path, bool include_last, mode_t last_mode)
{
    char work[4096];
    if (strlcpy(work, path, sizeof work) >= sizeof work) {
        oops("cannot create directory", path, 36);
        return false;
    }

    for (char *p = work; ; ) {
        while (*p == '/') p++;
        if (*p == '\0') break;
        char *slash = strchr(p, '/');
        bool last = false;
        if (slash) {
            /* Trailing slashes do not make another component. */
            char *q = slash;
            while (*q == '/') q++;
            if (*q == '\0') { *slash = '\0'; last = true; }
        } else {
            last = true;
        }
        if (last && !include_last)
            break;

        char saved = 0;
        if (slash && !last) { saved = *slash; *slash = '\0'; }

        long r = lp_mkdir(work, last ? last_mode : 0755);
        if (r == 0) {
            /* umask trimmed what mkdir was asked for; install promises
             * the mode it was given. */
            lp_chmod(work, last ? last_mode : 0755);
            if (opt_verbose) {
                quoteaf(qa, sizeof qa, work);
                printf("%s: creating directory %s\n", prog, qa);
            }
        } else if (r == -EEXIST) {
            if (!is_dir(work)) {
                oops("cannot create directory", work, last ? EEXIST : ENOTDIR);
                return false;
            }
            if (last && include_last && !opt_dir) {
                /* nothing to do */
            }
        } else {
            oops("cannot create directory", work, (int)-r);
            return false;
        }

        if (last) break;
        *slash = saved;
        p = slash + 1;
    }
    return true;
}

/* ── Backups ──
 * The suffix is '~' unless -S or SIMPLE_BACKUP_SUFFIX says otherwise,
 * and a numbered backup is name.~N~ with N one past the highest already
 * there. The default control, "existing", keeps whichever kind the
 * directory already has, so a tree that has been backed up numbered
 * stays that way. */
static int backup_control(const char *s)
{
    if (!s || !*s) return BK_EXISTING;
    if (!strcmp(s, "none") || !strcmp(s, "off"))      return BK_NONE;
    if (!strcmp(s, "numbered") || !strcmp(s, "t"))    return BK_NUMBERED;
    if (!strcmp(s, "existing") || !strcmp(s, "nil"))  return BK_EXISTING;
    if (!strcmp(s, "simple") || !strcmp(s, "never"))  return BK_SIMPLE;
    return -1;
}

static bool numbered_exists(const char *dst)
{
    char cand[4096];
    for (int i = 1; i < 1000; i++) {
        snprintf(cand, sizeof cand, "%s.~%d~", dst, i);
        if (lp_exists(cand)) return true;
    }
    return false;
}

static bool backup_name(const char *dst, char *out, size_t cap)
{
    int kind = backup_kind;
    if (kind == BK_EXISTING)
        kind = numbered_exists(dst) ? BK_NUMBERED : BK_SIMPLE;

    if (kind == BK_SIMPLE)
        return (size_t)snprintf(out, cap, "%s%s", dst, simple_suffix) < cap;

    for (int i = 1; i < 100000; i++) {
        if ((size_t)snprintf(out, cap, "%s.~%d~", dst, i) >= cap) return false;
        if (!lp_exists(out)) return true;
    }
    return false;
}

/* ── -C ──
 * "Do not touch the destination if nothing about it would change." The
 * comparison has to include mode and ownership as well as the bytes,
 * or `install -C -m 600` over a 0644 file would decide there was
 * nothing to do. */
static bool identical(const char *src, const char *dst, const lp_stat_t *ss)
{
    lp_stat_t ds;
    if (lp_stat(dst, &ds, true) != 0) return false;
    if ((ds.mode & LP_S_IFMT) != LP_S_IFREG) return false;
    if (ds.size != ss->size) return false;
    if ((ds.mode & 07777) != file_mode) return false;
    if (want_uid != (uid_t)-1 && ds.uid != want_uid) return false;
    if (want_gid != (gid_t)-1 && ds.gid != want_gid) return false;

    long a = lp_open(src, O_RDONLY, 0);
    if (a < 0) return false;
    long b = lp_open(dst, O_RDONLY, 0);
    if (b < 0) { lp_close((int)a); return false; }

    static char ba[32768], bb[32768];
    bool same = true;
    for (;;) {
        long na = lp_read((int)a, ba, sizeof ba);
        long nb = lp_read((int)b, bb, sizeof bb);
        if (na != nb) { same = false; break; }
        if (na <= 0) break;
        if (memcmp(ba, bb, (size_t)na) != 0) { same = false; break; }
    }
    lp_close((int)a);
    lp_close((int)b);
    return same;
}

static int install_file(const char *src, const char *dst)
{
    lp_stat_t ss;
    if (lp_stat(src, &ss, true) != 0) {
        long r = lp_stat(src, &ss, true);
        oops("cannot stat", src, (int)-r);
        return 1;
    }
    if ((ss.mode & LP_S_IFMT) == LP_S_IFDIR) {
        quoteaf(qa, sizeof qa, src);
        dprintf(STDERR_FILENO, "%s: omitting directory %s\n", prog, qa);
        failures = 1;
        return 1;
    }

    lp_stat_t ds;
    bool dst_exists = lp_stat(dst, &ds, true) == 0;
    if (dst_exists && ds.dev == ss.dev && ds.ino == ss.ino) {
        quoteaf(qa, sizeof qa, src);
        quoteaf(qb, sizeof qb, dst);
        dprintf(STDERR_FILENO, "%s: %s and %s are the same file\n", prog, qa, qb);
        failures = 1;
        return 1;
    }
    if (dst_exists && (ds.mode & LP_S_IFMT) == LP_S_IFDIR) {
        quoteaf(qa, sizeof qa, dst);
        dprintf(STDERR_FILENO,
                "%s: cannot overwrite directory %s with non-directory\n", prog, qa);
        failures = 1;
        return 1;
    }

    /* A name written with a trailing slash is a request for a
     * directory. The kernel answers the open with EISDIR, which reads
     * as though the destination were one; it is not, and ENOTDIR is
     * what actually went wrong. */
    size_t dlen = strlen(dst);
    if (dlen && dst[dlen - 1] == '/') {
        oops("cannot create regular file", dst, ENOTDIR);
        return 1;
    }

    if (opt_compare && dst_exists && identical(src, dst, &ss))
        return 0;

    char backup[4096];
    bool backed_up = false;
    if (dst_exists) {
        if (backup_kind != BK_NONE && backup_name(dst, backup, sizeof backup)) {
            long r = lp_rename(dst, backup);
            if (r < 0) { oops("cannot backup", dst, (int)-r); return 1; }
            backed_up = true;
        } else {
            /* Unlink rather than truncate: a program that is running
             * from this path keeps the inode it is executing. */
            lp_unlink(dst);
            if (opt_verbose) {
                quoteaf(qa, sizeof qa, dst);
                printf("removed %s\n", qa);
            }
        }
    }

    long in = lp_open(src, O_RDONLY, 0);
    if (in < 0) { oops("cannot open", src, (int)-in); return 1; }
    long out = lp_open(dst, O_WRONLY | O_CREAT | O_TRUNC, file_mode);
    if (out < 0) {
        lp_close((int)in);
        oops("cannot create regular file", dst, (int)-out);
        return 1;
    }

    static char buf[65536];
    int rc = 0;
    for (;;) {
        long n = lp_read((int)in, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0) { oops("error reading", src, (int)-n); rc = 1; break; }
        for (long off = 0; off < n; ) {
            long w = lp_write((int)out, buf + off, (size_t)(n - off));
            if (w <= 0) { oops("error writing", dst, w ? (int)-w : 5); rc = 1; break; }
            off += w;
        }
        if (rc) break;
    }
    lp_close((int)in);
    lp_close((int)out);
    if (rc) return 1;

    lp_chmod(dst, file_mode);
    if (want_uid != (uid_t)-1 || want_gid != (gid_t)-1) {
        long r = lp_chown(dst, want_uid, want_gid);
        if (r < 0) { oops("cannot change ownership of", dst, (int)-r); return 1; }
    }

    if (opt_verbose) {
        quoteaf(qa, sizeof qa, src);
        quoteaf(qb, sizeof qb, dst);
        if (backed_up) {
            char qc[4096];
            quoteaf(qc, sizeof qc, backup);
            printf("%s -> %s (backup: %s)\n", qa, qb, qc);
        } else {
            printf("%s -> %s\n", qa, qb);
        }
    }
    return 0;
}

static int into_dir(const char *src, const char *dir)
{
    char full[4096];
    if (!join(full, sizeof full, dir, basename_of(src))) {
        oops("cannot create regular file", src, 36);
        return 1;
    }
    return install_file(src, full);
}

static void usage(int fd)
{
    dprintf(fd,
        "Usage: install [OPTION]... [-T] SOURCE DEST\n"
        "  or:  install [OPTION]... SOURCE... DIRECTORY\n"
        "  or:  install [OPTION]... -t DIRECTORY SOURCE...\n"
        "  or:  install [OPTION]... -d DIRECTORY...\n\n"
        "In the first three forms, copy SOURCE to DEST or multiple SOURCE(s) to\n"
        "the existing DIRECTORY, while setting permission modes and owner/group.\n"
        "In the 4th form, create all components of the given DIRECTORY(ies).\n\n"
        "Mandatory arguments to long options are mandatory for short options too.\n"
        "      --backup[=CONTROL]  make a backup of each existing destination file\n"
        "  -b                  like --backup but does not accept an argument\n"
        "  -c                  (ignored)\n"
        "  -C, --compare       compare content of source and destination files, and\n"
        "                        if no change to content, ownership, and permissions,\n"
        "                        do not modify the destination at all\n"
        "  -d, --directory     treat all arguments as directory names; create all\n"
        "                        components of the specified directories\n"
        "  -D                  create all leading components of DEST except the last,\n"
        "                        or all components of --target-directory,\n"
        "                        then copy SOURCE to DEST\n"
        "  -g, --group=GROUP   set group ownership, instead of process' current group\n"
        "  -m, --mode=MODE     set permission mode (as in chmod), instead of rwxr-xr-x\n"
        "  -o, --owner=OWNER   set ownership (super-user only)\n"
        "  -S, --suffix=SUFFIX  override the usual backup suffix\n"
        "  -t, --target-directory=DIRECTORY  copy all SOURCE arguments into DIRECTORY\n"
        "  -T, --no-target-directory  treat DEST as a normal file\n"
        "  -v, --verbose       print the name of each created file or directory\n"
        "      --help        display this help and exit\n\n"
        "The backup suffix is '~', unless set with --suffix or SIMPLE_BACKUP_SUFFIX.\n"
        "The version control method may be selected via the --backup option or through\n"
        "the VERSION_CONTROL environment variable.  Here are the values:\n\n"
        "  none, off       never make backups (even if --backup is given)\n"
        "  numbered, t     make numbered backups\n"
        "  existing, nil   numbered if numbered backups exist, simple otherwise\n"
        "  simple, never   always make simple backups\n\n"
        "Timestamps are not copied (-p is not available): this system has no way to\n"
        "set them.\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "backup", 2, 'B' }, { "compare", 0, 'C' }, { "directory", 0, 'd' },
        { "group", 1, 'g' }, { "mode", 1, 'm' }, { "owner", 1, 'o' },
        { "suffix", 1, 'S' }, { "target-directory", 1, 't' },
        { "no-target-directory", 0, 'T' }, { "verbose", 0, 'v' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    const char *ctl = NULL;
    bool want_backup = false;
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "bcCdDg:m:o:S:t:Tv", lo);

    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'b': want_backup = true; break;
        case 'B': want_backup = true; ctl = g.arg; break;
        case 'c': break;                       /* accepted and ignored, as GNU does */
        case 'C': opt_compare = true; break;
        case 'd': opt_dir = true; break;
        case 'D': opt_parents = true; break;
        case 'T': opt_notarget = true; break;
        case 'v': opt_verbose = true; break;
        case 't': target_dir = g.arg; break;
        case 'S': want_backup = true; simple_suffix = g.arg; break;
        case 'm':
            if (!g.arg || !mode_parse(g.arg, &file_mode)) {
                quoteaf(qa, sizeof qa, g.arg ? g.arg : "");
                dprintf(STDERR_FILENO, "%s: invalid mode %s\n", prog, qa);
                return 1;
            }
            break;
        case 'o': {
            lp_user_t u;
            if (lp_user_by_name(g.arg, &u)) { want_uid = u.uid; break; }
            bool digits = g.arg && *g.arg;
            for (const char *p = g.arg; digits && *p; p++)
                if (*p < '0' || *p > '9') digits = false;
            if (digits) { want_uid = (uid_t)strtol(g.arg, NULL, 10); break; }
            quoteaf(qa, sizeof qa, g.arg ? g.arg : "");
            dprintf(STDERR_FILENO, "%s: invalid user %s\n", prog, qa);
            return 1;
        }
        case 'g': {
            gid_t gid;
            if (lp_group_by_name(g.arg, &gid)) { want_gid = gid; break; }
            bool digits = g.arg && *g.arg;
            for (const char *p = g.arg; digits && *p; p++)
                if (*p < '0' || *p > '9') digits = false;
            if (digits) { want_gid = (gid_t)strtol(g.arg, NULL, 10); break; }
            quoteaf(qa, sizeof qa, g.arg ? g.arg : "");
            dprintf(STDERR_FILENO, "%s: invalid group %s\n", prog, qa);
            return 1;
        }
        case 'H': usage(STDOUT_FILENO); return 0;
        default: lp_getopt_err(prog, &g); return 1;
        }
    }

    if (want_backup) {
        const char *env = getenv("SIMPLE_BACKUP_SUFFIX");
        if (env && *env && simple_suffix[0] == '~' && simple_suffix[1] == '\0')
            simple_suffix = env;
        if (!ctl) ctl = getenv("VERSION_CONTROL");
        backup_kind = backup_control(ctl);
        if (backup_kind < 0) {
            quoteaf(qa, sizeof qa, ctl);
            dprintf(STDERR_FILENO,
                    "%s: invalid argument %s for 'backup type'\n", prog, qa);
            dprintf(STDERR_FILENO,
                    "Valid arguments are:\n  - 'none', 'off'\n"
                    "  - 'simple', 'never'\n  - 'existing', 'nil'\n"
                    "  - 'numbered', 't'\n");
            try_help();
            return 1;
        }
    }

    int nops = argc - g.ind;
    char **ops = argv + g.ind;

    if (opt_dir && target_dir) {
        dprintf(STDERR_FILENO,
                "%s: target directory not allowed when installing a directory\n",
                prog);
        return 1;
    }
    if (nops == 0) {
        dprintf(STDERR_FILENO, "%s: missing file operand\n", prog);
        try_help();
        return 1;
    }

    if (opt_dir) {
        for (int i = 0; i < nops; i++)
            if (!make_parents(ops[i], true, file_mode))
                failures = 1;
        return failures;
    }

    if (target_dir) {
        if (opt_parents && !make_parents(target_dir, true, 0755))
            return 1;
        lp_stat_t ts;
        long r = lp_stat(target_dir, &ts, true);
        if (r < 0) { oops("failed to access", target_dir, (int)-r); return 1; }
        if ((ts.mode & LP_S_IFMT) != LP_S_IFDIR) {
            oops("failed to access", target_dir, ENOTDIR);
            return 1;
        }
        for (int i = 0; i < nops; i++)
            into_dir(ops[i], target_dir);
        return failures;
    }

    if (nops == 1) {
        quoteaf(qa, sizeof qa, ops[0]);
        dprintf(STDERR_FILENO,
                "%s: missing destination file operand after %s\n", prog, qa);
        try_help();
        return 1;
    }

    if (opt_notarget) {
        if (nops > 2) {
            quoteaf(qa, sizeof qa, ops[2]);
            dprintf(STDERR_FILENO, "%s: extra operand %s\n", prog, qa);
            try_help();
            return 1;
        }
        if (opt_parents && !make_parents(ops[1], false, 0755))
            return 1;
        install_file(ops[0], ops[1]);
        return failures;
    }

    const char *dst = ops[nops - 1];

    if (nops == 2) {
        if (opt_parents && !make_parents(dst, false, 0755))
            return 1;
        if (is_dir(dst))
            into_dir(ops[0], dst);
        else
            install_file(ops[0], dst);
        return failures;
    }

    /* Three or more operands can only mean "into this directory", so a
     * last operand that is not one is the mistake, not the sources. */
    if (!is_dir(dst)) {
        lp_stat_t ds;
        int err = (lp_stat(dst, &ds, true) == 0) ? ENOTDIR : ENOENT;
        quoteaf(qa, sizeof qa, dst);
        dprintf(STDERR_FILENO, "%s: target %s: %s\n", prog, qa, lp_strerror(err));
        return 1;
    }
    for (int i = 0; i < nops - 1; i++)
        into_dir(ops[i], dst);
    return failures;
}
