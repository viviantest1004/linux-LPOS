/* pmap - what a process has mapped, and how much of it is real.
 *
 *   pmap PID        every mapping, with its size
 *   pmap -x PID     with resident and dirty pages beside it
 *   pmap -d PID     with the file offset and the device it came from
 *   pmap -q PID     just the mappings, for feeding to something else
 *
 * The whole command is a reader for two files the kernel already
 * writes. /proc/PID/maps has one line per mapping and costs nothing to
 * read; /proc/PID/smaps has the same lines with twenty more under each
 * one, and the kernel walks that process's page tables to produce them.
 * So the plain and -d forms read maps, and only -x reads smaps - which
 * is also why -x is the slow one on a big process, and why the numbers
 * it prints can be a moment older than the mapping list they sit next
 * to.
 *
 * The three columns that are worth knowing apart:
 *
 *   Kbytes  the size of the mapping. Address space, not memory. A
 *           program that maps a gigabyte file has a gigabyte here and
 *           may be using four kilobytes of it.
 *   RSS     the part that is actually in memory now.
 *   Dirty   the part that has been written to and so cannot simply be
 *           dropped - shared and private dirty pages added together.
 *
 * The mapping name is deliberately not the path: everything in brackets
 * and everything anonymous prints as "[ anon ]", the stack prints as
 * "[ stack ]", and a file prints as its base name. -p asks for the
 * whole path instead. The totals at the bottom are the ones people
 * quote, so "writeable/private" counts only mappings that are both -
 * memory this process alone will have to pay for.
 *
 * Not implemented: -X and -XX (a dump of whatever fields the running
 * kernel's smaps happens to have, which is explicitly not a stable
 * format), the -c/-C/-n/-N configuration files, and -A. They are left
 * out rather than faked, and asking for one says so.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static bool opt_x, opt_d, opt_q, opt_p;

/* One /proc file, read whole. Line-at-a-time would be a system call per
 * byte here, and smaps for a large process is half a megabyte. */
static char  *slurped;
static size_t slurp_len, slurp_cap;

static bool slurp(const char *path)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return false;

    slurp_len = 0;
    for (;;) {
        if (slurp_len + 65536 > slurp_cap) {
            size_t ncap = slurp_cap ? slurp_cap * 2 : 262144;
            while (slurp_len + 65536 > ncap) ncap *= 2;
            char *n = realloc(slurped, ncap);
            if (!n) { lp_close((int)fd); return false; }
            slurped = n;
            slurp_cap = ncap;
        }
        long n = lp_read((int)fd, slurped + slurp_len, 65536);
        if (n <= 0)
            break;
        slurp_len += (size_t)n;
    }
    lp_close((int)fd);
    slurped[slurp_len] = '\0';
    return true;
}

static u64 hex(const char *s, const char **end)
{
    u64 v = 0;
    for (;; s++) {
        int d;
        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        v = v * 16 + (u64)d;
    }
    if (end) *end = s;
    return v;
}

/* One line of maps or smaps:
 *   start-end perms offset major:minor inode  path
 * The path is whatever is left after the inode, spaces and all, so it
 * is taken as the rest of the line rather than as a sixth field. */
typedef struct {
    u64  start, end, offset;
    char perms[5];
    u32  major, minor;
    const char *path;
} map_t;

static bool parse_map(char *line, map_t *m)
{
    const char *p;
    m->start = hex(line, &p);
    if (*p != '-')
        return false;
    m->end = hex(p + 1, &p);
    if (*p != ' ')
        return false;
    p++;
    int i = 0;
    while (i < 4 && *p && *p != ' ') m->perms[i++] = *p++;
    m->perms[i] = '\0';
    if (i != 4)
        return false;
    while (*p == ' ') p++;
    m->offset = hex(p, &p);
    while (*p == ' ') p++;
    m->major = (u32)hex(p, &p);
    if (*p != ':')
        return false;
    m->minor = (u32)hex(p + 1, &p);
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') p++;      /* the inode */
    while (*p == ' ') p++;
    m->path = p;
    return true;
}

/* Where this process's stack started, from field 28 of
 * /proc/PID/stat. Parsing begins after the LAST ')' because field 2 is
 * the process name and a process may put ") 1 (" in its own name.
 * 0 when the kernel will not say, which is not an error - a thread has
 * no start_stack of its own and neither does a kernel task. */
