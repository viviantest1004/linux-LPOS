/* lp-motd - the message of the day: what this machine is, where the
 * help is, a few numbers about its state, and when you were last here.
 *
 *   Welcome to linux-LP (GNU/Linux 6.8.12-lp x86_64)
 *
 *    * Documentation:  man <command>, <command> --help, info
 *    * Packages:       apt search <word>, apt list --installed | less
 *    ...
 *   Last login: Sat Sep 26 20:30:11 2026 on tty1
 *   Type help for shell builtins, commands for everything installed.
 *
 * Generated each time instead of kept in /etc/motd, because half of it
 * (the kernel, the load, the last login) is only true at the moment it
 * is printed - Ubuntu builds its motd from scripts in update-motd.d for
 * the same reason.
 *
 * Who runs it:
 *   login        after a successful console login, with --last "epoch|tty|host"
 *                (the record from BEFORE this login; --first when none)
 *   a terminal   with no arguments: the newest record in wtmp is used
 *   /etc/profile or ~/.profile may run it too; ~/.hushlogin silences
 *                login's call, as on every Linux.
 *
 * English by default, Korean when the language says ko (LC_ALL,
 * LC_MESSAGES, LANG), like the rest of the time and date output.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "osname.h"
#include "../login/wtmp.h"

static bool ko;

static const char *T(const char *en, const char *k)
{
    return ko ? k : en;
}

static bool lang_is_ko(void)
{
    const char *vars[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
    for (int i = 0; i < 3; i++) {
        const char *v = getenv(vars[i]);
        if (v && *v) return strncmp(v, "ko", 2) == 0;
    }
    return false;
}

/* NAME= and PRETTY_NAME= from /etc/os-release, when the image says LP;
 * a Debian base that has not been rebranded still prints our name. */
static void os_name(char *out, size_t cap)
{
    strlcpy(out, LP_OS_NAME, cap);
    char buf[2048];
    long fd = lp_open("/etc/os-release", O_RDONLY, 0);
    if (fd < 0) return;
    long n = lp_read((int)fd, buf, sizeof buf - 1);
    lp_close((int)fd);
    if (n <= 0) return;
    buf[n] = '\0';
    char *p = strstr(buf, "PRETTY_NAME=");
    if (!p) return;
    p += 12;
    if (*p == '"') p++;
    char *e = p;
    while (*e && *e != '"' && *e != '\n') e++;
    *e = '\0';
    if (strstr(p, "LP")) strlcpy(out, p, cap);
}

static void first_field(const char *path, char *out, size_t cap)
{
    char buf[256];
    out[0] = '\0';
    if (proc_read(path, buf, sizeof buf) <= 0) return;
    size_t i = 0;
    while (buf[i] && buf[i] != ' ' && buf[i] != '\n' && i + 1 < cap) { out[i] = buf[i]; i++; }
    out[i] = '\0';
}

static int count_processes(void)
{
    long fd = lp_open("/proc", O_RDONLY | 0200000, 0);
    if (fd < 0) return 0;
    int n = 0;
    u8 buf[8192];
    for (;;) {
        long got = sys_getdents((int)fd, buf, sizeof buf);
        if (got <= 0) break;
        for (long off = 0; off < got; ) {
            u16 reclen = *(u16 *)(buf + off + 16);
            const char *name = (const char *)(buf + off + 19);
            if (name[0] >= '1' && name[0] <= '9') n++;
            off += reclen;
        }
    }
    lp_close((int)fd);
    return n;
}

static int users_logged_in(void)
{
    long fd = lp_open(UTMP_PATH, O_RDONLY, 0);
    if (fd < 0) return 0;
    u8 rec[UT_RECLEN];
    int n = 0;
    while (lp_read((int)fd, rec, sizeof rec) == (long)sizeof rec) {
        short type;
        memcpy(&type, rec + UT_TYPE, sizeof type);
        s32 pid;
        memcpy(&pid, rec + UT_PID, sizeof pid);
        char p[32];
        snprintf(p, sizeof p, "/proc/%d", (int)pid);
        if (type == UT_USER_PROCESS && lp_exists(p)) n++;
    }
    lp_close((int)fd);
    return n;
}

/* The warmest thermal zone, in tenths of a degree; -1000 when none. */
static int temperature(void)
{
    int best = -1000;
    for (int i = 0; i < 16; i++) {
        char path[64], buf[32];
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", i);
        if (proc_read(path, buf, sizeof buf) <= 0) continue;
        long v = strtol(buf, NULL, 10);
        if (v > 0 && v < 150000 && v / 100 > best) best = (int)(v / 100);
    }
    return best;
}

static long meminfo(const char *text, const char *key)
{
    const char *p = strstr(text, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == ' ' || *p == ':') p++;
    return strtol(p, NULL, 10);
}

static void human(u64 bytes, char *out, size_t cap)
{
    const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    int i = 0;
    u64 whole = bytes, tenth = 0;
    while (whole >= 1024 && i < 4) { tenth = (whole % 1024) * 10 / 1024; whole /= 1024; i++; }
    snprintf(out, cap, "%llu.%llu%s", (unsigned long long)whole, (unsigned long long)tenth, u[i]);
}

/* One two-column row, padded by display width (Korean labels are two
 * columns a character, so strlen would misalign them). */
static void row(const char *l1, const char *v1, const char *l2, const char *v2)
{
    char left[160];
    snprintf(left, sizeof left, "  %s %s", l1, v1);
    int w = (int)utf8_str_width(left, strlen(left));
    printf("%s", left);
    for (int i = w; i < 38; i++) putchar(' ');
    if (l2) printf("%s %s", l2, v2);
    putchar('\n');
}

