/* env - run a command with a changed environment, or show it.
 *
 *   env                        print the environment
 *   env NAME=value command     run command with NAME set
 *   env -i command             start from an empty environment
 *   env -u NAME command        run without NAME
 *
 * The one every Makefile and container entrypoint uses, and the reason
 * "#!/usr/bin/env sh" works.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static const char *prog = "env";

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "ignore-environment", 0, 'i' }, { "unset", 1, 'u' },
        { "chdir", 1, 'C' }, { "null", 0, '0' },
        { "help", 0, 'H' }, { 0, 0, 0 }
    };
    bool clear = false;
    char eol = '\n';
    const char *chdir_to = NULL;

    /* env stops reading options at the first thing that is not one,
     * because everything after that belongs to the command it runs. */
    int i = 1;
    const char *unset[64];
    int nunset = 0;

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if (a[0] != '-' || a[1] == '\0') break;
        if (a[1] == '-') {
            int amb;
            (void)lo; (void)amb;
            if (strcmp(a, "--ignore-environment") == 0) { clear = true; continue; }
            if (strcmp(a, "--null") == 0) { eol = '\0'; continue; }
            if (strncmp(a, "--unset=", 8) == 0) {
                if (nunset < 64) unset[nunset++] = a + 8;
                continue;
            }
            if (strcmp(a, "--unset") == 0 && i + 1 < argc) {
                if (nunset < 64) unset[nunset++] = argv[++i];
                continue;
            }
            if (strncmp(a, "--chdir=", 8) == 0) { chdir_to = a + 8; continue; }
            if (strcmp(a, "--help") == 0) {
                printf("Usage: env [OPTION]... [-] [NAME=VALUE]... [COMMAND [ARG]...]\n"
                       "Set each NAME to VALUE in the environment and run COMMAND.\n\n"
                       "  -i, --ignore-environment  start with an empty environment\n"
                       "  -0, --null                end each output line with NUL\n"
                       "  -u, --unset=NAME          remove variable from the environment\n"
                       "  -C, --chdir=DIR           change working directory to DIR\n"
                       "      --help     display this help and exit\n");
                return 0;
            }
            dprintf(STDERR_FILENO, "%s: unrecognized option '%s'\n", prog, a);
            return 125;
        }
        for (const char *f = a + 1; *f; f++) {
            switch (*f) {
            case 'i': clear = true; break;
            case '0': eol = '\0'; break;
            case 'u':
                if (f[1]) { if (nunset < 64) unset[nunset++] = f + 1; f += strlen(f) - 1; }
                else if (i + 1 < argc) { if (nunset < 64) unset[nunset++] = argv[++i]; }
                break;
            case 'C':
                if (f[1]) { chdir_to = f + 1; f += strlen(f) - 1; }
                else if (i + 1 < argc) chdir_to = argv[++i];
                break;
            default:
                dprintf(STDERR_FILENO, "%s: invalid option -- '%c'\n", prog, *f);
                return 125;
            }
        }
    }

    if (clear) {
        static char *empty[1] = { NULL };
        environ = empty;
    }
    for (int k = 0; k < nunset; k++)
        unsetenv(unset[k]);

    /* NAME=value pairs come before the command. */
    for (; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) break;
        *eq = '\0';
        setenv(argv[i], eq + 1, 1);
        *eq = '=';
    }

    if (chdir_to && lp_chdir(chdir_to) < 0) {
        dprintf(STDERR_FILENO, "%s: cannot change directory to '%s'\n", prog, chdir_to);
        return 125;
    }

    if (i >= argc) {
        for (char **e = environ; e && *e; e++)
            printf("%s%c", *e, eol);
        return 0;
    }

    /* Find it on PATH the way a shell would, so `env python3 x.py` works. */
    const char *cmd = argv[i];
    char full[1024];
    if (!strchr(cmd, '/')) {
        const char *path = getenv("PATH");
        if (!path) path = "/bin:/sbin:/usr/bin:/usr/sbin";
        const char *p = path;
        bool found = false;
        while (*p && !found) {
            const char *end = strchr(p, ':');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            if (n && n < sizeof full - 2) {
                memcpy(full, p, n);
                full[n] = '/';
                strlcpy(full + n + 1, cmd, sizeof full - n - 1);
                if (lp_access(full, X_OK) == 0) found = true;
            }
            if (!end) break;
            p = end + 1;
        }
        if (found) cmd = full;
    }

    lp_execve(cmd, &argv[i], environ);
    dprintf(STDERR_FILENO, "%s: '%s': No such file or directory\n", prog, argv[i]);
    return 127;
}
