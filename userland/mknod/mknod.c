/* mknod - make a device node or a named pipe by hand.
 *
 *   mknod /dev/sda  b 8 0
 *   mknod /dev/null c 1 3
 *   mknod fifo      p
 *
 * On a running system devtmpfs and udev make the nodes in /dev, so this
 * is not how a device usually appears. It is how one appears when there
 * is no udev yet: building an initramfs, repairing a root filesystem
 * from a rescue shell, or putting /dev/console in place before init can
 * open it. That is why the command exists at all, and why it takes the
 * major and minor by hand - there is nothing to ask.
 *
 * The number the kernel wants is not major*256+minor, and believing it
 * is gives silently wrong nodes above minor 255. Linux splits both
 * fields: the low 8 bits of the minor and 12 bits of the major sit in
 * the low 20 bits, and the rest of each is carried above them. The
 * whole thing then has to fit in 32 bits, so a major over 4095 or a
 * minor over 1048575 is rejected here rather than being truncated into
 * a node that points somewhere else entirely.
 *
 * A type letter is read one character deep, the way coreutils reads it:
 * `mknod x bb 1 2` makes a block device. That looks like a bug and is
 * not worth diverging over - a script that relies on it would break on
 * Ubuntu.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define LP_S_IFBLK_MODE 0060000
#define LP_S_IFCHR_MODE 0020000
#define EINVAL 22

static const char *prog = "mknod";

static void try_help(void)
{
    dprintf(STDERR_FILENO, "Try '%s --help' for more information.\n", prog);
}

/* coreutils quotes an operand it is complaining about, always. */
static void quoted(char *out, size_t cap, const char *s)
{
    snprintf(out, cap, "'%s'", s);
}

/* A file name in a kernel diagnostic is quoted only when it needs it. */
static void quote_if_needed(char *out, size_t cap, const char *s)
{
    bool need = (*s == '\0' || *s == '~' || *s == '#');
    for (const unsigned char *p = (const unsigned char *)s; *p && !need; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9'))
            continue;
        if (!strchr("%+,-./:=@_", (char)*p))
            need = true;
    }
    if (need) quoted(out, cap, s);
    else      strlcpy(out, s, cap);
}

/* The umask, read out of /proc rather than by calling umask() twice -
 * there is no lp_umask, and /proc/self/status has carried the value
 * since Linux 4.7. It is needed for one case only: a symbolic mode that
 * names no class ("+x") leaves the umask in charge of which of user,
 * group and other actually get the bit. Without /proc the answer is 0,
 * which is the same as saying the umask is empty. */
static mode_t current_umask(void)
{
    char buf[4096];
    long n = proc_read("/proc/self/status", buf, sizeof buf - 1);
    if (n <= 0) return 0;
    buf[n] = '\0';
    const char *p = strstr(buf, "Umask:");
    if (!p) return 0;
    p += 6;
    while (*p == ' ' || *p == '\t') p++;
    mode_t v = 0;
    for (; *p >= '0' && *p <= '7'; p++)
        v = v * 8 + (mode_t)(*p - '0');
    return v & 07777;
}

/* ── The mode ──
 *
 * -m takes what chmod takes. The starting point is a=rw, so `-m u=rw,go=`
 * ends at 0600 and `-m 600` says the same thing the short way. Only the
 * permission bits may be set: setuid on a device node would be a bug in
 * the making, so 1777 is refused rather than quietly dropped.
 */
static bool mode_apply(const char *spec, mode_t *mode)
{
    if (*spec == '\0')
        return false;

    if (*spec >= '0' && *spec <= '7') {
        mode_t v = 0;
        for (const char *p = spec; *p; p++) {
            if (*p < '0' || *p > '7') return false;
            v = v * 8 + (mode_t)(*p - '0');
            if (v > 07777) return false;
        }
        *mode = v;
        return true;
    }

    mode_t cur = *mode;
    const char *p = spec;
    for (;;) {
        mode_t who = 0;
        for (; *p; p++) {
            if (*p == 'u')      who |= 04700;
            else if (*p == 'g') who |= 02070;
            else if (*p == 'o') who |= 00007;
            else if (*p == 'a') who |= 07777;
            else break;
        }
        /* No class named means all of them, less whatever the umask
         * says this process is not handing out. */
        if (who == 0) who = 07777 & ~current_umask();

        if (*p != '+' && *p != '-' && *p != '=')
            return false;

        for (; *p == '+' || *p == '-' || *p == '='; ) {
            char op = *p++;
            mode_t bits = 0;
            for (; *p && *p != ',' && *p != '+' && *p != '-' && *p != '='; p++) {
                switch (*p) {
                case 'r': bits |= 0444; break;
                case 'w': bits |= 0222; break;
                case 'x': bits |= 0111; break;
                case 's': bits |= 06000; break;
                case 't': bits |= 01000; break;
                default:  return false;
                }
            }
            bits &= who;
            if (op == '+')      cur |= bits;
            else if (op == '-') cur &= ~bits;
            else                cur = (cur & ~who) | bits;
        }

        if (*p == '\0') break;
        if (*p != ',') return false;
        p++;
    }
    *mode = cur;
    return true;
}

/* Leading blanks are allowed and trailing ones are not, which is what
 * strtoumax does and therefore what coreutils accepts. Base 0: 0x is
 * hex, a leading 0 is octal. */
