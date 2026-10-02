/* grep - print the lines that match.
 *
 *   grep [-invclqrEF] [-A n] [-B n] [-C n] <pattern> [file|dir]...
 *
 *   -i  ignore case          -v  the lines that do NOT match
 *   -n  line numbers         -c  how many, not which
 *   -l  just the filenames   -q  say nothing, set the status
 *   -r  walk directories     -E  extended regular expressions
 *   -F  a plain string, no pattern at all
 *   -A n -B n -C n           lines of context after, before, both
 *
 * The pattern is a POSIX basic regular expression, the same engine sed
 * and awk use - libc/include/regex.h. -E switches it to extended, where
 * ( ) | + ? { } mean what they mean without a backslash.
 *
 * This used to have a matcher of its own, written when grep was the
 * only thing here that needed one. It treated + and ? as operators in a
 * basic expression, which POSIX grep does not: `grep "a+b"` looked for
 * one-or-more a's rather than for the three characters a, +, b, and
 * anybody who had used grep before got the wrong answer with no hint
 * why. It also had no groups and no alternation, so `grep "cat\|dog"`
 * was not expressible at all. One engine for the three commands means
 * one answer to "what does this pattern mean", and it is the answer
 * every other system gives.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "regex.h"

static bool fold;                     /* -i */
static bool extended;                 /* -E */
static bool fixed;                    /* -F */
static bool word_only;                /* -w */
static bool line_only;                /* -x */
static bool  gave_up_somewhere;       /* the engine ran out of steps */
static const char *prog = "grep";

/* -e can be given more than once and -f reads a file of them, so there
 * is a list rather than one pattern. A line matches if any of them does,
 * which is what makes `grep -e cat -e dog` mean "either". */
#define MAX_PATS 512
typedef struct {
    char  *text;
    lpre  *program;      /* NULL when -F, or when the pattern is empty */
    bool   empty;
} pat_t;

static pat_t pats[MAX_PATS];
static int   npats;

