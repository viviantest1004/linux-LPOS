/* logname - the name you logged in as.
 *
 * Not the same question as `whoami`, and that is the whole point of it:
 * after `su`, whoami says root and logname still says the person who
 * sat down at the machine. GNU answers it from the login record -
 * getlogin() takes the terminal this process is attached to and finds
 * the utmp entry the login program wrote for it.
 *
 * There is no utmp on this system. Nothing here writes login records:
 * init starts a shell on the console, ssh hands one over, su changes the
 * uid in place, and none of them keep a file of who is where. So after
 * the two things GNU tries and this system may still have -
 * /proc/self/loginuid, which only a PAM-style login sets, and
 * /var/run/utmp if some other software ever creates one - this falls
 * back to $LOGNAME, then $USER, then the passwd entry for the real uid.
 *
 * That fallback is a deliberate difference from GNU, which prints
 * "logname: no login name" here and stops. On a machine where nobody
 * keeps login records, "no login name" is technically true and useless;
 * the environment the shell was started with is the same answer a login
 * would have recorded. When even that is empty the GNU message and its
 * exit status come back, unchanged.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

/* struct utmp, as the kernel ABI has had it for twenty years: 384 bytes
 * with the fields at fixed offsets. Only three of them are needed. */
#define UTMP_RECLEN     384
#define UT_TYPE_OFF       0
#define UT_LINE_OFF       8
#define UT_LINE_LEN      32
#define UT_USER_OFF      44
#define UT_USER_LEN      32
#define USER_PROCESS      7

static char answer[UT_USER_LEN + 1];

/* PAM writes the uid of whoever logged in here, and it survives su. -1
 * (as an unsigned int, 4294967295) means nothing ever set it. */
static bool from_loginuid(void)
{
    char buf[32];
    if (proc_read("/proc/self/loginuid", buf, sizeof buf) <= 0)
        return false;
    char *end;
    long v = strtol(buf, &end, 10);
    /* (uid_t)-1 is the "no such user" sentinel. On a 32-bit machine a
     * long cannot hold it, so the comparison has to be done in the
     * width the value actually has. */
    if (end == buf || v < 0 || (unsigned long long)v == 4294967295ULL)
        return false;
    lp_user_t u;
    if (!lp_user_by_uid((uid_t)v, &u))
        return false;
    strlcpy(answer, u.name, sizeof answer);
    return true;
}

/* The terminal on standard input, without the /dev/ - the form utmp
 * stores. Empty when this is a pipe or a file. */
static void my_tty(char *out, size_t n)
{
    char path[256];
    long len = lp_readlink("/proc/self/fd/0", path, sizeof path - 1);
    out[0] = '\0';
    if (len <= 0)
        return;
    path[len] = '\0';
    if (strncmp(path, "/dev/", 5) != 0)
        return;
    strlcpy(out, path + 5, n);
}

static bool from_utmp(void)
{
    char tty[64];
    my_tty(tty, sizeof tty);
    if (!tty[0])
        return false;

    long fd = lp_open("/var/run/utmp", O_RDONLY, 0);
    if (fd < 0)
        return false;

    char rec[UTMP_RECLEN];
    bool found = false;
    while (!found) {
        long n = lp_read((int)fd, rec, sizeof rec);
        if (n != (long)sizeof rec)
            break;
        u16 type;
        memcpy(&type, rec + UT_TYPE_OFF, sizeof type);
        if (type != USER_PROCESS)
            continue;
        char line[UT_LINE_LEN + 1];
        memcpy(line, rec + UT_LINE_OFF, UT_LINE_LEN);
        line[UT_LINE_LEN] = '\0';
        if (strcmp(line, tty) != 0)
            continue;
        memcpy(answer, rec + UT_USER_OFF, UT_USER_LEN);
        answer[UT_USER_LEN] = '\0';
        found = answer[0] != '\0';
    }
    lp_close((int)fd);
    return found;
}

static bool from_env(const char *name)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return false;
    strlcpy(answer, v, sizeof answer);
    return true;
}

static bool from_passwd(void)
{
    lp_user_t u;
    if (!lp_user_by_uid((uid_t)lp_getuid(), &u))
        return false;
    strlcpy(answer, u.name, sizeof answer);
    return true;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = { { "help", 0, 'H' }, { 0, 0, 0 } };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "", lo);

    for (int c; (c = lp_getopt(&g)) != -1; ) {
        if (c == 'H') {
            printf("Usage: logname [OPTION]\n"
                   "Print the user's login name.\n\n"
                   "      --help     display this help and exit\n");
            return 0;
        }
        lp_getopt_err("logname", &g);
        return 1;
    }

    if (g.ind < argc) {
        dprintf(STDERR_FILENO, "logname: extra operand '%s'\n", argv[g.ind]);
        dprintf(STDERR_FILENO, "Try 'logname --help' for more information.\n");
        return 1;
    }

    if (from_loginuid() || from_utmp() ||
        from_env("LOGNAME") || from_env("USER") || from_passwd()) {
        printf("%s\n", answer);
        return 0;
    }

    dprintf(STDERR_FILENO, "logname: no login name\n");
    return 1;
}