static bool parse_num(const char *s, u64 *out)
{
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '+') p++;

    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    else if (p[0] == '0' && p[1]) base = 8;

    if (!*p) return false;
    u64 v = 0;
    for (; *p; p++) {
        int d;
        if (*p >= '0' && *p <= '9')      d = *p - '0';
        else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else return false;
        if (d >= base) return false;
        if (v > (~(u64)0 - (u64)d) / (u64)base) return false;
        v = v * (u64)base + (u64)d;
    }
    /* major_t and minor_t are 32 bits; anything wider is not a number
     * this can mean. */
    if (v > 0xffffffffu) return false;
    *out = v;
    return true;
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "mode", 1, 'm' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    mode_t mode = 0666;
    bool   mode_given = false;
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "m:", lo);

    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case 'm':
            mode_given = true;
            if (!g.arg || !mode_apply(g.arg, &mode)) {
                dprintf(STDERR_FILENO, "%s: invalid mode\n", prog);
                return 1;
            }
            break;
        case 'H':
            printf("Usage: mknod [OPTION]... NAME TYPE [MAJOR MINOR]\n"
                   "Create the special file NAME of the given TYPE.\n\n"
                   "Mandatory arguments to long options are mandatory for short options too.\n"
                   "  -m, --mode=MODE    set file permission bits to MODE, not a=rw - umask\n"
                   "      --help        display this help and exit\n\n"
                   "Both MAJOR and MINOR must be specified when TYPE is b, c, or u, and they\n"
                   "must be omitted when TYPE is p.  If MAJOR or MINOR begins with 0x or 0X,\n"
                   "it is interpreted as hexadecimal; otherwise, if it begins with 0, as octal;\n"
                   "otherwise, as decimal.  TYPE may be:\n\n"
                   "  b      create a block (buffered) special file\n"
                   "  c, u   create a character (unbuffered) special file\n"
                   "  p      create a FIFO\n");
            return 0;
        default: lp_getopt_err(prog, &g); return 1;
        }
    }
    if (mode_given && (mode & ~(mode_t)0777)) {
        dprintf(STDERR_FILENO,
                "%s: mode must specify only file permission bits\n", prog);
        return 1;
    }

    int nops = argc - g.ind;
    char q[512];

    if (nops == 0) {
        dprintf(STDERR_FILENO, "%s: missing operand\n", prog);
        try_help();
        return 1;
    }
    if (nops == 1) {
        quoted(q, sizeof q, argv[g.ind]);
        dprintf(STDERR_FILENO, "%s: missing operand after %s\n", prog, q);
        try_help();
        return 1;
    }

    const char *name = argv[g.ind];
    const char *type = argv[g.ind + 1];
    mode_t      kind;
    u64         dev = 0;

    if (*type == 'p') {
        if (nops > 2) {
            quoted(q, sizeof q, argv[g.ind + 2]);
            dprintf(STDERR_FILENO, "%s: extra operand %s\n", prog, q);
            /* Four operands is somebody typing `mknod f p 1 3`, so say
             * what is actually wrong instead of only naming the extra. */
            if (nops == 4)
                dprintf(STDERR_FILENO,
                        "Fifos do not have major and minor device numbers.\n");
            try_help();
            return 1;
        }
        kind = LP_S_IFIFO_MODE;
    } else {
        if (nops < 4) {
            quoted(q, sizeof q, argv[argc - 1]);
            dprintf(STDERR_FILENO, "%s: missing operand after %s\n", prog, q);
            if (nops == 2)
                dprintf(STDERR_FILENO,
                        "Special files require major and minor device numbers.\n");
            try_help();
            return 1;
        }
        if (nops > 4) {
            quoted(q, sizeof q, argv[g.ind + 4]);
            dprintf(STDERR_FILENO, "%s: extra operand %s\n", prog, q);
            try_help();
            return 1;
        }
        if (*type == 'b') {
            kind = LP_S_IFBLK_MODE;
        } else if (*type == 'c' || *type == 'u') {
            kind = LP_S_IFCHR_MODE;
        } else {
            quoted(q, sizeof q, type);
            dprintf(STDERR_FILENO, "%s: invalid device type %s\n", prog, q);
            try_help();
            return 1;
        }

        u64 major, minor;
        if (!parse_num(argv[g.ind + 2], &major)) {
            quoted(q, sizeof q, argv[g.ind + 2]);
            dprintf(STDERR_FILENO, "%s: invalid major device number %s\n", prog, q);
            return 1;
        }
        if (!parse_num(argv[g.ind + 3], &minor)) {
            quoted(q, sizeof q, argv[g.ind + 3]);
            dprintf(STDERR_FILENO, "%s: invalid minor device number %s\n", prog, q);
            return 1;
        }
        dev = ((major & 0xfff) << 8) | (minor & 0xff) |
              ((major & ~(u64)0xfff) << 32) | ((minor & ~(u64)0xff) << 12);
    }

    /* mknodat takes the encoded number in 32 bits. A major or minor too
     * wide to fit would otherwise lose its top bits inside the kernel
     * and make a node for a different device without saying so. */
    long r = (dev >> 32) ? -EINVAL : lp_mknod(name, kind | mode, dev);
    if (r < 0) {
        quote_if_needed(q, sizeof q, name);
        dprintf(STDERR_FILENO, "%s: %s: %s\n", prog, q, lp_strerror((int)-r));
        return 1;
    }
    /* The kernel trimmed the mode with the umask on the way in. -m is an
     * instruction, not a suggestion, so put it back. */
    if (mode_given)
        lp_chmod(name, mode);
    return 0;
}
