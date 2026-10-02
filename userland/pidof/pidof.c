/* pidof - the process ids of a program, by name.
 *
 *   pidof guard              107
 *   pidof sh dropbear        142 98 109
 *   pidof -s guard           just the first
 *
 * Why this exists: `kill -9 `pidof guard`` is how everybody writes it,
 * and without pidof the shell handed the literal word "pidof" to kill,
 * which then reported that it could not find a process called "`pidof".
 * Nothing in that error points at the missing command.
 *
 * kill already takes a name directly - `kill -9 guard` works and is
 * shorter - but the pidof spelling is the one in every set of notes
 * anybody has ever read, and a command that is missing is a wall.
 *
 * The name is matched against /proc/<pid>/comm, which holds the first
 * fifteen characters of the program name and nothing else: no path, no
 * arguments. So a match cannot be some unrelated process that merely
 * mentions the name on its command line - which is the bug in every
 * `ps | grep` version of this.
 *
 * Exit status is 0 when something matched and 1 when nothing did, so
 * `pidof x > /dev/null && ...` works as a test.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "syscall.h"

/* linux_dirent64 offsets */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

#define MAX_HITS 256

static bool comm_of(pid_t pid, char *out, size_t size)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);

    char buf[64];
    if (proc_read(path, buf, sizeof buf) <= 0)
        return false;

    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    strlcpy(out, buf, size);
    return true;
}

/* Every pid whose comm is `name`, oldest pid first (which is the order
 * /proc lists them in, and usually means the parent before its
 * children). Returns how many were found. */
static int pids_named(const char *name, pid_t *out, int max)
{
    long fd = lp_open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 0;

    char buf[4096];
    int  n = 0;

    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0)
            break;

        for (long off = 0; off < got && n < max; ) {
            char *rec = buf + off;
            u16   len = *(u16 *)(rec + DIRENT_RECLEN);
            char *ent = rec + DIRENT_NAME;
            if (len == 0)
                break;
            off += len;

            /* Only the all-digit entries are processes. */
            if (ent[0] < '1' || ent[0] > '9')
                continue;
            bool numeric = true;
            for (char *c = ent; *c; c++)
                if (*c < '0' || *c > '9') { numeric = false; break; }
            if (!numeric)
                continue;

            pid_t pid = (pid_t)strtol(ent, NULL, 10);
            char  comm[64];
            if (!comm_of(pid, comm, sizeof comm))
                continue;           /* it exited while we were looking */
            if (strcmp(comm, name) == 0)
                out[n++] = pid;
        }
    }

    lp_close((int)fd);
    return n;
}

static void usage(void)
{
    printf("pidof - the process ids of a program\n\n");
    printf("  pidof <name> [name...]   every pid running under that name\n");
    printf("  pidof -s <name>          only the first one\n\n");
    printf("The name is the program's own name, not its command line -\n");
    printf("\"guard\", not \"/bin/guard -r 32\". Exit status is 0 when\n");
    printf("something matched, so this works as a test:\n\n");
    printf("  pidof dropbear > /dev/null && echo SSH is up\n\n");
    printf("kill takes a name directly too, which is shorter:\n\n");
    printf("  kill -9 guard        instead of  kill -9 `pidof guard`\n");
}

int main(int argc, char **argv)
{
    bool single = false;
    int  first  = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            single = true;
            first = i + 1;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else {
            first = i;
            break;
        }
    }

    if (first >= argc) {
        usage();
        return 2;
    }

    static pid_t hits[MAX_HITS];
    int  total = 0;
    bool any   = false;

    for (int i = first; i < argc; i++) {
        int n = pids_named(argv[i], hits + total,
                           MAX_HITS - total);
        for (int j = 0; j < n; j++) {
            printf("%s%d", any ? " " : "", (int)hits[total + j]);
            any = true;
            if (single) {
                printf("\n");
                return 0;
            }
        }
        total += n;
    }

    if (!any)
        return 1;                   /* nothing matched */

    printf("\n");
    return 0;
}
