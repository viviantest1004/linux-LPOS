/* ps - what is running, once.
 *
 *   ps [-l]
 *
 * top shows the same thing and keeps updating; this prints it and gets
 * out of the way, which is what you want inside a pipe:
 *
 *   ps | grep python
 *
 * The fields come from /proc/<pid>/stat and /proc/<pid>/statm. The name
 * in stat is wrapped in parentheses and may itself contain one, so it is
 * found by looking for the LAST closing paren rather than the first.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

typedef struct {
    pid_t pid, ppid;
    char  state;
    long  rss_kb;
    long  ticks;
    int   nice;
    char  name[32];
} proc_t;

static bool read_proc(pid_t pid, proc_t *p)
{
    char path[64], buf[512];

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    if (proc_read(path, buf, sizeof(buf)) <= 0)
        return false;

    char *close = strrchr(buf, ')');
    if (!close)
        return false;

    /* The name, between the first ( and that last ). */
    char *open = strchr(buf, '(');
    if (!open || open > close)
        return false;
    size_t nlen = (size_t)(close - open - 1);
    if (nlen >= sizeof(p->name)) nlen = sizeof(p->name) - 1;
    memcpy(p->name, open + 1, nlen);
    p->name[nlen] = '\0';

    /* From here the fields are positional: state, ppid, ... */
    char *s = close + 1;
    while (*s == ' ') s++;
    p->state = *s;

    /* field 4 is ppid, 14 utime, 15 stime, 19 nice.
     * We are standing on field 3, so count from there. */
    long fields[20];
    int  nf = 0;
    while (*s && nf < 20) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (nf == 0) {                 /* the state letter, not a number */
            while (*s && *s != ' ') s++;
            fields[nf++] = 0;
            continue;
        }
        fields[nf++] = strtol(s, &s, 10);
    }

    p->pid   = pid;
    p->ppid  = (nf > 1)  ? (pid_t)fields[1] : 0;
    p->ticks = (nf > 12) ? fields[11] + fields[12] : 0;
    p->nice  = (nf > 16) ? (int)fields[16] : 0;

    snprintf(path, sizeof(path), "/proc/%d/statm", (int)pid);
    p->rss_kb = 0;
    if (proc_read(path, buf, sizeof(buf)) > 0) {
        char *q = buf;
        while (*q && *q != ' ') q++;
        p->rss_kb = strtol(q, NULL, 10) * 4;
    }
    return true;
}

static const char *state_word(char s)
{
    switch (s) {
    case 'R': return "running";
    case 'S': return "sleeping";
    case 'D': return "waiting";       /* uninterruptible - usually disk */
    case 'Z': return "zombie";
    case 'T': return "stopped";
    default:  return "?";
    }
}

int main(int argc, char **argv)
{
    bool long_form = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) long_form = true;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: ps [-l]\n");
            printf("  -l  also show the parent and the nice value\n");
            return 0;
        }
    }

    long fd = lp_open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "ps: cannot read /proc (is it mounted?)\n");
        return 1;
    }

    if (long_form)
        printf("%5s %5s %4s %8s %9s  %s\n",
               "PID", "PPID", "NICE", "RSS(KB)", "STATE", "COMMAND");
    else
        printf("%5s %8s %9s  %s\n", "PID", "RSS(KB)", "STATE", "COMMAND");

    char buf[8192];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;

        for (long off = 0; off < n; ) {
            char       *rec  = buf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (name[0] < '1' || name[0] > '9')
                continue;
            bool numeric = true;
            for (const char *c = name; *c; c++)
                if (*c < '0' || *c > '9') { numeric = false; break; }
            if (!numeric)
                continue;

            proc_t p;
            if (!read_proc((pid_t)strtol(name, NULL, 10), &p))
                continue;

            if (long_form)
                printf("%5d %5d %4d %8ld %9s  %s\n",
                       (int)p.pid, (int)p.ppid, p.nice, p.rss_kb,
                       state_word(p.state), p.name);
            else
                printf("%5d %8ld %9s  %s\n",
                       (int)p.pid, p.rss_kb, state_word(p.state), p.name);
        }
    }

    lp_close((int)fd);
    return 0;
}