static bool isword(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static bool fold_eq(const char *hay, const char *needle, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char a = hay[i], b = needle[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

/* -w and -x are about where a match sits, not what it is, so they are
 * checked against the match bounds whichever matcher found them. */
static bool bounds_ok(const char *line, int b, int e, int len)
{
    if (line_only)
        return b == 0 && e == len;
    if (word_only) {
        if (b > 0 && isword((unsigned char)line[b - 1])) return false;
        if (e < len && isword((unsigned char)line[e]))   return false;
    }
    return true;
}

/* Does pattern `p` match `line` anywhere the bounds allow?  When `where`
 * is given it is filled with the first such match, for -o. */
static bool pat_match(const pat_t *p, const char *line, int from, int *where)
{
    int len = (int)strlen(line);

    if (p->empty) {
        if (line_only && len != 0) return false;
        if (word_only && len != 0) return false;
        if (from > len) return false;
        if (where) { where[0] = from; where[1] = from; }
        return true;
    }

    if (!p->program) {                       /* -F: a plain string */
        size_t n = strlen(p->text);
        for (int i = from; line[i]; i++) {
            if (fold ? fold_eq(line + i, p->text, n)
                     : strncmp(line + i, p->text, n) == 0) {
                if (!bounds_ok(line, i, i + (int)n, len)) continue;
                if (where) { where[0] = i; where[1] = i + (int)n; }
                return true;
            }
        }
        return false;
    }

    int caps[RE_MAX_CAPS];
    int at = from;
    while (at <= len && re_search(p->program, line, at, at > 0, caps)) {
        if (bounds_ok(line, caps[0], caps[1], len)) {
            if (where) { where[0] = caps[0]; where[1] = caps[1]; }
            return true;
        }
        at = (caps[0] < caps[1]) ? caps[0] + 1 : caps[0] + 1;
        if (at > len) break;
    }
    if (re_gave_up(p->program))
        gave_up_somewhere = true;
    return false;
}

static bool match_line(const char *line)
{
    for (int i = 0; i < npats; i++)
        if (pat_match(&pats[i], line, 0, NULL))
            return true;
    return false;
}

typedef struct {
    bool invert, numbers, count_only, names_only, quiet;
    bool only;          /* -o: print the matched part, not the line */
    bool without;       /* -L: name the files that did NOT match */
    bool byte_offset;   /* -b */
    long max_count;     /* -m: stop after this many matches per file */
    int  before;        /* -B: lines of context before a match */
    int  after;         /* -A: lines after */
} opts_t;

/* Returns the number of matching lines. */
/* ── Context lines: -A, -B, -C ──
 *
 * A match on its own is often not the answer - the answer is the line
 * after it, or the three before. Without these, reading a log meant
 * grepping for a line number and then paging to it by hand, and every
 * script that wanted context had to be written some other way.
 *
 * "Before" needs the lines kept until we know whether they matter, so
 * there is a small ring of them. "After" is only a countdown. */
#define CTX_MAX 32

static void emit(const char *name, bool show_name, long lineno,
                 const opts_t *o, const char *text, char sep)
{
    if (show_name)
        printf("%s%c", name, sep);
    if (o->numbers)
        printf("%ld%c", lineno, sep);
    printf("%s\n", text);
}

/* -o: every match on the line, each on its own line.
 *
 * Scripts want this far more often than they want the whole line -
 * pulling one field out of a line of JSON is the usual case, and
 * without it the only way is sed with a pattern written twice. */
static void emit_matches(const char *name, bool show_name, long lineno,
                         const opts_t *o, const char *line)
{
    int len = (int)strlen(line);
    int from = 0;

    while (from <= len) {
        /* The leftmost match among all the patterns, so -e a -e b prints
         * them in the order they appear on the line, not by pattern. */
        int best_b = -1, best_e = -1;
        for (int i = 0; i < npats; i++) {
            int w[2];
            if (!pat_match(&pats[i], line, from, w)) continue;
            if (best_b < 0 || w[0] < best_b || (w[0] == best_b && w[1] > best_e)) {
                best_b = w[0]; best_e = w[1];
            }
        }
        if (best_b < 0) return;
        if (best_e > best_b) {
            char save[8192];
            size_t k = (size_t)(best_e - best_b);
            if (k > sizeof save - 1) k = sizeof save - 1;
            memcpy(save, line + best_b, k);
            save[k] = '\0';
            emit(name, show_name, lineno, o, save, ':');
            from = best_e;
        } else {
            from = best_b + 1;      /* an empty match would spin here */
        }
    }
}

static long grep_fd(int fd, const opts_t *o,
                    const char *name, bool show_name)
{
    char line[8192];
    long lineno = 0, hits = 0;

    /* The ring of lines waiting to be shown if a match turns up. */
    static char ring[CTX_MAX][8192];
    static long ring_no[CTX_MAX];
    int  ring_n = 0, ring_head = 0;
    int  before = o->before > CTX_MAX ? CTX_MAX : o->before;

    int  after_left = 0;        /* lines still owed after a match */
    long last_shown = 0;        /* to place --  separators */
    bool any_output = false;

    while (readrec(fd, line, sizeof(line), '\n', NULL) >= 0) {
        lineno++;
        bool hit = match_line(line);
        if (o->invert)
            hit = !hit;

        if (!hit) {
            if (after_left > 0 && !o->quiet && !o->count_only &&
                !o->names_only) {
                emit(name, show_name, lineno, o, line, '-');
                last_shown = lineno;
                after_left--;
            } else if (before > 0) {
                strlcpy(ring[ring_head], line, sizeof ring[0]);
                ring_no[ring_head] = lineno;
                ring_head = (ring_head + 1) % before;
                if (ring_n < before)
                    ring_n++;
            }
            continue;
        }

        hits++;
        if (o->quiet)
            break;                    /* the exit code is all that is wanted */
        if (o->count_only) {
            if (o->max_count && hits >= o->max_count) break;
            continue;
        }
        if (o->without)
            break;                    /* one match is enough to disqualify it */
        if (o->names_only) {
            printf("%s\n", name);
            break;                    /* one mention per file is enough */
        }

        /* A gap between groups gets a separator, the way grep does it.
         * Without it two runs of context read as one continuous quote
         * from the file, which is exactly the wrong impression. */
        if ((o->before || o->after) && any_output &&
            last_shown && lineno > last_shown + 1)
            printf("--\n");

        for (int i = 0; i < ring_n; i++) {
            int idx = (ring_head + before - ring_n + i) % before;
            if (ring_no[idx] > last_shown)
                emit(name, show_name, ring_no[idx], o, ring[idx], '-');
        }
        ring_n = 0;

        if (o->only)
            emit_matches(name, show_name, lineno, o, line);
        else
            emit(name, show_name, lineno, o, line, ':');
        last_shown = lineno;
        any_output = true;
        after_left = o->after;
        if (o->max_count && hits >= o->max_count) break;
    }
    return hits;
}

/* dirent, as getdents64 lays it out */
#define DIRENT_RECLEN 16
#define DIRENT_TYPE   18
#define DIRENT_NAME   19
#define DT_DIR         4
#define DT_REG         8

static bool recursive = false;
static long recurse_total = 0;
static bool any_match = false;
static bool grep_quiet_errors = false;   /* -s */
static bool grep_had_error = false;

/* Walk a directory and grep every ordinary file under it.
 *
 * Depth is capped rather than unbounded: a symlink pointing at its own
 * parent makes an infinite tree, and a grep that never returns on a
 * machine reached only over SSH is worse than one that stops early and
 * says so. */
static void grep_tree(const char *path, opts_t *o, int depth)
{
    if (depth > 24) {
        dprintf(STDERR_FILENO,
                "grep: %s is more than 24 directories deep - stopping.\n"
                "grep:   A symlink pointing at its own parent looks like\n"
                "grep:   this.\n", path);
        return;
    }

    long fd = lp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        /* Not a directory: it is a file to search. */
        long f = lp_open(path, O_RDONLY, 0);
        if (f < 0) {
            if (!grep_quiet_errors)
                lp_diag(prog, NULL, NULL, "cannot open", path, (int)-f);
            grep_had_error = true;
            return;
        }
        long n = grep_fd((int)f, o, path, !o->names_only && !o->without);
        lp_close((int)f);
        recurse_total += n;
        if (n > 0) any_match = true;
        if (o->count_only) printf("%s:%ld\n", path, n);
        if (o->without && n == 0) printf("%s\n", path);
        return;
    }

    /* The names are collected before recursing: getdents' buffer is
     * reused by the directory below. */
    static char names[512][256];
    static u8   types[512];
    int n = 0;
    char buf[4096];

    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0) break;
        for (long off = 0; off < got && n < 512; ) {
            char *rec  = buf + off;
            u16   len  = *(u16 *)(rec + DIRENT_RECLEN);
            u8    type = *(u8 *)(rec + DIRENT_TYPE);
            char *name = rec + DIRENT_NAME;
            if (len == 0) break;
            off += len;
            if (name[0] == '.') continue;       /* and . and .. with it */
            strlcpy(names[n], name, 256);
            types[n] = type;
            n++;
        }
        if (n >= 512) break;
    }
    lp_close((int)fd);

    for (int i = 0; i < n; i++) {
        char child[768];
        snprintf(child, sizeof child, "%s/%s", path, names[i]);
        if (types[i] == DT_DIR)
            grep_tree(child, o, depth + 1);
        else if (types[i] == DT_REG || types[i] == 0)
            grep_tree(child, o, depth + 1);
    }
}

