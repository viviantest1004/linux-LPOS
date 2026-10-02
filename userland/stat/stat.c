/* stat - what the kernel actually recorded about a file.
 *
 *   stat PATH...              the whole record
 *   stat -c '%s' PATH         one field, for a script
 *   stat -t PATH              every field on one line
 *
 * The old version printed a friendly paragraph of its own invention.
 * It was readable and it was useless in a script, because `stat -c %s`
 * - the reason stat is reached for at all - printed the paragraph. This
 * is GNU's layout and GNU's format letters.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static bool opt_deref = false, opt_terse = false;
static const char *prog = "stat";

static const char *type_name(u32 mode)
{
    switch (mode & LP_S_IFMT) {
    case LP_S_IFDIR:  return "directory";
    case LP_S_IFREG:  return "regular file";
    case LP_S_IFLNK:  return "symbolic link";
    case LP_S_IFCHR:  return "character special file";
    case LP_S_IFBLK:  return "block special file";
    case LP_S_IFIFO:  return "fifo";
    case LP_S_IFSOCK: return "socket";
    default:          return "weird file";
    }
}

static char type_letter(u32 mode)
{
    switch (mode & LP_S_IFMT) {
    case LP_S_IFDIR:  return 'd';
    case LP_S_IFLNK:  return 'l';
    case LP_S_IFCHR:  return 'c';
    case LP_S_IFBLK:  return 'b';
    case LP_S_IFIFO:  return 'p';
    case LP_S_IFSOCK: return 's';
    default:          return '-';
    }
}

static void mode_string(u32 mode, char *out)
{
    static const char *rwx[] = { "---", "--x", "-w-", "-wx",
                                 "r--", "r-x", "rw-", "rwx" };
    out[0] = type_letter(mode);
    memcpy(out + 1, rwx[(mode >> 6) & 7], 3);
    memcpy(out + 4, rwx[(mode >> 3) & 7], 3);
    memcpy(out + 7, rwx[mode & 7], 3);
    out[10] = '\0';
    if (mode & 04000) out[3] = (out[3] == 'x') ? 's' : 'S';
    if (mode & 02000) out[6] = (out[6] == 'x') ? 's' : 'S';
    if (mode & 01000) out[9] = (out[9] == 'x') ? 't' : 'T';
}

/* "2026-09-07 08:55:05.684500031 +0000" */
static void time_string(char *out, size_t cap, s64 t, u32 ns)
{
    lp_tm_t tm;
    lp_localtime(t, &tm);
    int off = lp_tz_offset(t);
    char sign = off < 0 ? '-' : '+';
    int  a    = off < 0 ? -off : off;
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d.%09u %c%02d%02d",
             tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec, ns,
             sign, a / 60, a % 60);
}

static void owner_names(const lp_stat_t *st, char *user, size_t un,
                        char *group, size_t gn)
{
    lp_user_t u;
    if (lp_user_by_uid(st->uid, &u)) strlcpy(user, u.name, un);
    else                             snprintf(user, un, "UNKNOWN");
    lp_group_name(st->gid, group, gn);
    if (!group[0]) snprintf(group, gn, "UNKNOWN");
}

static u32 major_of(u64 dev) { return (u32)((dev >> 8) & 0xfff) | (u32)((dev >> 32) & ~0xfffu); }
static u32 minor_of(u64 dev) { return (u32)(dev & 0xff) | (u32)((dev >> 12) & ~0xffu); }

