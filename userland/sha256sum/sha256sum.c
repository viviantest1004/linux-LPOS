/* sha256sum, and the same program under three other names.
 *
 *   sha256sum FILE...            print "<hash>  FILE" for each
 *   sha256sum -c SUMS            check a list somebody else printed
 *   md5sum / sha1sum / sha512sum are the same file, chosen by argv[0]
 *
 * The old version had a -c that meant something else - "here is the
 * expected hash, here is the file" - which read well and was wrong.
 * Every SHA256SUMS on the internet is checked with `sha256sum -c`, and a
 * -c that does not do that is a trap for anyone who learned the real
 * one. So this is GNU's: -c takes a file of "<hash>  <name>" lines,
 * prints "name: OK" or "name: FAILED", and exits nonzero if any failed.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static int  algo    = LP_SHA256;
static const char *algo_name = "SHA256";
static int  hexlen  = 64;

static bool opt_check = false, opt_binary = false, opt_tag = false;
static bool opt_quiet = false, opt_status = false, opt_strict = false;
static bool opt_warn = false, opt_ignore_missing = false;
static char eol = '\n';
static const char *prog = "sha256sum";

static void pick_algo(const char *base)
{
    if (strcmp(base, "md5sum") == 0)         { algo = LP_MD5;    algo_name = "MD5"; }
    else if (strcmp(base, "sha1sum") == 0)   { algo = LP_SHA1;   algo_name = "SHA1"; }
    else if (strcmp(base, "sha512sum") == 0) { algo = LP_SHA512; algo_name = "SHA512"; }
    else                                     { algo = LP_SHA256; algo_name = "SHA256"; }
    hexlen = lp_digest_bits(algo) / 4;
}

static bool hash_of(const char *name, char *hex)
{
    if (strcmp(name, "-") == 0)
        return lp_digest_fd(STDIN_FILENO, algo, hex);
    return lp_digest_file(name, algo, hex);
}

static void print_line(const char *hex, const char *name)
{
    if (opt_tag)
        printf("%s (%s) = %s%c", algo_name, name, hex, eol);
    else
        printf("%s %c%s%c", hex, opt_binary ? '*' : ' ', name, eol);
}

/* One line of a checksum file, in either shape GNU writes:
 *     <hex>  name        (or "<hex> *name" for binary)
 *     ALGO (name) = <hex>
 * Returns false for a line that is neither. */
static bool parse_line(char *line, char **name, char **hex)
{
    char *paren = strstr(line, " (");
    char *eq    = strstr(line, ") = ");
    if (paren && eq && paren < eq) {
        *paren = '\0';
        *name  = paren + 2;
        *eq    = '\0';
        *hex   = eq + 4;
        return true;
    }
    int n = 0;
    while (line[n] && line[n] != ' ' && line[n] != '\t') n++;
    if (n != hexlen || !line[n])
        return false;
    line[n] = '\0';
    *hex = line;
    char *p = line + n + 1;
    if (*p == ' ' || *p == '*' || *p == '?' || *p == '^') p++;
    *name = p;
    return **name != '\0';
}

