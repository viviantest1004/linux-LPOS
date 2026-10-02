/* xargs - turn lines into arguments.
 *
 *   find /data -name '*.log' | xargs rm
 *   find . -name '*.c' | xargs -n 1 wc -l
 *   ls | xargs -I {} cp {} /mnt/backup/{}
 *
 *   -n N      at most N arguments per run
 *   -I S      replace S with one argument, one run each
 *   -0        input is separated by NULs, not whitespace (find -print0)
 *   -t        print each command before running it
 *   -r        do nothing at all if the input is empty
 *
 * Without this, "do the same thing to every file the last command
 * found" needs a loop written by hand, and this system's shell has no
 * command substitution to write it with. This is the piece that makes
 * find useful.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define MAX_ARGS 1024

static bool trace = false;

/* execve takes a path, not a name, so the PATH walk is ours to do.
 * Without it `xargs rm` fails with "not found" while `rm` sits in /bin,
 * which is the sort of error that makes a tool look broken. */
static void exec_on_path(const char *name, char **args, char **envp)
{
    if (strchr(name, '/')) { lp_execve(name, args, envp); return; }

    const char *path = getenv("PATH");
    if (!path || !*path) path = "/bin:/sbin:/usr/bin:/usr/sbin:/data/bin";

    while (*path) {
        const char *end = strchr(path, ':');
        size_t len = end ? (size_t)(end - path) : strlen(path);
        char full[512];
        if (len == 0 || len >= sizeof full - 2) { if (!end) break; path = end + 1; continue; }
        memcpy(full, path, len);
        full[len] = '\0';
        strlcat(full, "/", sizeof full);
        strlcat(full, name, sizeof full);
        lp_execve(full, args, envp);       /* returns only on failure */
        if (!end) break;
        path = end + 1;
    }
}

static int run(char **args, int n)
{
    args[n] = NULL;
    if (trace) {
        for (int i = 0; i < n; i++)
            dprintf(STDERR_FILENO, "%s%s", i ? " " : "", args[i]);
        dprintf(STDERR_FILENO, "\n");
    }

    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "xargs: cannot fork\n");
        return 1;
    }
    if (pid == 0) {
        extern char **environ;
        exec_on_path(args[0], args, environ);
        dprintf(STDERR_FILENO, "xargs: %s: not found\n", args[0]);
        lp_exit(127);
    }
    int status = 0;
    lp_waitpid(pid, &status, 0);
    int rc = LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : 1;
    /* 255 is xargs' own "the command asked me to stop". Anything else
     * is reported at the end rather than abandoning the remaining
     * files, because half a job done silently is worse than a report. */
    if (rc == 255) {
        dprintf(STDERR_FILENO, "xargs: %s exited 255 - stopping\n", args[0]);
        lp_exit(124);
    }
    return rc;
}

int main(int argc, char **argv)
{
    int  max_args = 0;
    const char *replace = NULL;
    bool nul = false, skip_empty = false;
    int  i = 1;

    for (; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) max_args = atoi(argv[++i]);
        else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) replace = argv[++i];
        else if (strcmp(argv[i], "-0") == 0) nul = true;
        else if (strcmp(argv[i], "-t") == 0) trace = true;
        else if (strcmp(argv[i], "-r") == 0) skip_empty = true;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: xargs [-0trn N] [-I S] command [args...]\n");
            printf("  -n N  N arguments per run    -I S  replace S, one run each\n");
            printf("  -0    NUL separated input    -t    show each command\n");
            printf("  -r    do nothing if input is empty\n");
            return 0;
        }
        else break;
    }

    /* The command, with whatever fixed arguments came with it. */
    char *base[MAX_ARGS];
    int   base_n = 0;
    if (i >= argc) base[base_n++] = (char *)"echo";
    else for (; i < argc && base_n < MAX_ARGS - 8; i++) base[base_n++] = argv[i];

    /* Read the whole input. Streaming would be tidier, but the point of
     * xargs is to batch, and the largest thing anybody pipes into it
     * here is a file list. */
    static char in[512 * 1024];
    size_t len = 0;
    for (;;) {
        long n = lp_read(STDIN_FILENO, in + len, sizeof in - len - 1);
        if (n <= 0) break;
        len += (size_t)n;
        if (len >= sizeof in - 1) {
            dprintf(STDERR_FILENO,
                    "xargs: more input than fits in %uKB - use -n to work"
                    " in batches\n", (unsigned)(sizeof in / 1024));
            break;
        }
    }
    in[len] = '\0';

    /* Split it. */
    static char *item[MAX_ARGS * 8];
    int items = 0;
    {
        size_t p = 0;
        while (p < len && items < (int)(sizeof item / sizeof *item)) {
            if (nul) {
                if (!in[p]) { p++; continue; }
                item[items++] = in + p;
                while (p < len && in[p]) p++;
                if (p < len) in[p++] = '\0';
            } else {
                while (p < len && (in[p] == ' ' || in[p] == '\t' ||
                                   in[p] == '\n' || in[p] == '\r')) p++;
                if (p >= len) break;
                item[items++] = in + p;
                while (p < len && in[p] != ' ' && in[p] != '\t' &&
                       in[p] != '\n' && in[p] != '\r') p++;
                if (p < len) in[p++] = '\0';
            }
        }
    }

    if (items == 0) {
        if (skip_empty) return 0;
        /* Without -r, xargs runs the command once with no arguments,
         * which is what `xargs rm` doing nothing quietly would hide. */
        char *args[MAX_ARGS];
        for (int k = 0; k < base_n; k++) args[k] = base[k];
        return run(args, base_n);
    }

    int worst = 0;

    if (replace) {
        for (int k = 0; k < items; k++) {
            char *args[MAX_ARGS];
            char  subst[MAX_ARGS][512];
            int   n = 0;
            for (int b = 0; b < base_n; b++) {
                const char *hit = strstr(base[b], replace);
                if (!hit) { args[n++] = base[b]; continue; }
                /* Substitute every occurrence, not only the first:
                 * `-I {} cp {} /backup/{}` is the usual shape. */
                size_t o = 0;
                const char *src = base[b];
                size_t rl = strlen(replace);
                while (*src && o < sizeof subst[0] - 1) {
                    if (strncmp(src, replace, rl) == 0) {
                        const char *v = item[k];
                        while (*v && o < sizeof subst[0] - 1) subst[n][o++] = *v++;
                        src += rl;
                    } else subst[n][o++] = *src++;
                }
                subst[n][o] = '\0';
                args[n] = subst[n];
                n++;
            }
            int rc = run(args, n);
            if (rc > worst) worst = rc;
        }
        return worst;
    }

    int k = 0;
    while (k < items) {
        char *args[MAX_ARGS];
        int   n = 0;
        for (int b = 0; b < base_n; b++) args[n++] = base[b];
        int taken = 0;
        while (k < items && n < MAX_ARGS - 2 &&
               (max_args <= 0 || taken < max_args)) {
            args[n++] = item[k++];
            taken++;
        }
        int rc = run(args, n);
        if (rc > worst) worst = rc;
    }
    return worst;
}