/* One % directive. Returns the number of characters written. */
static void one_spec(char c, const char *path, const lp_stat_t *st,
                     const char *flags, int width, int prec)
{
    char buf[512], pad[32];
    char user[64], group[64], perm[16];

    /* Build "%<flags><width>.<prec>s" once and reuse it, so a %-10n and
     * a %-10s line up the same way GNU's do. */
    if (width || prec >= 0) {
        if (prec >= 0) snprintf(pad, sizeof pad, "%%%s%d.%ds", flags, width, prec);
        else           snprintf(pad, sizeof pad, "%%%s%ds", flags, width);
    } else {
        snprintf(pad, sizeof pad, "%%s");
    }

    buf[0] = '\0';
    switch (c) {
    case 'n': strlcpy(buf, path, sizeof buf); break;
    case 'N': {
        if ((st->mode & LP_S_IFMT) == LP_S_IFLNK) {
            char tgt[512];
            long n = lp_readlink(path, tgt, sizeof tgt - 1);
            if (n < 0) n = 0;
            tgt[n] = '\0';
            snprintf(buf, sizeof buf, "'%s' -> '%s'", path, tgt);
        } else {
            snprintf(buf, sizeof buf, "'%s'", path);
        }
        break;
    }
    case 's': snprintf(buf, sizeof buf, "%llu", (unsigned long long)st->size); break;
    case 'b': snprintf(buf, sizeof buf, "%llu", (unsigned long long)st->blocks); break;
    case 'B': snprintf(buf, sizeof buf, "512"); break;
    case 'f': snprintf(buf, sizeof buf, "%x", st->mode); break;
    case 'F': strlcpy(buf, type_name(st->mode), sizeof buf); break;
    case 'a': snprintf(buf, sizeof buf, "%o", st->mode & 07777); break;
    case 'A': mode_string(st->mode, perm); strlcpy(buf, perm, sizeof buf); break;
    case 'u': snprintf(buf, sizeof buf, "%u", (unsigned)st->uid); break;
    case 'g': snprintf(buf, sizeof buf, "%u", (unsigned)st->gid); break;
    case 'U': owner_names(st, user, sizeof user, group, sizeof group);
              strlcpy(buf, user, sizeof buf); break;
    case 'G': owner_names(st, user, sizeof user, group, sizeof group);
              strlcpy(buf, group, sizeof buf); break;
    case 'd': snprintf(buf, sizeof buf, "%llu", (unsigned long long)st->dev); break;
    case 'D': snprintf(buf, sizeof buf, "%llx", (unsigned long long)st->dev); break;
    case 'i': snprintf(buf, sizeof buf, "%llu", (unsigned long long)st->ino); break;
    case 'h': snprintf(buf, sizeof buf, "%u", st->nlink); break;
    case 'o': snprintf(buf, sizeof buf, "%llu", (unsigned long long)st->blksize); break;
    case 'r': snprintf(buf, sizeof buf, "%llu", (unsigned long long)st->rdev); break;
    case 'R': snprintf(buf, sizeof buf, "%llx", (unsigned long long)st->rdev); break;
    case 't': snprintf(buf, sizeof buf, "%x", major_of(st->rdev)); break;
    case 'T': snprintf(buf, sizeof buf, "%x", minor_of(st->rdev)); break;
    case 'x': time_string(buf, sizeof buf, st->atime, st->atime_ns); break;
    case 'y': time_string(buf, sizeof buf, st->mtime, st->mtime_ns); break;
    case 'z': time_string(buf, sizeof buf, st->ctime, st->ctime_ns); break;
    case 'w': if (st->has_btime) time_string(buf, sizeof buf, st->btime, st->btime_ns);
              else               strlcpy(buf, "-", sizeof buf);
              break;
    case 'X': snprintf(buf, sizeof buf, "%lld", (long long)st->atime); break;
    case 'Y': snprintf(buf, sizeof buf, "%lld", (long long)st->mtime); break;
    case 'Z': snprintf(buf, sizeof buf, "%lld", (long long)st->ctime); break;
    case 'W': snprintf(buf, sizeof buf, "%lld",
                       st->has_btime ? (long long)st->btime : 0LL); break;
    case 'm': strlcpy(buf, "/", sizeof buf); break;
    case '%': strlcpy(buf, "%", sizeof buf); break;
    default:
        dprintf(STDERR_FILENO, "%s: %%%c: invalid directive\n", prog, c);
        return;
    }
    printf(pad, buf);
}

static void run_format(const char *fmt, const char *path, const lp_stat_t *st)
{
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { putchar(*p); continue; }
        p++;
        if (!*p) { putchar('%'); return; }

        char flags[8]; int nf = 0;
        while (*p && strchr("-+ #0", *p) && nf < 7) flags[nf++] = *p++;
        flags[nf] = '\0';
        int width = 0;
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int prec = -1;
        if (*p == '.') { p++; prec = 0; while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0'); }
        if (!*p) return;
        one_spec(*p, path, st, flags, width, prec);
    }
}

/* --printf takes backslash escapes; -c does not, and appends a newline. */
static void unescape(const char *in, char *out, size_t cap)
{
    size_t k = 0;
    for (const char *p = in; *p && k < cap - 1; p++) {
        if (*p != '\\') { out[k++] = *p; continue; }
        p++;
        switch (*p) {
        case 'n': out[k++] = '\n'; break;
        case 't': out[k++] = '\t'; break;
        case 'r': out[k++] = '\r'; break;
        case '0': out[k++] = '\0'; break;
        case '\\': out[k++] = '\\'; break;
        case '\0': out[k++] = '\\'; p--; break;
        default: out[k++] = *p; break;
        }
    }
    out[k] = '\0';
}