/* Compile one pattern into the list. */
static bool add_pattern(const char *text)
{
    if (npats == MAX_PATS) {
        dprintf(STDERR_FILENO, "%s: more than %d patterns\n", prog, MAX_PATS);
        return false;
    }
    pat_t *p = &pats[npats];
    p->text  = strdup(text);
    p->empty = (text[0] == '\0');
    p->program = NULL;
    if (!p->text) return false;

    if (fixed || p->empty) { npats++; return true; }

    /* -x is a question about the whole line, so it is asked by anchoring
     * the pattern rather than by measuring the match afterwards - the
     * engine finds the leftmost match, which need not be the long one. */
    char wrapped[8192];
    const char *use = p->text;
    if (line_only) {
        snprintf(wrapped, sizeof wrapped, extended ? "^(%s)$" : "^\\(%s\\)$", p->text);
        use = wrapped;
    }

    const char *err = NULL;
    p->program = re_compile(use, extended, fold, &err);
    if (!p->program) {
        dprintf(STDERR_FILENO, "%s: %s\n", prog,
                err ? err : "that pattern makes no sense");
        /* The commonest reason, and it is not obvious from the message
         * alone if you are used to another grep. */
        if (!extended && (strchr(p->text, '(') || strchr(p->text, '|')))
            dprintf(STDERR_FILENO,
                    "%s:   ( and | are ordinary characters without -E."
                    " Try grep -E.\n", prog);
        return false;
    }
    npats++;
    return true;
}