static u64 start_stack(pid_t pid)
{
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    if (proc_read(path, buf, sizeof buf) <= 0)
        return 0;
    char *p = strrchr(buf, ')');
    if (!p)
        return 0;
    p++;
    /* The tokens after the name are fields 3 upwards; start_stack is
     * field 28, so it is the 26th of them. */
    for (int i = 0; i < 26; i++) {
        while (*p == ' ') p++;
        if (i == 25)
            return (u64)strtol(p, NULL, 10);
        while (*p && *p != ' ') p++;
    }
    return 0;
}

/* What pmap calls a mapping.
 *
 * The name in maps is not what decides "[ stack ]": the kernel labels
 * one mapping [stack] and pmap ignores that label, asking instead
 * whether the stack pointer this process started with lies inside the
 * mapping. The two answers differ - init here has a [stack] mapping and
 * a start_stack of 0, and pmap calls it anonymous - and matching pmap
 * means asking the same question it asks. Everything else in brackets
 * (heap, vdso, vvar, vsyscall) is anonymous memory too, and prints that
 * way; only a file keeps its name. */
static const char *mapping_name(const map_t *m, u64 stack)
{
    if (m->path[0] == '\0' || m->path[0] == '[')
        return (stack >= m->start && stack <= m->end) ? "  [ stack ]"
                                                      : "  [ anon ]";
    if (opt_p)
        return m->path;
    const char *slash = strrchr(m->path, '/');
    return slash ? slash + 1 : m->path;
}

static void mode_of(const map_t *m, char *out)
{
    out[0] = m->perms[0];
    out[1] = m->perms[1];
    out[2] = m->perms[2];
    out[3] = m->perms[3] == 's' ? 's' : '-';
    out[4] = '-';
    out[5] = '\0';
}

/* "MemFree:  123 kB" -> 123. The smaps field names are unique within a
 * mapping's block, so a plain prefix match is enough. */
static u64 smaps_kb(const char *line, const char *key, size_t klen)
{
    if (strncmp(line, key, klen) != 0 || line[klen] != ':')
        return 0;
    const char *p = line + klen + 1;
    while (*p == ' ' || *p == '\t') p++;
    return (u64)strtol(p, NULL, 10);
}

static void header(pid_t pid)
{
    char path[64], buf[4096];
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    long n = proc_read(path, buf, sizeof buf);

    if (n > 0) {
        /* The arguments are NUL separated and the last one ends with a
         * NUL too, which must not become a trailing space.
         *
         * A command line is one line here whatever it contained, so a
         * newline inside an argument becomes a space; every other
         * control character becomes a question mark, so that an argument
         * full of escape sequences cannot repaint the terminal of
         * whoever ran pmap. Bytes above 127 are left alone: they are the
         * UTF-8 of a filename somebody typed, and procps only mangles
         * those when it is running in a locale that cannot read them. */
        if (buf[n - 1] == '\0') n--;
        for (long i = 0; i < n; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (c == '\0' || c == '\n')       buf[i] = ' ';
            else if (c < 0x20 || c == 0x7f)   buf[i] = '?';
        }
        buf[n] = '\0';
        printf("%d:   %s\n", (int)pid, buf);
        return;
    }

    /* No command line at all: a kernel thread. Its name in brackets is
     * what ps prints for it too. */
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
    if (proc_read(path, buf, sizeof buf) > 0) {
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
    } else {
        buf[0] = '\0';
    }
    printf("%d:   [%s]\n", (int)pid, buf);
}

