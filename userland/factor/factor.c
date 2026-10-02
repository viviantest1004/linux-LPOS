/* factor - the prime factors of a number.
 *
 *   factor 360     ->  360: 2 2 2 3 3 5
 *
 * Trial division up to the square root, which is instant for anything
 * that fits in 64 bits and small enough to say out loud.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static void factor(u64 v)
{
    printf("%llu:", (unsigned long long)v);
    if (v < 2) { printf("\n"); return; }
    while (v % 2 == 0) { printf(" 2"); v /= 2; }
    for (u64 d = 3; d * d <= v; d += 2)
        while (v % d == 0) { printf(" %llu", (unsigned long long)d); v /= d; }
    if (v > 1) printf(" %llu", (unsigned long long)v);
    printf("\n");
}

static bool parse(const char *s, u64 *out)
{
    if (!*s) return false;
    u64 v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (u64)(*p - '0');
    }
    *out = v;
    return true;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: factor [NUMBER]...\n"
               "Print the prime factors of each specified integer NUMBER.\n"
               "If none are specified on the command line, read them from standard input.\n\n"
               "      --help     display this help and exit\n");
        return 0;
    }

    int rc = 0;
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            u64 v;
            if (!parse(argv[i], &v)) {
                dprintf(STDERR_FILENO,
                        "factor: '%s' is not a valid positive integer\n", argv[i]);
                rc = 1;
                continue;
            }
            factor(v);
        }
        return rc;
    }

    char line[128];
    while (readrec(STDIN_FILENO, line, sizeof line, '\n', NULL) >= 0) {
        char *p = line;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            char *s = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            char save = *p;
            *p = '\0';
            u64 v;
            if (!parse(s, &v)) {
                dprintf(STDERR_FILENO,
                        "factor: '%s' is not a valid positive integer\n", s);
                rc = 1;
            } else {
                factor(v);
            }
            *p = save;
        }
    }
    return rc;
}
