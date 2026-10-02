/* users - who is logged in, on one line.
 *
 *   users            everyone with a session open
 *   users FILE       read that login record file instead of /var/run/utmp
 *
 * The names come out sorted and space separated, and somebody logged in
 * twice is listed twice - that is what makes `users | wc -w` a count of
 * sessions rather than of people.
 *
 * The catch on this system is the same one logname has: there is no
 * utmp. Nothing here writes login records, so the file the command is
 * named after does not exist, and GNU's answer in that situation is a
 * blank line - "nobody is logged in", on a machine somebody is plainly
 * typing at. So when the default file is missing, this prints the one
 * session it can prove is there, the current one, from $LOGNAME or
 * $USER or the passwd entry for the uid. Give it a file explicitly and
 * there is no guessing: it reads that file and reports exactly what is
 * in it, which is what makes `users /var/log/wtmp` on Ubuntu and here
 * the same command.
 *
 * The record layout is the kernel's twenty-year-old struct utmp, read at
 * fixed offsets rather than through a struct, because the C compiler's
 * padding is not part of that ABI and the byte offsets are.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define UTMP_RECLEN     384
#define UT_TYPE_OFF       0
#define UT_USER_OFF      44
#define UT_USER_LEN      32
#define USER_PROCESS      7

#define NAME_CAP  (UT_USER_LEN + 1)

static char  (*names)[NAME_CAP];
static int   count, cap;

static bool add(const char *name)
{
    if (count == cap) {
        int  ncap = cap ? cap * 2 : 64;
        void *p = realloc(names, (size_t)ncap * NAME_CAP);
        if (!p)
            return false;
        names = p;
        cap = ncap;
    }
    strlcpy(names[count++], name, NAME_CAP);
    return true;
}

static bool read_utmp(const char *path)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return false;

    char rec[UTMP_RECLEN];
    for (;;) {
        long n = lp_read((int)fd, rec, sizeof rec);
        if (n != (long)sizeof rec)
            break;
        u16 type;
        memcpy(&type, rec + UT_TYPE_OFF, sizeof type);
        if (type != USER_PROCESS)
            continue;
        /* ut_user fills all 32 bytes when the name is that long, so it
         * is not necessarily terminated. */
        char name[NAME_CAP];
        memcpy(name, rec + UT_USER_OFF, UT_USER_LEN);
        name[UT_USER_LEN] = '\0';
        if (name[0] && !add(name))
            break;
    }
    lp_close((int)fd);
    return true;
}

/* The session this command is running in, for when there is no utmp. */
static void add_current(void)
{
    const char *env = getenv("LOGNAME");
    if (!env || !*env) env = getenv("USER");
    if (env && *env) { add(env); return; }

    lp_user_t u;
    if (lp_user_by_uid((uid_t)lp_getuid(), &u))
        add(u.name);
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = { { "help", 0, 'H' }, { 0, 0, 0 } };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "", lo);

    for (int c; (c = lp_getopt(&g)) != -1; ) {
        if (c == 'H') {
            printf("Usage: users [OPTION]... [FILE]\n"
                   "Output who is currently logged in according to FILE.\n"
                   "If FILE is not specified, use /var/run/utmp.  "
                   "/var/log/wtmp as FILE is common.\n\n"
                   "      --help     display this help and exit\n");
            return 0;
        }
        lp_getopt_err("users", &g);
        return 1;
    }

    if (g.ind < argc) {
        /* A named file is read as given, with no fallback: the caller
         * asked about that file and an empty answer is an answer. */
        read_utmp(argv[g.ind]);
    } else if (!read_utmp("/var/run/utmp")) {
        add_current();
    }

    /* GNU sorts by name; insertion sort because a login record file
     * holds sessions, not rows of a database. */
    for (int i = 1; i < count; i++) {
        char tmp[NAME_CAP];
        strlcpy(tmp, names[i], sizeof tmp);
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], tmp) > 0) {
            strlcpy(names[j + 1], names[j], NAME_CAP);
            j--;
        }
        strlcpy(names[j + 1], tmp, NAME_CAP);
    }

    for (int i = 0; i < count; i++)
        printf("%s%s", i ? " " : "", names[i]);
    if (count)
        printf("\n");
    free(names);
    return 0;
}