/* -e "a\nb" is two patterns, and so is every line of a -f file. */
static bool add_pattern_lines(const char *text)
{
    char one[8192];
    size_t k = 0;
    for (const char *p = text; ; p++) {
        if (*p == '\n' || *p == '\0') {
            one[k] = '\0';
            if (!add_pattern(one)) return false;
            k = 0;
            if (*p == '\0') return true;
            continue;
        }
        if (k < sizeof one - 1) one[k++] = *p;
    }
}

static bool add_pattern_file(const char *path)
{
    int fd = STDIN_FILENO;
    if (strcmp(path, "-") != 0) {
        long f = lp_open(path, O_RDONLY, 0);
        if (f < 0) {
            lp_diag(prog, NULL, NULL, "cannot open", path, (int)-f);
            return false;
        }
        fd = (int)f;
    }
    char line[8192];
    while (readrec(fd, line, sizeof line, '\n', NULL) >= 0)
        if (!add_pattern(line)) { if (fd != STDIN_FILENO) lp_close(fd); return false; }
    if (fd != STDIN_FILENO) lp_close(fd);
    return true;
}

static void usage(int fd)
{
    dprintf(fd, "Usage: grep [OPTION]... PATTERNS [FILE]...\n"
                "Search for PATTERNS in each FILE.\n\n"
                "  -E, --extended-regexp     PATTERNS are extended regular expressions\n"
                "  -F, --fixed-strings       PATTERNS are strings\n"
                "  -e, --regexp=PATTERNS     use PATTERNS for matching\n"
                "  -f, --file=FILE           take PATTERNS from FILE\n"
                "  -i, --ignore-case         ignore case distinctions\n"
                "  -w, --word-regexp         match only whole words\n"
                "  -x, --line-regexp         match only whole lines\n"
                "  -v, --invert-match        select non-matching lines\n"
                "  -c, --count               print only a count of selected lines\n"
                "  -l, --files-with-matches  print only names of FILEs with selected lines\n"
                "  -L, --files-without-match print only names of FILEs with no selected lines\n"
                "  -o, --only-matching       show only nonempty parts of lines that match\n"
                "  -q, --quiet, --silent     suppress all normal output\n"
                "  -s, --no-messages         suppress error messages about missing files\n"
                "  -n, --line-number         print line number with output lines\n"
                "  -H, --with-filename       print file name with output lines\n"
                "  -h, --no-filename         suppress the file name prefix on output\n"
                "  -m, --max-count=NUM       stop after NUM selected lines\n"
                "  -r, -R, --recursive       search directories recursively\n"
                "  -A, --after-context=NUM   print NUM lines of trailing context\n"
                "  -B, --before-context=NUM  print NUM lines of leading context\n"
                "  -C, --context=NUM         print NUM lines of output context\n"
                "      --help     display this help and exit\n\n"
                "Exit status is 0 if any line is selected, 1 otherwise;\n"
                "if any error occurs the exit status is 2.\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "extended-regexp", 0, 'E' }, { "fixed-strings", 0, 'F' },
        { "basic-regexp", 0, 'G' }, { "regexp", 1, 'e' }, { "file", 1, 'f' },
        { "ignore-case", 0, 'i' }, { "word-regexp", 0, 'w' },
        { "line-regexp", 0, 'x' }, { "invert-match", 0, 'v' },
        { "count", 0, 'c' }, { "files-with-matches", 0, 'l' },
        { "files-without-match", 0, 'L' }, { "only-matching", 0, 'o' },
        { "quiet", 0, 'q' }, { "silent", 0, 'q' }, { "no-messages", 0, 's' },
        { "line-number", 0, 'n' }, { "with-filename", 0, 'H' },
        { "no-filename", 0, 'h' }, { "max-count", 1, 'm' },
        { "recursive", 0, 'r' }, { "dereference-recursive", 0, 'R' },
        { "after-context", 1, 'A' }, { "before-context", 1, 'B' },
        { "context", 1, 'C' }, { "byte-offset", 0, 'b' },
        { "text", 0, 'a' }, { "binary-files", 1, 1001 },
        { "color", 2, 1002 }, { "colour", 2, 1002 },
        { "null-data", 0, 'z' }, { "help", 0, 1003 }, { 0, 0, 0 }
    };
    opts_t o;
    memset(&o, 0, sizeof o);
    bool no_messages = false;
    int  show_names  = -1;                 /* -1 auto, 0 never, 1 always */
    const char *epat[MAX_PATS];
    int  nepat = 0;
    const char *fpat[16];
    int  nfpat = 0;

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "EFGe:f:iwxvclLoqsnHhm:rRA:B:C:bazUZ", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'E': extended = true; break;
        case 'F': fixed = true; break;
        case 'G': extended = false; break;
        case 'e': if (nepat < MAX_PATS) epat[nepat++] = g.arg; break;
        case 'f': if (nfpat < 16) fpat[nfpat++] = g.arg; break;
        case 'i': fold = true; break;
        case 'w': word_only = true; break;
        case 'x': line_only = true; break;
        case 'v': o.invert = true; break;
        case 'c': o.count_only = true; break;
        case 'l': o.names_only = true; break;
        case 'L': o.without = true; break;
        case 'o': o.only = true; break;
        case 'q': o.quiet = true; break;
        case 's': no_messages = true; break;
        case 'n': o.numbers = true; break;
        case 'H': show_names = 1; break;
        case 'h': show_names = 0; break;
        case 'b': o.byte_offset = true; break;
        case 'm': o.max_count = strtol(g.arg, NULL, 10); break;
        case 'r': case 'R': recursive = true; break;
        case 'A': o.after  = (int)strtol(g.arg, NULL, 10); break;
        case 'B': o.before = (int)strtol(g.arg, NULL, 10); break;
        case 'C': o.after = o.before = (int)strtol(g.arg, NULL, 10); break;
        case 'a': case 'U': case 'z': case 'Z': case 1001: case 1002: break;
        case 1003: usage(STDOUT_FILENO); return 0;
        default:
            /* grep puts its one-line usage between the complaint and the
             * hint, which nothing else does. */
            if (g.missing && g.badlong) {
                char name[128];
                strlcpy(name, g.badlong, sizeof name);
                char *eq = strchr(name, '=');
                if (eq) *eq = '\0';
                dprintf(STDERR_FILENO, "%s: option '%s' requires an argument\n",
                        prog, name);
            } else if (g.missing) {
                dprintf(STDERR_FILENO, "%s: option requires an argument -- '%c'\n",
                        prog, g.badchar);
            } else if (g.badlong) {
                dprintf(STDERR_FILENO, "%s: unrecognized option '%s'\n",
                        prog, g.badlong);
            } else {
                dprintf(STDERR_FILENO, "%s: invalid option -- '%c'\n",
                        prog, g.badchar);
            }
            dprintf(STDERR_FILENO, "Usage: %s [OPTION]... PATTERNS [FILE]...\n", prog);
            dprintf(STDERR_FILENO, "Try '%s --help' for more information.\n", prog);
            return 2;
        }
    }
    grep_quiet_errors = no_messages;

    /* Patterns come from -e, from -f, or - if neither was given - from
     * the first operand. */
    for (int i = 0; i < nepat; i++)
        if (!add_pattern_lines(epat[i])) return 2;
    for (int i = 0; i < nfpat; i++)
        if (!add_pattern_file(fpat[i])) return 2;

    int first_file = g.ind;
    if (npats == 0) {
        if (g.ind >= argc) {
            /* No pattern at all: the one-line usage, not the whole help.
             * grep prints the long help only when it was asked for. */
            dprintf(STDERR_FILENO,
                    "Usage: %s [OPTION]... PATTERNS [FILE]...\n", prog);
            dprintf(STDERR_FILENO,
                    "Try '%s --help' for more information.\n", prog);
            return 2;
        }
        if (!add_pattern_lines(argv[g.ind])) return 2;
        first_file = g.ind + 1;
    }

    int files = argc - first_file;
    bool show = (show_names == 1) || (show_names == -1 && (files > 1 || recursive));

    if (recursive) {
        bool searched = false;
        for (int i = first_file; i < argc; i++) {
            grep_tree(argv[i], &o, 0);
            searched = true;
        }
        if (!searched)
            grep_tree(".", &o, 0);
        (void)recurse_total;
        if (gave_up_somewhere)
            dprintf(STDERR_FILENO,
                    "%s: a pattern took too long on some line and was given up on\n", prog);
        if (grep_had_error && !o.quiet) return 2;
        return any_match ? 0 : 1;
    }

    if (files == 0) {
        long n = grep_fd(STDIN_FILENO, &o, "(standard input)", show_names == 1);
        if (o.count_only) {
            if (show_names == 1) printf("(standard input):%ld\n", n);
            else                 printf("%ld\n", n);
        }
        if (o.names_only && n > 0)  printf("(standard input)\n");
        if (o.without   && n == 0)  printf("(standard input)\n");
        if (gave_up_somewhere)
            dprintf(STDERR_FILENO,
                    "%s: a pattern took too long on some line and was given up on\n", prog);
        return n > 0 ? 0 : 1;
    }

    bool matched = false;
    bool had_error = false;

    for (int i = first_file; i < argc; i++) {
        int fd;
        if (strcmp(argv[i], "-") == 0) {
            fd = STDIN_FILENO;
        } else {
            long f = lp_open(argv[i], O_RDONLY, 0);
            if (f < 0) {
                if (!no_messages)
                    lp_diag(prog, NULL, NULL, "cannot open", argv[i], (int)-f);
                had_error = true;
                continue;
            }
            fd = (int)f;
        }
        const char *name = (fd == STDIN_FILENO) ? "(standard input)" : argv[i];
        long n = grep_fd(fd, &o, name, show && !o.names_only && !o.without);
        if (fd != STDIN_FILENO) lp_close(fd);

        if (o.count_only) {
            if (show) printf("%s:%ld\n", name, n);
            else      printf("%ld\n", n);
        }
        if (o.without && n == 0) printf("%s\n", name);
        if (n > 0) matched = true;
        if (o.quiet && matched) return 0;
    }

    if (gave_up_somewhere)
        dprintf(STDERR_FILENO,
                "%s: a pattern took too long on some line and was given up on\n", prog);
    if (had_error && !o.quiet) return 2;
    return matched ? 0 : 1;
}