static int one_proc(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d", (int)pid);
    if (!lp_exists(path))
        return 42;

    header(pid);
    u64 stack = start_stack(pid);

    snprintf(path, sizeof path, "/proc/%d/%s", (int)pid,
             opt_x ? "smaps" : "maps");
    bool have = slurp(path);

    if (opt_x && !opt_q)
        printf("Address           Kbytes     RSS   Dirty Mode  Mapping\n");
    if (opt_d && !opt_q)
        printf("Address           Kbytes Mode  Offset           Device"
               "    Mapping\n");

    u64 total = 0, total_rss = 0, total_dirty = 0;
    u64 shared = 0, writeable_private = 0;

    /* -x has to hold the current mapping until its smaps fields have
     * been read, because the size line comes after the header line. */
    bool  pending = false;
    map_t m;
    char  mode[6] = "-----";
    u64   rss = 0, dirty = 0;
    const char *name = "";

    char *p = slurped;
    char *end = slurped + (have ? slurp_len : 0);
    while (p < end) {
        char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        *nl = '\0';

        bool is_header = (*p >= '0' && *p <= '9') ||
                         (*p >= 'a' && *p <= 'f');
        if (is_header) {
            if (pending) {
                printf("%016llx %7llu %7llu %7llu %s %s\n",
                       (unsigned long long)m.start,
                       (unsigned long long)((m.end - m.start) / 1024),
                       (unsigned long long)rss, (unsigned long long)dirty,
                       mode, name);
                total_rss += rss;
                total_dirty += dirty;
                pending = false;
            }
            if (!parse_map(p, &m)) { p = nl + 1; continue; }

            u64 kb = (m.end - m.start) / 1024;
            total += kb;
            if (m.perms[3] == 's')                       shared += kb;
            else if (m.perms[1] == 'w')                  writeable_private += kb;
            mode_of(&m, mode);
            name = mapping_name(&m, stack);

            if (opt_x) {
                rss = dirty = 0;
                pending = true;
            } else if (opt_d) {
                printf("%016llx %7llu %s %016llx %03x:%05x %s\n",
                       (unsigned long long)m.start, (unsigned long long)kb,
                       mode, (unsigned long long)m.offset,
                       m.major, m.minor, name);
            } else {
                printf("%016llx %6lluK %s %s\n",
                       (unsigned long long)m.start, (unsigned long long)kb,
                       mode, name);
            }
        } else if (pending) {
            rss   += smaps_kb(p, "Rss", 3);
            dirty += smaps_kb(p, "Shared_Dirty", 12);
            dirty += smaps_kb(p, "Private_Dirty", 13);
        }
        p = nl + 1;
    }

    if (pending) {
        printf("%016llx %7llu %7llu %7llu %s %s\n",
               (unsigned long long)m.start,
               (unsigned long long)((m.end - m.start) / 1024),
               (unsigned long long)rss, (unsigned long long)dirty,
               mode, name);
        total_rss += rss;
        total_dirty += dirty;
    }

    if (opt_q)
        return 0;

    if (opt_x) {
        printf("---------------- ------- ------- ------- \n");
        printf("total kB%16llu %7llu %7llu\n", (unsigned long long)total,
               (unsigned long long)total_rss, (unsigned long long)total_dirty);
    } else if (opt_d) {
        printf("mapped: %lluK    writeable/private: %lluK    shared: %lluK\n",
               (unsigned long long)total,
               (unsigned long long)writeable_private,
               (unsigned long long)shared);
    } else {
        printf(" total %16lluK\n", (unsigned long long)total);
    }
    return 0;
}

static void usage(int fd)
{
    dprintf(fd, "\nUsage:\n pmap [options] PID [PID ...]\n\n"
                "Options:\n"
                " -x, --extended     show details\n"
                " -d, --device       show the device format\n"
                " -q, --quiet        do not display header and footer\n"
                " -p, --show-path    show path in the mapping\n\n"
                " -h, --help         display this help and exit\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "extended", 0, 'x' }, { "device", 0, 'd' }, { "quiet", 0, 'q' },
        { "show-path", 0, 'p' }, { "help", 0, 'h' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "xdqph", lo);

    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'x': opt_x = true; break;
        case 'd': opt_d = true; break;
        case 'q': opt_q = true; break;
        case 'p': opt_p = true; break;
        case 'h': usage(STDOUT_FILENO); return 0;
        default:
            dprintf(STDERR_FILENO, "pmap: invalid option -- '%c'\n", g.badchar);
            usage(STDERR_FILENO);
            return 1;
        }
    }

    if (opt_x && opt_d) {
        dprintf(STDERR_FILENO, "pmap: options -c, -C, -d, -n, -N, -x, -X are "
                               "mutually exclusive\n");
        return 1;
    }

    if (g.ind >= argc) {
        usage(STDERR_FILENO);
        return 1;
    }

    /* Every pid is checked before any of them is printed, the way pmap
     * does it: a typo in the third one should not leave two processes
     * already dumped to the screen. */
    for (int i = g.ind; i < argc; i++) {
        char *end;
        long v = strtol(argv[i], &end, 10);
        if (end == argv[i] || *end || v < 1) {
            usage(STDERR_FILENO);
            return 1;
        }
    }

    int rc = 0;
    for (int i = g.ind; i < argc; i++) {
        int r = one_proc((pid_t)strtol(argv[i], NULL, 10));
        if (r)
            rc = r;
    }
    return rc;
}
