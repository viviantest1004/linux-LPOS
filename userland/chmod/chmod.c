/* chmod - change what may be done with a file.
 *
 *   chmod [-cfvR] <mode> <file>...
 *   chmod [-cfvR] --reference=RFILE <file>...
 *
 * The mode is either octal (755, 4755) or the symbolic grammar in full:
 * a list of clauses separated by commas, each of them who (u g o a), an
 * operator (+ - =) and permissions (r w x X s t, or u g o to copy one
 * class onto another). `u=rw,go=r`, `a+x`, `g-w`, `+X` all work.
 *
 * The one part of that grammar that surprises people: a clause with no
 * "who" is not the same as "a". POSIX says the umask applies to it, so
 * with the usual umask 022 `chmod +w f` gives the owner write and
 * nobody else, while `chmod a+w f` gives everybody write. Getting this
 * wrong makes files group-writable that were not meant to be, so the
 * umask is read from /proc/self/status - there is no umask(2) wrapper in
 * libc, and guessing 022 would be wrong on any machine that set it.
 *
 * X is the other one worth knowing: it means "execute, but only if this
 * is a directory or somebody can already execute it", which is what
 * makes `chmod -R a+rX` safe on a source tree.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

/* linux_dirent64 offsets (same as ls.c) */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

static bool opt_changes   = false;   /* -c */
static bool opt_quiet     = false;   /* -f */
static bool opt_verbose   = false;   /* -v */
static bool opt_recursive = false;   /* -R */
static int  failures      = 0;

/* The mode to apply, in whichever of the two forms was given. */
static bool   numeric = false;
static mode_t octal   = 0;
static const char *symbolic = NULL;

static mode_t the_umask(void)
{
    static int cached = -1;
    if (cached >= 0) return (mode_t)cached;
    cached = 022;                    /* the usual one, if /proc is not there */
    char buf[4096];
    long n = proc_read("/proc/self/status", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        for (char *p = buf; *p; p++) {
            if (p != buf && p[-1] != '\n') continue;
            if (strncmp(p, "Umask:", 6) != 0) continue;
            p += 6;
            while (*p == ' ' || *p == '\t') p++;
            int v = 0;
            while (*p >= '0' && *p <= '7') v = v * 8 + (*p++ - '0');
            cached = v;
            break;
        }
    }
    return (mode_t)cached;
}

/* "rwxr-xr-x", with the s/t bits shown where GNU shows them. */
static void mode_string(mode_t m, char *out)
{
    static const char rwx[] = "rwx";
    for (int c = 0; c < 3; c++)
        for (int b = 0; b < 3; b++)
            out[c * 3 + b] = (m & (mode_t)(0400 >> (c * 3 + b))) ? rwx[b] : '-';
    if (m & 04000) out[2] = (m & 0100) ? 's' : 'S';
    if (m & 02000) out[5] = (m & 0010) ? 's' : 'S';
    if (m & 01000) out[8] = (m & 0001) ? 't' : 'T';
    out[9] = '\0';
}

static void report(const char *path, mode_t from, mode_t to)
{
    char a[10], b[10];
    mode_string(from, a);
    mode_string(to, b);
    if (from != to) {
        if (opt_verbose || opt_changes)
            printf("mode of '%s' changed from %04o (%s) to %04o (%s)\n",
                   path, from, a, to, b);
    } else if (opt_verbose) {
        printf("mode of '%s' retained as %04o (%s)\n", path, to, b);
    }
}

/* One symbolic clause list applied to the mode a file already has.
 * Returns false if the text is not a mode at all. */
static bool apply_symbolic(const char *spec, mode_t *m, bool is_dir)
{
    mode_t um = the_umask();
    const char *p = spec;

    for (;;) {
        mode_t who = 0;
        bool   who_given = false;
        for (; *p; p++) {
            if      (*p == 'u') who |= 04700;
            else if (*p == 'g') who |= 02070;
            else if (*p == 'o') who |= 01007;
            else if (*p == 'a') who |= 07777;
            else break;
            who_given = true;
        }
        if (!who_given) who = 07777;

        if (*p != '+' && *p != '-' && *p != '=')
            return false;

        /* A clause can carry several operators: u+r-w is legal. */
        while (*p == '+' || *p == '-' || *p == '=') {
            char op = *p++;
            mode_t bits = 0;

            for (; *p; p++) {
                if (*p == 'r') bits |= 0444;
                else if (*p == 'w') bits |= 0222;
                else if (*p == 'x') bits |= 0111;
                else if (*p == 'X') { if (is_dir || (*m & 0111)) bits |= 0111; }
                else if (*p == 's') bits |= 06000;
                else if (*p == 't') bits |= 01000;
                else if (*p == 'u' || *p == 'g' || *p == 'o') {
                    /* u=g: take the bits one class already has and
                     * spread them across every class this clause names. */
                    mode_t src = *p == 'u' ? (*m & 0700) >> 6
                               : *p == 'g' ? (*m & 0070) >> 3
                                           : (*m & 0007);
                    bits |= src | (src << 3) | (src << 6);
                }
                else break;
            }

            /* No "who" means the umask decides who is left out. */
            mode_t mask = who;
            if (!who_given) mask &= ~um;

            if (op == '+')      *m |= bits & mask;
            else if (op == '-') *m &= ~(bits & mask);
            else {
                /* "=" clears the classes it names before setting them,
                 * and the umask does not come into it. */
                *m &= ~(who & 07777);
                *m |= bits & who;
            }
        }

        if (*p == '\0') return true;
        if (*p != ',')  return false;
        p++;
    }
}