int main(int argc, char **argv)
{
    ko = lang_is_ko();
    const char *last_arg = NULL;
    bool first = false, brief = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--last") && i + 1 < argc) last_arg = argv[++i];
        else if (!strcmp(argv[i], "--first")) first = true;
        else if (!strcmp(argv[i], "--brief") || !strcmp(argv[i], "-b")) brief = true;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: lp-motd [--brief] [--last EPOCH|TTY|HOST | --first]\n\n"
                   "Print the message of the day: the system, where help is, the machine's\n"
                   "state and the last login. --brief leaves out the numbers.\n");
            return 0;
        }
    }

    char name[128];
    os_name(name, sizeof name);
    u8 uts[390];
    memset(uts, 0, sizeof uts);
    lp_uname(uts);
    const char *release = (const char *)uts + 130, *machine = (const char *)uts + 260;
    if (ko) printf("%s (GNU/Linux %s %s)에 오신 것을 환영합니다\n\n", name, release, machine);
    else printf("Welcome to %s (GNU/Linux %s %s)\n\n", name, release, machine);
    printf(" * %s  man <command>, <command> --help, info\n", T("Documentation:", "도움말:      "));
    printf(" * %s  apt search <word>, apt list --installed | less\n", T("Packages:     ", "패키지:      "));
    printf(" * %s  timedatectl, localectl, hostnamectl, sudo\n", T("Settings:     ", "설정:        "));
    printf("\n");

    if (!brief) {
        lp_tm_t tm;
        lp_localtime(lp_time(), &tm);
        char date[96];
        lp_strftime_lang(date, sizeof date, ko ? "%Y년 %-m월 %-d일 (%a) %H:%M:%S %Z"
                                               : "%a %b %e %H:%M:%S %Z %Y", &tm, 0,
                         ko ? LP_LANG_KO : LP_LANG_C);
        printf("  %s %s\n\n", T("System information as of", "시스템 정보:"), date);

        char load[32], procs[16], disk[64], mem[32], users[16], temp[32];
        first_field("/proc/loadavg", load, sizeof load);
        snprintf(procs, sizeof procs, "%d", count_processes());
        /* df's arithmetic: used against what the user could have, so
         * the blocks reserved for root do not read as free space. */
        lp_statfs_t sf;
        if (lp_statfs("/", &sf) == 0 && sf.blocks) {
            u64 bs = sf.frsize ? sf.frsize : sf.bsize;
            u64 used = sf.blocks - sf.bfree, avail = sf.bavail, whole = used + avail;
            char h[32];
            human(sf.blocks * bs, h, sizeof h);
            snprintf(disk, sizeof disk, "%llu.%llu%% %s %s",
                     (unsigned long long)(whole ? used * 100 / whole : 0),
                     (unsigned long long)(whole ? used * 1000 / whole % 10 : 0), T("of", "/"), h);
        } else {
            strlcpy(disk, "?", sizeof disk);
        }
        char mi[4096];
        mem[0] = '\0';
        if (proc_read("/proc/meminfo", mi, sizeof mi) > 0) {
            long t = meminfo(mi, "MemTotal"), a = meminfo(mi, "MemAvailable");
            if (t > 0 && a >= 0) snprintf(mem, sizeof mem, "%ld%%", (t - a) * 100 / t);
        }
        snprintf(users, sizeof users, "%d", users_logged_in());
        int tc = temperature();
        if (tc > -1000) snprintf(temp, sizeof temp, "%d.%d °C", tc / 10, tc % 10);
        else strlcpy(temp, "-", sizeof temp);
        row(T("System load: ", "부하:        "), load, T("Processes:      ", "프로세스:       "), procs);
        row(T("Usage of /:  ", "디스크 (/):  "), disk, T("Users logged in:", "로그인한 사용자:"), users);
        row(T("Memory usage:", "메모리:      "), mem[0] ? mem : "?", T("Temperature:    ", "온도:           "), temp);
        printf("\n");
    }

    /* Last login. */
    wtmp_last_t last;
    bool have = false;
    if (last_arg) {
        char buf[400];
        strlcpy(buf, last_arg, sizeof buf);
        char *a = buf, *b = strchr(a, '|'), *c = b ? strchr(b + 1, '|') : NULL;
        if (b && c) {
            *b = *c = '\0';
            last.when = strtoll(a, NULL, 10);
            strlcpy(last.line, b + 1, sizeof last.line);
            strlcpy(last.host, c + 1, sizeof last.host);
            have = true;
        }
    } else if (!first) {
        const char *u = getenv("USER");
        lp_user_t me;
        if ((!u || !*u) && lp_user_by_uid((uid_t)lp_getuid(), &me)) u = me.name;
        if (u && *u) have = wtmp_last(u, &last);
    }
    if (have) {
        lp_tm_t tm;
        lp_localtime(last.when, &tm);
        char when[96];
        lp_strftime_lang(when, sizeof when, ko ? "%Y년 %-m월 %-d일 (%a) %H:%M:%S" : "%a %b %e %H:%M:%S %Y",
                         &tm, 0, ko ? LP_LANG_KO : LP_LANG_C);
        if (ko)
            printf("마지막 로그인: %s, %s%s%s\n", when, last.line, last.host[0] ? ", " : "", last.host);
        else
            printf("Last login: %s on %s%s%s\n", when, last.line, last.host[0] ? " from " : "", last.host);
    }
    printf("%s\n", T("Type help for shell builtins, commands for everything installed.",
                     "셸 내장 명령은 help, 설치된 모든 명령은 commands 를 입력하세요."));
    return 0;
}