static void show_default(const char *path, const lp_stat_t *st)
{
    char perm[16], user[64], group[64], t[64];
    mode_string(st->mode, perm);
    owner_names(st, user, sizeof user, group, sizeof group);

    char name[1024];
    if ((st->mode & LP_S_IFMT) == LP_S_IFLNK) {
        char tgt[512];
        long n = lp_readlink(path, tgt, sizeof tgt - 1);
        if (n < 0) n = 0;
        tgt[n] = '\0';
        snprintf(name, sizeof name, "%s -> %s", path, tgt);
    } else {
        strlcpy(name, path, sizeof name);
    }

    char size[32], blocks[32], io[32], ino[32];
    snprintf(size,   sizeof size,   "%llu", (unsigned long long)st->size);
    snprintf(blocks, sizeof blocks, "%llu", (unsigned long long)st->blocks);
    snprintf(io,     sizeof io,     "%llu", (unsigned long long)st->blksize);
    snprintf(ino,    sizeof ino,    "%llu", (unsigned long long)st->ino);

    printf("  File: %s\n", name);
    printf("  Size: %-10s\tBlocks: %-10s IO Block: %-6s %s\n",
           size, blocks, io, type_name(st->mode));
    bool is_dev = (st->mode & LP_S_IFMT) == LP_S_IFCHR ||
                  (st->mode & LP_S_IFMT) == LP_S_IFBLK;
    if (is_dev)
        printf("Device: %u,%u\tInode: %-11s Links: %-5u Device type: %u,%u\n",
               major_of(st->dev), minor_of(st->dev), ino, st->nlink,
               major_of(st->rdev), minor_of(st->rdev));
    else
        printf("Device: %u,%u\tInode: %-11s Links: %u\n",
               major_of(st->dev), minor_of(st->dev), ino, st->nlink);
    printf("Access: (%04o/%s)  Uid: (%5u/%8s)   Gid: (%5u/%8s)\n",
           st->mode & 07777, perm, (unsigned)st->uid, user,
           (unsigned)st->gid, group);
    time_string(t, sizeof t, st->atime, st->atime_ns); printf("Access: %s\n", t);
    time_string(t, sizeof t, st->mtime, st->mtime_ns); printf("Modify: %s\n", t);
    time_string(t, sizeof t, st->ctime, st->ctime_ns); printf("Change: %s\n", t);
    if (st->has_btime) {
        time_string(t, sizeof t, st->btime, st->btime_ns);
        printf(" Birth: %s\n", t);
    } else {
        printf(" Birth: -\n");
    }
}

static void usage(int fd)
{
    dprintf(fd, "Usage: stat [OPTION]... FILE...\n"
                "Display file status.\n\n"
                "  -L, --dereference     follow links\n"
                "  -c  --format=FORMAT   use the specified FORMAT instead of the default;\n"
                "                          output a newline after each use of FORMAT\n"
                "      --printf=FORMAT   like --format, but interpret backslash escapes,\n"
                "                          and do not output a mandatory trailing newline\n"
                "  -t, --terse           print the information in terse form\n"
                "      --help     display this help and exit\n\n"
                "The valid format sequences for files:\n"
                "  %%a   permission bits in octal      %%A   permission bits, symbolic\n"
                "  %%b   number of blocks allocated    %%B   the size of each block\n"
                "  %%d   device number in decimal      %%D   device number in hex\n"
                "  %%f   raw mode in hex               %%F   file type\n"
                "  %%g   group ID of owner             %%G   group name of owner\n"
                "  %%h   number of hard links          %%i   inode number\n"
                "  %%n   file name                     %%N   quoted name, with target\n"
                "  %%o   optimal I/O transfer size     %%s   total size, in bytes\n"
                "  %%u   user ID of owner              %%U   user name of owner\n"
                "  %%x/%%X  time of last access         %%y/%%Y  time of last modification\n"
                "  %%z/%%Z  time of last change\n");
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "dereference", 0, 'L' }, { "format", 1, 'c' },
        { "printf", 1, 'P' }, { "terse", 0, 't' },
        { "file-system", 0, 'f' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    const char *format = NULL;
    bool raw_printf = false;

    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "Lc:tf", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'L': opt_deref = true; break;
        case 't': opt_terse = true; break;
        case 'c': format = g.arg; raw_printf = false; break;
        case 'P': format = g.arg; raw_printf = true;  break;
        case 'f': dprintf(STDERR_FILENO,
                          "%s: -f is not available here - there is no statfs\n", prog);
                  return 1;
        case 'H': usage(STDOUT_FILENO); return 0;
        default: lp_getopt_err(prog, &g); return 1;
        }
    }

    if (g.ind >= argc) {
        dprintf(STDERR_FILENO, "%s: missing operand\n", prog);
        dprintf(STDERR_FILENO, "Try '%s --help' for more information.\n", prog);
        return 1;
    }

    char fmt[2048];
    if (format) {
        if (raw_printf) unescape(format, fmt, sizeof fmt);
        else            strlcpy(fmt, format, sizeof fmt);
    } else if (opt_terse) {
        strlcpy(fmt, "%n %s %b %f %u %g %D %i %h %t %T %X %Y %Z %W %o", sizeof fmt);
    }

    int rc = 0;
    for (int i = g.ind; i < argc; i++) {
        lp_stat_t st;
        /* Without -L this is the link itself, not what it points at -
         * otherwise a broken symlink looks like a missing file. */
        long r = lp_stat(argv[i], &st, opt_deref);
        if (r < 0) {
            if (lp_voice() == LP_VOICE_GNU)
                dprintf(STDERR_FILENO, "%s: cannot statx '%s': %s\n",
                        prog, argv[i], lp_strerror((int)-r));
            else
                lp_diag(prog, NULL, NULL, "cannot stat", argv[i], (int)-r);
            rc = 1;
            continue;
        }
        if (format || opt_terse) {
            run_format(fmt, argv[i], &st);
            if (!raw_printf) putchar('\n');
        } else {
            show_default(argv[i], &st);
        }
    }
    return rc;
}