static bool parse_octal(const char *s, mode_t *out)
{
    if (!*s) return false;
    mode_t v = 0;
    int digits = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '7') return false;
        v = (mode_t)(v * 8 + (mode_t)(*p - '0'));
        if (++digits > 5) return false;
    }
    *out = v;
    return true;
}

static void do_path(const char *path);

static void descend(const char *path)
{
    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        if (!opt_quiet)
            lp_diag("chmod", "cannot read directory", NULL, "cannot open",
                    path, (int)-fd);
        failures = 1;
        return;
    }
    char dbuf[8192];
    for (;;) {
        long n = sys_getdents((int)fd, dbuf, sizeof dbuf);
        if (n <= 0) break;
        for (long off = 0; off < n; ) {
            char       *rec  = dbuf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            const char *name = rec + DIRENT_NAME;
            off += len;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
            char child[512];
            size_t l = strlcpy(child, path, sizeof child);
            if (l && child[l - 1] != '/' && l + 1 < sizeof child) {
                child[l++] = '/';
                child[l] = '\0';
            }
            if (strlcat(child, name, sizeof child) >= sizeof child) {
                failures = 1;
                continue;
            }
            do_path(child);
        }
    }
    lp_close((int)fd);
}

static void do_path(const char *path)
{
    lp_stat_t st;
    long sr = lp_stat(path, &st, true);
    if (sr < 0) {
        if (!opt_quiet)
            lp_diag("chmod", "cannot access", NULL, "cannot read", path, (int)-sr);
        failures = 1;
        return;
    }

    mode_t old = (mode_t)(st.mode & 07777);
    mode_t want = old;
    if (numeric)
        want = octal;
    else if (!apply_symbolic(symbolic, &want,
                             (st.mode & LP_S_IFMT) == LP_S_IFDIR)) {
        /* Already checked once before any file was touched. */
        return;
    }

    if (want != old) {
        long r = lp_chmod(path, want);
        if (r < 0) {
            if (!opt_quiet)
                lp_diag("chmod", "changing permissions of", NULL,
                        "cannot change", path, (int)-r);
            failures = 1;
            return;
        }
    }
    report(path, old, want);

    if (opt_recursive && (st.mode & LP_S_IFMT) == LP_S_IFDIR)
        descend(path);
}

static void usage(int fd)
{
    dprintf(fd, "Usage: chmod [OPTION]... MODE[,MODE]... FILE...\n"
                "  or:  chmod [OPTION]... OCTAL-MODE FILE...\n"
                "  or:  chmod [OPTION]... --reference=RFILE FILE...\n"
                "Change the mode of each FILE to MODE.\n"
                "With --reference, change the mode of each FILE to that of RFILE.\n\n"
                "  -c, --changes          like verbose but report only when a change is made\n"
                "  -f, --silent, --quiet  suppress most error messages\n"
                "  -v, --verbose          output a diagnostic for every file processed\n"
                "  -R, --recursive        change files and directories recursively\n"
                "      --reference=RFILE  use RFILE's mode instead of MODE values\n"
                "      --help     display this help and exit\n\n"
                "Each MODE is of the form '[ugoa]*([-+=]([rwxXst]*|[ugo]))+|[-+=][0-7]+'.\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "changes", 0, 'c' }, { "silent", 0, 'f' }, { "quiet", 0, 'f' },
        { "verbose", 0, 'v' }, { "recursive", 0, 'R' },
        { "reference", 1, 'F' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    const char *reference = NULL;
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "cfvR", lo);
    for (int c; (c = lp_getopt(&g)) != -1; )
        switch (c) {
        case 'c': opt_changes = true; break;
        case 'f': opt_quiet = true; break;
        case 'v': opt_verbose = true; break;
        case 'R': opt_recursive = true; break;
        case 'F': reference = g.arg; break;
        case 'H': usage(STDOUT_FILENO); return 0;
        default:  lp_getopt_err("chmod", &g); return 1;
        }

    int i = g.ind;
    if (!reference) {
        if (i >= argc) {
            dprintf(STDERR_FILENO, "chmod: missing operand\n"
                                   "Try 'chmod --help' for more information.\n");
            return 1;
        }
        const char *spec = argv[i++];
        if (parse_octal(spec, &octal)) {
            numeric = true;
        } else {
            /* Check the text once, on a mode of 0, so a typo is caught
             * before any file has been changed. */
            mode_t probe = 0;
            if (!apply_symbolic(spec, &probe, false)) {
                dprintf(STDERR_FILENO, "chmod: invalid mode: '%s'\n"
                        "Try 'chmod --help' for more information.\n", spec);
                return 1;
            }
            symbolic = spec;
        }
        if (i >= argc) {
            dprintf(STDERR_FILENO, "chmod: missing operand after '%s'\n"
                                   "Try 'chmod --help' for more information.\n", spec);
            return 1;
        }
    } else {
        lp_stat_t rs;
        long rr = lp_stat(reference, &rs, true);
        if (rr < 0) {
            lp_diag("chmod", "cannot stat", NULL, "cannot read", reference, (int)-rr);
            return 1;
        }
        numeric = true;
        octal = (mode_t)(rs.mode & 07777);
        if (i >= argc) {
            dprintf(STDERR_FILENO, "chmod: missing operand\n"
                                   "Try 'chmod --help' for more information.\n");
            return 1;
        }
    }

    for (; i < argc; i++)
        do_path(argv[i]);
    return failures;
}