static int check_file(const char *listname)
{
    int fd = STDIN_FILENO;
    if (strcmp(listname, "-") != 0) {
        long f = lp_open(listname, O_RDONLY, 0);
        if (f < 0) {
            lp_diag(prog, NULL, NULL, "cannot open", listname, (int)-f);
            return 1;
        }
        fd = (int)f;
    }

    long bad = 0, unreadable = 0, malformed = 0, checked = 0, lineno = 0;
    char line[4096];

    for (;;) {
        long n = readline(fd, line, sizeof line);
        if (n < 0) break;
        lineno++;
        if (line[0] == '\0' || line[0] == '#') continue;

        char *name, *want;
        if (!parse_line(line, &name, &want)) {
            malformed++;
            if (opt_warn && !opt_status)
                dprintf(STDERR_FILENO,
                        "%s: %s: %ld: improperly formatted %s checksum line\n",
                        prog, listname, lineno, algo_name);
            continue;
        }

        char got[160];
        if (!hash_of(name, got)) {
            if (opt_ignore_missing) continue;
            unreadable++;
            if (!opt_status) {
                dprintf(STDERR_FILENO, "%s: %s: No such file or directory\n", prog, name);
                printf("%s: FAILED open or read\n", name);
            }
            continue;
        }
        checked++;
        if (strcmp(got, want) == 0) {
            if (!opt_quiet && !opt_status)
                printf("%s: OK\n", name);
        } else {
            bad++;
            if (!opt_status)
                printf("%s: FAILED\n", name);
        }
    }
    if (fd != STDIN_FILENO)
        lp_close(fd);

    /* A file with nothing usable in it gets one message, not a warning
     * for every line it could not read. */
    if (checked == 0 && unreadable == 0) {
        dprintf(STDERR_FILENO, "%s: %s: %s\n", prog, listname,
                opt_ignore_missing && malformed == 0
                    ? "no file was verified"
                    : "no properly formatted checksum lines found");
        return 1;
    }

    if (!opt_status) {
        if (malformed)
            dprintf(STDERR_FILENO, "%s: WARNING: %ld line%s improperly formatted\n",
                    prog, malformed, malformed == 1 ? " is" : "s are");
        if (unreadable)
            dprintf(STDERR_FILENO, "%s: WARNING: %ld listed file%s could not be read\n",
                    prog, unreadable, unreadable == 1 ? "" : "s");
        if (bad)
            dprintf(STDERR_FILENO, "%s: WARNING: %ld computed checksum%s did NOT match\n",
                    prog, bad, bad == 1 ? "" : "s");
    }
    if (bad || unreadable || (opt_strict && malformed))
        return 1;
    return 0;
}

static void usage(int fd, const char *base)
{
    dprintf(fd, "Usage: %s [OPTION]... [FILE]...\n"
                "Print or check %s (%d-bit) checksums.\n\n"
                "With no FILE, or when FILE is -, read standard input.\n\n"
                "  -b, --binary          read in binary mode\n"
                "  -c, --check           read checksums from the FILEs and check them\n"
                "      --tag             create a BSD-style checksum\n"
                "  -t, --text            read in text mode (default)\n"
                "  -z, --zero            end each output line with NUL, not newline\n\n"
                "The following five options are useful only when verifying checksums:\n"
                "      --ignore-missing  don't fail or report status for missing files\n"
                "      --quiet           don't print OK for each successfully verified file\n"
                "      --status          don't output anything, status code shows success\n"
                "      --strict          exit non-zero for improperly formatted checksum lines\n"
                "  -w, --warn            warn about improperly formatted checksum lines\n\n"
                "      --help     display this help and exit\n",
                base, algo_name, lp_digest_bits(algo));
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "binary", 0, 'b' }, { "check", 0, 'c' }, { "tag", 0, 'T' },
        { "text", 0, 't' }, { "zero", 0, 'z' },
        { "ignore-missing", 0, 'I' }, { "quiet", 0, 'Q' },
        { "status", 0, 'S' }, { "strict", 0, 'R' }, { "warn", 0, 'w' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    prog = base;
    pick_algo(base);

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "bctzw", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'b': opt_binary = true; break;
        case 't': opt_binary = false; break;
        case 'c': opt_check = true; break;
        case 'T': opt_tag = true; break;
        case 'z': eol = '\0'; break;
        case 'I': opt_ignore_missing = true; break;
        case 'Q': opt_quiet = true; break;
        case 'S': opt_status = true; break;
        case 'R': opt_strict = true; break;
        case 'w': opt_warn = true; break;
        case 'H': usage(STDOUT_FILENO, base); return 0;
        default: lp_getopt_err(base, &g); return 1;
        }
    }

    if (opt_check) {
        int rc = 0;
        if (g.ind == argc)
            return check_file("-");
        for (int i = g.ind; i < argc; i++)
            if (check_file(argv[i]) != 0) rc = 1;
        return rc;
    }

    char hex[160];
    if (g.ind == argc) {
        if (!lp_digest_fd(STDIN_FILENO, algo, hex))
            return 1;
        print_line(hex, "-");
        return 0;
    }

    int rc = 0;
    for (int i = g.ind; i < argc; i++) {
        if (!hash_of(argv[i], hex)) {
            long f = lp_open(argv[i], O_RDONLY, 0);
            int err = (f < 0) ? (int)-f : 5;
            if (f >= 0) lp_close((int)f);
            lp_diag(base, NULL, NULL, "cannot read", argv[i], err);
            rc = 1;
            continue;
        }
        print_line(hex, argv[i]);
    }
    return rc;
}
