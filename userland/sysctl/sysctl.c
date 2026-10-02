/* sysctl - read and set the kernel's tunables.
 *
 *   sysctl -a                       everything
 *   sysctl net.ipv4.ip_forward      one value
 *   sysctl -w vm.swappiness=10      set it
 *   sysctl -p                       apply /etc/sysctl.conf
 *
 * These are files under /proc/sys with the slashes written as dots.
 * Knowing that is most of what sysctl is; the rest is doing it for a
 * whole file at boot.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"

#define DIRENT_RECLEN 16
#define DIRENT_TYPE   18
#define DIRENT_NAME   19
#define DT_DIR         4
#define DT_REG         8

static const char *prog = "sysctl";
static bool quiet = false, names_only = false, values_only = false;

static void to_path(const char *key, char *out, size_t n)
{
    snprintf(out, n, "/proc/sys/%s", key);
    for (char *p = out + 10; *p; p++)
        if (*p == '.') *p = '/';
}

static void to_key(const char *path, char *out, size_t n)
{
    strlcpy(out, path + 10, n);
    for (char *p = out; *p; p++)
        if (*p == '/') *p = '.';
}

static bool show(const char *key)
{
    char path[512], buf[4096];
    to_path(key, path, sizeof path);
    long n = proc_read(path, buf, sizeof buf);
    if (n < 0) {
        if (!quiet)
            dprintf(STDERR_FILENO, "%s: cannot stat /proc/sys/%s: "
                                   "No such file or directory\n", prog, key);
        return false;
    }
    /* Multi-line values are printed with tabs, the way sysctl does. */
    for (long i = 0; i < n; i++) if (buf[i] == '\n' && i + 1 < n) buf[i] = '\t';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\t')) buf[--n] = '\0';
    buf[n] = '\0';

    if (names_only)       printf("%s\n", key);
    else if (values_only) printf("%s\n", buf);
    else                  printf("%s = %s\n", key, buf);
    return true;
}

static bool set(const char *key, const char *value)
{
    char path[512];
    to_path(key, path, sizeof path);
    long fd = lp_open(path, O_WRONLY, 0);
    if (fd < 0) {
        if (!quiet)
            dprintf(STDERR_FILENO, "%s: cannot stat /proc/sys/%s: "
                                   "No such file or directory\n", prog, key);
        return false;
    }
    long w = lp_write((int)fd, value, strlen(value));
    lp_close((int)fd);
    if (w < 0) {
        dprintf(STDERR_FILENO, "%s: setting key \"%s\": %s\n",
                prog, key, lp_strerror((int)-w));
        return false;
    }
    if (!quiet) printf("%s = %s\n", key, value);
    return true;
}

static void walk(const char *dir, int depth)
{
    if (depth > 12) return;
    long fd = lp_open(dir, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return;

    static char names[512][256];
    static u8 types[512];
    int n = 0;
    char buf[8192];

    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0) break;
        for (long off = 0; off < got && n < 512; ) {
            char *rec = buf + off;
            u16 len = *(u16 *)(rec + DIRENT_RECLEN);
            u8 type = *(u8 *)(rec + DIRENT_TYPE);
            char *name = rec + DIRENT_NAME;
            if (len == 0) break;
            off += len;
            if (name[0] == '.') continue;
            strlcpy(names[n], name, 256);
            types[n] = type;
            n++;
        }
        if (n >= 512) break;
    }
    lp_close((int)fd);

    for (int i = 0; i < n; i++) {
        char child[1024];
        snprintf(child, sizeof child, "%s/%s", dir, names[i]);
        if (types[i] == DT_DIR) { walk(child, depth + 1); continue; }
        if (types[i] != DT_REG && types[i] != 0) continue;
        char key[512];
        to_key(child, key, sizeof key);
        bool q = quiet; quiet = true;    /* write-only knobs are normal */
        show(key);
        quiet = q;
    }
}

static int apply_file(const char *path)
{
    int fd = STDIN_FILENO;
    if (strcmp(path, "-") != 0) {
        long f = lp_open(path, O_RDONLY, 0);
        if (f < 0) {
            dprintf(STDERR_FILENO, "%s: cannot open \"%s\": %s\n",
                    prog, path, lp_strerror((int)-f));
            return 1;
        }
        fd = (int)f;
    }
    char line[1024];
    int rc = 0;
    while (readrec(fd, line, sizeof line, '\n', NULL) >= 0) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == ';') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *v = eq + 1;
        char *end = eq - 1;
        while (end > p && (*end == ' ' || *end == '\t')) *end-- = '\0';
        while (*v == ' ' || *v == '\t') v++;
        char *ve = v + strlen(v);
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) *--ve = '\0';
        if (!set(p, v)) rc = 1;
    }
    if (fd != STDIN_FILENO) lp_close(fd);
    return rc;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "all", 0, 'a' }, { "write", 0, 'w' }, { "load", 2, 'p' },
        { "names", 0, 'N' }, { "values", 0, 'n' }, { "quiet", 0, 'q' },
        { "ignore", 0, 'e' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    bool all = false, load = false;
    const char *loadfile = "/etc/sysctl.conf";

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "awp::Nnqe", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'a': all = true; break;
        case 'w': break;                    /* implied by NAME=VALUE */
        case 'p': load = true; if (g.arg) loadfile = g.arg; break;
        case 'N': names_only = true; break;
        case 'n': values_only = true; break;
        case 'q': case 'e': quiet = true; break;
        case 'H':
            printf("Usage: sysctl [OPTION]... [VARIABLE[=VALUE]]...\n"
                   "Read or write kernel parameters under /proc/sys.\n\n"
                   "  -a, --all        display all values\n"
                   "  -w, --write      write a value (implied by NAME=VALUE)\n"
                   "  -p[FILE], --load[=FILE]  read settings from FILE\n"
                   "                             (default /etc/sysctl.conf)\n"
                   "  -n, --values     print only the values\n"
                   "  -N, --names      print only the names\n"
                   "  -q, --quiet      do not echo the value set\n"
                   "      --help     display this help and exit\n");
            return 0;
        default: lp_getopt_err(prog, &g); return 1;
        }
    }

    if (load) return apply_file(loadfile);
    if (all)  { walk("/proc/sys", 0); return 0; }

    if (g.ind >= argc) {
        dprintf(STDERR_FILENO, "%s: no variables specified\n", prog);
        dprintf(STDERR_FILENO, "Try '%s --help' for more information.\n", prog);
        return 1;
    }

    int rc = 0;
    for (int i = g.ind; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = '\0';
            if (!set(argv[i], eq + 1)) rc = 1;
        } else if (!show(argv[i])) {
            rc = 1;
        }
    }
    return rc;
}
